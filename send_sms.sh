#!/bin/sh
# Usage: send_sms.sh <number> <text>
# Sends one SMS in text mode over /dev/modem-at. Run as root.
NUM="$1"; TXT="$2"
DEV=/dev/modem-at
LOG=/home/ceres/sms-out.log
: > "$LOG"
cat "$DEV" > "$LOG" &
CATPID=$!
sleep 0.5
printf 'AT+CMGF=1\r\n' > "$DEV"; sleep 1
printf 'AT+CSCA?\r\n'  > "$DEV"; sleep 1
printf 'AT+CMGS="%s"\r' "$NUM" > "$DEV"
sleep 2
printf '%s\032' "$TXT" > "$DEV"
sleep 12
kill "$CATPID" 2>/dev/null
cat "$LOG"
