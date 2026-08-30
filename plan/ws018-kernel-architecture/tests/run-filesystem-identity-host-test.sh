#!/bin/sh
# KA-T030/KA-T031 filesystem-owned identity runner and source audit.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-fs-identity.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

ufs1_dir=$repo_dir/src/drivers/fs/ufs1
ufs2_dir=$repo_dir/src/drivers/fs/ufs2
common_flags="-std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I$repo_dir/include -I$repo_dir/include/uapi -I$repo_dir/src -I$repo_dir/libc/include -I$repo_dir -I$ufs1_dir -I$ufs2_dir"

compile()
{
	source=$1
	object=$2
	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags -c "$repo_dir/$source" -o "$temporary/$object"
}

compile src/kern/mount.c mount.o
compile src/kern/block-identity.c block-identity.o
compile src/drivers/fs/fat.c fat.o
compile src/kern/swap.c swap.o
compile src/drivers/fs/ufs1/ufs1-endian.c ufs1-endian.o
compile src/drivers/fs/ufs1/ufs1-super.c ufs1-super.o
compile src/drivers/fs/ufs1/ufs1-vfs.c ufs1-vfs.o
compile src/drivers/fs/ufs2/ufs2-endian.c ufs2-endian.o
compile src/drivers/fs/ufs2/ufs2-super.c ufs2-super.o
compile src/drivers/fs/ufs2/ufs2-vfs.c ufs2-vfs.o

# shellcheck disable=SC2086
"${CC:-cc}" $common_flags \
	"$test_dir/filesystem-identity-host-test.c" "$temporary"/*.o \
	-Wl,--gc-sections -o "$temporary/filesystem-identity-test"
"$temporary/filesystem-identity-test"

identity_source=$repo_dir/src/kern/block-identity.c
if grep -En 'probe_(fat|ufs)|bootfat|ufs[12]_(get|put|super)|UFS[12]?_(MAGIC|FS_|SUPER)' \
	"$identity_source"; then
	echo "KA-T031: FAIL: filesystem-format parser remains in block-identity.c" >&2
	exit 1
fi
if ! grep -q 'filesystem_identify' "$identity_source" ||
	! grep -q 'partition_identity_fill' "$identity_source" ||
	! grep -q 'swap_header_parse' "$identity_source"; then
	echo "KA-T031: FAIL: generic filesystem/partition/swap composition is missing" >&2
	exit 1
fi

echo "KA-T031: PASS (generic block-identity source ownership audit)"
