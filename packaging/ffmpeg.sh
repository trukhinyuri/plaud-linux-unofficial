#!/bin/sh
# Plaud Desktop for Linux (unofficial): the official app runs
# "<resources>/ffmpeg" to encode recordings (Ogg/Opus) before upload.
# This wrapper runs $PLAUD_FFMPEG or the first other ffmpeg on $PATH, so the
# distribution's ffmpeg (and its security updates) is used.

if [ -n "${PLAUD_FFMPEG:-}" ]; then
  exec "$PLAUD_FFMPEG" "$@"
fi

self=$(readlink -f -- "$0")
set -f
old_ifs=$IFS
IFS=:
for dir in $PATH; do
  [ -n "$dir" ] || continue
  candidate="$dir/ffmpeg"
  if [ -x "$candidate" ] && [ ! -d "$candidate" ] && [ "$(readlink -f -- "$candidate")" != "$self" ]; then
    IFS=$old_ifs
    exec "$candidate" "$@"
  fi
done
IFS=$old_ifs

echo "plaud-linux: ffmpeg not found; install ffmpeg with the libopus encoder or set PLAUD_FFMPEG" >&2
exit 127
