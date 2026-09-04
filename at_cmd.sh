#!/bin/sh
# Usage: at_cmd.sh <wait_seconds> "<AT cmd 1>" ["<AT cmd 2>" ...]
# Must be run as root (needs /dev/modem-at access).
WAIT="$1"; shift
LOG=/home/ceres/at-cmd-out.log
: > "$LOG"
( cat /dev/modem-at & echo $! > /tmp/at_catpid; sleep "$WAIT"; kill "$(cat /tmp/at_catpid)" 2>/dev/null ) > "$LOG" &
sleep 0.3
for cmd in "$@"; do
  printf '%s\r\n' "$cmd" > /dev/modem-at
  sleep 1
done
sleep "$WAIT"
cat "$LOG"
