#!/usr/bin/env bash
# WS006 IN-T41 production USB HID/evdev QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
phase_config=$script_dir/qemu-usb-hid-config.mk
phase_makefile=$script_dir/qemu-usb-hid-acceptance.mk
guest_source=$script_dir/usb-hid-guest-probe.c
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
build_timeout=${BUILD_TIMEOUT_SECONDS:-1800}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-180}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-180}
hotplug_timeout=${HOTPLUG_TIMEOUT_SECONDS:-90}
cell_timeout=${CELL_TIMEOUT_SECONDS:-600}
key_delay=${KEY_DELAY_SECONDS:-0.08}
cells=${USB_HID_QEMU_CELLS:-"xhci paired"}

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Build a private IN-T41 image, boot a fresh copy per selected topology, and
verify production USB keyboard/mouse/tablet capability discovery, evdev
records, routed keyboard console input, stale-fd hotplug generations, and the
same-controller USB Storage root. USB_HID_QEMU_CELLS may be "xhci", "paired",
or "xhci paired". If production driver integration is not present yet, source,
guest-syntax, and QEMU-topology gates run and the script exits 77.
EOF
}

if [[ $# -eq 1 && ($1 == -h || $1 == --help) ]]; then
	usage
	exit 0
fi
if [[ $# -gt 1 ]]; then
	usage >&2
	exit 2
fi
case $cells in
xhci|paired|'xhci paired') ;;
*) echo "USB_HID_QEMU_CELLS must be 'xhci', 'paired', or 'xhci paired'" >&2; exit 2 ;;
esac
case $build_timeout:$boot_timeout:$command_timeout:$hotplug_timeout:$cell_timeout in
*[!0-9:]*|0:*|*:0:*|*:*:0:*|*:*:*:0:*|*:*:*:*:0)
	echo "timeouts must be positive integers" >&2
	exit 2
	;;
esac
case $key_delay in
''|*[!0-9.]*|.*|*.*.*)
	echo "KEY_DELAY_SECONDS must be a non-negative decimal" >&2
	exit 2
	;;
esac

for file in "$phase_config" "$phase_makefile" "$guest_source"; do
	[[ -f $file ]] || { echo "required input not found: $file" >&2; exit 2; }
done
for command in "$qemu" awk cc cp date find make mkdir rg sed sha256sum \
	sleep timeout tr truncate wc; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -f $ovmf_code ]] || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
[[ -f $ovmf_vars ]] || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }

