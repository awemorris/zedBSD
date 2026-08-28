#!/bin/sh
# WS018 KA-T080/KA-T081 independent graphics frontend host runner.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ws018-graphics.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

common_flags="-std=c11 -O2 -Wall -Wextra -Werror -I$repo/include -I$repo/include/uapi -I$repo/src -I$repo"
test_source="$repo/plan/ws018-kernel-architecture/tests/graphics-frontends-host-test.c"

# PC/AT is also the LP64 provider, so this run checks the 64-bit UAPI layout.
cc $common_flags -DZEDBSD_USER_ABI_LP64 -DGRAPHICS_TEST_PCAT \
	"$test_source" "$repo/src/drivers/graphics/pcat/device.c" \
	-o "$temporary/pcat"
"$temporary/pcat"

# PC-98 uses the 32-bit UAPI pointer layout.  The fixture's synthetic user
# address space keeps embedded pointers valid without requiring host -m32 libc.
cc $common_flags -DGRAPHICS_TEST_PC98 \
	"$test_source" "$repo/src/drivers/graphics/pc98/device.c" \
	-o "$temporary/pc98"
"$temporary/pc98"

# The frontend bodies must remain explicit copies.  Normalizing only the
# platform-private names proves that neither implementation silently drifted.
sed -e 's|drivers/graphics/pcat/backend.h|drivers/graphics/PLATFORM/backend.h|' \
	-e 's/pcat_graphics_backend_/platform_graphics_backend_/g' \
	-e 's/pcat_graphics_image/platform_graphics_image/g' \
	"$repo/src/drivers/graphics/pcat/device.c" >"$temporary/pcat.normalized"
sed -e 's|drivers/graphics/pc98/backend.h|drivers/graphics/PLATFORM/backend.h|' \
	-e 's/pc98_graphics_backend_/platform_graphics_backend_/g' \
	-e 's/pc98_graphics_image/platform_graphics_image/g' \
	"$repo/src/drivers/graphics/pc98/device.c" >"$temporary/pc98.normalized"
cmp "$temporary/pcat.normalized" "$temporary/pc98.normalized"

test ! -e "$repo/src/kern/graphics-device.c"
test ! -e "$repo/src/kern/pc98/font.c"
if rg -n 'graphics_driver_register|struct graphics_driver_ops' \
	"$repo/src" "$repo/include"; then
	echo "retired graphics registry remains" >&2
	exit 1
fi
test "$(rg -l 'cdev_register\("graphics"' \
	"$repo/src/drivers/graphics" | wc -l)" -eq 2

echo "KA-T080/KA-T081 graphics ownership audit: PASS"
