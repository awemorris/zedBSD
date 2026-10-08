#!/bin/sh
# ws177-p016: builds and runs the host test of Mail's compatibility (host-mail-n.c with userland/desktop/mailer/
# {tls,conn,imap,smtp,mime,jis,structure,compose,code}.c, the host's OpenSSL loaded as Mail loads the package's) under
# ASan and UBSan, against a fresh plan/tools/mail/fake-mail-server.py with --caps "MOVE UIDPLUS" --large
# --japanese-folders --smtp-auth LOGIN, in a new directory under build/tmp (nothing is removed here; Q1's
# plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-mail-n.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-mail-n"
out=$fresh_dir
M=userland/desktop/mailer
cd "$repo"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I. \
	plan/ws177/tests/host-mail-n.c $M/tls.c $M/conn.c $M/imap.c $M/smtp.c $M/mime.c $M/jis.c $M/structure.c $M/compose.c \
	$M/code.c -ldl -o "$out/host-mail-n"
# The tests' CA and the server's certificate for localhost.
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=ws177-test-ca -keyout "$out/ca.key" -out "$out/ca.pem" \
	-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout "$out/server.key" -out "$out/server.csr" >/dev/null 2>&1
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > "$out/server.ext"
openssl x509 -req -in "$out/server.csr" -CA "$out/ca.pem" -CAkey "$out/ca.key" -CAcreateserial -days 2 \
	-extfile "$out/server.ext" -out "$out/server.pem" >/dev/null 2>&1
mkdir -p "$out/server"
python3 plan/tools/mail/fake-mail-server.py "$out/server.pem" "$out/server.key" "$out/server" \
	--caps "MOVE UIDPLUS" --large --japanese-folders --smtp-auth LOGIN > "$out/server/ports" &
server=$!
trap 'kill $server 2>/dev/null' EXIT
tries=0
while ! grep -q PORTS "$out/server/ports" 2>/dev/null; do
	tries=$((tries + 1))
	[ $tries -lt 50 ] || { echo "fake-mail-server did not start"; exit 1; }
	sleep 0.1
done
set -- $(cat "$out/server/ports")
timeout 60 "$out/host-mail-n" "$out/ca.pem" "$2" "$4" "$out/server"
