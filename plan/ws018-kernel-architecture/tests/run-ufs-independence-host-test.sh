#!/bin/sh
# Historical entry point; compile only the unified production owner.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
case "${1:-strict}" in
strict|--strict) ;;
*) echo "Only the current unified strict gate is supported" >&2; exit 2 ;;
esac
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-unified-ufs.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$repo" -I"$repo/include" -I"$repo/src" -I"$repo/src/drivers/fs/ufs" "$repo/plan/ws018-kernel-architecture/tests/ufs-super-endian-host-test.c" "$repo/src/drivers/fs/ufs/ufs-endian.c" "$repo/src/drivers/fs/ufs/ufs-super.c" -o "$temporary/test"
"$temporary/test"
if nm "$temporary/test" | rg "[[:space:]]ufs[12]_"; then
    echo "Retired implementation symbol remains" >&2
    exit 1
fi
