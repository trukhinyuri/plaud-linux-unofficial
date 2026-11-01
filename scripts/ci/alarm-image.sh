#!/bin/sh
# Creates a local container image "alarm:latest" with Arch Linux ARM
# (aarch64): there is no official Arch Linux container image for aarch64.
# The generic root file system is verified with the GPG signature of the
# Arch Linux ARM Build System key, then imported and initialised.
#
#   scripts/ci/alarm-image.sh [docker|podman]
set -eu

engine=${1:-docker}
url=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
# Arch Linux ARM Build System <builder@archlinuxarm.org>
# (https://archlinuxarm.org/about/package-signing)
key=68B3537F39A313B3E574D06777193F152BDBE6A6

if [ "$(uname -m)" != aarch64 ]; then
  echo "alarm-image.sh: run on an aarch64 host" >&2
  exit 1
fi

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
export GNUPGHOME="$dir/gnupg"
mkdir -m 700 "$GNUPGHOME"

curl -fsSL --retry 3 -o "$dir/rootfs.tar.gz" "$url"
curl -fsSL --retry 3 -o "$dir/rootfs.tar.gz.sig" "$url.sig"
gpg --batch --quiet --keyserver hkps://keyserver.ubuntu.com --recv-keys "$key"
gpg --batch --status-fd 1 --verify "$dir/rootfs.tar.gz.sig" "$dir/rootfs.tar.gz" | grep -q "^\[GNUPG:\] VALIDSIG $key "

"$engine" import --change 'CMD ["/bin/bash"]' "$dir/rootfs.tar.gz" alarm:base >/dev/null
"$engine" rm -f alarm-init >/dev/null 2>&1 || true
# pacman's download sandbox needs Landlock, which container runtimes may
# not offer; the free-space check fails on overlay file systems.
"$engine" run --name alarm-init alarm:base /bin/bash -c '
  set -eu
  sed -i -e "s/^#\?DisableSandbox.*/DisableSandbox/" -e "s/^CheckSpace/#CheckSpace/" /etc/pacman.conf
  grep -q "^DisableSandbox" /etc/pacman.conf || sed -i "/^\[options\]/a DisableSandbox" /etc/pacman.conf
  pacman-key --init
  pacman-key --populate archlinuxarm
  pacman -Syu --noconfirm
  pacman -Rns --noconfirm linux-aarch64 linux-firmware 2>/dev/null || true
  pacman -Scc --noconfirm >/dev/null
'
"$engine" commit --change 'CMD ["/bin/bash"]' alarm-init alarm:latest >/dev/null
"$engine" rm alarm-init >/dev/null
"$engine" rmi alarm:base >/dev/null 2>&1 || true
echo "alarm:latest ready"
