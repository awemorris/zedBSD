#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

image=$1
output=$2
kernel=${AMD64_KERNEL:-build/amd64/vmunix}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-30}

case $boot_timeout in
'' | *[!0-9]* | 0)
	echo "BOOT_TIMEOUT_SECONDS must be a positive integer" >&2
	exit 2
	;;
esac

test -f "$image" || {
	echo "image not found: $image" >&2
	exit 2
}
test -f "$kernel" || {
	echo "kernel not found: $kernel" >&2
	exit 2
}
for command in "$qemu" objdump od mcopy mdir rg sha256sum dd; do
	command -v "$command" >/dev/null
done

mkdir -p "$output"
base_digest=$(sha256sum "$image" | awk '{print $1}')
results=$output/results.tsv
qemu_pid=

cleanup()
{
	if [ -n "$qemu_pid" ]; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
}
trap cleanup 0
trap 'exit 130' HUP INT TERM

# amd64_cmain installs the GDT/TSS and IDT immediately before the call site
# selected here.  The test patches only a disposable kernel copy at the first
# instruction after amd64_int_init() returns.
patch_address=$(
	objdump -d --disassemble=amd64_cmain "$kernel" | awk '
		found && /^[[:space:]]*[0-9A-Fa-f]+:/ {
			address = $1
			sub(/:$/, "", address)
			print address
			exit
		}
		/call.*<amd64_int_init>/ { found = 1 }
	'
)
case $patch_address in
????????????????) ;;
*)
	echo "could not locate the instruction after amd64_int_init" >&2
	exit 1
	;;
esac
patch_low=${patch_address#????????}
patch_offset=$((0x$patch_low - 0x80200000 + 0x1000))
kernel_size=$(stat -c %s "$kernel")
if [ "$patch_offset" -lt 4096 ] ||
    [ $((patch_offset + 12)) -gt "$kernel_size" ]; then
	echo "derived early-IDT patch offset is outside the kernel" >&2
	exit 1
fi

find_boot_volume_offset()
{
	medium=$1
	found=
	for index in 0 1 2 3; do
		entry_offset=$((1024 + index * 128 + 32))
		start_lba=$(od -An -v -tu8 -N8 -j "$entry_offset" "$medium" |
		    tr -d '[:space:]')
		case $start_lba in
		'' | *[!0-9]* | 0) continue ;;
		esac
		candidate=$((start_lba * 512))
		if mdir -i "$medium@@$candidate" ::/zedbsd.cfg \
		    >/dev/null 2>&1; then
			if [ -n "$found" ]; then
				echo "multiple zedbsd.cfg payload volumes" >&2
				return 1
			fi
			found=$candidate
		fi
	done
	if [ -z "$found" ]; then
		echo "zedbsd.cfg payload volume not found" >&2
		return 1
	fi
	echo "$found"
}

printf 'case\tclass\tvector\telapsed_seconds\tfirst_failure\n' >"$results"

for kind in ud gp pf; do
	run_image=$output/$kind.img
	run_kernel=$output/vmunix-$kind
	guest_log=$output/$kind-guest.log
	qemu_log=$output/$kind-qemu.log
	cp --reflink=auto --sparse=always "$image" "$run_image"
	cp "$kernel" "$run_kernel"
	: >"$guest_log"
	: >"$qemu_log"

	case $kind in
	ud)
		# UD2: prove the early IDT handles vector 6.
		printf '\017\013' |
		    dd of="$run_kernel" bs=1 seek="$patch_offset" \
		    conv=notrunc status=none
		vector=6
		extra_pattern=
		;;
	gp)
		# mov ax,-1; mov ds,ax: the invalid selector raises #GP.
		printf '\146\270\377\377\216\330' |
		    dd of="$run_kernel" bs=1 seek="$patch_offset" \
		    conv=notrunc status=none
		vector=13
		extra_pattern=
		;;
	pf)
		# movabs rax,0xdeadbeef; jmp rax: instruction-fetch #PF.
		printf '\110\270\357\276\255\336\000\000\000\000\377\340' |
		    dd of="$run_kernel" bs=1 seek="$patch_offset" \
		    conv=notrunc status=none
		vector=14
		extra_pattern='cr2=00000000:DEADBEEF'
		;;
	esac

	boot_offset=$(find_boot_volume_offset "$run_image")
	mcopy -o -i "$run_image@@$boot_offset" "$run_kernel" ::/vmunix

	"$qemu" \
		-machine q35 \
		-m 512 \
		-smp 1 \
		-drive file="$run_image",format=raw,if=ide \
		-display none \
		-monitor none \
		-serial none \
		-debugcon file:"$guest_log" \
		-no-reboot >"$qemu_log" 2>&1 &
	qemu_pid=$!
	start=$(date +%s)
	deadline=$((start + boot_timeout))
	class=
	first_failure=

	while :; do
		now=$(date +%s)
		if rg -a -q "amd64 fault v=$vector " "$guest_log" 2>/dev/null &&
		    rg -a -q 'fatal: .*unhandled amd64 fault' "$guest_log" \
		    2>/dev/null; then
			class=pass
			break
		fi
		if rg -a -q 'A64 ACPI RSDP PASS|A64 IRQ READY|boot: HAL initialized successfully' \
		    "$guest_log" 2>/dev/null; then
			class=false-pass
			first_failure="guest passed the injected early-fault boundary"
			break
		fi
		if [ "$now" -ge "$deadline" ]; then
			class=boot-timeout
			first_failure="expected vector $vector was not observed"
			break
		fi
		if ! kill -0 "$qemu_pid" 2>/dev/null; then
			class=early-qemu-exit
			first_failure="QEMU exited before vector $vector"
			break
		fi
		sleep 0.1
	done

	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=

	if [ "$class" = pass ] &&
	    ! rg -a -F -q 'A64 PAGING PASS' "$guest_log"; then
		class=missing-marker
		first_failure="paging marker missing before vector $vector"
	fi
	if [ "$class" = pass ] && [ -n "$extra_pattern" ] &&
	    ! rg -a -F -q "$extra_pattern" "$guest_log"; then
		class=missing-marker
		first_failure="missing $extra_pattern"
	fi
	if [ "$class" = pass ] &&
	    rg -a -q 'A64 ACPI RSDP PASS|A64 IRQ READY|boot: HAL initialized successfully' \
	    "$guest_log"; then
		class=false-pass
		first_failure="guest passed the injected early-fault boundary"
	fi
	printf '%s\t%s\t%s\t%s\t%s\n' "$kind" "$class" "$vector" \
	    "$(($(date +%s) - start))" "$first_failure" >>"$results"
	if [ "$class" != pass ]; then
		echo "early-IDT fault regression FAIL: $kind: $class: $first_failure" >&2
		echo "guest log: $guest_log" >&2
		exit 1
	fi
done

if [ "$(sha256sum "$image" | awk '{print $1}')" != "$base_digest" ]; then
	echo "early-IDT fault regression FAIL: pristine input image changed" >&2
	exit 1
fi

echo "CF-SV7 early-IDT fault regression: PASS"
echo "results: $results"
