#!/bin/sh
# The browser's https over libssl-compat on zedBSD (amd64, QEMU): boots an image of the platform's programs with
# the compat libraries and libbrowser built with -DWB_TLS_COMPAT (plan/ws335/tests/config-p036-guest.mk, the image
# made by the top Makefile), logs in on the serial console (plan/tools/guest/serial.py; the console is mirrored to
# the first serial port and the graphical login is off), takes a DHCP address on QEMU's user network (a USB
# adapter, ue0; the host's 127.0.0.1 is 10.0.2.2 there) and has the browser draw https pages with its CPU renderer
# (browser --render, the page's first screen as a PPM):
#   local    the host's `openssl s_server` with a test CA made here (a server certificate for 10.0.2.2), the CA
#            given with --ca-file (written into the guest over the serial line): the page is drawn;
#   untrusted the same page without --ca-file: the browser says the certificate did not verify;
#   site     https://example.com/ against the system's roots (ca-certificates' /etc/ssl/cert.pem, which
#            libssl-compat reads through its default paths): the page is drawn (needs the host's way out).
# Each picture comes back over the serial line in base64, is checked against the guest's cksum and is written as a
# PNG; the screen of the guest (QMP screendump) is kept as a PNG too.  The guest's libbrowser.so must name
# libssl-compat.so and no OpenSSL library (the image also has the OpenSSL package for its other programs).
#   sh plan/ws335/tests/run-browser-guest.sh [IMAGE]      (IMAGE default build/x86_64/hdd-image.img)
# The QEMU run is named ws335-p2-p036; its directory build/tmp/ws335-browser-guest (a fresh run directory); the disk
# is opened with snapshot=on; PORT=<n> is the s_server's port (18463).  The verdict is read from the programs' own
# output over the serial line and from the pictures, not from a console log.
# The last line is run-browser-guest: PASS or run-browser-guest: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
image=${1:-build/x86_64/hdd-image.img}
. plan/tools/fresh-out.sh
out=build/tmp/ws335-browser-guest
fresh_out "$out"
python=/usr/bin/python3
port=${PORT:-18463}
status=0
fail() {
	echo "run-browser-guest: FAIL ($1)"
	exit 1
}
[ -f "$image" ] || fail "no image $image"

# 1. The test CA (P-256, so that its PEM is short), the server's certificate for 10.0.2.2, and the page.
mkdir -p "$out/pki" "$out/site"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 -subj "/CN=ws335 browser test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -keyout "$out/pki/ca.key" -out "$out/pki/ca.pem" > "$out/pki.log" 2>&1
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj "/CN=10.0.2.2" -keyout "$out/pki/server.key" \
    -out "$out/pki/server.csr" >> "$out/pki.log" 2>&1
printf 'subjectAltName=IP:10.0.2.2\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' > "$out/pki/server.ext"
openssl x509 -req -in "$out/pki/server.csr" -CA "$out/pki/ca.pem" -CAkey "$out/pki/ca.key" -CAcreateserial \
    -days 2 -extfile "$out/pki/server.ext" -out "$out/pki/server.pem" >> "$out/pki.log" 2>&1
[ -s "$out/pki/server.pem" ] || fail "test certificates, $out/pki.log"
cat > "$out/site/page.html" <<'EOF'
<!DOCTYPE html>
<html><head><title>libssl-compat</title></head>
<body style="background: #1e5aa8; color: #ffffff; font-size: 28px">
<h1 style="background: #f0c000; color: #000000">https over libssl-compat</h1>
<p>This page came from the host's openssl s_server over TLS, verified with the test CA.</p>
</body></html>
EOF

# 2. The server, for as long as the guest runs (-WWW serves files below its directory).
(cd "$out/site" && exec timeout 3000 openssl s_server -accept "127.0.0.1:$port" -cert ../pki/server.pem \
    -key ../pki/server.key -WWW -quiet > ../server.log 2>&1) &
server=$!

# 3. The guest: UEFI, NVMe (snapshot), the serial console and QMP on unix sockets, the user network on USB.
. plan/tools/guest/qemu-accel.sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd"
socket=$top/$out/serial.sock
qmp=$top/$out/qmp.sock
timeout 3000 qemu-system-x86_64 -name ws335-p2-p036 -machine q35 -m 8G -smp 4 $(qemu_accel_args max) \
    -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
    -drive "if=pflash,format=raw,file=$out/vars.fd" \
    -drive "if=none,id=boot,file=$image,format=raw,snapshot=on" \
    -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
    -device qemu-xhci,id=xhci \
    -netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15,dns=10.0.2.3 \
    -device usb-net,bus=xhci.0,port=1,id=ecm,netdev=net0,mac=52:54:00:33:50:36,msos-desc=on \
    -vga std -display none -no-reboot \
    -serial "unix:$socket,server=on,wait=off" -qmp "unix:$qmp,server=on,wait=off" -monitor none \
    > "$out/qemu.txt" 2>&1 &
qemu=$!
trap 'kill "$qemu" "$server" 2> /dev/null; wait "$qemu" "$server" 2> /dev/null; sync' EXIT
tries=0
while [ ! -S "$socket" ] && [ $tries -lt 600 ]; do
	kill -0 "$qemu" 2> /dev/null || fail "QEMU ended, $out/qemu.txt"
	sleep 0.1
	tries=$((tries + 1))
done
serial="$python plan/tools/guest/serial.py --socket $socket --timeout 300"
$serial expect 'login: ' > /dev/null || fail "no login prompt"
$serial login > /dev/null || fail "login"
echo "run-browser-guest: logged in on the serial console"

