#!/bin/sh
# ws035-p047: unplugs and replugs the guest's USB NIC (usb-net on xhci.0)
# through QMP and counts the DHCP exchanges on the wire.
#   MODE=auto    networkd alone must configure the new adapter, once
#   MODE=manual  "net dhcp ue0" as soon as ue0 appears; networkd must not
#                run a second DHCP after it
# Needs the SSH harness guest running (plan/tools/guest/guest.sh start).
set -eu
cd /home/awe/zedBSD-rpi4
MODE=${MODE:-auto}
PCAP=${PCAP:-/tmp/replug-$MODE.pcap}
S=build/guest/serial.sock
Q="python3 plan/tools/qmp.py build/guest/qmp.sock"
if [ "$MODE" = manual ]; then
	cat > /tmp/replug-race.sh <<'INNER'
while ifconfig ue0 >/dev/null 2>&1; do sleep 1; done
while ! ifconfig ue0 >/dev/null 2>&1; do :; done
net dhcp ue0 > /tmp/manual.log 2>&1
echo "manual-status=$?" >> /tmp/manual.log
INNER
	plan/tools/guest/guest.sh put /tmp/replug-race.sh /tmp/replug-race.sh
	python3 plan/tools/guest/serial.py --socket $S --timeout 30 login > /dev/null 2>&1 || true
	python3 plan/tools/guest/serial.py --socket $S --timeout 30 run 'sh /tmp/replug-race.sh > /dev/null 2>&1 &' > /dev/null
fi
rm -f "$PCAP"
$Q object-add "{\"qom-type\":\"filter-dump\",\"id\":\"replug-$MODE-$$\",\"netdev\":\"net0\",\"file\":\"$PCAP\"}" > /dev/null
$Q device_del '{"id":"ecm"}' > /dev/null
sleep 3
$Q device_add '{"driver":"usb-net","bus":"xhci.0","port":"2","id":"ecm","netdev":"net0","mac":"52:54:00:33:00:01","msos-desc":true}' > /dev/null
sleep 25
python3 plan/ws035/tests/pcap-dhcp.py "$PCAP"
echo "exchanges=$(python3 plan/ws035/tests/pcap-dhcp.py "$PCAP" | grep -c ' ACK ')"
python3 plan/tools/guest/serial.py --socket $S --timeout 30 run 'cat /tmp/manual.log 2>/dev/null; net show; ifconfig ue0'
