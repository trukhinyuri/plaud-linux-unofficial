#!/bin/sh
# Installs an installer package in a fresh distribution container (as root),
# which downloads, verifies and patches the official release in its install
# script, then checks the result and removes the package again.
#
#   scripts/ci/installer-smoke.sh <package> [--user]
#
# --user also builds a per-user copy as an unprivileged user (the path taken
# when the system-wide build is missing).
#
# Local use: podman run --rm -v "$PWD:/src" ubuntu:24.04 /src/scripts/ci/installer-smoke.sh /src/dist-installer/<file>.deb
set -eu

pkg=${1:?usage: installer-smoke.sh <package> [--user]}
user_test=false
[ "${2:-}" = --user ] && user_test=true
SRC=$(cd -- "$(dirname -- "$0")/../.." && pwd)
want=$(sed -n 's/^ *"version": "\([^"]*\)",*$/\1/p' "$SRC/official.json")
APP=/opt/plaud-linux
LIB=/usr/lib/plaud-linux-unofficial-installer
ICON=/usr/local/share/icons/hicolor/256x256/apps/io.github.trukhinyuri.PlaudLinux.png

step() { printf '\n== %s\n' "$*"; }

case "$pkg" in
  *.deb)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    install_pkg() { apt-get install -y -q "$pkg"; }
    remove_pkg() { apt-get purge -y -q plaud-linux-unofficial-installer; }
    extra() { apt-get install -y -q --no-install-recommends desktop-file-utils; }
    ;;
  *.rpm)
    # ffmpeg (ffmpeg-free) comes from EPEL on RHEL-compatible systems.
    if [ -f /etc/redhat-release ] && ! grep -qi fedora /etc/redhat-release; then
      dnf install -y -q dnf-plugins-core epel-release
      dnf config-manager --set-enabled crb
    fi
    install_pkg() { dnf install -y -q "$pkg"; }
    remove_pkg() { dnf remove -y -q plaud-linux-unofficial-installer; }
    extra() { dnf install -y -q desktop-file-utils procps-ng; }
    ;;
  *.pkg.tar.zst)
    pacman -Syu --noconfirm --needed >/dev/null
    install_pkg() { pacman -U --noconfirm "$pkg"; }
    remove_pkg() { pacman -Rns --noconfirm plaud-linux-unofficial-installer; }
    extra() { pacman -S --noconfirm --needed desktop-file-utils procps-ng >/dev/null; }
    ;;
  *)
    echo "unknown package type: $pkg" >&2
    exit 2
    ;;
esac

step "install $(basename "$pkg") (runs plaud-linux-setup --system)"
install_pkg
extra

step "check the system-wide build"
test -x "$APP/plaud-linux"
test -u "$APP/chrome-sandbox"
test "$(stat -c %u "$APP/chrome-sandbox")" = 0
test -f "$ICON"
plaud-linux-setup --status
grep -q "\"upstreamVersion\": \"$want\"" "$APP/resources/plaud-linux/build-info.json"
desktop-file-validate /usr/share/applications/io.github.trukhinyuri.PlaudLinux.desktop
# The patched archive starts the Linux loader, and both addons load in the
# app's Electron.
ELECTRON_RUN_AS_NODE=1 "$APP/plaud-linux-bin" -e '
  const { AsarArchive } = require(process.argv[1] + "/patcher/lib/asar");
  const a = AsarArchive.open(process.argv[2] + "/resources/app.asar");
  const pkg = JSON.parse(a.readFile("package.json"));
  if (pkg.main !== "linux-runtime/loader.js") throw new Error("unexpected main " + pkg.main);
  const manifest = JSON.parse(a.readFile("linux-runtime/patch-manifest.json"));
  console.log("patched Plaud", manifest.upstreamVersion, "with", manifest.patches.length, "patches");
  for (const f of ["recorder_linux.node", "audio_monitor_linux.node"]) {
    const m = require(process.argv[2] + "/resources/" + f);
    console.log(f, Object.keys(m).join(","));
  }
' "$LIB" "$APP"
# Nothing of Plaud's is left in the work directory except the verified downloads.
if find /var/cache/plaud-linux-unofficial-installer -name app.asar | grep -q .; then
  echo "an extracted official archive was left in the cache" >&2
  exit 1
fi

if [ "$user_test" = true ]; then
  step "per-user build as an unprivileged user"
  id tester >/dev/null 2>&1 || useradd -m tester
  su tester -s /bin/sh -c 'set -eu; plaud-linux-setup --user; test -x "$HOME/.local/share/plaud-linux/app/plaud-linux"; plaud-linux-setup --user --status; plaud-linux-setup --user --remove; test ! -e "$HOME/.local/share/plaud-linux/app"'
fi

step "remove the package"
remove_pkg
test ! -e "$APP"
test ! -e "$ICON"
test ! -e "$LIB"
test ! -e /var/cache/plaud-linux-unofficial-installer
echo "installer smoke test passed"
