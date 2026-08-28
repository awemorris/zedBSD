#!/bin/sh
# KA-T021 UFS2-owned journal and snapshot runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
mode=${1:-strict}
binary=$(mktemp "${TMPDIR:-/tmp}/zedbsd-ufs2-consistency.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM

case "$mode" in
strict|--strict) mode=strict ;;
baseline|--baseline) mode=baseline ;;
*)
	echo "usage: $0 [--strict|--baseline]" >&2
	exit 2
	;;
esac

if [ "$mode" = baseline ]; then
	if [ ! -f "$repo_dir/src/kern/ufs/ufs-journal.c" ]; then
		echo "KA-T021: --baseline rejected after UFS2 took ownership" >&2
		exit 2
	fi
	"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
		-I"$repo_dir/include" -I"$repo_dir/src" \
		-DKA_LEGACY_UFS_COMMON \
		"$repo_dir/src/kern/ufs/ufs-journal.c" \
		"$repo_dir/src/kern/ufs/ufs-snapshot.c" \
		"$test_dir/ufs2-consistency-host-test.c" -o "$binary"
	"$binary"
	echo "KA-T021: BASELINE ONLY (journal/snapshot still have common ufs_* names)"
	exit 0
fi

ufs2_dir=$repo_dir/src/drivers/fs/ufs2
for required in ufs2-consistency.h ufs2-journal.c ufs2-snapshot.c; do
	if [ ! -f "$ufs2_dir/$required" ]; then
		echo "KA-T021: FAIL: missing UFS2-owned $required" >&2
		exit 1
	fi
done

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$repo_dir/include" -I"$repo_dir/src" -I"$ufs2_dir" \
	"$ufs2_dir/ufs2-journal.c" "$ufs2_dir/ufs2-snapshot.c" \
	"$test_dir/ufs2-consistency-host-test.c" -o "$binary"
"$binary"

if nm "$binary" | grep '[[:space:]]ufs1_' >/dev/null 2>&1; then
	echo "KA-T021: FAIL: UFS2 consistency binary contains a UFS1 symbol" >&2
	exit 1
fi
if nm "$binary" | grep '[[:space:]]ufs_journal\|[[:space:]]ufs_snapshot' \
	>/dev/null 2>&1; then
	echo "KA-T021: FAIL: retired common UFS consistency symbol remains" >&2
	exit 1
fi

echo "KA-T021: PASS (UFS2-owned journal/snapshot compile/link)"
