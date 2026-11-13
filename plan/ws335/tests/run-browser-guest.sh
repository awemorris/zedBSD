#!/bin/sh
# The browser's https over libssl-compat on zedBSD (amd64, QEMU): boots an image of the platform's programs (the
# image of plan/ws335/tests/config-p036-guest.mk made by the top Makefile; libbrowser.so is linked with the two compat
# libraries), logs in on the serial console (plan/tools/guest/serial.py; the console is mirrored to the first serial
# port and the graphical login is off), takes a DHCP address on QEMU's user network (a USB adapter, ue0; the host's
# 127.0.0.1 is 10.0.2.2 there) and has the browser draw https pages with its CPU renderer (browser --render, the
# page's first screen as a PPM):
#   local     the host's `openssl s_server` with a test CA made here (a server certificate for 10.0.2.2), the CA
#             given with --ca-file (written into the guest over the serial line): the page is drawn;
#   untrusted the same page without --ca-file: the browser says the certificate did not verify;
#   name      a second s_server reached by a name (ws335-name.test, put in the guest's /etc/hosts as 10.0.2.2) that
#             gives the certificate for that name only to a client that sends it (SNI) and one for another name
#             (ws335-default.test) otherwise, both from the test CA: the page is drawn (the name went out and matched);
#   othername the same server by another name (ws335-other.test, also 10.0.2.2): the browser says the certificate
#             did not verify for the name (hostname mismatch);
#   site      a public site (SITE=<url>, default https://example.com/) against the system's roots
#             (ca-certificates' /etc/ssl/cert.pem, which libssl-compat reads through its default paths): the page is
#             drawn (needs the host's way out to the site).  The image must hold /etc/ssl/cert.pem.
# Each picture comes back over the serial line in base64, is checked against the guest's cksum and is written as a
# PNG; the screen of the guest (QMP screendump) is kept as a PNG too.  The guest's /lib/libbrowser.so must be the
# build's build/x86_64/dynamic/libbrowser.so (cksum), whose NEEDED (readelf -d) hold libssl-compat.so and
# libcrypto-compat.so and no OpenSSL library.
#   sh plan/ws335/tests/run-browser-guest.sh [IMAGE]      (IMAGE default build/x86_64/hdd-image.img)
# The QEMU run is named ws335-browser-guest; its directory build/tmp/ws335-browser-guest (a fresh run directory); the
# disk is opened with snapshot=on; PORT=<n> is the first s_server's port (18463), the second's is the next.  The
# verdict is read from the programs' own output over the serial line and from the pictures, not from a console log.
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
port2=$((port + 1))
site=${SITE:-https://example.com/}
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
# The certificates of the second server, one for each name.
for name in name default; do
	openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj "/CN=ws335-$name.test" \
	    -keyout "$out/pki/$name.key" -out "$out/pki/$name.csr" >> "$out/pki.log" 2>&1
	printf 'subjectAltName=DNS:ws335-%s.test\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' "$name" \
	    > "$out/pki/$name.ext"
	openssl x509 -req -in "$out/pki/$name.csr" -CA "$out/pki/ca.pem" -CAkey "$out/pki/ca.key" -CAcreateserial \
	    -days 2 -extfile "$out/pki/$name.ext" -out "$out/pki/$name.pem" >> "$out/pki.log" 2>&1
done
[ -s "$out/pki/server.pem" ] && [ -s "$out/pki/name.pem" ] && [ -s "$out/pki/default.pem" ] ||
    fail "test certificates, $out/pki.log"
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
(cd "$out/site" && exec timeout 3000 openssl s_server -accept "127.0.0.1:$port2" -cert ../pki/default.pem \
    -key ../pki/default.key -servername ws335-name.test -cert2 ../pki/name.pem -key2 ../pki/name.key -WWW \
    > ../server2.log 2>&1) &
server2=$!

# 3. The guest: UEFI, NVMe (snapshot), the serial console and QMP on unix sockets, the user network on USB.
. plan/tools/guest/qemu-accel.sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd"
socket=$top/$out/serial.sock
qmp=$top/$out/qmp.sock
timeout 3000 qemu-system-x86_64 -name ws335-browser-guest -machine q35 -m 8G -smp 4 $(qemu_accel_args max) \
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
trap 'kill "$qemu" "$server" "$server2" 2> /dev/null; wait "$qemu" "$server" "$server2" 2> /dev/null; sync' EXIT
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

# 5. What the guest has: the libraries (libbrowser.so the build's, with its NEEDED), the system's roots.
$serial run 'uname -a; ls -l /lib/libssl-compat.so /lib/libcrypto-compat.so /lib/libbrowser.so' \
    > "$out/files.txt" 2>&1 || { cat "$out/files.txt"; fail "the files in the guest"; }
cat "$out/files.txt"
if $serial run 'ls -l /etc/ssl/cert.pem' > "$out/cert-pem.txt" 2>&1; then
	echo "run-browser-guest: $(tr -d '\r' < "$out/cert-pem.txt" | grep cert.pem)"
else
	echo "run-browser-guest: the image has no /etc/ssl/cert.pem"
	status=1
fi
library=build/x86_64/dynamic/libbrowser.so
$serial run 'cksum /lib/libbrowser.so' > "$out/libbrowser-cksum.txt" 2>&1
if [ "$(tr -d '\r' < "$out/libbrowser-cksum.txt" | grep 'libbrowser' | awk '{print $1, $2}')" = \
    "$(cksum < "$library" | awk '{print $1, $2}')" ]; then
	echo "run-browser-guest: /lib/libbrowser.so is $library (cksum)"
else
	echo "run-browser-guest: /lib/libbrowser.so differs from $library, $out/libbrowser-cksum.txt"
	status=1
fi
readelf -d "$library" 2> /dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' > "$out/needed.txt"
echo "run-browser-guest: libbrowser.so needs: $(tr '\n' ' ' < "$out/needed.txt")"
if ! grep -qx 'libssl-compat.so' "$out/needed.txt" || ! grep -qx 'libcrypto-compat.so' "$out/needed.txt" ||
    grep -q '^lib\(ssl\|crypto\)\.so' "$out/needed.txt"; then
	echo "run-browser-guest: libbrowser.so does not need libssl-compat.so and libcrypto-compat.so alone"
	status=1
fi

# The two names of the second server, to the host.
$serial run "printf '10.0.2.2 ws335-name.test ws335-other.test\\n' >> /etc/hosts" > /dev/null ||
    fail "the names in /etc/hosts"

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

# Brings a picture back and writes it as a PNG; fetch <name>.  The console is mirrored to the same serial line, so
# a message of the system (a daemon's line) can fall into the base64 text; the copy is then taken again, up to three
# times, until it matches the guest's cksum.
fetch() {
	name=$1
	$serial run "cksum /root/$name.ppm" > "$out/$name.cksum.txt" 2>&1
	attempt=1
	while [ $attempt -le 3 ]; do
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
			echo "run-browser-guest: $name: the picture did not come back (copy $attempt)"
		elif [ "$(tr -d '\r' < "$out/$name.cksum.txt" | awk '{print $1, $2}')" = "$(cksum < "$out/$name.ppm" | awk '{print $1, $2}')" ]; then
			echo "run-browser-guest: $name: $out/$name.png (copy $attempt)"
			return
		else
			echo "run-browser-guest: $name: the picture differs from the guest's (copy $attempt)"
		fi
		attempt=$((attempt + 1))
	done
	status=1
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

# 9. name: the second server by the name its certificate is for (SNI picks it), the page is drawn.
draw name "--ca-file=/root/ca.pem https://ws335-name.test:$port2/page.html"
if grep -qx 'exit=0' "$out/name.clean.txt"; then
	fetch name
else
	echo "run-browser-guest: name: the browser did not draw the page"
	status=1
fi

# 10. othername: the same server by another name, the certificate does not verify for it.
draw othername "--ca-file=/root/ca.pem https://ws335-other.test:$port2/page.html"
if ! grep -q 'certificate verify failed: hostname mismatch' "$out/othername.clean.txt"; then
	echo "run-browser-guest: othername: the browser did not refuse the certificate for the name"
	status=1
fi

# 11. site: a public site against the system's roots alone (no CA given), the page is drawn.
draw site "$site"
if grep -qx 'exit=0' "$out/site.clean.txt"; then
	fetch site
else
	echo "run-browser-guest: site: the browser did not draw $site"
	status=1
fi

# 12. The guest's screen.
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
