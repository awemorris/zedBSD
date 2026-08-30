#!/usr/bin/env bash
# WS018 KA-T080/KA-T081 residual production graphics runtime matrix.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
qemu_i386=${QEMU_SYSTEM_I386:-qemu-system-i386}
qemu_x86_64=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
qemu_pc98=${QEMU_PC98:-$repo/build/qemu-pc98/build/qemu-system-i386}
output=${1:-}

if [[ -z $output ]]; then
	echo "usage: $0 OUTPUT" >&2
	exit 2
fi
output=$(realpath -m -- "$output")
if [[ -e $output && -n $(find "$output" -mindepth 1 -print -quit) ]]; then
	echo "output directory is not empty: $output" >&2
	exit 2
fi
mkdir -p -- "$output"

for command in cc cmp cp cut file find make nm realpath rg sed sha256sum sleep timeout; do
	command -v "$command" >/dev/null || {
		echo "required host command not found: $command" >&2
		exit 2
	}
done
for qemu in "$qemu_i386" "$qemu_x86_64" "$qemu_pc98"; do
	[[ -x $(command -v "$qemu" 2>/dev/null || printf '%s' "$qemu") ]] || {
		echo "QEMU not found: $qemu" >&2
		exit 2
	}
done

temporary=$(mktemp -d "$repo/plan/ws018-kernel-architecture/temp/q035-p009-runtime.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
decoder=$temporary/pc98-vram-decoder
cc -std=c11 -Wall -Wextra -Werror \
	"$repo/plan/ws003-bringup/tests/boot-parameter-image-tool.c" \
	-o "$decoder"

pcat_build=$repo/build/q035-p009-pcat
pc98_build=$repo/build/q035-p009-pc98
disabled_build=$repo/build/q035-p009-amd64-disabled

make -C "$repo" -j16 ZEDBSD_PLATFORM=i386 ZEDBSD_ARCHITECTURE=i386 \
	ZEDBSD_BOARD=pcat BUILD=build/q035-p009-pcat \
	CONFIG_DRIVER_GRAPHICS=y CONFIG_DRIVER_LGY98=n disk-image \
	>"$output/build-pcat.log" 2>&1
make -C "$repo" -j16 ZEDBSD_PLATFORM=pc98 ZEDBSD_ARCHITECTURE=i386 \
	ZEDBSD_BOARD=pc98 BUILD=build/q035-p009-pc98 \
	CONFIG_DRIVER_GRAPHICS=y CONFIG_DRIVER_LGY98=y disk-image \
	>"$output/build-pc98.log" 2>&1
make -C "$repo" -j16 ZEDBSD_PLATFORM=amd64 ZEDBSD_ARCHITECTURE=amd64 \
	ZEDBSD_BOARD=pcat BUILD=build/q035-p009-amd64-disabled \
	CONFIG_DRIVER_GRAPHICS=n ZEDBSD_USER_PROGRAMS='ls stat' disk-image \
	>"$output/build-amd64-disabled.log" 2>&1

[[ $(<"$disabled_build/.graphics-device-config") == \
	'CONFIG_DRIVER_GRAPHICS_DEVICE=n' ]]
if nm "$disabled_build/vmunix" | \
	rg 'graphics_device_register|pcat_graphics_backend_|pc98_graphics_backend_'; then
	echo "graphics-disabled kernel contains a graphics frontend/backend" >&2
	exit 1
fi

printf 'case\tresult\txzed_sha256\trestored_sha256\n' >"$output/results.tsv"

wait_log()
{
	local pattern=$1 file=$2 limit=$3 index=0
	while ((index < limit)); do
		[[ -f $file ]] && rg -a -q -- "$pattern" "$file" && return 0
		sleep 1
		((index += 1))
	done
	return 1
}

send_text()
{
	local layout=$1 text=$2 character key
	while [[ -n $text ]]; do
		character=${text::1}
		case $character in
		' ') key=spc ;;
		/) key=slash ;;
		.) key=dot ;;
		-) key=minus ;;
		';') key=semicolon ;;
		'&') [[ $layout == pc98 ]] && key=shift-6 || key=shift-7 ;;
		'$') key=shift-4 ;;
		'!') key=shift-1 ;;
		[A-Z]) key=shift-${character,,} ;;
		[a-z0-9]) key=$character ;;
		*) echo "unsupported sendkey character: $character" >&2; return 1 ;;
		esac
		printf 'sendkey %s\n' "$key"
		text=${text:1}
		sleep 0.04
	done
}

validate_no_fatal()
{
	local file=$1
	if rg -a -q 'fatal:|kernel panic|panic:| fault v=|VFS initialization failed' \
		"$file"; then
		rg -a -m 1 'fatal:|kernel panic|panic:| fault v=|VFS initialization failed' \
			"$file" >&2
		return 1
	fi
}

