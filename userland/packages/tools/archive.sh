#!/bin/sh
# Verified acquisition and extraction of external release archives.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# An external package names an immutable release archive by URL, byte size,
# SHA-256 and the single directory every member must live under.  Nothing here
# trusts the network: an archive becomes visible under its real name only after
# every declared property holds, and extraction refuses members that could
# write outside the destination.
#
# A release that is one plain file rather than an archive (a certificate
# bundle, say) is named with the root "-": its size and SHA-256 are checked
# and nothing is looked for inside it.  It is never extracted.
#
# usage:
#   archive.sh verify  <archive> <size> <sha256> <root|->
#   archive.sh fetch   <url> <archive> <size> <sha256> <root|->
#   archive.sh extract <archive> <root> <destination> [patch...]
set -eu

program=${0##*/}

fail() {
	echo "$program: $*" >&2
	exit 1
}

# Every property the package declared about the archive, checked against the
# bytes actually present.  Called both for a freshly fetched temporary file and
# for one already sitting in the distfiles directory.
# A POSIX shell has no local variables, so every name here is prefixed to keep
# it clear of the caller's archive/url/root bindings.
verify_archive() {
	va_archive=$1 va_size=$2 va_hash=$3 va_root=$4

	test -n "$va_root" || fail "empty archive root"
	if test ! -f "$va_archive" || test -L "$va_archive"; then
		fail "missing or unsafe archive: $va_archive"
	fi

	va_got_size=$(wc -c < "$va_archive" | tr -d '[:space:]')
	test "$va_got_size" = "$va_size" ||
		fail "archive size mismatch: expected $va_size, got $va_got_size: $va_archive"

	va_got_hash=$(${ZEDBSD_SHA256:-sha256sum} "$va_archive" | awk '{print $1}')
	test "$va_got_hash" = "$va_hash" ||
		fail "archive SHA-256 mismatch: expected $va_hash, got $va_got_hash: $va_archive"

	# A single file has no members to examine.
	test "$va_root" != - || return 0

	# Every member must sit under the one declared release directory, and no
	# member may name an absolute path or step above it.
	va_bad_entry=$(tar -tf "$va_archive" | awk -v root="$va_root" '
		$0 != root && $0 != root "/" && index($0, root "/") != 1 { print; exit }
		$0 ~ /(^|\/)\.\.($|\/)/ || $0 ~ /^\// { print; exit }')
	test -z "$va_bad_entry" || fail "unsafe archive member: $va_bad_entry"

	# Regular files, directories, symbolic links and hard links only: no
	# devices, sockets or fifos land on the build machine.
	va_bad_type=$(tar -tvf "$va_archive" |
		awk 'index("-dlh", substr($0, 1, 1)) == 0 { print; exit }')
	test -z "$va_bad_type" || fail "unsupported archive member type: $va_bad_type"

	# A link may point anywhere inside the release directory and nowhere else.
	va_bad_link=$(tar -tvf "$va_archive" | awk -v root="$va_root" '
		function outside(path, target,   base, joined, n, i, part, depth) {
			if (target ~ /^\//) return 1;
			if (index(target, root "/") == 1) joined = target;
			else { base = path; sub(/[^\/]*$/, "", base); joined = base target; }
			n = split(joined, part, "/"); depth = 0;
			for (i = 1; i <= n; ++i) {
				if (part[i] == "" || part[i] == ".") continue;
				if (part[i] == "..") { if (--depth < 1) return 1; } else ++depth;
			}
			return 0;
		}
		substr($0, 1, 1) == "l" { if (outside($6, $8)) { print; exit } }
		substr($0, 1, 1) == "h" { if (outside($6, $9)) { print; exit } }')
	test -z "$va_bad_link" || fail "archive link escapes its release root: $va_bad_link"
}

case ${1-} in
verify)
	test $# -eq 5 || fail "usage: $program verify <archive> <size> <sha256> <root>"
	verify_archive "$2" "$3" "$4" "$5"
	;;

fetch)
	test $# -eq 6 || fail "usage: $program fetch <url> <archive> <size> <sha256> <root>"
	url=$2 archive=$3 size=$4 hash=$5 root=$6
	directory=${archive%/*}
	mkdir -p "$directory"

	# One acquisition at a time, so a parallel build cannot see a partial
	# file and cannot race two writers onto the same name.
	lock="$archive.lock"
	temporary=
	locked=0
	cleanup() {
		if test -n "$temporary" && test -f "$temporary"; then rm -f -- "$temporary"; fi
		if test "$locked" = 1 && test -d "$lock"; then rmdir -- "$lock"; fi
	}
	mkdir "$lock" 2>/dev/null ||
		fail "archive acquisition already in progress: $archive"
	locked=1
	trap cleanup EXIT HUP INT TERM

	temporary=$(mktemp "$directory/.${archive##*/}.XXXXXX")
	# shellcheck disable=SC2086
	${ZEDBSD_FETCH:-curl --fail --location --silent --show-error} \
		--output "$temporary" "$url"
	verify_archive "$temporary" "$size" "$hash" "$root"
	mv -- "$temporary" "$archive"
	temporary=
	rmdir -- "$lock"
	locked=0
	trap - EXIT HUP INT TERM
	;;

extract)
	test $# -ge 4 || fail "usage: $program extract <archive> <root> <destination> [patch...]"
	archive=$2 root=$3 destination=$4
	shift 4

	case $destination in
	/*) ;;
	*) fail "destination must be an absolute path: $destination" ;;
	esac
	test "$destination" != / || fail "refusing to use / as a destination"
	test "$root" != - || fail "a single file is not extracted: $archive"

	parent=${destination%/*}
	mkdir -p "$parent"

	# A half-patched tree must never be reachable.  Build the whole thing
	# beside the destination and move it into place only when every patch
	# has applied.
	staging=$(mktemp -d "$parent/.extract.XXXXXX")
	trap 'rm -rf -- "$staging"' EXIT HUP INT TERM

	tar -xf "$archive" -C "$staging"
	test -d "$staging/$root" || fail "archive did not contain its root: $root"

	for patch in "$@"; do
		test -f "$patch" || fail "missing patch: $patch"
		${ZEDBSD_PATCH:-patch} -p1 -s -d "$staging/$root" < "$patch" ||
			fail "patch did not apply: $patch"
	done

	rm -rf -- "$destination"
	mv -- "$staging/$root" "$destination"
	rm -rf -- "$staging"
	trap - EXIT HUP INT TERM
	;;

*)
	fail "usage: $program verify|fetch|extract ..."
	;;
esac