# 4. The network: a DHCP lease on ue0.
tries=0
while [ $tries -lt 15 ]; do
	$serial run 'dhcpc -t 20 ue0' > "$out/dhcpc.txt" 2>&1
	if $serial run 'ifconfig ue0' > "$out/ifconfig.txt" 2>&1 && grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt"; then
		break
	fi
	sleep 2
	tries=$((tries + 1))
done
grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt" || fail "no DHCP address on ue0, $out/ifconfig.txt"
echo "run-browser-guest: ue0 $(grep -o 'inet 10\.0\.2\.[0-9]*' "$out/ifconfig.txt")"

# 5. What the guest has: the libraries, and the names libbrowser.so opens.
$serial run 'uname -a; ls -l /lib/libssl-compat.so /lib/libcrypto-compat.so /lib/libbrowser.so /etc/ssl/cert.pem' \
    > "$out/files.txt" 2>&1 || { cat "$out/files.txt"; fail "the files in the guest"; }
cat "$out/files.txt"
$serial run "strings /lib/libbrowser.so | grep '^lib.*\.so'" > "$out/strings.txt" 2>&1
tr -d '\r' < "$out/strings.txt" | grep -x 'lib\(ssl\|crypto\)\(-compat\)\{0,1\}\.so\(\.3\)\{0,1\}' > "$out/names.txt"
echo "run-browser-guest: libbrowser.so opens: $(tr '\n' ' ' < "$out/names.txt")"
if ! grep -qx 'libssl-compat.so' "$out/names.txt" || grep -q '^libssl\.so' "$out/names.txt"; then
	echo "run-browser-guest: libbrowser.so does not open libssl-compat.so alone"
	status=1
fi

# 6. The test CA into the guest, a line at a time.
$serial run ': > /root/ca.pem' > /dev/null
while IFS= read -r line; do
	$serial run "printf '%s\\n' '$line' >> /root/ca.pem" > /dev/null || fail "writing the CA"
done < "$out/pki/ca.pem"
$serial run 'cksum /root/ca.pem' > "$out/ca-cksum.txt" 2>&1
[ "$(tr -d '\r' < "$out/ca-cksum.txt" | awk '{print $1, $2}')" = "$(cksum < "$out/pki/ca.pem" | awk '{print $1, $2}')" ] ||
    fail "the CA in the guest differs, $out/ca-cksum.txt"

# One drawing: draw <name> <browser options...>; the browser's own words in $out/<name>.txt.
draw() {
	name=$1
	shift
	$serial run "browser --render --output=/root/$name.ppm --width=640 --height=360 --settle-ms=3000 $*; echo exit=\$?" \
	    > "$out/$name.txt" 2>&1
	tr -d '\r' < "$out/$name.txt" > "$out/$name.clean.txt"
	echo "--- $name"
	cat "$out/$name.clean.txt"
}

# Brings a picture back and writes it as a PNG; fetch <name>.
fetch() {
	name=$1
	$serial run "cksum /root/$name.ppm" > "$out/$name.cksum.txt" 2>&1
	$serial run "base64 /root/$name.ppm" > "$out/$name.b64" 2>&1
	if ! "$python" -I - "$out/$name.b64" "$out/$name.ppm" "$out/$name.png" <<'EOF'
import base64, sys
from PIL import Image
text = open(sys.argv[1], encoding="ascii", errors="replace").read()
lines = [line.strip() for line in text.splitlines()]
data = base64.b64decode("".join(line for line in lines if line and not line.startswith("base64")), validate=False)
open(sys.argv[2], "wb").write(data)
Image.open(sys.argv[2]).save(sys.argv[3])
EOF
	then
		echo "run-browser-guest: $name: the picture did not come back"
		status=1
		return
	fi
	if [ "$(tr -d '\r' < "$out/$name.cksum.txt" | awk '{print $1, $2}')" != "$(cksum < "$out/$name.ppm" | awk '{print $1, $2}')" ]; then
		echo "run-browser-guest: $name: the picture differs from the guest's"
		status=1
		return
	fi
	echo "run-browser-guest: $name: $out/$name.png"
}

# 7. local: the test CA given, the page is drawn.
draw local "--ca-file=/root/ca.pem https://10.0.2.2:$port/page.html"
if grep -qx 'exit=0' "$out/local.clean.txt"; then
	fetch local
else
	echo "run-browser-guest: local: the browser did not draw the page"
	status=1
fi

# 8. untrusted: no CA given, the certificate does not verify.
draw untrusted "https://10.0.2.2:$port/page.html"
if ! grep -q 'certificate verify failed' "$out/untrusted.clean.txt"; then
	echo "run-browser-guest: untrusted: the browser did not refuse the certificate"
	status=1
fi

# 9. site: a public site against the system's roots (reported, not required: it needs the host's way out).
draw site "https://example.com/"
if grep -qx 'exit=0' "$out/site.clean.txt"; then
	fetch site
else
	echo "run-browser-guest: site: not drawn (see $out/site.clean.txt)"
fi

# 10. The guest's screen.
"$python" -I - "$qmp" "$top/$out/screen.ppm" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX)
s.settimeout(30)
s.connect(sys.argv[1])
f = s.makefile("rw")
f.readline()
for command in ({"execute": "qmp_capabilities"}, {"execute": "screendump", "arguments": {"filename": sys.argv[2]}}):
	f.write(json.dumps(command) + "\n")
	f.flush()
	while True:
		reply = json.loads(f.readline())
		if "return" in reply or "error" in reply:
			break
EOF
"$python" -I -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$out/screen.ppm" "$out/screen.png" &&
    echo "run-browser-guest: screen: $out/screen.png"

$serial run 'sync' > /dev/null
sync
if [ $status -eq 0 ]; then
	echo "run-browser-guest: PASS"
else
	echo "run-browser-guest: FAIL"
fi
exit $status