run_pcat()
{
	local name=$1 video=$2 enter_marker=$3
	local case_dir=$output/$name run_dir=$temporary/$name
	local image=$run_dir/run.img guest_log=$case_dir/guest.log
	local result=$case_dir/controller-result.txt qemu_status
	mkdir -p -- "$case_dir" "$run_dir"
	cp --reflink=auto --sparse=always "$pcat_build/hdd-image.img" "$image"
	: >"$guest_log"
	set +e
	(
		if ! wait_log '^login:' "$guest_log" 70; then
			echo login-timeout >"$result"; printf 'quit\n'; exit
		fi
		send_text pcat root; printf 'sendkey ret\n'
		if ! wait_log 'Password:' "$guest_log" 12; then
			echo password-timeout >"$result"; printf 'quit\n'; exit
		fi
		printf 'sendkey ret\n'
		if ! wait_log 'root@zedbsd:' "$guest_log" 15; then
			echo shell-timeout >"$result"; printf 'quit\n'; exit
		fi
		send_text pcat \
			'Xzed --size 800x600 -- /bin/sh /etc/Xzed/Xzedrc & sleep 12; kill -TERM -$!'
		printf 'sendkey ret\n'
		if ! wait_log "$enter_marker" "$guest_log" 25; then
			echo graphics-enter-timeout >"$result"; printf 'quit\n'; exit
		fi
		sleep 5
		printf 'screendump %s\n' "$case_dir/xzed.ppm"
		sleep 2
		if ! wait_log 'graphics: PC/AT text mode restored' "$guest_log" 30; then
			echo graphics-leave-timeout >"$result"; printf 'quit\n'; exit
		fi
		sleep 2
		printf 'screendump %s\n' "$case_dir/restored.ppm"
		sleep 1
		echo pass >"$result"
		printf 'quit\n'
	) | timeout 160 "$qemu_i386" -machine pc -cpu pentium3 -m 128 -smp 1 \
		-vga "$video" -drive "file=$image,format=raw,if=ide,index=0" \
		-display none -serial none -debugcon "file:$guest_log" \
		-monitor stdio -no-reboot >"$case_dir/monitor.log" 2>&1
	qemu_status=${PIPESTATUS[1]}
	set -e
	[[ $qemu_status -eq 0 && -f $result && $(<"$result") == pass ]] || {
		echo "$name: $(<"$result" 2>/dev/null || echo QEMU-status-$qemu_status)" >&2
		return 1
	}
	[[ -s $case_dir/xzed.ppm && -s $case_dir/restored.ppm ]]
	! cmp -s "$case_dir/xzed.ppm" "$case_dir/restored.ppm"
	validate_no_fatal "$guest_log"
	printf '%s\tPASS\t%s\t%s\n' "$name" \
		"$(sha256sum "$case_dir/xzed.ppm" | cut -d' ' -f1)" \
		"$(sha256sum "$case_dir/restored.ppm" | cut -d' ' -f1)" \
		>>"$output/results.tsv"
}

capture_pc98_text()
{
	local vram=$1 screen=$2
	rm -f -- "$vram"
	printf 'pmemsave 0xa0000 0x2000 "%s"\n' "$vram"
	local index=0
	while ((index < 100)); do
		[[ -s $vram ]] && break
		sleep 0.02
		((index += 1))
	done
	[[ -s $vram ]] || return 1
	"$decoder" decode-pc98-vram "$vram" >"$screen"
}

wait_pc98_screen()
{
	local pattern=$1 vram=$2 screen=$3 limit=$4 index=0
	while ((index < limit)); do
		capture_pc98_text "$vram" "$screen" && \
			rg -q -- "$pattern" "$screen" && return 0
		sleep 0.5
		((index += 1))
	done
	return 1
}

