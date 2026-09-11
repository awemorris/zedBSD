#!/bin/sh
# KA-T050 aggregate boot-header freestanding compile runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
fixture=$test_dir/boot-header-aggregate-compile.c
x86_cc=${CC:-cc}
m68k_cc=${M68K_CC:-m68k-linux-gnu-gcc}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-boot-header.XXXXXX")

kernel64_obj=$work_dir/kernel-amd64.o
kernel32_obj=$work_dir/kernel-i386.o
amd64_hal_obj=$work_dir/hal-amd64.o
i386_hal_obj=$work_dir/hal-i386.o
pc98_obj=$work_dir/handoff-pc98.o
x68k_obj=$work_dir/stage2-x68k.o
kernel64_dep=$work_dir/kernel-amd64.d
kernel32_dep=$work_dir/kernel-i386.d
amd64_hal_dep=$work_dir/hal-amd64.d
i386_hal_dep=$work_dir/hal-i386.d
pc98_dep=$work_dir/handoff-pc98.d
x68k_dep=$work_dir/stage2-x68k.d

cleanup()
{
	rm -f "$kernel64_obj" "$kernel32_obj" "$amd64_hal_obj" \
	    "$i386_hal_obj" "$pc98_obj" "$x68k_obj" \
	    "$kernel64_dep" "$kernel32_dep" "$amd64_hal_dep" \
	    "$i386_hal_dep" "$pc98_dep" "$x68k_dep"
	rmdir "$work_dir"
}
trap cleanup EXIT HUP INT TERM

common_flags="-std=c11 -nostdinc -I$repo_dir/include -I$repo_dir/include/uapi -I$repo_dir/src -I$repo_dir/libc/include -I$repo_dir -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -Wall -Wextra -Werror"

compile_x86()
{
	output=$1
	depfile=$2
	shift 2
	# Intentional word splitting: common_flags and the remaining arguments
	# are compiler option lists controlled by this fixture.
	# shellcheck disable=SC2086
	"$x86_cc" $common_flags "$@" -MMD -MF "$depfile" \
	    -c "$fixture" -o "$output"
}

compile_x86 "$kernel64_obj" "$kernel64_dep" -m64 \
	-DZEDBSD_USER_ABI_LP64 -DKA_T050_KERNEL
compile_x86 "$kernel32_obj" "$kernel32_dep" -m32 -march=i386 \
	-DKA_T050_KERNEL
compile_x86 "$amd64_hal_obj" "$amd64_hal_dep" -m64 \
	-DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DZEDBSD_USER_ABI_LP64 \
	-DKA_T050_HAL
compile_x86 "$i386_hal_obj" "$i386_hal_dep" -m32 -march=i386 \
	-DHAL_ARCH_I386 -DHAL_BOARD_PCAT -DKA_T050_HAL
compile_x86 "$pc98_obj" "$pc98_dep" -m32 -march=i386 \
	-DHAL_ARCH_I386 -DHAL_BOARD_PC98 -DKA_T050_PC98

if ! command -v "$m68k_cc" >/dev/null 2>&1; then
	echo "KA-T050: M68K_CC compiler not found: $m68k_cc" >&2
	exit 2
fi

# Intentional word splitting: common_flags is a compiler option list.
# shellcheck disable=SC2086
"$m68k_cc" $common_flags -m68030 -msoft-float \
	-DHAL_ARCH_M68K -DHAL_BOARD_X68K -DZEDBSD_USER_ABI_M68K \
	-DKA_T050_X68K -MMD -MF "$x68k_dep" \
	-c "$fixture" -o "$x68k_obj"

for depfile in "$kernel64_dep" "$kernel32_dep" "$amd64_hal_dep" \
	"$i386_hal_dep" "$pc98_dep" "$x68k_dep"
do
	if grep -Eq 'include/kern/(boot-parameters|boot-source|fat|mount|fs)\.h' \
	    "$depfile"; then
		echo "KA-T050: aggregate boot header leaked a retired/private dependency:" >&2
		grep -E 'include/kern/(boot-parameters|boot-source|fat|mount|fs)\.h' \
		    "$depfile" >&2
		exit 1
	fi
done

echo "KA-T050: PASS (kernel 32/64, amd64/i386 HAL, PC-98, X68k)"
