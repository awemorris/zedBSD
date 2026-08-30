#!/bin/sh
set -eu

usage()
{
	echo "usage: $0 [--dry-run] IMAGE OUTPUT-DIRECTORY" >&2
}

dry_run=0
case ${1:-} in
--dry-run)
	dry_run=1
	shift
	;;
--help)
	usage
	exit 0
	;;
esac
if [ "$#" -ne 2 ]; then
	usage
	exit 2
fi

image=$1
output=$2
larger_media_sectors=${LARGER_MEDIA_SECTORS:-60549120}
ovmf_memory_mib=${OVMF_MEMORY_MIB:-4096}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
legacy_runner=$script_directory/legacy-xhci-usb-boot.sh
uefi_runner=$script_directory/uefi-high-memory-usb-boot.sh

case $larger_media_sectors in
'' | *[!0-9]* | 0*)
	echo "LARGER_MEDIA_SECTORS must be a positive sector count" >&2
	exit 2
	;;
esac
if [ "${#larger_media_sectors}" -gt 16 ]; then
	echo "LARGER_MEDIA_SECTORS exceeds the supported host size" >&2
	exit 2
fi
case $ovmf_memory_mib in
'' | *[!0-9]* | 0)
	echo "OVMF_MEMORY_MIB must be a positive MiB count" >&2
	exit 2
	;;
esac
test -f "$image" || {
	echo "image not found: $image" >&2
	exit 2
}
test -x "$legacy_runner" || {
	echo "legacy USB runner not executable: $legacy_runner" >&2
	exit 2
}
test -x "$uefi_runner" || {
	echo "UEFI USB runner not executable: $uefi_runner" >&2
	exit 2
}
test -f "$ovmf_code" || {
	echo "OVMF code image not found: $ovmf_code" >&2
	exit 2
}
test -f "$ovmf_vars" || {
	echo "OVMF variables image not found: $ovmf_vars" >&2
	exit 2
}
command -v "$qemu" >/dev/null
command -v rg >/dev/null
command -v sha256sum >/dev/null
command -v truncate >/dev/null

source_bytes=$(wc -c <"$image")
case $source_bytes in
'' | *[!0-9]*)
	echo "could not determine image size: $image" >&2
	exit 2
	;;
esac
if [ $((source_bytes % 512)) -ne 0 ]; then
	echo "production image size must be a multiple of 512 bytes" >&2
	exit 2
fi
source_sectors=$((source_bytes / 512))
if [ "$larger_media_sectors" -le "$source_sectors" ]; then
	echo "LARGER_MEDIA_SECTORS must exceed the source image sector count" >&2
	exit 2
fi
logical_last=$((source_sectors - 1))
physical_last=$((larger_media_sectors - 1))
ignored_sectors=$((larger_media_sectors - source_sectors))
base_digest=$(sha256sum "$image" | awk '{print $1}')
bounded_diagnostic="gpt: sda bounded extent accepted: logical-last=$logical_last physical-last=$physical_last declared-sectors=$source_sectors physical-sectors=$larger_media_sectors ignored-tail-sectors=$ignored_sectors"

mkdir -p "$output"
metadata=$output/metadata.txt
results=$output/results.tsv
{
	echo "case=BR-T53"
	echo "base_image=$image"
	echo "base_sha256=$base_digest"
	echo "base_bytes=$source_bytes"
	echo "logical_sectors=$source_sectors"
	echo "larger_media_sectors=$larger_media_sectors"
	echo "ignored_trailing_sectors=$ignored_sectors"
	echo "bounded_diagnostic=$bounded_diagnostic"
	echo "ovmf_memory_mib=$ovmf_memory_mib"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "topologies=SeaBIOS/OVMF q35 qemu-xhci USB-storage-only system disk"
	echo "dry_run=$dry_run"
} >"$metadata"
printf 'firmware\tclass\telapsed_seconds\tfirst_failure\tguest_log\n' >"$results"

