#!/bin/sh
# WS032 p002: host test for userland/packages/tools/archive.sh.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Builds adversarial archives on the spot and checks that each declared
# property is actually enforced, that a rejected archive never appears under
# its real name, and that a failed patch leaves no destination behind.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
archive_sh="$root/userland/packages/tools/archive.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

checks=0
failures=0

ok() { checks=$((checks + 1)); }
bad() {
	checks=$((checks + 1))
	failures=$((failures + 1))
	echo "FAIL: $*" >&2
}

# $1 = description, rest = command that must succeed
accept() {
	description=$1
	shift
	if "$@" >"$work/out" 2>&1; then ok; else bad "$description (expected accept): $(cat "$work/out")"; fi
}

# $1 = description, rest = command that must fail
reject() {
	description=$1
	shift
	if "$@" >"$work/out" 2>&1; then bad "$description (expected reject, but it was accepted)"; else ok; fi
}

sha() { sha256sum "$1" | awk '{print $1}'; }
size() { wc -c < "$1" | tr -d '[:space:]'; }

# ---------------------------------------------------------------- fixtures

mkdir -p "$work/src/pkg-1.0/sub"
echo 'int main(void) { return 0; }' > "$work/src/pkg-1.0/main.c"
echo 'original' > "$work/src/pkg-1.0/sub/data.txt"
ln -s ../main.c "$work/src/pkg-1.0/sub/inside.c"
(cd "$work/src" && tar -cf "$work/good.tar" pkg-1.0)

# two release directories in one archive
mkdir -p "$work/src2/other-9.9"
echo x > "$work/src2/other-9.9/x"
cp -R "$work/src/pkg-1.0" "$work/src2/pkg-1.0"
(cd "$work/src2" && tar -cf "$work/two-roots.tar" pkg-1.0 other-9.9)

# a member that names an absolute path, and one that steps above the root
(cd "$work/src" && tar -P -cf "$work/absolute.tar" pkg-1.0 /etc/hostname)
mkdir -p "$work/escape" && echo secret > "$work/escape/secret"
(cd "$work/src" && tar -P -cf "$work/dotdot.tar" pkg-1.0 ../escape/secret)

# a symbolic link that points outside the release directory
cp -R "$work/src/pkg-1.0" "$work/src3-pkg-1.0"
mkdir -p "$work/src3" && mv "$work/src3-pkg-1.0" "$work/src3/pkg-1.0"
ln -s ../../../etc/passwd "$work/src3/pkg-1.0/sub/outside"
(cd "$work/src3" && tar -cf "$work/link-escape.tar" pkg-1.0)

# a device-like member
mkdir -p "$work/src4/pkg-1.0" && echo y > "$work/src4/pkg-1.0/y"
mkfifo "$work/src4/pkg-1.0/pipe"
(cd "$work/src4" && tar -cf "$work/fifo.tar" pkg-1.0)

good_sha=$(sha "$work/good.tar")
good_size=$(size "$work/good.tar")

# ---------------------------------------------------------------- verify

accept "good archive" \
	sh "$archive_sh" verify "$work/good.tar" "$good_size" "$good_sha" pkg-1.0
reject "wrong size" \
	sh "$archive_sh" verify "$work/good.tar" $((good_size + 1)) "$good_sha" pkg-1.0
reject "wrong SHA-256" \
	sh "$archive_sh" verify "$work/good.tar" "$good_size" \
	0000000000000000000000000000000000000000000000000000000000000000 pkg-1.0
reject "wrong declared root" \
	sh "$archive_sh" verify "$work/good.tar" "$good_size" "$good_sha" pkg-2.0
reject "missing archive" \
	sh "$archive_sh" verify "$work/absent.tar" "$good_size" "$good_sha" pkg-1.0

for name in two-roots absolute dotdot link-escape fifo; do
	reject "$name" sh "$archive_sh" verify "$work/$name.tar" \
		"$(size "$work/$name.tar")" "$(sha "$work/$name.tar")" pkg-1.0
done

# a symbolic link wholly inside the release directory is ordinary content
accept "symlink inside the root" \
	sh "$archive_sh" verify "$work/good.tar" "$good_size" "$good_sha" pkg-1.0

# ---------------------------------------------------------------- fetch

dist="$work/dist"
accept "fetch from a verified source" \
	sh "$archive_sh" fetch "file://$work/good.tar" "$dist/good.tar" \
	"$good_size" "$good_sha" pkg-1.0
if test -f "$dist/good.tar"; then ok; else bad "fetch did not produce the archive"; fi

rm -f "$dist/good.tar"
reject "fetch with a mismatched digest" \
	sh "$archive_sh" fetch "file://$work/good.tar" "$dist/good.tar" \
	"$good_size" 0000000000000000000000000000000000000000000000000000000000000000 pkg-1.0
if test -e "$dist/good.tar"; then
	bad "a rejected download appeared under its real name"
else
	ok
fi
if test -z "$(ls -A "$dist" 2>/dev/null | grep -v '^\.good.tar.lock$' || true)"; then
	ok
else
	bad "a rejected download left files behind: $(ls -A "$dist")"
fi

mkdir -p "$dist/held.tar.lock"
reject "fetch while another acquisition holds the lock" \
	sh "$archive_sh" fetch "file://$work/good.tar" "$dist/held.tar" \
	"$good_size" "$good_sha" pkg-1.0
rmdir "$dist/held.tar.lock"

# ---------------------------------------------------------------- extract

cat > "$work/0001-change-data.patch" <<'PATCH'
--- a/sub/data.txt
+++ b/sub/data.txt
@@ -1 +1 @@
-original
+patched
PATCH
cat > "$work/0002-will-not-apply.patch" <<'PATCH'
--- a/sub/data.txt
+++ b/sub/data.txt
@@ -1 +1 @@
-this line is not present
+replacement
PATCH

destination="$work/out-src"
accept "extract and patch" \
	sh "$archive_sh" extract "$work/good.tar" pkg-1.0 "$destination" \
	"$work/0001-change-data.patch"
if test "$(cat "$destination/sub/data.txt" 2>/dev/null)" = patched; then
	ok
else
	bad "the patch did not reach the destination"
fi

# a second extraction must replace the tree rather than patch it twice
accept "re-extract over an existing tree" \
	sh "$archive_sh" extract "$work/good.tar" pkg-1.0 "$destination" \
	"$work/0001-change-data.patch"
if test "$(cat "$destination/sub/data.txt" 2>/dev/null)" = patched; then
	ok
else
	bad "re-extraction did not produce a clean tree"
fi

failed_destination="$work/out-failed"
reject "extract with a patch that does not apply" \
	sh "$archive_sh" extract "$work/good.tar" pkg-1.0 "$failed_destination" \
	"$work/0001-change-data.patch" "$work/0002-will-not-apply.patch"
if test -e "$failed_destination"; then
	bad "a failed patch left a destination behind"
else
	ok
fi
if test -z "$(ls -A "$work" | grep '^\.extract\.' || true)"; then
	ok
else
	bad "a failed extraction left its staging directory behind"
fi

reject "extract to a relative destination" \
	sh "$archive_sh" extract "$work/good.tar" pkg-1.0 relative/path
reject "extract with a missing patch" \
	sh "$archive_sh" extract "$work/good.tar" pkg-1.0 "$work/out-missing" \
	"$work/absent.patch"

echo "external archive host test: $checks checks, $failures failures"
test "$failures" = 0
