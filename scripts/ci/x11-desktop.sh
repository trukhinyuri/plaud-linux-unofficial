#!/bin/sh
# Runs a command on the X display in $DISPLAY (CI: Xvfb from xvfb-run) with a
# window manager and one application window under the pointer, as an X11
# desktop session would provide them: the native recorder's live
# screen-capture tests capture the displays and the window at the cursor.
# Start the X server with -noreset: without it, Xvfb resets whenever its last
# client disconnects, and connections made during a reset are refused.
#
# Usage: xvfb-run -a -s '-screen 0 1920x1080x24 -noreset' scripts/ci/x11-desktop.sh <command> [args...]
set -eu
: "${DISPLAY:?x11-desktop.sh needs an X display}"

# Polls `xprop -root <property>` until it names a window (10 s at most); an
# empty list prints as "window id # " with no id.
wait_for_root_window() {
  i=0
  until xprop -root "$1" 2>/dev/null | grep -q 'window id # 0x'; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || { echo "x11-desktop: $2 did not come up" >&2; exit 1; }
    sleep 0.1
  done
}

openbox --sm-disable >/dev/null 2>&1 &
wm=$!
wait_for_root_window _NET_SUPPORTING_WM_CHECK 'the window manager'
# The X server starts with the pointer in the middle of the screen; the window
# covers the middle half of it.
width=$(xwininfo -root | awk '/^ +Width:/ { print $2 }')
height=$(xwininfo -root | awk '/^ +Height:/ { print $2 }')
xlogo -geometry "$((width / 2))x$((height / 2))+$((width / 4))+$((height / 4))" >/dev/null 2>&1 &
app=$!
wait_for_root_window _NET_CLIENT_LIST 'the application window'

status=0
"$@" || status=$?
kill "$app" "$wm" 2>/dev/null || true
wait 2>/dev/null || true
exit "$status"
