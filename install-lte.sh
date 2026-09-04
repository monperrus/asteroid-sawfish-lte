#!/bin/sh
# Install the LTE stack on the watch. Run from this directory on a host with
# adb access:  ./install-lte.sh [adb-serial]
# Idempotent; safe to re-run after editing any unit or script.
set -e

SERIAL="${1:-}"
[ -n "$SERIAL" ] && ADB="adb -s $SERIAL" || ADB="adb"
HERE=$(cd "$(dirname "$0")" && pwd)

echo "== creating directories"
$ADB shell 'mkdir -p /usr/local/bin /usr/local/lib/lte /usr/share/doc/lte /etc/default'

echo "== shims, rmtfs and the patched QMI tools"
for f in libqipcrtr4msmipc.so libsmdpkt_wrapper.so libqmi-glib.so.5 libqrtr-glib.so.0; do
    $ADB push "$HERE/bin/$f" "/usr/local/lib/lte/$f"
done
# Kept at the legacy path too: at_cmd.sh and anything else already written
# against /usr/lib/preload keeps working.
$ADB shell 'mkdir -p /usr/lib/preload && cp /usr/local/lib/lte/lib*wrapper.so /usr/local/lib/lte/libqipcrtr4msmipc.so /usr/lib/preload/'
$ADB push "$HERE/bin/rmtfs"         /usr/local/bin/lte-rmtfs
$ADB push "$HERE/bin/qmicli-lte"    /usr/local/bin/qmicli-lte
$ADB push "$HERE/bin/msm-rmnet-ctl" /usr/local/bin/msm-rmnet-ctl

echo "== scripts and units"
for f in lte-bearer lte-modem-arm lte-modem-hold lte-modem-disarm lte-soak; do
    $ADB push "$HERE/$f" "/usr/local/bin/$f"
done
for f in lte-rmtfs.service lte-modem.service lte-bearer.service \
         lte-firmware-disarm.service lte-soak@.service; do
    $ADB push "$HERE/$f" "/etc/systemd/system/$f"
done
$ADB push "$HERE/lte-bearer.default" /etc/default/lte-bearer
$ADB push "$HERE/99-lte-modem.rules" /etc/udev/rules.d/99-lte-modem.rules
$ADB push "$HERE/at_cmd.sh"          /usr/local/bin/at_cmd.sh
$ADB push "$HERE/send_sms.sh"        /usr/local/bin/lte-send-sms
$ADB push "$HERE/README.md"          /usr/share/doc/lte/README.md

$ADB shell 'chmod +x /usr/local/bin/lte-* /usr/local/bin/at_cmd.sh \
    /usr/local/bin/qmicli-lte /usr/local/bin/msm-rmnet-ctl
  udevadm control --reload-rules && udevadm trigger
  systemctl daemon-reload
  systemctl enable lte-firmware-disarm.service'

echo
echo "Installed. The LTE units are deliberately NOT enabled at boot -- the"
echo "modem-up half of the battery soak has not been run. Start by hand:"
echo "    systemctl start lte-bearer.service"
echo "and see /usr/share/doc/lte/README.md before enabling them permanently."
