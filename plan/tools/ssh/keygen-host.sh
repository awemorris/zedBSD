#!/bin/sh
# The host test of zedBSD's ssh-keygen program (userland/base/ssh/ssh-keygen-main.c): builds it with ASan and UBSan
# against the host's OpenSSL 3.5 (through plan/tools/ssh/host-crypto) and runs it on temporary directories.
# Expected values come from the specification of the formats (RFC 8709 key blob, the SHA256 fingerprint, the key
# file modes) and are computed here by shell and python, never from another implementation's output.  The last
# block (OpenSSH's ssh-keygen, when installed) checks that the two programs read each other's files; it generates
# nothing that the earlier blocks compare against.
# KEYGEN_SOURCE=<file> and KEYGEN_OUT=<name> replace the program source and the output name (used to try changed
# copies of the program under build/tmp).
#   sh plan/tools/ssh/keygen-host.sh
# Last line: keygen-host: PASS or keygen-host: FAIL.  Output under build/tmp/keygen-host.
# SSH_CRYPTO_INCLUDE=<dir whose compat/crypto/ holds libcrypto-compat's headers> and SSH_CRYPTO_LIB=<what to link for
# it> build against libcrypto-compat instead of the host's OpenSSL (plan/tools/ssh/compat-host.sh sets both).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
crypto_include=${SSH_CRYPTO_INCLUDE:-plan/tools/ssh/host-crypto}
crypto_lib=${SSH_CRYPTO_LIB:--lcrypto}
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
source=${KEYGEN_SOURCE:-userland/base/ssh/ssh-keygen-main.c}
out=${KEYGEN_OUT:-build/tmp/keygen-host}
fresh_out "$out"
out=$(cd "$out" && pwd -P)
root=$(pwd -P)

modules=""
for file in userland/base/ssh/ssh-*.c; do
	case "$file" in
	*-main.c|*/ssh-client-*.c) ;;
	*) modules="$modules $file" ;;
	esac
done

if ! timeout 300 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I. -Iuserland/base/ssh -I"$crypto_include" \
    "$source" $modules $crypto_lib -o "$out/ssh-keygen" > "$out/build.log" 2>&1; then
	head -20 "$out/build.log"
	echo "keygen-host: FAIL (build)"
	exit 1
fi
keygen="$out/ssh-keygen"

# 99 is the exit value of a sanitizer report, never an answer of the program.
ASAN_OPTIONS=detect_leaks=1:exitcode=99
UBSAN_OPTIONS=halt_on_error=1:exitcode=99
export ASAN_OPTIONS UBSAN_OPTIONS

work="$out/work"
mkdir -p "$work/nohome"
HOME="$work/nohome"
export HOME
checks=0
failures=0

fail() {
	failures=$((failures + 1))
	echo "FAIL: $*"
}

# with_home DIRECTORY ARGS...: run with HOME set to the directory.
with_home() {
	saved_home=$HOME
	HOME=$1
	export HOME
	shift
	run "$@"
	HOME=$saved_home
	export HOME
}

# expect_eq DESCRIPTION ACTUAL EXPECTED
expect_eq() {
	checks=$((checks + 1))
	if [ "$2" != "$3" ]; then
		fail "$1: got [$2] expected [$3]"
	fi
}

# expect_match DESCRIPTION TEXT REGEX (extended, whole text)
expect_match() {
	checks=$((checks + 1))
	if ! printf '%s\n' "$2" | grep -q -E -e "$3"; then
		fail "$1: [$2] does not match [$3]"
	fi
}

# expect_same DESCRIPTION FILE FILE
expect_same() {
	checks=$((checks + 1))
	if ! cmp -s "$2" "$3"; then
		fail "$1: $2 and $3 differ"
	fi
}

# expect_missing DESCRIPTION PATH
expect_missing() {
	checks=$((checks + 1))
	if [ -e "$2" ] || [ -L "$2" ]; then
		fail "$1: $2 exists"
	fi
}

# expect_mode DESCRIPTION PATH MODE
expect_mode() {
	checks=$((checks + 1))
	actual=$(stat -c %a "$2")
	if [ "$actual" != "$3" ]; then
		fail "$1: $2 has mode $actual, expected $3"
	fi
}

# expect_listing DESCRIPTION DIRECTORY NAMES...: the directory holds exactly these names (no temporary file stays).
expect_listing() {
	description=$1
	directory=$2
	shift 2
	checks=$((checks + 1))
	actual=$(ls -A "$directory" | tr '\n' ' ')
	expected=""
	if [ "$#" -gt 0 ]; then
		expected=$(printf '%s\n' "$@" | sort | tr '\n' ' ')
	fi
	if [ "$actual" != "$expected" ]; then
		fail "$description: $directory holds [$actual] expected [$expected]"
	fi
}

