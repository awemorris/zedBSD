#!/bin/sh
# KA-T020 independent UFS1/UFS2 superblock and endian runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
mode=${1:-strict}
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ufs-independent.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

case "$mode" in
strict|--strict) mode=strict ;;
baseline|--baseline) mode=baseline ;;
*)
	echo "usage: $0 [--strict|--baseline]" >&2
	exit 2
	;;
esac

if [ -d "$repo_dir/src/drivers/fs/ufs1" ]; then
	ufs1_dir=$repo_dir/src/drivers/fs/ufs1
	ufs2_dir=$repo_dir/src/drivers/fs/ufs2
else
	ufs1_dir=$repo_dir/src/kern/ufs1
	ufs2_dir=$repo_dir/src/kern/ufs2
fi

common_flags="-std=c11 -Wall -Wextra -Werror -I$repo_dir/include -I$repo_dir/src"

# shellcheck disable=SC2086
"${CC:-cc}" $common_flags -I"$ufs1_dir" -DKA_UFS1 \
	"$ufs1_dir/ufs1-endian.c" "$ufs1_dir/ufs1-super.c" \
	"$test_dir/ufs-super-endian-host-test.c" -o "$temporary/ufs1"
"$temporary/ufs1"

if [ "$mode" = baseline ]; then
	if [ -f "$ufs2_dir/ufs2-endian.c" ]; then
		echo "KA-T020: --baseline rejected after UFS2 became independent" >&2
		exit 2
	fi
	# The pre-p003 implementation deliberately records its known dependency:
	# UFS2 behavior links with the UFS1 endian translation unit.
	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags -I"$ufs1_dir" -I"$ufs2_dir" \
		-DKA_UFS2 -DKA_LEGACY_UFS_COMMON \
		"$ufs1_dir/ufs1-endian.c" "$ufs2_dir/ufs2-super.c" \
		"$test_dir/ufs-super-endian-host-test.c" \
		-o "$temporary/ufs2-baseline"
	"$temporary/ufs2-baseline"
	echo "KA-T020: BASELINE ONLY (UFS2 still links ufs1_get*/ufs1_put*)"
	exit 0
fi

if [ ! -f "$ufs2_dir/ufs2-endian.c" ] ||
	[ ! -f "$ufs2_dir/ufs2-endian.h" ]; then
	echo "KA-T020: FAIL: UFS2-owned endian source/header are absent" >&2
	exit 1
fi

# shellcheck disable=SC2086
"${CC:-cc}" $common_flags -I"$ufs2_dir" -DKA_UFS2 \
	"$ufs2_dir/ufs2-endian.c" "$ufs2_dir/ufs2-super.c" \
	"$test_dir/ufs-super-endian-host-test.c" -o "$temporary/ufs2"
"$temporary/ufs2"

if nm "$temporary/ufs1" | grep '[[:space:]]ufs2_' >/dev/null 2>&1; then
	echo "KA-T020: FAIL: UFS1 binary contains a UFS2 implementation symbol" >&2
	exit 1
fi
if nm "$temporary/ufs2" | grep '[[:space:]]ufs1_' >/dev/null 2>&1; then
	echo "KA-T020: FAIL: UFS2 binary contains a UFS1 implementation symbol" >&2
	exit 1
fi

echo "KA-T020: PASS (independent UFS1 and UFS2 compile/link)"
