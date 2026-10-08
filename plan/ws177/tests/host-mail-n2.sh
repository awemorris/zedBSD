#!/bin/sh
# ws177-p015: builds and runs the host test of Mail's basic operations below the window (host-mail-n2.c with
# userland/desktop/mailer/{tls,conn,imap,smtp,mime,compose,code,store,account,secret}.c, the host's OpenSSL loaded as Mail
# loads the package's) under ASan and UBSan, against a fresh plan/tools/mail/fake-mail-server.py whose certificate is
# signed by a CA made here and not given to Mail (so it does not verify).  Then the view's test (host-mail-view.c, see
# its comment).  Everything goes in a new directory under build/tmp (nothing is removed here; Q1's
# plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-mail-n2.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-mail-n2"
out=$fresh_dir
M=$repo/userland/desktop/mailer
cd "$repo"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I. -Iuserland/desktop/include \
	plan/ws177/tests/host-mail-n2.c $M/tls.c $M/conn.c $M/imap.c $M/smtp.c $M/mime.c $M/compose.c $M/code.c \
	$M/store.c $M/account.c $M/secret.c -ldl -o "$out/host-mail-n2"
# The CA (not given to Mail) and the server's certificate for localhost.
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=ws177-test-ca -keyout "$out/ca.key" -out "$out/ca.pem" \
	-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout "$out/server.key" -out "$out/server.csr" >/dev/null 2>&1
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > "$out/server.ext"
openssl x509 -req -in "$out/server.csr" -CA "$out/ca.pem" -CAkey "$out/ca.key" -CAcreateserial -days 2 \
	-extfile "$out/server.ext" -out "$out/server.pem" >/dev/null 2>&1
mkdir -p "$out/server" "$out/home"
python3 plan/tools/mail/fake-mail-server.py "$out/server.pem" "$out/server.key" "$out/server" > "$out/server/ports" &
server=$!
trap 'kill $server 2>/dev/null' EXIT
tries=0
while ! grep -q PORTS "$out/server/ports" 2>/dev/null; do
	tries=$((tries + 1))
	[ $tries -lt 50 ] || { echo "fake-mail-server did not start"; exit 1; }
	sleep 0.1
done
set -- $(cat "$out/server/ports")
status=0
SSL_CERT_FILE=/nonexistent timeout 60 "$out/host-mail-n2" "$2" "$4" "$out/home" || status=1
# The view, with libkeiland's widgets (as plan/ws169/tests/run-host-mailer.sh builds it).
mkdir -p "$out/inc/keiland" "$out/inc/truetype"
cp userland/desktop/include/truetype/truetype.h "$out/inc/truetype/"
cp userland/desktop/include/keiland/keiland.h "$out/inc/keiland/"
ln -sfn "$repo/include/libc/compat" "$out/inc/compat"
U=userland/desktop
K=$U/libkeiland/ui
L=$U/libkeiland
${CC:-cc} -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$out/inc" -I. -I$K -I$U/libtruetype \
	plan/ws177/tests/host-mail-view.c $M/view.c $M/store.c $M/conn.c $M/tls.c \
	$K/canvas.c $K/text.c $K/icons.c $K/icons-line.c $K/theme.c $K/input.c $K/scroll.c $K/scroll-bar.c \
	$K/text-touch.c $K/ui.c $K/widgets.c $K/field.c $K/text-area.c $K/list.c $K/cards.c \
	$L/gesture.c $L/motion.c $L/scroll.c \
	$U/libtruetype/*.c $U/picture/color-glyph.c \
	userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libpng-compat/read.c \
	-ldl -lm -o "$out/host-mail-view"
timeout 120 "$out/host-mail-view" $U/fonts/Mahora-Regular.ttf $U/fonts/DroidSansFallbackFull.ttf || status=1
exit $status