# run EXPECTED_STATUS ARGS...: runs the program; its output is in $work/stdout and $work/stderr.
run() {
	expected_status=$1
	shift
	timeout 60 "$keygen" "$@" > "$work/stdout" 2> "$work/stderr"
	status=$?
	checks=$((checks + 1))
	if [ "$status" != "$expected_status" ]; then
		fail "ssh-keygen $*: exit $status, expected $expected_status ($(head -c 300 "$work/stderr"))"
	fi
	checks=$((checks + 1))
	if grep -q -E 'Sanitizer|runtime error' "$work/stderr"; then
		fail "ssh-keygen $*: sanitizer report"
	fi
	stdout=$(cat "$work/stdout")
	stderr=$(cat "$work/stderr")
}

# fingerprint_of PUBFILE: the SHA256 fingerprint of the key blob, computed from the RFC 4253 6.6 definition.
fingerprint_of() {
	awk '{print $2}' "$1" | /usr/bin/python3 -I -c '
import base64, hashlib, sys
blob = base64.b64decode(sys.stdin.read().strip())
sys.stdout.write("SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("="))'
}

# blob_hex PUBFILE: the key blob of a public key line as hex.
blob_hex() {
	awk '{print $2}' "$1" | base64 -d | od -An -tx1 -v | tr -d ' \n'
}

umask 022

# --- generating a key ---
d="$work/gen"
mkdir "$d"
run 0 -t ed25519 -f "$d/k" -N "" -C c -q
expect_eq "quiet generation prints nothing" "$stdout" ""
expect_listing "generation leaves exactly the two files" "$d" k k.pub
expect_mode "private key mode" "$d/k" 600
expect_mode "public key mode" "$d/k.pub" 644
expect_eq "public key is one line" "$(wc -l < "$d/k.pub" | tr -d ' ')" 1
expect_match "public key line form" "$(cat "$d/k.pub")" '^ssh-ed25519 [A-Za-z0-9+/]{68} c$'
blob=$(blob_hex "$d/k.pub")
expect_eq "blob length is 51 bytes" "$((${#blob} / 2))" 51
# RFC 8709 4: string "ssh-ed25519" then a string of 32 bytes.
expect_eq "blob header" "$(printf '%s' "$blob" | cut -c1-38)" "0000000b7373682d6564323535313900000020"
expect_eq "private key begins with the armour" "$(head -1 "$d/k")" "-----BEGIN OPENSSH PRIVATE KEY-----"
expect_eq "private key ends with the armour" "$(tail -1 "$d/k")" "-----END OPENSSH PRIVATE KEY-----"
expect_eq "private key lines are at most 70 characters" "$(awk 'length($0) > 70' "$d/k" | wc -l | tr -d ' ')" 0
# PROTOCOL.key: the file starts with the magic "openssh-key-v1" and a NUL.
magic=$(sed '1d;$d' "$d/k" | tr -d '\n' | base64 -d 2>/dev/null | head -c 15 | od -An -tx1 | tr -d ' \n')
expect_eq "private key magic" "$magic" "6f70656e7373682d6b65792d763100"

# PROTOCOL.key: the two check values are 4 bytes each at offset 98 (magic 15, cipher 8, kdf 8, options 4, key count 4, public blob 55, section length 4).
checkint=$(sed '1d;$d' "$d/k" | tr -d '\n' | base64 -d 2>/dev/null | head -c 106 | tail -c 8 | od -An -tx1 | tr -d ' \n')
expect_eq "the two check values are the same" "$(printf '%s' "$checkint" | cut -c1-8)" "$(printf '%s' "$checkint" | cut -c9-16)"
checks=$((checks + 1))
if [ "$(printf '%s' "$checkint" | cut -c1-8)" = "00000000" ]; then
	fail "the check value is zero"
fi

run 0 -l -f "$d/k.pub"
expect_match "-l line form" "$stdout" '^256 SHA256:[A-Za-z0-9+/]{43} c \(ED25519\)$'
expect_eq "-l fingerprint is SHA-256 of the blob" "$(printf '%s' "$stdout" | awk '{print $2}')" "$(fingerprint_of "$d/k.pub")"
list_public=$stdout
run 0 -l -f "$d/k"
expect_eq "-l of the private file gives the same line" "$stdout" "$list_public"
run 0 -y -f "$d/k"
printf '%s\n' "$stdout" > "$work/y.pub"
expect_same "-y equals the public key file" "$work/y.pub" "$d/k.pub"

# Two keys differ.
run 0 -f "$d/k2" -N "" -C c -q
checks=$((checks + 1))
if cmp -s "$d/k.pub" "$d/k2.pub"; then
	fail "two generated keys are equal"
fi

# Not -q: the announcement.
run 0 -f "$d/k3" -N "" -C c3
expect_match "announcement names the private file" "$stdout" "saved in $d/k3"
expect_match "announcement names the public file" "$stdout" "saved in $d/k3.pub"
expect_match "announcement carries the fingerprint and the comment" "$stdout" "SHA256:[A-Za-z0-9+/]{43} c3\$"

# The private key is mode 0600 whatever the umask is; the public key follows the umask.
umask 000
run 0 -f "$d/u0" -N "" -C c -q
umask 022
expect_mode "private mode under umask 000" "$d/u0" 600
expect_mode "public mode under umask 000" "$d/u0.pub" 644
umask 077
run 0 -f "$d/u7" -N "" -C c -q
umask 022
expect_mode "private mode under umask 077" "$d/u7" 600
expect_mode "public mode under umask 077" "$d/u7.pub" 600

# No comment given: user@host.
run 0 -f "$d/dc" -N "" -q
expect_match "default comment is user@host" "$(cat "$d/dc.pub")" '^ssh-ed25519 [A-Za-z0-9+/]{68} [^ @]+@[^ ]+$'
# An empty comment stays empty.
run 0 -f "$d/nc" -N "" -C "" -q
expect_match "empty comment" "$(cat "$d/nc.pub")" '^ssh-ed25519 [A-Za-z0-9+/]{68}$'
run 0 -l -f "$d/nc.pub"
expect_match "-l of a key without a comment" "$stdout" '^256 SHA256:[A-Za-z0-9+/]{43} no comment \(ED25519\)$'
# A long comment (256 or more bytes) is cut, not refused.
long=$(/usr/bin/python3 -I -c 'print("x" * 400)')
run 0 -f "$d/lc" -N "" -C "$long" -q
expect_match "long comment is cut" "$(cat "$d/lc.pub")" '^ssh-ed25519 [A-Za-z0-9+/]{68} x{255}$'

# -t ed25519 is the default and may be left out.
run 0 -f "$d/dt" -N "" -C c -q
expect_match "default type" "$(cat "$d/dt.pub")" '^ssh-ed25519 '

# --- refusals ---
d="$work/refuse"
mkdir "$d"
run 0 -f "$d/k" -N "" -C c -q
cp "$d/k" "$d/k.saved"
cp "$d/k.pub" "$d/k.pub.saved"
run 1 -f "$d/k" -N "" -C c -q
expect_match "existing key is refused with a message" "$stderr" "already exists"
expect_same "refused run keeps the private key" "$d/k" "$d/k.saved"
expect_same "refused run keeps the public key" "$d/k.pub" "$d/k.pub.saved"
expect_listing "refused run leaves no temporary file" "$d" k k.pub k.pub.saved k.saved
# Only the public key exists: refused as well, and no private key appears.
echo "ssh-ed25519 AAAA x" > "$d/only.pub"
run 1 -f "$d/only" -N "" -C c -q
expect_missing "no private key beside an existing public key" "$d/only"
expect_eq "the existing public key text stays" "$(cat "$d/only.pub")" "ssh-ed25519 AAAA x"
# A dangling link at the private name counts as existing.
ln -s "$d/nowhere" "$d/dangling"
run 1 -f "$d/dangling" -N "" -C c -q
expect_missing "nothing is written through a dangling link" "$d/nowhere"

run 1 -t rsa -f "$d/r" -N "" -q
expect_match "rsa is refused" "$stderr" "not supported"
expect_missing "no file for rsa" "$d/r"
run 1 -t ecdsa -f "$d/r" -N "" -q
expect_missing "no file for ecdsa" "$d/r"
run 1 -t ed25519 -f "$d/p" -N "secret" -q
expect_match "a passphrase is refused" "$stderr" "passphrase"
expect_missing "no private key for a passphrase" "$d/p"
expect_missing "no public key for a passphrase" "$d/p.pub"
run 1 -t ed25519 -f "$d/np" -q
expect_match "no -N is refused" "$stderr" "-N"
expect_missing "no file without -N" "$d/np"
run 1 -f "$d/nl" -N "" -C "a
b" -q
expect_match "a line feed in the comment is named" "$stderr" "line break"
expect_missing "a comment with a line break is refused" "$d/nl"
run 1 -f "$d/cr" -N "" -C "$(printf 'a\rb')" -q
expect_match "a CR in the comment is named" "$stderr" "line break"
expect_missing "a comment with CR is refused" "$d/cr"
run 1 -f "$d/nodir/k" -N "" -q
expect_missing "a missing directory is an error" "$d/nodir"
run 1 -l -y -f "$d/k"
run 1 -z
run 1 -f
run 1 -f "$d/k" extra
run 1 -l -f "$d/k.pub" extra
run 1 -l -A
run 1 -y -A -f "$d/k"
run 1 -R host -l
run 1 -R h1 -l -f "$d/k.pub"
run 1 -R h1 -y -f "$d/k"
run 1 -t rsa -l -f "$d/k"
if [ "$(id -u)" != 0 ]; then
	mkdir "$d/ro"
	chmod 500 "$d/ro"
	run 1 -f "$d/ro/k" -N "" -q
	expect_missing "a read-only directory leaves nothing" "$d/ro/k"
	expect_listing "a read-only directory stays empty" "$d/ro"
	chmod 700 "$d/ro"
fi

# --- reading files that are not usable keys ---
d="$work/bad"
mkdir "$d"
: > "$d/empty"
run 1 -l -f "$d/empty"
expect_match "empty file" "$stderr" "not a valid key file"
run 1 -y -f "$d/empty"
expect_match "-y of an empty file" "$stderr" "not a valid private key"
printf 'hello world\n' > "$d/text"
run 1 -l -f "$d/text"
run 1 -y -f "$d/text"
run 1 -l -f "$d/missing"
expect_match "missing file" "$stderr" "No such file"
run 1 -y -f "$d/missing"
run 1 -l -f "$d"
run 1 -y -f "$d"
/usr/bin/python3 -I -c 'import sys; sys.stdout.write("x" * 20000)' > "$d/big"
run 1 -l -f "$d/big"
# -y wants a private key: a public key file is refused.
run 0 -f "$d/k" -N "" -C c -q
run 1 -y -f "$d/k.pub"
expect_eq "-y prints nothing for a public key file" "$stdout" ""
# A truncated private key.
head -c 100 "$d/k" > "$d/trunc"
run 1 -y -f "$d/trunc"
run 1 -l -f "$d/trunc"
# A public key with another type.
echo "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC0 c" > "$d/rsa.pub"
run 1 -l -f "$d/rsa.pub"
expect_match "another type" "$stderr" "ssh-ed25519"

# --- the default key file under HOME ---
d="$work/home"
mkdir "$d"
with_home "$d" 0 -N "" -C h -q
expect_mode "key directory mode" "$d/.ssh" 700
expect_listing "default key files" "$d/.ssh" id_ed25519 id_ed25519.pub
expect_mode "default private mode" "$d/.ssh/id_ed25519" 600
with_home "$d" 0 -l
expect_match "-l without -f" "$stdout" '^256 SHA256:[A-Za-z0-9+/]{43} h \(ED25519\)$'
with_home "$d" 0 -y
printf '%s\n' "$stdout" > "$work/hy.pub"
expect_same "-y without -f" "$work/hy.pub" "$d/.ssh/id_ed25519.pub"
with_home "$d" 1 -N "" -C h -q
expect_match "default key is not overwritten" "$stderr" "already exists"
d="$work/home2"
mkdir "$d"
mkdir -m 755 "$d/.ssh"
with_home "$d" 0 -N "" -q
expect_mode "an existing key directory keeps its mode" "$d/.ssh" 755
expect_mode "key in an existing key directory" "$d/.ssh/id_ed25519" 600

# --- host keys (-A) ---
d="$work/host"
mkdir "$d"
run 1 -A -f "$d" -q
expect_missing "no host key without the directory" "$d/etc/ssh/ssh_host_ed25519_key"
mkdir -p "$d/etc/ssh"
run 0 -A -f "$d"
expect_match "-A announces the key" "$stdout" "ED25519"
expect_listing "-A makes the two files" "$d/etc/ssh" ssh_host_ed25519_key ssh_host_ed25519_key.pub
expect_mode "host private key mode" "$d/etc/ssh/ssh_host_ed25519_key" 600
expect_mode "host public key mode" "$d/etc/ssh/ssh_host_ed25519_key.pub" 644
cp "$d/etc/ssh/ssh_host_ed25519_key" "$work/host.saved"
cp "$d/etc/ssh/ssh_host_ed25519_key.pub" "$work/host.pub.saved"
run 0 -A -f "$d"
expect_eq "-A prints nothing when the key exists" "$stdout" ""
expect_same "-A keeps an existing private key" "$d/etc/ssh/ssh_host_ed25519_key" "$work/host.saved"
expect_same "-A keeps an existing public key" "$d/etc/ssh/ssh_host_ed25519_key.pub" "$work/host.pub.saved"
run 0 -l -f "$d/etc/ssh/ssh_host_ed25519_key.pub"
expect_match "host key is readable" "$stdout" '^256 SHA256:[A-Za-z0-9+/]{43} [^ ]+@[^ ]+ \(ED25519\)$'
# A public key left without a private key is replaced.
d="$work/host2"
mkdir -p "$d/etc/ssh"
echo "stale" > "$d/etc/ssh/ssh_host_ed25519_key.pub"
run 0 -A -f "$d" -q
expect_eq "-q -A prints nothing" "$stdout" ""
expect_match "a left-over public key is replaced" "$(cat "$d/etc/ssh/ssh_host_ed25519_key.pub")" '^ssh-ed25519 '
run 0 -y -f "$d/etc/ssh/ssh_host_ed25519_key"
printf '%s\n' "$stdout" > "$work/h2y.pub"
# The comment is the same text, so the files match.
expect_same "host key pair belongs together" "$work/h2y.pub" "$d/etc/ssh/ssh_host_ed25519_key.pub"
# A private key without a public key is left as it is.
d="$work/host3"
mkdir -p "$d/etc/ssh"
echo "not a key" > "$d/etc/ssh/ssh_host_ed25519_key"
run 0 -A -f "$d" -q
expect_eq "an existing private file is kept" "$(cat "$d/etc/ssh/ssh_host_ed25519_key")" "not a key"
expect_missing "no public key is made beside an existing private file" "$d/etc/ssh/ssh_host_ed25519_key.pub"
run 1 -A -N secret -f "$d"

# --- removing a host (-R) ---
d="$work/known"
mkdir "$d"
run 0 -f "$d/k" -N "" -C c -q
key=$(awk '{print $2}' "$d/k.pub")
cat > "$d/expected.start" <<EOF
# a comment
h1 ssh-ed25519 $key one

h2,h1 ssh-ed25519 $key two
[h1]:2222 ssh-ed25519 $key three
h3 ssh-ed25519 $key four
|1|c2FsdA==|aGFzaA== ssh-ed25519 $key hashed
@revoked h4 ssh-ed25519 $key revoked
h5 ssh-rsa AAAAB3NzaC1yc2E other
EOF
# The file ends without a line feed.
printf 'h6 ssh-ed25519 %s six' "$key" >> "$d/expected.start"
cp "$d/expected.start" "$d/kh"
chmod 640 "$d/kh"
run 0 -R h1 -f "$d/kh"
expect_match "-R reports the removal" "$stdout" "Removed 2 line"
cat > "$d/expected.h1" <<EOF
# a comment

[h1]:2222 ssh-ed25519 $key three
h3 ssh-ed25519 $key four
|1|c2FsdA==|aGFzaA== ssh-ed25519 $key hashed
@revoked h4 ssh-ed25519 $key revoked
h5 ssh-rsa AAAAB3NzaC1yc2E other
EOF
printf 'h6 ssh-ed25519 %s six' "$key" >> "$d/expected.h1"
expect_same "-R removes the lines of the host and keeps the others byte for byte" "$d/kh" "$d/expected.h1"
expect_same "-R keeps the old contents as .old" "$d/kh.old" "$d/expected.start"
expect_mode "-R keeps the mode of the file" "$d/kh" 640
expect_mode ".old has the mode of the file" "$d/kh.old" 640
expect_listing "-R leaves no temporary file" "$d" expected.h1 expected.start k k.pub kh kh.old
# A port in brackets names that port only.
run 0 -R "[h1]:2222" -f "$d/kh" -q
expect_eq "-q -R prints nothing" "$stdout" ""
cat > "$d/expected.p" <<EOF
# a comment

h3 ssh-ed25519 $key four
|1|c2FsdA==|aGFzaA== ssh-ed25519 $key hashed
@revoked h4 ssh-ed25519 $key revoked
h5 ssh-rsa AAAAB3NzaC1yc2E other
EOF
printf 'h6 ssh-ed25519 %s six' "$key" >> "$d/expected.p"
expect_same "-R of a bracketed host" "$d/kh" "$d/expected.p"
expect_same ".old is replaced by the latest previous contents" "$d/kh.old" "$d/expected.h1"
# A host that is not there changes nothing and is not an error.
cp "$d/kh" "$d/kh.before"
cp "$d/kh.old" "$d/kh.old.before"
run 0 -R nothere -f "$d/kh"
expect_match "not found is said" "$stdout" "not found"
expect_same "an absent host leaves the file" "$d/kh" "$d/kh.before"
expect_same "an absent host leaves .old" "$d/kh.old" "$d/kh.old.before"
cp "$d/expected.start" "$d/fresh"
run 0 -R nothere -f "$d/fresh" -q
expect_eq "-q -R of an absent host prints nothing" "$stdout" ""
expect_missing "an absent host makes no .old" "$d/fresh.old"
# Another port of the same name stays.
run 0 -R h3 -f "$d/fresh" -q
sed '/^h3 /d' "$d/expected.start" > "$d/expected.h3"
expect_same "only the lines of h3 are removed" "$d/fresh" "$d/expected.h3"
# A bad argument or a missing file is an error.
run 1 -R "[h1" -f "$d/fresh"
run 1 -R "[h1]" -f "$d/fresh"
run 1 -R "[h1]:0" -f "$d/fresh"
run 1 -R "[h1]:65536" -f "$d/fresh"
run 1 -R "[h1]:22x" -f "$d/fresh"
run 1 -R "[]:22" -f "$d/fresh"
expect_match "an empty bracketed name is refused as an argument" "$stderr" "not a host name"
run 1 -R "" -f "$d/fresh"
expect_match "an empty name is refused as an argument" "$stderr" "not a host name"
run 1 -R h1 -f "$d/missing"
expect_match "missing known_hosts" "$stderr" "No such file"
expect_missing "no .old for a missing file" "$d/missing.old"
run 1 -R h1 -f "$d"
# The default file is under HOME.
d="$work/homekh"
mkdir -p "$d/.ssh"
cp "$work/known/expected.start" "$d/.ssh/known_hosts"
with_home "$d" 0 -R h2 -q
expect_eq "-R without -f uses the default file" "$(grep -c '^h2,h1' "$d/.ssh/known_hosts")" 0
expect_missing "the default file has its .old" "$d/.ssh/known_hosts.nonesuch"
expect_same "default .old" "$d/.ssh/known_hosts.old" "$work/known/expected.start"

# --- limits and odd states ---
d="$work/edge"
mkdir "$d"
run 0 -f "$d/k" -N "" -C c -q
key=$(awk '{print $2}' "$d/k.pub")

# A temporary name left by another process is never reused: the key is not made, the left-over stays untouched.
mkdir "$d/stale"
timeout 60 sh -c 'f=$1; shift; : > "$f.tmp.$$"; exec "$@"' sh "$d/stale/k" "$keygen" -f "$d/stale/k" -N "" -q > "$work/stdout" 2> "$work/stderr"
expect_eq "a left-over temporary file stops the run" "$?" 1
expect_missing "no private key beside a left-over temporary file" "$d/stale/k"
expect_missing "no public key beside a left-over temporary file" "$d/stale/k.pub"
expect_eq "one file stays, the left-over" "$(ls -A "$d/stale" | wc -l | tr -d ' ')" 1
expect_eq "the left-over temporary file is untouched" "$(wc -c < "$d/stale/$(ls -A "$d/stale")" | tr -d ' ')" 0

# The old contents cannot be kept (the .old name is a directory): the known_hosts file stays as it was and no temporary file stays.
printf 'h1 ssh-ed25519 %s one\nh2 ssh-ed25519 %s two\n' "$key" "$key" > "$d/kh2"
cp "$d/kh2" "$d/kh2.saved"
mkdir "$d/kh2.old"
run 1 -R h1 -f "$d/kh2"
expect_same "a failed .old leaves the known_hosts file" "$d/kh2" "$d/kh2.saved"
expect_listing "a failed rewrite leaves no temporary file" "$d" k k.pub kh2 kh2.old kh2.saved stale

# A directory where the public key goes: the private key was written and the message says how to get the public key.
mkdir -p "$d/hostx/etc/ssh/ssh_host_ed25519_key.pub"
run 1 -A -f "$d/hostx" -q
expect_match "a public key that cannot be written is explained" "$stderr" "ssh-keygen -y -f"
expect_listing "the host directory keeps the private key and the directory" "$d/hostx/etc/ssh" ssh_host_ed25519_key ssh_host_ed25519_key.pub
expect_eq "no temporary file inside the public key directory" "$(ls -A "$d/hostx/etc/ssh/ssh_host_ed25519_key.pub" | wc -l | tr -d ' ')" 0

# A path that is too long is refused and nothing is made.
longpath=$(/usr/bin/python3 -I -c 'print("/".join(["a" * 200] * 25))')
run 1 -f "$d/$longpath" -N "" -q
run 1 -l -f "$d/$longpath"
run 1 -R h1 -f "$d/$longpath"
run 1 -A -f "$d/$longpath"
expect_missing "nothing is made for a long path" "$d/a"

# Paths at the longest length the program builds (4095 characters; the .old name of -R has 4 more).
exactpath() {
	/usr/bin/python3 -I -c '
import sys
total = int(sys.argv[1])
text = ""
while len(text) < total:
    remaining = total - len(text)
    text += "/" + "a" * min(200, remaining - 1)
assert len(text) == total
sys.stdout.write(text)' "$1"
}
run 1 -l -f "$(exactpath 4095)"
expect_match "a path of 4095 characters is used" "$stderr" "No such file"
run 1 -l -f "$(exactpath 4096)"
expect_match "a path of 4096 characters is refused" "$stderr" "File name too long"
run 1 -R h1 -f "$(exactpath 4091)"
expect_match "a known_hosts path of 4091 characters is used" "$stderr" "No such file"
run 1 -R h1 -f "$(exactpath 4092)"
expect_match "a known_hosts path of 4092 characters is refused" "$stderr" "File name too long"

# The known_hosts file may be 1 MiB; one byte more is refused.
/usr/bin/python3 -I -c '
import sys
line = "x9 ssh-ed25519 AAAA c\n"
size = 1048576
text = line * (size // len(line))
text += "#" * (size - len(text) - 1) + "\n"
assert len(text) == size
sys.stdout.write(text)' > "$d/kh.max"
expect_eq "limit file is 1 MiB" "$(wc -c < "$d/kh.max" | tr -d ' ')" 1048576
run 0 -R h1 -f "$d/kh.max"
expect_match "a file of 1 MiB is read" "$stdout" "not found"
printf '\n' >> "$d/kh.max"
run 1 -R h1 -f "$d/kh.max"
expect_eq "one byte over 1 MiB is refused" "$(wc -c < "$d/kh.max" | tr -d ' ')" 1048577

# Ports and host names at their limits.
printf 'h1 ssh-ed25519 %s a\n[h1]:1 ssh-ed25519 %s b\n[h1]:65535 ssh-ed25519 %s c\n[h1]:9090 ssh-ed25519 %s d\n[h1]:2200 ssh-ed25519 %s e\n' "$key" "$key" "$key" "$key" "$key" > "$d/kh3"
run 0 -R "[h1]:9090" -f "$d/kh3" -q
expect_eq "a port with the digits 9 and 0" "$(grep -c '9090' "$d/kh3")" 0
run 0 -R "[h1]:2200" -f "$d/kh3" -q
expect_eq "a port with zeros" "$(grep -c '2200' "$d/kh3")" 0
run 1 -R "[h1]x5" -f "$d/kh3"
run 0 -R "[h1]:65535" -f "$d/kh3" -q
expect_eq "port 65535 is accepted" "$(grep -c '65535' "$d/kh3")" 0
expect_eq "the other port stays" "$(grep -c '^\[h1\]:1 ' "$d/kh3")" 1
run 0 -R "[h1]:1" -f "$d/kh3" -q
expect_eq "port 1 is accepted" "$(grep -c '^\[h1\]:1 ' "$d/kh3")" 0
expect_eq "the standard port stays" "$(grep -c '^h1 ' "$d/kh3")" 1
name255=$(/usr/bin/python3 -I -c 'print("h" * 255)')
name256=$(/usr/bin/python3 -I -c 'print("h" * 256)')
run 0 -R "$name255" -f "$d/kh3" -q
run 0 -R "[$name255]:22" -f "$d/kh3" -q
run 1 -R "$name256" -f "$d/kh3"
run 1 -R "[$name256]:22" -f "$d/kh3"
run 1 -R "$(printf 'a\001b')" -f "$d/kh3"

# Public key files in other shapes: no final line feed, CR LF, a second line, comment lines are not skipped.
printf '%s' "$(cat "$d/k.pub")" > "$d/nolf.pub"
run 0 -l -f "$d/nolf.pub"
expect_match "a public key line without a line feed" "$stdout" '^256 SHA256:[A-Za-z0-9+/]{43} c \(ED25519\)$'
printf 'ssh-ed25519 %s c\r\n' "$key" > "$d/crlf.pub"
run 0 -l -f "$d/crlf.pub"
expect_match "a public key line with CR LF" "$stdout" '^256 SHA256:[A-Za-z0-9+/]{43} c \(ED25519\)$'
printf 'ssh-ed25519 %s first\nssh-rsa AAAA second\n' "$key" > "$d/two.pub"
run 0 -l -f "$d/two.pub"
expect_match "only the first line is read" "$stdout" ' first \(ED25519\)$'
printf '# comment\nssh-ed25519 %s c\n' "$key" > "$d/lead.pub"
run 1 -l -f "$d/lead.pub"

# -t ed25519 is accepted with the other jobs; an empty comment is announced without a trailing space.
run 0 -t ed25519 -l -f "$d/k.pub"
run 0 -f "$d/ann" -N "" -C ""
expect_match "announcement of a key without a comment ends with the fingerprint" "$stdout" 'SHA256:[A-Za-z0-9+/]{43}$'
expect_eq "no trailing space in the announcement" "$(grep -c ' $' "$work/stdout")" 0

# --- OpenSSH's ssh-keygen reads our files and we read its files ---
if command -v ssh-keygen > /dev/null 2>&1; then
	d="$work/interop"
	mkdir "$d"
	run 0 -f "$d/ours" -N "" -C ourkey -q
	theirs_l=$(ssh-keygen -l -f "$d/ours" 2>&1)
	our_l=$("$keygen" -l -f "$d/ours")
	expect_eq "OpenSSH reads our private key (-l)" "$theirs_l" "$our_l"
	theirs_pub=$(ssh-keygen -y -f "$d/ours" 2>&1)
	expect_eq "OpenSSH derives our public key (-y)" "$theirs_pub" "$(cat "$d/ours.pub")"
	theirs_l=$(ssh-keygen -l -f "$d/ours.pub" 2>&1)
	expect_eq "OpenSSH reads our public key (-l)" "$theirs_l" "$our_l"

	ssh-keygen -q -t ed25519 -N "" -C theirkey -f "$d/theirs" > /dev/null 2>&1
	run 0 -l -f "$d/theirs"
	expect_eq "we read OpenSSH's private key (-l)" "$stdout" "$(ssh-keygen -l -f "$d/theirs")"
	run 0 -l -f "$d/theirs.pub"
	expect_eq "we read OpenSSH's public key (-l)" "$stdout" "$(ssh-keygen -l -f "$d/theirs.pub")"
	run 0 -y -f "$d/theirs"
	expect_eq "we derive OpenSSH's public key (-y)" "$stdout" "$(cat "$d/theirs.pub")"

	# A passphrase-protected key and another type are refused, with a message and no output.
	ssh-keygen -q -t ed25519 -N "pass" -C enc -f "$d/enc" > /dev/null 2>&1
	run 1 -y -f "$d/enc"
	expect_match "an encrypted key is refused" "$stderr" "unencrypted"
	expect_eq "nothing is printed for an encrypted key" "$stdout" ""
	run 1 -l -f "$d/enc"
	ssh-keygen -q -t rsa -b 2048 -N "" -C rsakey -f "$d/rsa" > /dev/null 2>&1
	run 1 -l -f "$d/rsa.pub"
	run 1 -y -f "$d/rsa"

	# A known_hosts file written by OpenSSH: its -R and ours remove the same lines.
	printf 'hostA ssh-ed25519 %s a\nhostB,hostA ssh-ed25519 %s b\n[hostA]:2222 ssh-ed25519 %s c\nhostC ssh-ed25519 %s d\n' \
	    "$(awk '{print $2}' "$d/theirs.pub")" "$(awk '{print $2}' "$d/theirs.pub")" \
	    "$(awk '{print $2}' "$d/theirs.pub")" "$(awk '{print $2}' "$d/theirs.pub")" > "$d/kh.ours"
	cp "$d/kh.ours" "$d/kh.theirs"
	run 0 -R hostA -f "$d/kh.ours" -q
	ssh-keygen -R hostA -f "$d/kh.theirs" > /dev/null 2>&1
	expect_same "removing a host leaves what OpenSSH leaves" "$d/kh.ours" "$d/kh.theirs"
	run 0 -R "[hostA]:2222" -f "$d/kh.ours" -q
	ssh-keygen -R "[hostA]:2222" -f "$d/kh.theirs" > /dev/null 2>&1
	expect_same "removing a bracketed host leaves what OpenSSH leaves" "$d/kh.ours" "$d/kh.theirs"
else
	echo "note: OpenSSH's ssh-keygen is not installed; the interoperation block was skipped"
fi

echo "keygen-host: $checks checks, $failures failures"
if [ "$failures" = 0 ]; then
	echo "keygen-host: PASS"
	exit 0
fi
echo "keygen-host: FAIL"
exit 1
