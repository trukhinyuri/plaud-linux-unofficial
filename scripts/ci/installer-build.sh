#!/bin/sh
# Builds the installer package of one format inside a distribution container
# (as root), from a copy of the source tree mounted at $SRC (default: the
# repository this script belongs to). The Linux addons are compiled in the
# container, so they link against the oldest distribution the package
# targets. Packages are written to $SRC/dist-installer/.
#
#   scripts/ci/installer-build.sh deb      # in ubuntu:22.04
#   scripts/ci/installer-build.sh rpm      # in rockylinux:9
#   scripts/ci/installer-build.sh pacman   # in archlinux (x86_64) or Arch Linux ARM (aarch64)
#
# Local use: podman run --rm -v "$PWD:/src" -e SRC=/src ubuntu:22.04 /src/scripts/ci/installer-build.sh deb
set -eu

format=${1:?usage: installer-build.sh deb|rpm|pacman}
SRC=${SRC:-$(cd -- "$(dirname -- "$0")/../.." && pwd)}
WORK=/tmp/plaud-linux-installer-build

# Node.js for the build tools (the distributions' versions are too old on
# Ubuntu 22.04 and Rocky Linux 9). Pinned by SHA-256 (nodejs.org SHASUMS256.txt).
NODE_VERSION=22.23.4
NODE_SHA256_x64=1805bcd35e6815559bd2eb97db438d3fdb0eb3a5e6a977b7c1badca76337e9ce
NODE_SHA256_arm64=6287aa46b6c375d816e63ae66d024fbbfd70404825aabdfdf898a970871d2f4b

install_node() {
  if command -v node >/dev/null 2>&1 && node -e 'const [a,b]=process.versions.node.split(".").map(Number);process.exit(a>22||(a===22&&b>=12)?0:1)'; then
    return
  fi
  case "$(uname -m)" in
    x86_64) node_arch=x64; sha=$NODE_SHA256_x64 ;;
    aarch64) node_arch=arm64; sha=$NODE_SHA256_arm64 ;;
    *) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
  esac
  tarball=/tmp/node-v$NODE_VERSION-linux-$node_arch.tar.xz
  curl -fsSL --retry 3 -o "$tarball" "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-$node_arch.tar.xz"
  printf '%s  %s\n' "$sha" "$tarball" | sha256sum -c -
  mkdir -p /opt/node
  tar -xJf "$tarball" -C /opt/node --strip-components=1
  PATH=/opt/node/bin:$PATH
  export PATH
}

case "$format" in
  deb)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q --no-install-recommends \
      ca-certificates curl xz-utils git build-essential cmake ninja-build meson python3 pkg-config binutils dpkg-dev \
      libpipewire-0.3-dev libopus-dev libogg-dev libdbus-1-dev libpng-dev zlib1g-dev libx11-dev libxrandr-dev
    ;;
  rpm)
    dnf install -y -q dnf-plugins-core
    dnf config-manager --set-enabled crb >/dev/null 2>&1 || true
    # The image's curl-minimal provides curl.
    dnf install -y -q --setopt=install_weak_deps=False \
      ca-certificates xz tar git gcc-c++ make cmake ninja-build python3 pkgconf-pkg-config binutils rpm-build \
      pipewire-devel opus-devel libogg-devel dbus-devel libpng-devel zlib-devel libX11-devel libXrandr-devel
    ;;
  pacman)
    pacman -Syu --noconfirm --needed base-devel git nodejs npm cmake ninja meson python pkgconf curl \
      libpipewire opus libogg dbus libpng zlib libx11 libxrandr
    id builder >/dev/null 2>&1 || useradd -m builder
    ;;
  *)
    echo "unknown format $format" >&2
    exit 2
    ;;
esac
install_node

# Work on a copy: the build must not leave container-built addons or root-
# owned files in the source tree.
rm -rf "$WORK"
mkdir -p "$WORK"
tar -C "$SRC" \
  --exclude=./node_modules --exclude=./build --exclude=./dist --exclude=./dist-installer --exclude=./.cache --exclude=./.git \
  --exclude='./native/*/build' --exclude='./native/*/node_modules' \
  --exclude='./native/recorder/deps/build' --exclude='./native/recorder/deps/prefix' --exclude='./native/recorder/deps/src' \
  -cf - . | tar -C "$WORK" -xf -

build() {
  cd "$WORK"
  npm ci --no-audit --no-fund
  npm run build:native
  node scripts/package-installer.js --targets "$format"
  node scripts/package-installer.js --verify dist/*
}

if [ "$format" = pacman ]; then
  chown -R builder: "$WORK"
  su builder -s /bin/sh -c "set -eu; cd '$WORK'; export PATH='$PATH'; npm ci --no-audit --no-fund; npm run build:native; node scripts/package-installer.js --targets pacman; node scripts/package-installer.js --verify dist/*"
else
  build
fi

mkdir -p "$SRC/dist-installer"
cp "$WORK"/dist/* "$SRC/dist-installer/"
ls -l "$SRC/dist-installer"
