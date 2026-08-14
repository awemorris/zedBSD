#!/usr/bin/env bash
# zedBSD build driver: ./build.sh <command> <platform> [make options...]
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")" && pwd)"

available_platforms()
{
	local mk
	for mk in "$repo"/platform/*/platform.mk; do
		test -f "$mk" || continue
		basename "$(dirname "$mk")"
	done
}

usage()
{
	cat <<EOF
usage: $0 <command> <platform> [make options or additional targets...]
       $0 help [platform]

Platform roles:
  pcat          i386 PC/AT BIOS kernel and image
  pc98          i386 PC-98 BIOS kernel and image
  amd64         amd64 PC/AT BIOS kernel and image
  unified       PC-98/PC-AT BIOS + amd64 UEFI disk image

Common commands:
  all             Build the platform's normal artifacts
  hdd-image       Build its installable HDD image
  check           Build and run its host tests
  messages        Generate the kernel message header

Native kernel commands (pcat, pc98, amd64):
  vmunix          Build the zedBSD kernel
  SH              Build /bin/sh
  NOCT.ELF        Build the Noct user program
  bios-bootloader Build the native BIOS Stage 1/2 loader
  bios-hdd-image  Build the native MBR/FAT16 image

Unified and special image commands:
  unified-bootloader Build the PC-98/PC/AT BIOS dispatcher and loaders
  unified-hdd-image  Build the four-path BIOS/UEFI image
  uefi-loader        Build EFI/BOOT/BOOTX64.EFI
  legacy-pc98-hdd-image Build the legacy NEC98-partition image
  grub-iso           Build the PC/AT GRUB test ISO

QEMU and image checks:
  hdd-boot-qemu-test         Test a native HDD boot
  bios-loader-host-check     Verify a native BIOS image
  bios-loader-qemu-test      Test native BIOS ELF payloads
  unified-loader-host-check  Verify the unified disk layout and files
  unified-loader-qemu-test   Test its three BIOS loader paths
  uefi-loader-host-check     Verify the PE32+ UEFI application
  uefi-entry-qemu-test       Test OVMF, amd64 HAL, IDE root, and userland

Maintenance:
  clean           Remove build/<platform>
  distclean       Remove the complete build directory
  help            Show this help

Examples:
  $0 all pcat
  $0 all pc98
  $0 all amd64
  $0 hdd-image unified
  $0 check amd64
  $0 unified-loader-qemu-test unified
  $0 uefi-entry-qemu-test unified

Any Make target may be used as <command>.

Additional targets and Make variable assignments may follow the platform:
  $0 all pc98 check
  $0 messages pc98 PYTHON=python3

Available platforms:
EOF
	available_platforms | sed 's/^/  /'
}

if test "$#" -eq 0; then
	usage >&2
	exit 2
fi

command_name="$1"
shift

if test "$command_name" = help || test "$command_name" = -h || \
   test "$command_name" = --help; then
	if test "$#" -gt 1; then
		echo "help accepts at most one platform" >&2
		exit 2
	fi
	if test "$#" -eq 1 && ! test -f "$repo/platform/$1/platform.mk"; then
		echo "unknown platform: $1" >&2
		usage >&2
		exit 2
	fi
	usage
	exit 0
fi

if test "$#" -lt 1; then
	echo "missing platform for command '$command_name'" >&2
	usage >&2
	exit 2
fi

platform="$1"
shift

if ! test -f "$repo/platform/$platform/platform.mk"; then
	echo "unknown platform: $platform" >&2
	usage >&2
	exit 2
fi

jobs="${ZEDBSD_JOBS:-$(nproc)}"
make_command=(make -C "$repo" "ARCH=$platform" "-j$jobs")

# All compilation and image commands update the generated kernel message
# header.  Generate it in a separate Make process: multiple goals passed to a
# parallel Make invocation are not ordered, so compilation could otherwise
# start before messages.h exists.
case "$command_name" in
	clean|distclean)
		exec "${make_command[@]}" "$command_name" "$@"
		;;
	messages)
		exec "${make_command[@]}" messages "$@"
		;;
	*)
		message_options=()
		for argument in "$@"; do
			case "$argument" in
				-*|*=*) message_options+=("$argument") ;;
			esac
		done
		"${make_command[@]}" messages "${message_options[@]}"
		exec "${make_command[@]}" "$command_name" "$@"
		;;
esac
