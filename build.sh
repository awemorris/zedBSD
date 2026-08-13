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

Build commands:
  all             Build every artifact for the platform
  messages        Generate the kernel message header
  vmunix          Build the zedBSD kernel
  INIT.ELF        Build the initial user process
  NOCT.ELF        Build the Noct user program
  SH              Build /bin/sh
  LINUX           Build /bin/linux
  hdd-image       Build an installable HDD image
  bios-bootloader Build the native BIOS Stage 1/2 loader
  bios-hdd-image  Build the MBR/FAT16 native-loader image
  pc-unified-bootloader
                  Build the dual PC/AT and PC-98 BIOS loader
  pc-unified-hdd-image
                  Build one FAT16 image bootable on PC/AT and PC-98
  legacy-pc98-hdd-image
                  Build the legacy NEC98-partition image (PC-98 only)
  grub-iso        Build the PC/AT GRUB Multiboot test ISO

Test commands:
  check                       Build and run all host tests
  hdd-boot-qemu-test          Test HDD boot (and PC/AT GRUB boot) in QEMU
  bios-loader-host-check      Verify the native BIOS image on the host
  bios-loader-qemu-test       Test native-loader ELF payloads in QEMU
  pc-unified-loader-host-check Verify the dual-machine BIOS image
  pc-unified-loader-qemu-test  Test one image on PC/AT and PC-98 QEMU
  sh-builtins-qemu-test        Test /bin/sh filesystem builtins in QEMU

Maintenance commands:
  clean           Remove build/<platform>
  distclean       Remove the complete build directory
  help            Show this help

Any Make target may be used as <command>.  For example:
  $0 hdd-image pc98
  $0 vmunix pc98
  $0 check pc98
  $0 NOCT.ELF pc98
  $0 hdd-boot-qemu-test pc98
  $0 bios-bootloader pcat
  $0 bios-hdd-image pcat
  $0 bios-loader-qemu-test pcat
  $0 bios-bootloader pc98
  $0 bios-hdd-image pc98
  $0 legacy-pc98-hdd-image pc98
  $0 pc-unified-hdd-image pcat
  $0 pc-unified-loader-qemu-test pcat

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
