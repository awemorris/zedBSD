#!/bin/bash
# Reproduce docs/howto/build-from-source.md from a clean source tree and boot
# the amd64 images with plan/tools/boot-test.sh (screen read, no console log).
#
#   plan/ws009/tests/p028-reproduce.sh [VARIANT...]
#
# Run from the repository root of a checkout whose HEAD is the commit to
# reproduce.  VARIANTs default to "native hybrid uefi bios"; native is always
# built first because it is the menu default.  Output goes to a new directory
# build/tmp/p028-<time>/ (src/ is the clean tree, logs/, boot-<variant>/ and
# summary.txt).  Nothing is deleted.
#
# The toolchain steps of the guide (make toolchain-cache, make -j16 toolchain)
# are not reproduced: the clean tree uses the shared, read-only LLVM and host
# Noct through symlinks.  Everything else (config.mk from the menu defaults,
# the amd64 sysroot, firmware fetches, the disk image) is made inside the
# clean tree.
set -u

shared=/home/awe/zedBSD-claude1/build
root=$(pwd)
stamp=$(date +%Y%m%d-%H%M%S)
out=$root/build/tmp/p028-$stamp
src=$out/src
logs=$out/logs
summary=$out/summary.txt
variants=${*:-native hybrid uefi bios}
jobs=${P028_JOBS:-16}

# P028_BOOT_ONLY=DIR boots the hdd-VARIANT.img images an earlier run left in
# DIR again (into new boot-*-<time> directories) without building.
if [ -n "${P028_BOOT_ONLY:-}" ]; then
	out=$P028_BOOT_ONLY
	src=$out/src
	logs=$out/logs
	summary=$out/summary.txt
fi
mkdir -p "$src" "$logs" || exit 1
sha=$(git rev-parse HEAD) || exit 1

say() {
	printf '%s\n' "$*" | tee -a "$summary"
}

# step NAME TIMEOUT COMMAND...: runs in the clean tree, logs, records the result.
step() {
	local name=$1 limit=$2 status
	shift 2
	( cd "$src" && timeout "$limit" "$@" ) >"$logs/$name.log" 2>&1
	status=$?
	say "step $name: exit $status ($(date +%H:%M:%S))"
	return $status
}

boot_one() {
	local variant=$1 mode=$2 image=$out/hdd-$variant.img
	# BOOT_TEST_WORK stays the tool's default (a new directory under TMPDIR):
	# the QMP socket's path must be shorter than 108 bytes.  KERNEL names the
	# 64-bit vmunix the image carries: boot-test.sh's bios-ide picks
	# qemu-system-x86_64 from it, and qemu-system-i386 when no kernel lies
	# beside the image copy (which then stops at "Booting from Hard Disk...").
	KERNEL=$src/build/amd64/vmunix OUTPUT=$out/boot-$variant-$mode-$stamp BOOT_MODE=$mode BOOT_TIMEOUT=300 \
		timeout 600 "$src/plan/tools/boot-test.sh" "$image" \
		>"$logs/boot-$variant-$mode-$stamp.log" 2>&1
	say "boot $variant $mode: exit $? (PNG under $out/boot-$variant-$mode-$stamp)"
}

say "p028 reproduce: commit $sha, output $out, started $(date '+%Y-%m-%d %H:%M:%S')"

if [ -n "${P028_BOOT_ONLY:-}" ]; then
	for variant in $variants; do
		[ -f "$out/hdd-$variant.img" ] || { say "boot $variant: no image"; continue; }
		case $variant in
		native|uefi) boot_one "$variant" uefi-nvme ;;
		hybrid) boot_one "$variant" uefi-nvme; boot_one "$variant" bios-ide ;;
		bios) boot_one "$variant" bios-ide ;;
		esac
	done
	say "p028 boot only: finished $(date '+%Y-%m-%d %H:%M:%S')"
	exit 0
fi

# The clean tree: the committed files only, no build state.
git archive --format=tar "$sha" | tar -x -C "$src" || { say "FAIL archive"; exit 1; }
mkdir -p "$src/build" || exit 1
for name in llvm llvm-source llvm-build NoctLang host-noct-state; do
	ln -s "$shared/$name" "$src/build/$name" || { say "FAIL link $name"; exit 1; }
done
say "shared read-only links: build/{llvm,llvm-source,llvm-build,NoctLang,host-noct-state}"

# Guide section 3: config.mk from the build menu's defaults.
step menuconfig-defaults 120 python3 tools/menuconfig.py --output config.mk --defaults || exit 1
say "config: $(grep -E '^ZEDBSD_(PLATFORM|VARIANT) ' "$src/config.mk" | tr '\n' ' ')"

# Guide: help and list-targets agree with the documented interface.
step help 120 make help
step list-targets 120 make list-targets

# Firmware licenses, answered first as an unattended build does.
step firmware-licenses 1800 sh -c 'yes | make firmware-licenses'

# The amd64 sysroot inside the clean tree (part of make toolchain).
step sysroot-amd64 7200 make -j"$jobs" sysroot-amd64 || { say "FAIL sysroot"; exit 1; }


for variant in $variants; do
	# Guide: ZEDBSD_VARIANT is set in config.mk by hand.
	sed -i "s/^ZEDBSD_VARIANT := .*/ZEDBSD_VARIANT := $variant/" "$src/config.mk"
	step "disk-image-$variant" 10800 make -j"$jobs" disk-image || continue
	cp "$src/build/amd64/hdd-image.img" "$out/hdd-$variant.img" || continue
	case $variant in
	native|uefi) boot_one "$variant" uefi-nvme ;;
	hybrid) boot_one "$variant" uefi-nvme; boot_one "$variant" bios-ide ;;
	bios) boot_one "$variant" bios-ide ;;
	esac
done

say "p028 reproduce: finished $(date '+%Y-%m-%d %H:%M:%S')"
