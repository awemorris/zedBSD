#!/bin/sh
# fetch's https in QEMU (x86_64, KVM, the harness's USB network with the emulator's NAT): builds the default x86_64
# image without its firmware packages (config-amd64-fetch.mk) with plan/tools/guest/test-image.sh, boots it with plan/tools/guest/guest.sh and
# fetches public https URLs in the guest, copying each body out and checking its SHA-256 against the same URL fetched
# on the host with curl (a site whose content changes between the two fetches would show as a mismatch).
#   sh plan/ws343/tests/run-fetch-qemu.sh [BUILD]     (default build/x86_64)
# GUEST_RUNTIME defaults to build/p8-ws343-guest.  Output under build/tmp/ws343-fetch-qemu (a fresh run).
# The last line is run-fetch-qemu: PASS (<N> cases), run-fetch-qemu: FAIL, or run-fetch-qemu: SKIP (no network on
# the host).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
PATH=/usr/bin:/bin:$PATH
export PATH
build=${1:-build/x86_64}
GUEST_RUNTIME=${GUEST_RUNTIME:-build/p8-ws343-guest}
export GUEST_RUNTIME
ZEDBSD_JOBS=${ZEDBSD_JOBS:-4}
export ZEDBSD_JOBS
. plan/tools/fresh-out.sh
out=build/tmp/ws343-fetch-qemu
fresh_out "$out"
guest="plan/tools/guest/guest.sh"
status=0

# The URLs: a plain page, a text file of the RFC Editor, and http redirected to https.
urls="https://example.com/ https://www.rfc-editor.org/rfc/rfc9110.txt http://www.rfc-editor.org/rfc/rfc8446.txt"

# 0. The host must reach the sites, or the run says SKIP.
if ! timeout 30 curl -sS -f -o /dev/null https://example.com/ 2> "$out/host-net.log"; then
	cat "$out/host-net.log"
	echo "run-fetch-qemu: SKIP (no network on the host)"
	exit 0
fi

# 1. The image.
if ! timeout 7000 plan/tools/guest/test-image.sh plan/ws343/tests/config-amd64-fetch.mk "$build" > "$out/image.log" 2>&1; then
	tail -30 "$out/image.log"
	echo "run-fetch-qemu: FAIL (the image; $out/image.log)"
	exit 1
fi
sync

# 2. The guest.
timeout 120 "$guest" start "$build/hdd-image.img" > "$out/start.log" 2>&1 || { cat "$out/start.log"; echo "run-fetch-qemu: FAIL (start)"; exit 1; }
trap 'timeout 60 "$guest" stop > /dev/null 2>&1; sync' EXIT
if ! timeout 400 "$guest" wait --timeout 360 > "$out/wait.log" 2>&1; then
	cat "$out/wait.log"
	echo "run-fetch-qemu: FAIL (the guest did not answer on SSH)"
	exit 1
fi
timeout 60 "$guest" run 'ls -l /etc/ssl/cert.pem /lib/libssl-compat.so /lib/libcrypto-compat.so; ls -l /usr/lib/libcurl.so 2>&1; true' > "$out/files.txt" 2>&1
cat "$out/files.txt"

# 3. Each URL in the guest and on the host.
count=0
for url in $urls; do
	count=$((count + 1))
	timeout 120 curl -sS -f -L -o "$out/host-$count" "$url" 2> "$out/host-$count.log"
	timeout 180 "$guest" run "fetch -o /tmp/f$count '$url'" > "$out/guest-$count.log" 2>&1
	code=$?
	[ "$code" = 0 ] && timeout 60 "$guest" get "/tmp/f$count" "$out/guest-$count" >> "$out/guest-$count.log" 2>&1
	want=$(sha256sum "$out/host-$count" 2> /dev/null | cut -d' ' -f1)
	got=$(sha256sum "$out/guest-$count" 2> /dev/null | cut -d' ' -f1)
	if [ "$code" = 0 ] && [ -n "$want" ] && [ "$got" = "$want" ]; then
		echo "fetch-qemu: $url: PASS ($(wc -c < "$out/host-$count") bytes, sha256 equal)"
	else
		echo "fetch-qemu: $url: FAIL (exit $code)"
		tail -6 "$out/guest-$count.log"
		status=1
	fi
done

# 4. A server that does not verify (a self-signed certificate) is refused.
count=$((count + 1))
timeout 180 "$guest" run "fetch -o /tmp/bad https://self-signed.badssl.com/; echo exit=\$?; ls /tmp/bad 2>&1" > "$out/guest-$count.log" 2>&1
if grep -q 'certificate verify failed' "$out/guest-$count.log" && grep -q 'exit=1' "$out/guest-$count.log"; then
	echo "fetch-qemu: self-signed.badssl.com refused: PASS ($(grep 'certificate verify failed' "$out/guest-$count.log" | head -1))"
else
	echo "fetch-qemu: self-signed.badssl.com refused: FAIL"
	tail -6 "$out/guest-$count.log"
	status=1
fi

if [ $status -eq 0 ]; then
	echo "run-fetch-qemu: PASS ($count cases)"
else
	echo "run-fetch-qemu: FAIL"
fi
exit $status
