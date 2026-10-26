#!/bin/sh
# Runs a command inside a private D-Bus session with PipeWire and WirePlumber
# and two virtual audio devices (a speaker and a microphone), as a desktop
# session would provide them. Used by CI; the official app refuses to start
# on a machine without any audio device.
#
# The session is isolated so that it is also safe on a workstation: its own
# runtime, configuration and state directories, no D-Bus service activation,
# and WirePlumber without hardware monitors (no ALSA, Bluetooth or camera
# devices are opened, no WirePlumber state of the user is read or written).
#
# Usage: scripts/ci/session.sh <command> [args...]
set -eu

SESSION_DIR=$(mktemp -d)
chmod 700 "$SESSION_DIR"
trap 'rm -rf "$SESSION_DIR"' EXIT

mkdir -p "$SESSION_DIR/runtime" "$SESSION_DIR/config/wireplumber/wireplumber.conf.d" "$SESSION_DIR/state" \
  "$SESSION_DIR/config/wireplumber/main.lua.d" "$SESSION_DIR/config/wireplumber/bluetooth.lua.d"
chmod 700 "$SESSION_DIR/runtime"
# WirePlumber 0.5 reads the monitor switches from the profile below;
# WirePlumber 0.4 (Ubuntu 24.04, Debian 12) from Lua fragments, which it merges
# by file name with its own *.lua.d files (51-* runs after the 50-*-config.lua
# defaults and before 90-enable-all.lua).
cat >"$SESSION_DIR/config/wireplumber/wireplumber.conf.d/90-ci-no-hardware.conf" <<'EOF'
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
  }
}
EOF
cat >"$SESSION_DIR/config/wireplumber/main.lua.d/51-ci-no-hardware.lua" <<'EOF'
if alsa_monitor then alsa_monitor.enabled = false end
if v4l2_monitor then v4l2_monitor.enabled = false end
if libcamera_monitor then libcamera_monitor.enabled = false end
EOF
cat >"$SESSION_DIR/config/wireplumber/bluetooth.lua.d/51-ci-no-hardware.lua" <<'EOF'
if bluez_monitor then bluez_monitor.enabled = false end
if bluez_midi_monitor then bluez_midi_monitor.enabled = false end
EOF
cat >"$SESSION_DIR/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:dir=$SESSION_DIR/runtime</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF

export SESSION_DIR
dbus-run-session --config-file="$SESSION_DIR/bus.conf" -- sh -eu -c '
  export XDG_RUNTIME_DIR="$SESSION_DIR/runtime"
  daemon_env="XDG_CONFIG_HOME=$SESSION_DIR/config XDG_STATE_HOME=$SESSION_DIR/state"
  env $daemon_env pipewire >"$SESSION_DIR/pipewire.log" 2>&1 &
  pw=$!
  i=0
  until pw-cli info 0 >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || { echo "session: PipeWire did not start" >&2; cat "$SESSION_DIR/pipewire.log" >&2; exit 1; }
    sleep 0.1
  done
  env $daemon_env wireplumber >"$SESSION_DIR/wireplumber.log" 2>&1 &
  wp=$!
  pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=ci-speaker node.description=\"CI speaker\" media.class=Audio/Sink object.linger=true audio.position=[FL FR] }" >/dev/null
  pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=ci-microphone node.description=\"CI microphone\" media.class=Audio/Source/Virtual object.linger=true audio.position=[MONO] }" >/dev/null
  sleep 1
  status=0
  "$@" || status=$?
  kill "$wp" "$pw" 2>/dev/null || true
  wait 2>/dev/null || true
  exit "$status"
' session "$@"
