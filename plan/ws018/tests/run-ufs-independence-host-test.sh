#!/bin/sh
# Historical entry point; compile only the unified production owner.
set -eu
python3 "$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)/plan/ws025/tests/prepare-driver-fragments.py"
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
case "${1:-strict}" in
strict|--strict) ;;
*) echo "Only the current unified strict gate is supported" >&2; exit 2 ;;
esac
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-unified-ufs.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$repo" -I"$repo/include" -I"$repo/src" -I"$repo/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs" "$repo/plan/ws018/tests/ufs-super-endian-host-test.c" "$repo/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-endian.c" "$repo/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-super.c" -o "$temporary/test"
"$temporary/test"
if nm "$temporary/test" | rg "[[:space:]]ufs[12]_"; then
    echo "Retired implementation symbol remains" >&2
    exit 1
fi
