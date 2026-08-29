#!/bin/sh
# KA-T011 platform and disk-label source ownership audit.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
platform_dir=$repo_dir/src/kern/platform
disklabel_dir=$repo_dir/src/drivers/disklabel

fail()
{
	echo "KA-T011: FAIL: $*" >&2
	exit 1
}

test -d "$platform_dir" || fail "canonical platform directory is absent"
test -d "$disklabel_dir" || fail "canonical disk-label directory is absent"
test ! -e "$repo_dir/src/drivers/parttable" ||
	fail "parallel parttable driver directory exists"

expected_platform='README.md
pc98.c
pcat.c
rpi4.c
sun4u.c
x68k.c'
actual_platform=$(find "$platform_dir" -mindepth 1 -maxdepth 1 -printf '%f\n' |
	LC_ALL=C sort)
test "$actual_platform" = "$expected_platform" || {
	echo "KA-T011: unexpected src/kern/platform contents:" >&2
	echo "$actual_platform" >&2
	exit 1
}

expected_disklabel='gpt.c
mbr.c
pc98-auto.c
pc98.c
pcat-auto.c
sun.c
x68k.c'
actual_disklabel=$(find "$disklabel_dir" -mindepth 1 -maxdepth 1 -printf '%f\n' |
	LC_ALL=C sort)
test "$actual_disklabel" = "$expected_disklabel" || {
	echo "KA-T011: unexpected src/drivers/disklabel contents:" >&2
	echo "$actual_disklabel" >&2
	exit 1
}

for retired in pcat pc98 rpi4 sun4u x68k; do
	test ! -e "$repo_dir/src/kern/$retired" ||
		fail "historical src/kern/$retired directory remains"
done
for retired in \
	include/kern/mbr-partition.h \
	include/kern/sun-disklabel.h \
	include/kern/x68k-partition.h \
	include/kern/pc98/partition-auto.h; do
	test ! -e "$repo_dir/$retired" || fail "retired header $retired remains"
done

test -f "$repo_dir/include/drivers/disklabel.h" ||
	fail "focused disk-label driver header is absent"
for scheme in mbr gpt pcat_auto pc98 pc98_auto sun x68k; do
	grep -q "partition_scheme_$scheme" \
		"$repo_dir/include/drivers/disklabel.h" ||
		fail "partition_scheme_$scheme declaration is absent"
done

hooks='init refresh_devices input_init block_device debug_write halt reboot'
for platform in pcat pc98 rpi4 sun4u x68k; do
	source=$platform_dir/$platform.c
	for hook in $hooks; do
		grep -q "kern_platform_$hook[[:space:]]*(" "$source" ||
			fail "$platform.c lacks kern_platform_$hook"
	done
done

grep -q 'partition_set_scheme(&partition_scheme_pcat_auto)' \
	"$platform_dir/pcat.c" || fail "PC/AT no longer selects strict GPT/MBR auto"
grep -q 'partition_set_scheme(&partition_scheme_pc98_auto)' \
	"$platform_dir/pc98.c" || fail "PC-98 no longer selects auto disk label"
grep -q 'partition_set_scheme(&partition_scheme_mbr)' \
	"$platform_dir/rpi4.c" || fail "RPi4 no longer selects MBR"
grep -q 'partition_set_scheme(&partition_scheme_sun)' \
	"$platform_dir/sun4u.c" || fail "sun4u no longer selects Sun disk label"
grep -q 'partition_set_scheme(&partition_scheme_x68k)' \
	"$platform_dir/x68k.c" || fail "X68k no longer selects its native label"

test -f "$repo_dir/src/drivers/graphics/pcat/font.c" ||
	fail "PC/AT font is not graphics-owned"
test -f "$repo_dir/src/drivers/graphics/pcat/vgafont.c" ||
	fail "PC/AT VGA font is not graphics-owned"
test -f "$repo_dir/src/drivers/graphics/pc98/display-glyph.c" ||
	fail "PC-98 glyph implementation is not graphics-owned"

old_paths='src/kern/(mbr-partition\.c|sun-disklabel\.c|(pcat|pc98|rpi4|sun4u|x68k)/)'
if grep -En "$old_paths" \
	"$repo_dir/platform/pcat/vmunix.mk" \
	"$repo_dir/platform/pc98/vmunix.mk" \
	"$repo_dir/platform/amd64/vmunix.mk" \
	"$repo_dir/platform/arm64/vmunix.mk" \
	"$repo_dir/platform/sparcv9/vmunix.mk" \
	"$repo_dir/platform/x68k/vmunix.mk"; then
	fail "a supported build manifest retains a historical source path"
fi

test -f "$repo_dir/src/kern/partition.c" ||
	fail "generic partition registry left kernel core"
test -f "$repo_dir/include/kern/partition.h" ||
	fail "generic partition contract left kernel core"

echo "KA-T011: PASS (five independent platform TUs and driver-owned disk labels)"