run_pc98()
{
	local name=$1 coregraph=$2 expected_size=$3
	local case_dir=$output/$name run_dir=$temporary/$name
	local image=$run_dir/run.img guest_log=$case_dir/guest.log
	local screen=$case_dir/restored-screen.log vram=$run_dir/vram.bin
	local result=$case_dir/controller-result.txt qemu_status
	mkdir -p -- "$case_dir" "$run_dir"
	cp --reflink=auto --sparse=always "$pc98_build/hdd-image.img" "$image"
	: >"$guest_log"
	set +e
	(
		if ! wait_pc98_screen 'login:' "$vram" "$screen" 180; then
			echo login-timeout >"$result"; printf 'quit\n'; exit
		fi
		send_text pc98 root; printf 'sendkey ret\n'
		if ! wait_pc98_screen 'Password:' "$vram" "$screen" 40; then
			echo password-timeout >"$result"; printf 'quit\n'; exit
		fi
		printf 'sendkey ret\n'
		if ! wait_pc98_screen 'root@zedbsd:' "$vram" "$screen" 50; then
			echo shell-timeout >"$result"; printf 'quit\n'; exit
		fi
		send_text pc98 \
			'Xzed --size 800x600 -- /bin/sh /etc/Xzed/Xzedrc & sleep 12; kill -TERM -$!'
		printf 'sendkey ret\n'
		sleep 8
		printf 'screendump %s\n' "$case_dir/xzed.ppm"
		sleep 10
		if ! capture_pc98_text "$vram" "$screen" || \
		    ! rg -q 'root@zedbsd:' "$screen"; then
			echo console-restore-failed >"$result"; printf 'quit\n'; exit
		fi
		printf 'screendump %s\n' "$case_dir/restored.ppm"
		sleep 2
		echo pass >"$result"
		printf 'quit\n'
	) | timeout 180 "$qemu_pc98" \
		-M "pc9821,pegc=off,coregraph=$coregraph" -cpu 486 -m 64M -smp 1 \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
		-display none -serial none -debugcon "file:$guest_log" \
		-monitor stdio -no-reboot >"$case_dir/monitor.log" 2>&1
	qemu_status=${PIPESTATUS[1]}
	set -e
	[[ $qemu_status -eq 0 && -f $result && $(<"$result") == pass ]] || {
		echo "$name: $(<"$result" 2>/dev/null || echo QEMU-status-$qemu_status)" >&2
		return 1
	}
	[[ $(sed -n '2p' "$case_dir/xzed.ppm") == "$expected_size" ]]
	[[ $(sed -n '2p' "$case_dir/restored.ppm") == '640 400' ]]
	! cmp -s "$case_dir/xzed.ppm" "$case_dir/restored.ppm"
	validate_no_fatal "$guest_log"
	printf '%s\tPASS\t%s\t%s\n' "$name" \
		"$(sha256sum "$case_dir/xzed.ppm" | cut -d' ' -f1)" \
		"$(sha256sum "$case_dir/restored.ppm" | cut -d' ' -f1)" \
		>>"$output/results.tsv"
}

run_disabled()
{
	local name=amd64-graphics-disabled
	local case_dir=$output/$name run_dir=$temporary/$name
	local image=$run_dir/run.img guest_log=$case_dir/guest.log
	local result=$case_dir/controller-result.txt
	local qemu_status
	mkdir -p -- "$case_dir" "$run_dir"
	cp --reflink=auto --sparse=always "$disabled_build/hdd-image.img" "$image"
	: >"$guest_log"
	set +e
	(
		if ! wait_log '^login:' "$guest_log" 70; then
			echo login-timeout >"$result"; printf 'quit\n'; exit
		fi
		send_text pcat root; printf 'sendkey ret\n'
		if ! wait_log 'Password:' "$guest_log" 12; then
			echo password-timeout >"$result"; printf 'quit\n'; exit
		fi
		printf 'sendkey ret\n'
		if ! wait_log 'root@zedbsd:' "$guest_log" 15; then
			echo shell-timeout >"$result"; printf 'quit\n'; exit
		fi
		send_text pcat 'stat /dev/graphics; echo disableddone'
		printf 'sendkey ret\n'
		if ! wait_log '^disableddone' "$guest_log" 20; then
			echo query-timeout >"$result"; printf 'quit\n'; exit
		fi
		sleep 1
		printf 'screendump %s\n' "$case_dir/result.ppm"
		sleep 1
		echo pass >"$result"
		printf 'quit\n'
	) | timeout 130 "$qemu_x86_64" -machine pc -m 512 -smp 4 -vga std \
		-drive "file=$image,format=raw,if=ide,index=0" \
		-display none -serial none -debugcon "file:$guest_log" \
		-monitor stdio -no-reboot >"$case_dir/monitor.log" 2>&1
	qemu_status=${PIPESTATUS[1]}
	set -e
	[[ $qemu_status -eq 0 && -f $result && $(<"$result") == pass ]] || {
		echo "$name: $(<"$result" 2>/dev/null || echo QEMU-status-$qemu_status)" >&2
		return 1
	}
	rg -a -q 'stat: /dev/graphics: No such file or directory' "$guest_log"
	! rg -a -q 'graphics: PC/AT .* (fallback|stride=)' "$guest_log"
	validate_no_fatal "$guest_log"
	printf '%s\tPASS\t-\t-\n' "$name" >>"$output/results.tsv"
}

run_pcat pcat-vga std 'graphics: PC/AT VGA fallback 640x480x4 planar'
run_pcat pcat-cirrus cirrus 'graphics: PC/AT Cirrus 640x480x24 stride=1920'
run_pc98 pc98-gdc off '640 400'
run_pc98 pc98-cirrus on '640 480'
run_disabled

printf 'KA-T080/KA-T081 production graphics runtime matrix: PASS\n'