if [ "$dry_run" -eq 1 ]; then
	printf 'seabios\tdry-run\t0\t\t%s\n' "$output/seabios/guest.log" >>"$results"
	printf 'ovmf\tdry-run\t0\t\t%s\n' \
	    "$output/ovmf/boot-${ovmf_memory_mib}m.log" >>"$results"
	final_digest=$(sha256sum "$image" | awk '{print $1}')
	echo "base_sha256_after=$final_digest" >>"$metadata"
	if [ "$final_digest" != "$base_digest" ]; then
		echo "BR-T53 dry run FAIL: source image changed" >&2
		exit 1
	fi
	echo "BR-T53 dry run PASS: BIOS and OVMF larger-media cells validated"
	exit 0
fi

validation_failure=
validate_guest_log()
{
	log=$1
	if [ ! -f "$log" ]; then
		validation_failure="missing guest log"
		return 1
	fi
	for marker in \
	    "usb-storage: sda blocks=$larger_media_sectors block-size=512" \
	    "$bounded_diagnostic" \
	    'vfs: loop0 <- boot0:rootfs.img (private, read-only)' \
	    'vfs: loop1 <- boot0:data.img (private, read-write)' \
	    'vfs: root=overlay lower=boot0:rootfs.img upper=boot0:data.img' \
	    'vfs: runtime filesystems mounted' \
	    'login:'; do
		if ! rg -a -F -q -- "$marker" "$log"; then
			validation_failure="missing $marker"
			return 1
		fi
	done
	if ! rg -a -q \
	    'vfs: boot0 UUID=[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4} -> /dev/sda[0-9]+ \(private FAT\)' \
	    "$log"; then
		validation_failure="missing boot0 UUID resolution to the private FAT"
		return 1
	fi
	if rg -a -q 'gpt: sda rejected:|VFS initialization failed' "$log"; then
		validation_failure=$(rg -a -m 1 \
		    'gpt: sda rejected:|VFS initialization failed' "$log" |
		    tr '\t\r\n' '   ')
		return 1
	fi
	return 0
}

passes=0
start=$(date +%s)
class=runner-failure
first_failure="legacy USB runner failed"
if USB_MEDIA_SECTORS=$larger_media_sectors \
    "$legacy_runner" "$image" "$output/seabios"; then
	validation_failure=
	if validate_guest_log "$output/seabios/guest.log"; then
		class=pass
		first_failure=
		passes=$((passes + 1))
	else
		class=marker-failure
		first_failure=$validation_failure
	fi
fi
printf 'seabios\t%s\t%s\t%s\t%s\n' "$class" \
    "$(($(date +%s) - start))" "$first_failure" \
    "$output/seabios/guest.log" >>"$results"

start=$(date +%s)
class=runner-failure
first_failure="OVMF USB runner failed"
if USB_MEDIA_SECTORS=$larger_media_sectors \
    MEMORY_MIB_LIST=$ovmf_memory_mib \
    "$uefi_runner" "$image" "$output/ovmf"; then
	validation_failure=
	if validate_guest_log "$output/ovmf/boot-${ovmf_memory_mib}m.log"; then
		class=pass
		first_failure=
		passes=$((passes + 1))
	else
		class=marker-failure
		first_failure=$validation_failure
	fi
fi
printf 'ovmf\t%s\t%s\t%s\t%s\n' "$class" \
    "$(($(date +%s) - start))" "$first_failure" \
    "$output/ovmf/boot-${ovmf_memory_mib}m.log" >>"$results"

final_digest=$(sha256sum "$image" | awk '{print $1}')
echo "base_sha256_after=$final_digest" >>"$metadata"
if [ "$final_digest" = "$base_digest" ]; then
	echo "source_image=preserved" >>"$metadata"
else
	echo "source_image=mutated" >>"$metadata"
	echo "BR-T53 FAIL: pristine production image changed" >&2
	exit 1
fi
if [ "$passes" -ne 2 ]; then
	echo "BR-T53 FAIL: pass=$passes expected=2" >&2
	echo "results: $results" >&2
	exit 1
fi
echo "BR-T53 PASS: SeaBIOS and OVMF bounded-GPT USB copies reached login"
echo "results: $results"
