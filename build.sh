#!/usr/bin/env bash
# zedBSD build driver
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")" && pwd)"

fail()
{
	echo "$*" >&2
	exit 2
}

platform_arch()
{
	case "$1" in
		i386)  echo pcat ;;
		amd64) echo amd64 ;;
		pc98)  echo pc98 ;;
		rpi4)  echo arm64 ;;
		sun4u) echo sparcv9 ;;
		x68k)  echo x68k ;;
		*) return 1 ;;
	esac
}

usage()
{
	cat <<EOF
usage: $0 <command> <platform> [make options or additional targets...]
       $0 help [platform]

Main commands:
  menuconfig                   Configure an interactive build
  make                         Build the configured release artifacts

Commands for specific artifacts:
  vmunix    [platform]         Build a kernel
  root      [platform]         Build a rootfs tree
  rootufs   [platform]         Build a rootfs UFS image
  bootdisk  [platform]         Build a boot disk image
  loader    [platform]         Build a boot loader
  app       [name] [platform]  Build a specific userland program
  test      [platform]         Build and run its host tests

Maintenance:
  clean           Remove build/<platform>
  distclean       Remove the complete build directory
  help            Show this help

Available platforms:
  i386            PC/AT i386
  amd64           PC/AT x86_64
  pc98            NEC PC-9800
  rpi4            Raspberry Pi 4 Arm64
  sun4u           sun4u SPARC V9 64-bit
  x68k            Sharp X68000 MC68030
EOF
}

config_value()
{
	local key="$1"
	awk -v key="$key" '
		$1 == key && ($2 == ":=" || $2 == "=") {
			$1 = ""; $2 = ""; sub(/^[[:space:]]+/, ""); print; exit
		}' "$repo/config.mk"
}

configured_build()
{
	local config="$repo/config.mk"
	local platform_name platform target_jobs value key

	test -f "$config" || fail "config.mk is missing; run '$0 menuconfig' first"
	platform_name="$(config_value ZEDBSD_PLATFORM)"
	test -n "$platform_name" || fail "config.mk does not define ZEDBSD_PLATFORM"
	platform="$(platform_arch "$platform_name")" ||
		fail "config.mk selects an unknown platform: $platform_name"
	for key in CONFIG_DRIVER_NE2000 CONFIG_DRIVER_LGY98 \
	    CONFIG_DRIVER_GRAPHICS CONFIG_KERNEL_LOCKDEP \
	    CONFIG_KERNEL_TEST_CHECKPOINTS; do
		value="$(config_value "$key")"
		case "$value" in y|n) ;; *) fail "config.mk has invalid $key: $value" ;; esac
	done
	value="$(config_value CONFIG_BUF_CACHE_KIB)"
	case "$value" in ""|*[!0-9]*) fail "config.mk has invalid CONFIG_BUF_CACHE_KIB: $value" ;; esac
	target_jobs="${ZEDBSD_JOBS:-$(nproc)}"
	exec make -C "$repo" ARCH="$platform" ZEDBSD_CONFIG="$config" \
		"-j$target_jobs" build-release "$@"
}

if test "$#" -eq 0; then
	usage >&2
	exit 2
fi

command_name="$1"
shift

case "$command_name" in
menuconfig)
	exec python3 "$repo/tools/menuconfig.py" --output "$repo/config.mk" "$@"
	;;
make)
	configured_build "$@"
	exit 0
	;;
help|-h|--help)
	if test "$#" -gt 1; then
		fail "help accepts at most one platform"
	fi
	if test "$#" -eq 1 && ! platform_arch "$1" >/dev/null; then
		fail "unknown platform: $1"
	fi
	usage
	exit 0
	;;
distclean)
	if test "$#" -ne 0; then
		fail "usage: $0 distclean"
	fi
	jobs="${ZEDBSD_JOBS:-$(nproc)}"
	exec make -C "$repo" ARCH=pc98 "-j$jobs" distclean
	;;
app)
	if test "$#" -lt 2; then
		fail "usage: $0 app <name> <platform> [make options...]"
	fi
	app_name="$1"
	platform_name="$2"
	shift 2
	case "$app_name" in
		""|*[!a-zA-Z0-9._-]*)
			fail "invalid app name: $app_name"
			;;
	esac
	if ! platform="$(platform_arch "$platform_name")"; then
		fail "unknown platform: $platform_name"
	fi
	target="build/$platform/bin/$app_name"
	;;
*)
	if test "$#" -lt 1; then
		fail "missing platform for command '$command_name'"
	fi
	platform_name="$1"
	shift
	if ! platform="$(platform_arch "$platform_name")"; then
		fail "unknown platform: $platform_name"
	fi
	case "$command_name" in
		vmunix|clean)
			target="$command_name"
			;;
		root)
			target=build-rootfs
			;;
		rootufs)
			target=build-rootfs-image
			;;
		bootdisk)
			target=build-boot-disk-image
			;;
		test)
			target=check
			;;
		loader)
			case "$platform_name" in
				i386|amd64|pc98)
					target=bios-bootloader
					;;
				x68k)
					target=x68k-bootloader
					;;
				sun4u)
					target=sparcv9-bootloader
					;;
				rpi4)
					fail "rpi4 has no zedBSD boot loader"
					;;
			esac
			;;
		*)
			# Maintenance and diagnostic Make targets remain
			# available even when they are not listed by help.
			target="$command_name"
			;;
	esac
	;;
esac

jobs="${ZEDBSD_JOBS:-$(nproc)}"
make_command=(make -C "$repo" "ARCH=$platform" "-j$jobs")

# All compilation and image commands update the generated kernel message
# header.  Generate it in a separate Make process: multiple goals passed to a
# parallel Make invocation are not ordered, so compilation could otherwise
# start before messages.h exists.
case "$target" in
clean)
	exec "${make_command[@]}" clean "$@"
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
	exec "${make_command[@]}" "$target" "$@"
		;;
esac