if [[ $# -eq 1 ]]; then
	output=$1
	[[ ! -e $output ]] || { echo "output already exists: $output" >&2; exit 2; }
	mkdir -p -- "$output"
else
	temp_root=$repo/plan/ws006-input/temp
	mkdir -p -- "$temp_root"
	output=$(mktemp -d "$temp_root/q048-p008-in-t41.XXXXXX")
fi
output=$(cd -- "$output" && pwd)
task_tmp=$output/tmp
private_build=$output/build/amd64
private_arch=$output/build/arch-images
private_data=$output/build/data.img
private_swap=$output/build/swapfile
image=$private_build/tests/ws006-p008-hdd-image.img
metadata=$output/metadata.txt
results=$output/results.tsv
mkdir -p -- "$task_tmp" "$output/build"
export TMPDIR=$task_tmp

config_mk=$repo/config.mk
config_hash=missing
[[ ! -f $config_mk ]] || config_hash=$(sha256sum "$config_mk" | awk '{print $1}')
printf 'gate\tresult\tevidence\n' >"$results"
{
	printf 'test=IN-T41 ws006-p008\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'output=%s\n' "$output"
	printf 'tmpdir=%s\n' "$task_tmp"
	printf 'config_mk_sha256_before=%s\n' "$config_hash"
	printf 'phase_config=%s\n' "$phase_config"
	printf 'phase_config_sha256=%s\n' "$(sha256sum "$phase_config" | awk '{print $1}')"
	printf 'guest_source_sha256=%s\n' "$(sha256sum "$guest_source" | awk '{print $1}')"
	printf 'cells=%s\n' "$cells"
	printf 'qemu=%s\n' "$("$qemu" --version | sed -n '1p')"
	printf 'input_routing=USB-HID-display-video0,i8042-disabled\n'
} >"$metadata"

finish()
{
	local status=$1 current_hash=missing integrity=pass

	trap - EXIT
	set +e
	[[ ! -f $config_mk ]] || current_hash=$(sha256sum "$config_mk" | awk '{print $1}')
	if [[ $current_hash != "$config_hash" ]]; then
		echo "config.mk changed during IN-T41" >&2
		integrity=fail
		status=1
	fi
	{
		printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'config_mk_sha256_after=%s\n' "$current_hash"
		printf 'input_integrity=%s\n' "$integrity"
		printf 'acceptance_exit_status=%s\n' "$status"
	} >>"$metadata"
	printf 'input-integrity\t%s\tmetadata.txt\n' "$integrity" >>"$results"
	if [[ $status -eq 0 ]]; then
		echo "WS006 IN-T41 QEMU acceptance: PASS ($output)"
	elif [[ $status -eq 77 ]]; then
		echo "WS006 IN-T41 runtime not run: production driver integration pending ($output)" >&2
	else
		echo "WS006 IN-T41 QEMU acceptance: FAIL ($output)" >&2
	fi
	exit "$status"
}
trap 'finish "$?"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Role discovery must remain capability-based and event numbers dynamic.
if rg -n '/dev/input/event[0-9]|strcmp[(][^,]+,[[:space:]]*"QEMU|EVIOCGNAME.*ROLE_' \
	"$guest_source" >"$output/forbidden-source.log" 2>&1; then
	echo "guest probe contains a fixed event number or identity role match" >&2
	exit 1
fi
if ! TMPDIR="$task_tmp" cc -std=c11 -Wall -Wextra -Werror \
	-I"$repo/libc/include" -I"$repo/include/uapi" -fsyntax-only \
	"$guest_source" >"$output/guest-syntax.log" 2>&1; then
	echo "guest probe host syntax gate failed" >&2
	exit 1
fi
printf 'guest-source-syntax\tpass\tguest-syntax.log\n' >>"$results"

parser_image=$output/qemu-parser.img
truncate -s 1048576 "$parser_image"
run_parser_preflight()
{
	local kind=$1 log
	local -a topology
	log=$output/qemu-parser-$kind.log

	if [[ $kind == xhci ]]; then
		topology=(
			-device qemu-xhci,id=xhci
			-device usb-storage,bus=xhci.0,port=4,drive=boot,id=rootstick,bootindex=1
			-device usb-kbd,bus=xhci.0,port=1,id=kbd,serial=in-t41-kbd,display=video0
			-device usb-mouse,bus=xhci.0,port=2,id=mouse,serial=in-t41-mouse
			-device usb-tablet,bus=xhci.0,port=3,id=tablet,serial=in-t41-tablet,display=video0
		)
	else
		topology=(
			-device ich9-usb-ehci1,id=ehci
			-device ich9-usb-uhci1,id=uhci1,masterbus=ehci.0,firstport=0
			-device ich9-usb-uhci2,id=uhci2,masterbus=ehci.0,firstport=2
			-device ich9-usb-uhci3,id=uhci3,masterbus=ehci.0,firstport=4
			-device usb-storage,bus=ehci.0,port=6,drive=boot,id=rootstick,bootindex=1
			-device usb-kbd,bus=ehci.0,port=1,id=kbd,serial=in-t41-kbd,usb_version=1,display=video0
			-device usb-mouse,bus=ehci.0,port=2,id=mouse,serial=in-t41-mouse,usb_version=1
			-device usb-tablet,bus=ehci.0,port=3,id=tablet,serial=in-t41-tablet,usb_version=1,display=video0
		)
	fi
	set +e
	printf 'info usb\nquit\n' | TMPDIR="$task_tmp" timeout --foreground \
		--kill-after=2 20 "$qemu" -S -machine q35,usb=off,i8042=off -m 64 \
		-device VGA,id=video0 -drive "if=none,id=boot,file=$parser_image,format=raw" \
		"${topology[@]}" -display none -serial none -monitor stdio \
		>"$log" 2>&1
	local status=$?
	set -e
	if [[ $status -ne 0 ]] || rg -a -i -q \
		'unknown command|invalid parameter|duplicate id|device .* not found|property .* not found|Error:' "$log"; then
		echo "$kind QEMU topology parser preflight failed" >&2
		return 1
	fi
	for product in 'QEMU USB Keyboard' 'QEMU USB Mouse' 'QEMU USB Tablet' 'QEMU USB MSD'; do
		rg -a -q "Product $product" "$log" || {
			echo "$kind parser topology missing $product" >&2
			return 1
		}
	done
	printf 'qemu-parser-%s\tpass\tqemu-parser-%s.log\n' "$kind" "$kind" >>"$results"
}

for cell in $cells; do
	run_parser_preflight "$cell"
done

source_ready=yes
[[ -f $repo/src/drivers/usb-hid.c ]] || source_ready=no
[[ -f $repo/include/drivers/usb-hid.h ]] || source_ready=no
rg -q 'CONFIG_DRIVER_USB_HID' "$repo/Makefile" || source_ready=no
rg -q 'src/drivers/usb-hid[.]c' "$repo/platform/amd64/vmunix.mk" || source_ready=no
rg -q 'drv_usb_hid_driver_register' "$repo/src/kern/platform/pcat.c" || source_ready=no
printf 'production_driver_source_ready=%s\n' "$source_ready" >>"$metadata"
if [[ $source_ready != yes ]]; then
	printf 'production-driver-runtime\tnot-run\tproduction integration pending\n' >>"$results"
	exit 77
fi

build_log=$output/build.log
build_command=(make -C "$repo" -j16 -f Makefile -f "$phase_makefile"
	ZEDBSD_CONFIG="$phase_config" BUILD="$private_build"
	ARCH_IMAGE_DIR="$private_arch" DATA_IMAGE="$private_data"
	SWAP_IMAGE="$private_swap" qemu-usb-hid-image)
printf 'build_command=' >>"$metadata"
printf '%q ' env TMPDIR="$task_tmp" timeout --foreground --kill-after=10 \
	"${build_timeout}s" "${build_command[@]}" >>"$metadata"
printf '\n' >>"$metadata"
set +e
TMPDIR="$task_tmp" timeout --foreground --kill-after=10 "${build_timeout}s" \
	"${build_command[@]}" >"$build_log" 2>&1
build_status=$?
set -e
if [[ $build_status -ne 0 || ! -f $image ]]; then
	echo "IN-T41 image build failed or timed out (status $build_status)" >&2
	exit 1
fi
image_hash=$(sha256sum "$image" | awk '{print $1}')
printf 'image=%s\nimage_sha256_before=%s\n' "$image" "$image_hash" >>"$metadata"
printf 'private-image-build\tpass\tbuild.log\n' >>"$results"

marker_count()
{
	local pattern=$1 file=$2 count
	count=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

run_cell()
{
	local kind=$1 cell_dir run_image vars_copy guest_log qmp_log
	local controller_result cell_metadata cell_deadline
	local -a topology
	cell_dir=$output/$kind
	mkdir -p -- "$cell_dir"
	run_image=$cell_dir/run.img
	vars_copy=$cell_dir/OVMF_VARS.fd
	guest_log=$cell_dir/guest.log
	qmp_log=$cell_dir/qmp.log
	controller_result=$cell_dir/controller-result.txt
	cell_metadata=$cell_dir/metadata.txt
	cp --reflink=auto --sparse=always "$image" "$run_image"
	cp -- "$ovmf_vars" "$vars_copy"
	: >"$guest_log"
	: >"$qmp_log"
	: >"$controller_result"
	if [[ $kind == xhci ]]; then
		topology=(
			-device qemu-xhci,id=xhci
			-device usb-storage,bus=xhci.0,port=4,drive=boot,id=rootstick,bootindex=1
			-device usb-kbd,bus=xhci.0,port=1,id=kbd,serial=in-t41-kbd,display=video0
		)
		usb_bus=xhci.0
		usb_version=''
		root_owner='xHCI'
	else
		topology=(
			-device ich9-usb-ehci1,id=ehci
			-device ich9-usb-uhci1,id=uhci1,masterbus=ehci.0,firstport=0
			-device ich9-usb-uhci2,id=uhci2,masterbus=ehci.0,firstport=2
			-device ich9-usb-uhci3,id=uhci3,masterbus=ehci.0,firstport=4
			-device usb-storage,bus=ehci.0,port=6,drive=boot,id=rootstick,bootindex=1
			-device usb-kbd,bus=ehci.0,port=1,id=kbd,serial=in-t41-kbd,usb_version=1,display=video0
		)
		usb_bus=ehci.0
		usb_version=',usb_version=1'
		root_owner='EHCI storage plus companion-UHCI HID'
	fi
	cell_deadline=$(( $(date +%s) + 10#$cell_timeout ))
	{
		printf 'cell=%s\n' "$kind"
		printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'source_image=%s\nsource_image_sha256=%s\n' "$image" "$image_hash"
		printf 'run_image=%s\nroot_owner=%s\n' "$run_image" "$root_owner"
		printf 'keyboard_routing=display video0\ncontrol_routing=USB keyboard only\n'
	} >"$cell_metadata"

	first_failure()
	{
		local failure
		failure=$(rg -a -m 1 -- \
			'fatal:|kernel panic|panic:|amd64 fault v=|VFS initialization failed|Input/output error|controller quarantined|USB-HID-GUEST FAIL|usb-storage: .*error=[1-9]|loop[0-9]+: .*error=[1-9]' \
			"$guest_log" 2>/dev/null || true)
		printf '%s' "$failure"
	}

	wait_for()
	{
		local pattern=$1 minimum=$2 seconds=$3 label=$4 deadline count failure
		deadline=$(( $(date +%s) + 10#$seconds ))
		((cell_deadline < deadline)) && deadline=$cell_deadline
		while :; do
			failure=$(first_failure)
			if [[ -n $failure ]]; then
				printf 'fail\t%s: %s\n' "$label" "$failure" >"$controller_result"
				return 1
			fi
			count=$(marker_count "$pattern" "$guest_log")
			((count >= minimum)) && return 0
			if (( $(date +%s) >= deadline )); then
				printf 'fail\t%s timeout (%s/%s)\n' "$label" "$count" "$minimum" >"$controller_result"
				return 1
			fi
			sleep 0.05
		done
	}

	qmp_hmp()
	{
		local value escaped
		value=$1
		escaped=$(printf '%s' "$value" | sed 's/\\/\\\\/g; s/"/\\"/g')
		printf '{"execute":"human-monitor-command","arguments":{"command-line":"%s"}}\n' "$escaped"
	}

	qmp_key()
	{
		local key=$1 shifted=$2 route=${3:-usb} route_field=''
		[[ -z $route ]] || route_field='"device":"video0",'
		if [[ $shifted == yes ]]; then
			printf '{"execute":"input-send-event","arguments":{%s"events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"shift"}}}]}}\n' "$route_field"
			sleep "$key_delay"
		fi
		printf '{"execute":"input-send-event","arguments":{%s"events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"%s"}}}]}}\n' "$route_field" "$key"
		sleep "$key_delay"
		printf '{"execute":"input-send-event","arguments":{%s"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"%s"}}}]}}\n' "$route_field" "$key"
		sleep "$key_delay"
		if [[ $shifted == yes ]]; then
			printf '{"execute":"input-send-event","arguments":{%s"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"shift"}}}]}}\n' "$route_field"
			sleep "$key_delay"
		fi
	}

	clear_line()
	{
		printf '%s\n' '{"execute":"input-send-event","arguments":{"device":"video0","events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"ctrl"}}}]}}'
		sleep "$key_delay"
		printf '%s\n' '{"execute":"input-send-event","arguments":{"device":"video0","events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"u"}}}]}}'
		sleep "$key_delay"
		printf '%s\n' '{"execute":"input-send-event","arguments":{"device":"video0","events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"u"}}}]}}'
		sleep "$key_delay"
		printf '%s\n' '{"execute":"input-send-event","arguments":{"device":"video0","events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"ctrl"}}}]}}'
		sleep "$key_delay"
	}

	send_text()
	{
		local value=$1 route=${2:-usb} character key shifted lower
		while [[ -n $value ]]; do
			character=${value:0:1}
			value=${value:1}
			shifted=no
			case $character in
			[a-z0-9]) key=$character ;;
			[A-Z]) lower=${character,,}; key=$lower; shifted=yes ;;
			' ') key=spc ;;
			/) key=slash ;;
			-) key=minus ;;
			.) key=dot ;;
			=) key=equal ;;
			'&') key=7; shifted=yes ;;
			*) echo "unsupported input character: $character" >&2; return 1 ;;
			esac
			qmp_key "$key" "$shifted" "$route"
		done
		qmp_key ret no "$route"
	}

	login_pattern='(^|[[:blank:]])login:[[:blank:]]*$'
	password_pattern='Password:'
	shell_pattern='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'
	driver_pattern='usb[0-9]+: device [0-9]+ interface [0-9]+ class 03/[0-9a-fA-F]{2}/[0-9a-fA-F]{2} driver=usb-hid'
	event_pattern='usb-hid: event device usb[0-9]+ device=[0-9]+ interface=[0-9]+ endpoint=[0-9a-fA-F]+ report-bytes=[0-9]+'
	disconnect_pattern='usb[0-9]+: device [0-9]+ port [0-9]+ disconnected'

	send_shell()
	{
		local command=$1 before
		before=$(marker_count "$shell_pattern" "$guest_log")
		clear_line
		send_text "$command"
		wait_for "$shell_pattern" $((before + 1)) "$command_timeout" "shell command $command"
	}

	start_probe()
	{
		local command=$1 ready=$2 pass_pattern=$3 before
		before=$(marker_count "$shell_pattern" "$guest_log")
		async_prompt=$((before + 1))
		async_pass_pattern=$pass_pattern
		async_pass_target=$(( $(marker_count "$pass_pattern" "$guest_log") + 1 ))
		ready_before=$(marker_count "$ready" "$guest_log")
		clear_line
		send_text "$command"
		wait_for "$ready" $((ready_before + 1)) "$command_timeout" "probe ready $command"
	}

	wait_probe_pass()
	{
		wait_for "$async_pass_pattern" "$async_pass_target" \
			"$command_timeout" "probe pass"
		wait_for "$shell_pattern" "$async_prompt" "$command_timeout" "probe shell return"
	}

	controller_sequence()
	{
		local password_before shell_before driver_count disconnect_count

		printf '%s\n' '{"execute":"qmp_capabilities"}'
		wait_for 'usb-storage: sd[a-z]+ blocks=[0-9]+ block-size=[0-9]+' 1 \
			"$boot_timeout" 'USB Storage root registration' || return 1
		wait_for 'vfs: root=overlay ' 1 "$boot_timeout" 'USB overlay root' || return 1
		wait_for "$driver_pattern" 1 "$boot_timeout" 'USB keyboard binding' || return 1
		wait_for "$event_pattern" 1 "$boot_timeout" 'USB keyboard event publication' || return 1
		wait_for "$login_pattern" 1 "$boot_timeout" 'login prompt' || return 1
		password_before=$(marker_count "$password_pattern" "$guest_log")
		shell_before=$(marker_count "$shell_pattern" "$guest_log")
		send_text root
		wait_for "$password_pattern" $((password_before + 1)) "$command_timeout" 'password prompt' || return 1
		send_text ''
		wait_for "$shell_pattern" $((shell_before + 1)) "$command_timeout" 'root shell' || return 1

		send_shell '/usr/bin/usb-hid-guest-probe inventory keyboard' || return 1
		# i8042 is disabled, so the login and every shell command above also
		# prove that the routed USB keyboard remains the console input source.
		shell_before=$(marker_count "$shell_pattern" "$guest_log")
		send_text 'echo usbconsolepass' usb
		wait_for '^usbconsolepass\r?$' 1 "$command_timeout" 'USB keyboard console input' || return 1
		wait_for "$shell_pattern" $((shell_before + 1)) "$command_timeout" 'USB console shell return' || return 1

		start_probe '/usr/bin/usb-hid-guest-probe record keyboard' \
			'^USB-HID-GUEST READY role=keyboard ' \
			'^USB-HID-GUEST RECORD PASS role=keyboard\r?$' || return 1
		qmp_key a no usb
		wait_probe_pass || return 1

		driver_count=$(marker_count "$driver_pattern" "$guest_log")
		qmp_hmp "device_add usb-mouse,bus=$usb_bus,port=2,id=mouse,serial=in-t41-mouse${usb_version}"
		wait_for "$driver_pattern" $((driver_count + 1)) "$hotplug_timeout" 'USB mouse binding' || return 1
		send_shell '/usr/bin/usb-hid-guest-probe inventory keyboard relative' || return 1
		start_probe '/usr/bin/usb-hid-guest-probe record relative' \
			'^USB-HID-GUEST READY role=relative ' \
			'^USB-HID-GUEST RECORD PASS role=relative\r?$' || return 1
		printf '%s\n' '{"execute":"input-send-event","arguments":{"device":"video0","events":[{"type":"rel","data":{"axis":"x","value":7}},{"type":"rel","data":{"axis":"y","value":-5}}]}}'
		wait_probe_pass || return 1

		start_probe '/usr/bin/usb-hid-guest-probe hotplug relative' \
			'^USB-HID-GUEST HOTPLUG READY role=relative ' \
			'^USB-HID-GUEST HOTPLUG PASS role=relative reused=/dev/input/event[0-9]+\r?$' || return 1
		disconnect_count=$(marker_count "$disconnect_pattern" "$guest_log")
		qmp_hmp 'device_del mouse'
		wait_for '^USB-HID-GUEST HOTPLUG DETACHED role=relative ' 1 \
			"$hotplug_timeout" 'old relative fd terminal' || return 1
		wait_for "$disconnect_pattern" $((disconnect_count + 1)) \
			"$hotplug_timeout" 'first mouse disconnect' || return 1
		qmp_hmp "device_add usb-mouse,bus=$usb_bus,port=2,id=mouse2,serial=in-t41-mouse2${usb_version}"
		wait_for '^USB-HID-GUEST HOTPLUG RELEASED role=relative ' 1 \
			"$hotplug_timeout" 'distinct event number while stale fd open' || return 1
		disconnect_count=$(marker_count "$disconnect_pattern" "$guest_log")
		qmp_hmp 'device_del mouse2'
		wait_for '^USB-HID-GUEST HOTPLUG SECOND-DETACHED role=relative\r?$' 1 \
			"$hotplug_timeout" 'second mouse detach' || return 1
		wait_for "$disconnect_pattern" $((disconnect_count + 1)) \
			"$hotplug_timeout" 'second mouse disconnect' || return 1
		qmp_hmp "device_add usb-mouse,bus=$usb_bus,port=2,id=mouse3,serial=in-t41-mouse3${usb_version}"
		wait_probe_pass || return 1

		# Keep 64 MiB of root-storage reads active while the reinserted
		# pointer delivers input on the same USB controller hierarchy.
		send_shell '/usr/bin/usb-hid-guest-probe storage /dev/sda &' || return 1
		wait_for 'USB-HID-GUEST STORAGE READY path=/dev/sda bytes=67108864' \
			1 "$command_timeout" 'concurrent USB root read start' || return 1
		start_probe '/usr/bin/usb-hid-guest-probe record relative' \
			'^USB-HID-GUEST READY role=relative ' \
			'^USB-HID-GUEST RECORD PASS role=relative\r?$' || return 1
		printf '%s\n' '{"execute":"input-send-event","arguments":{"device":"video0","events":[{"type":"rel","data":{"axis":"x","value":7}},{"type":"rel","data":{"axis":"y","value":-5}}]}}'
		wait_probe_pass || return 1
		send_shell 'wait' || return 1
		wait_for 'USB-HID-GUEST STORAGE PASS bytes=67108864' 1 \
			"$command_timeout" \
			'concurrent USB root read' || return 1

		disconnect_count=$(marker_count "$disconnect_pattern" "$guest_log")
		qmp_hmp 'device_del mouse3'
		wait_for "$disconnect_pattern" $((disconnect_count + 1)) \
			"$hotplug_timeout" 'mouse removal before tablet' || return 1
		driver_count=$(marker_count "$driver_pattern" "$guest_log")
		qmp_hmp "device_add usb-tablet,bus=$usb_bus,port=2,id=tablet,serial=in-t41-tablet${usb_version},display=video0"
		wait_for "$driver_pattern" $((driver_count + 1)) "$hotplug_timeout" 'USB tablet binding' || return 1
		send_shell '/usr/bin/usb-hid-guest-probe inventory keyboard absolute' || return 1
		start_probe '/usr/bin/usb-hid-guest-probe record absolute' \
			'^USB-HID-GUEST READY role=absolute ' \
			'^USB-HID-GUEST RECORD PASS role=absolute\r?$' || return 1
		printf '%s\n' '{"execute":"input-send-event","arguments":{"device":"video0","events":[{"type":"abs","data":{"axis":"x","value":20000}},{"type":"abs","data":{"axis":"y","value":400}}]}}'
		wait_probe_pass || return 1
		send_shell 'echo USB-HID-CELL-PASS' || return 1
		printf 'pass\n' >"$controller_result"
	}

	controller_body()
	{
		local status

		if controller_sequence; then
			status=0
		else
			status=$?
		fi
		# QEMU does not exit merely because its QMP stdio reaches EOF. Always
		# terminate the private VM, including after a fatal acceptance oracle.
		printf '%s\n' '{"execute":"quit"}'
		return "$status"
	}

	set +e
	controller_body |
		TMPDIR="$task_tmp" timeout --foreground --kill-after=5 \
		"${cell_timeout}s" "$qemu" -machine q35,usb=off,i8042=off -m 512 -smp 4 \
		-drive "if=pflash,format=raw,unit=0,readonly=on,file=$ovmf_code" \
		-drive "if=pflash,format=raw,unit=1,file=$vars_copy" \
		-device VGA,id=video0 \
		-drive "if=none,id=boot,file=$run_image,format=raw" \
		"${topology[@]}" -display none -serial none \
		-debugcon "file:$guest_log" -qmp stdio -no-reboot \
		>"$qmp_log" 2>&1
	pipeline_status=("${PIPESTATUS[@]}")
	set -e
	if [[ ${pipeline_status[0]} -ne 0 || ${pipeline_status[1]} -ne 0 ||
	      $(<"$controller_result") != pass ]]; then
		echo "$kind controller/QEMU failed: ${pipeline_status[*]} $(<"$controller_result")" >&2
		return 1
	fi
	if rg -a -q '"error"[[:space:]]*:' "$qmp_log"; then
		echo "$kind QMP rejected a command" >&2
		rg -a -m 1 '"error"[[:space:]]*:' "$qmp_log" >&2 || true
		return 1
	fi
	for pattern in '^usbconsolepass\r?$' \
		'^USB-HID-GUEST RECORD PASS role=keyboard\r?$' \
		'^USB-HID-GUEST RECORD PASS role=relative\r?$' \
		'^USB-HID-GUEST RECORD PASS role=absolute\r?$' \
		'^USB-HID-GUEST HOTPLUG PASS role=relative reused=/dev/input/event[0-9]+\r?$' \
		'^USB-HID-CELL-PASS\r?$'; do
		rg -a -q -- "$pattern" "$guest_log" || {
			echo "$kind missing final oracle: $pattern" >&2
			return 1
		}
	done
	if ! rg -a -q \
		'USB-HID-GUEST STORAGE PASS bytes=67108864' "$guest_log"; then
		echo "$kind missing concurrent USB-root oracle" >&2
		return 1
	fi
	printf 'result=pass\n' >>"$cell_metadata"
	printf '%s\tpass\t%s/guest.log\n' "$kind" "$kind" >>"$results"
}

for cell in $cells; do
	run_cell "$cell"
done

after_hash=$(sha256sum "$image" | awk '{print $1}')
if [[ $after_hash != "$image_hash" ]]; then
	echo "private source image changed during IN-T41" >&2
	exit 1
fi
printf 'image_sha256_after=%s\n' "$after_hash" >>"$metadata"
exit 0
