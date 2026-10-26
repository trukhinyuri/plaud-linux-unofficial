#!/bin/sh
# Launcher for Plaud Desktop for Linux (unofficial).
#
# Chooses the display backend and the Chromium sandbox mode, adds user flags
# from ${XDG_CONFIG_HOME:-~/.config}/plaud-linux-flags.conf (one flag per
# line, "#" starts a comment) and starts the bundled Electron.
#
# Environment:
#   PLAUD_LINUX_OZONE=auto|x11|wayland  display backend (default: auto)
#     auto runs through XWayland when it is available: the app positions its
#     tray panel, recording widget and screenshot overlays itself, which
#     Wayland clients cannot do.
#   PLAUD_FFMPEG=/path/to/ffmpeg        ffmpeg used for uploads

set -u

self=$(readlink -f -- "$0")
appdir=$(dirname -- "$self")
bin="$appdir/@EXECUTABLE@"

: "${PLAUD_LINUX_LAUNCHER:=$self}"
export PLAUD_LINUX_LAUNCHER

user_argc=$#

ozone="${PLAUD_LINUX_OZONE:-auto}"
if [ "$ozone" = auto ]; then
  if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -n "${DISPLAY:-}" ]; then
    ozone=x11
  elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
    ozone=wayland
  else
    ozone=x11
  fi
fi
case "$ozone" in
  x11)
    set -- "$@" --ozone-platform=x11
    ;;
  wayland)
    set -- "$@" --ozone-platform=wayland --enable-features=WaylandWindowDecorations --enable-wayland-ime
    ;;
  *)
    echo "plaud-linux: PLAUD_LINUX_OZONE must be auto, x11 or wayland (got '$ozone')" >&2
    exit 2
    ;;
esac

# Chromium needs either unprivileged user namespaces (its namespace sandbox)
# or a setuid-root chrome-sandbox helper. AppImages and per-user installs
# cannot ship a setuid helper.
sandbox="$appdir/chrome-sandbox"
if [ -u "$sandbox" ] && [ "$(stat -c %u -- "$sandbox" 2>/dev/null)" = 0 ]; then
  :
elif command -v unshare >/dev/null 2>&1 && unshare --user --map-root-user --pid --net --fork true 2>/dev/null; then
  :
else
  echo "plaud-linux: no usable Chromium sandbox (user namespaces are restricted and chrome-sandbox is not setuid root); starting with --no-sandbox" >&2
  set -- "$@" --no-sandbox
fi

flags_file="${XDG_CONFIG_HOME:-$HOME/.config}/plaud-linux-flags.conf"
if [ -r "$flags_file" ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      ''|'#'*) continue ;;
    esac
    set -- "$@" "$line"
  done < "$flags_file"
fi

# Move the user's arguments behind the flags added above.
i=0
while [ "$i" -lt "$user_argc" ]; do
  set -- "$@" "$1"
  shift
  i=$((i + 1))
done

exec "$bin" "$@"
