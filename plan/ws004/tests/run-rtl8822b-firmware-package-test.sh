#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-rtl8822b-firmware.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT

for command_name in awk curl find git grep make mktemp python3 sha256sum wc; do
	command -v "$command_name" >/dev/null || {
		echo "missing host command: $command_name" >&2
		exit 1
	}
done

package_makefile=$repo/userland/firmware/rtl8822b/Makefile
manifest=$repo/userland/firmware/rtl8822b/rtl8822b-firmware.manifest
absent_config=$temporary/absent-config.mk
fetch_log=$temporary/fetch.log
fetcher=$temporary/fetch
fail_fetcher=$temporary/fail-fetch

printf '%s\n' \
	'#!/bin/sh' \
	'set -eu' \
	': "${FETCH_LOG:?}"' \
	'printf "%s\n" "$*" >>"$FETCH_LOG"' \
	'exec curl --fail --location --silent --show-error "$@"' >"$fetcher"
printf '%s\n' \
	'#!/bin/sh' \
	'echo "unexpected firmware network fetch" >&2' \
	'exit 97' >"$fail_fetcher"
chmod +x "$fetcher" "$fail_fetcher"

if git -C "$repo" ls-files | grep -E '/rtw8822b_fw\.bin$' >/dev/null; then
	echo 'RTL8822B firmware blob must not be tracked' >&2
	exit 1
fi

# Production metadata has two deliberately distinct WHENCE identities: the
# GitHub acquisition mirror bytes and the official upstream provenance bytes.
grep -Fq 'RTL8822B_FIRMWARE_MIRROR_REVISION ?= 2f56219d20e4becccd718963fc3bcc671c543ce5' "$package_makefile"
grep -Fq 'RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256 ?= 54474263d8418a29e7b11485e79936d15a81dd75381308b29cadf2d53b78aa51' "$package_makefile"
grep -Fq 'RTL8822B_FIRMWARE_OFFICIAL_WHENCE_SHA256 := 34f954c7d068ec4fd5fcc216471912dd3cf40ff60a7ffa8d06ff6f9b5999551f' "$package_makefile"
grep -Fq 'acquisition-whence-sha256=54474263d8418a29e7b11485e79936d15a81dd75381308b29cadf2d53b78aa51' "$manifest"
grep -Fq 'official-whence-sha256=34f954c7d068ec4fd5fcc216471912dd3cf40ff60a7ffa8d06ff6f9b5999551f' "$manifest"
grep -Fq 'redistribution-notice=Copyright and disclaimer must accompany the unmodified binary; consult the exact installed Realtek license' "$manifest"
if grep -F 'acquisition-whence-sha256=' "$manifest" | grep -Fq '34f954c7'; then
	echo 'mirror WHENCE was incorrectly identified as official WHENCE' >&2
	exit 1
fi

# Production identity is immutable even when a caller attempts to replace all
# metadata on the make command line.  The installed static manifest must never
# describe different bytes from those accepted by the package recipe.
locked_metadata=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$absent_config" ZEDBSD_PLATFORM=amd64 \
	ZEDBSD_ARCHITECTURE=amd64 ZEDBSD_BOARD=pcat ZEDBSD_VARIANT=uefi \
	RTL8822B_FIRMWARE_MIRROR_BASE_URL=https://example.invalid/attacker \
	RTL8822B_FIRMWARE_MIRROR_REVISION=attacker-revision \
	RTL8822B_FIRMWARE_SIZE=1 RTL8822B_FIRMWARE_SHA256=deadbeef \
	RTL8822B_FIRMWARE_LICENSE_SIZE=1 \
	RTL8822B_FIRMWARE_LICENSE_SHA256=deadbeef \
	RTL8822B_FIRMWARE_MIRROR_WHENCE_SIZE=1 \
	RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256=deadbeef \
	RTL8822B_FIRMWARE_SHA256_COMMAND=false \
	RTL8822B_FIRMWARE_MANIFEST=/tmp/attacker-manifest \
	RTL8822B_FIRMWARE_DATA=/tmp/attacker-data \
	'USERLAND_rtl8822b-firmware_DATA=/tmp/attacker-userland-data' \
	--eval='q056-print-locked-metadata:;@printf "%s\n" "$(RTL8822B_FIRMWARE_MIRROR_BASE_URL)|$(RTL8822B_FIRMWARE_MIRROR_REVISION)|$(RTL8822B_FIRMWARE_SIZE)|$(RTL8822B_FIRMWARE_SHA256)|$(RTL8822B_FIRMWARE_LICENSE_SIZE)|$(RTL8822B_FIRMWARE_LICENSE_SHA256)|$(RTL8822B_FIRMWARE_MIRROR_WHENCE_SIZE)|$(RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256)|$(RTL8822B_FIRMWARE_SHA256_COMMAND)|$(RTL8822B_FIRMWARE_MANIFEST)|$(RTL8822B_FIRMWARE_DATA)|$(USERLAND_rtl8822b-firmware_DATA)"' \
	q056-print-locked-metadata)
grep -Fq 'https://raw.githubusercontent.com/endlessm/linux-firmware|2f56219d20e4becccd718963fc3bcc671c543ce5|161240|a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418|2115|a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e|364259|54474263d8418a29e7b11485e79936d15a81dd75381308b29cadf2d53b78aa51|sha256sum|userland/firmware/rtl8822b/rtl8822b-firmware.manifest|' <<<"$locked_metadata"
grep -Fq '/lib/firmware/rtw88/rtw8822b_fw.bin=' <<<"$locked_metadata"
if grep -Eq 'attacker|deadbeef|/tmp/attacker|\|false\|' <<<"$locked_metadata"; then
	echo 'production firmware metadata accepted a command-line substitution' >&2
	exit 1
fi

# Listing packages and creating a default config may discover metadata, but
# must neither select firmware nor execute an acquisition command.
default_cache=$temporary/default-cache
default_config=$temporary/default.mk
: >"$fetch_log"
FETCH_LOG=$fetch_log \
RTL8822B_FIRMWARE_FETCH=$fetcher \
RTL8822B_FIRMWARE_CACHE_ROOT=$default_cache \
	make -C "$repo" --no-print-directory ZEDBSD_CONFIG="$absent_config" \
	list-user-programs >"$temporary/programs"
grep -Fq 'rtl8822b-firmware|Realtek RTL8822B firmware|i386 pc98 amd64|n|firmware|firmware/rtl8822b|' "$temporary/programs"
FETCH_LOG=$fetch_log \
RTL8822B_FIRMWARE_FETCH=$fetcher \
RTL8822B_FIRMWARE_CACHE_ROOT=$default_cache \
	ZEDBSD_CONFIG="$absent_config" \
	python3 "$repo/tools/menuconfig.py" --defaults --output "$default_config"
if grep '^ZEDBSD_USER_PROGRAMS' "$default_config" | grep -Fq 'rtl8822b-firmware'; then
	echo 'RTL8822B firmware must be default-off' >&2
	exit 1
fi
test ! -e "$default_cache"
test ! -s "$fetch_log"
default_data=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$default_config" \
	--eval='q056-print-default-data:;@printf "%s\n" "$(ZEDBSD_USERLAND_DATA_INPUTS)"' \
	q056-print-default-data)
if grep -Fq 'rtw8822b' <<<"$default_data"; then
	echo 'ordinary default image unexpectedly depends on RTL8822B firmware' >&2
	exit 1
fi
test ! -e "$default_cache"
test ! -s "$fetch_log"

# A valid old/minimal config with no user-program assignment must retain the
# registered default choices.  It must not mean "select every optional
# package", which would make an ordinary build fetch firmware.
minimal_config=$temporary/minimal.mk
printf '%s\n' \
	'ZEDBSD_PLATFORM := amd64' \
	'ZEDBSD_ARCHITECTURE := amd64' \
	'ZEDBSD_BOARD := pcat' \
	'ZEDBSD_VARIANT := uefi' >"$minimal_config"
minimal_programs=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$minimal_config" \
	--eval='q056-print-minimal-programs:;@printf "%s\n" "$(ZEDBSD_USER_PROGRAMS)"' \
	q056-print-minimal-programs)
if grep -Fq 'rtl8822b-firmware' <<<"$minimal_programs"; then
	echo 'legacy/minimal config selected default-off RTL8822B firmware' >&2
	exit 1
fi
test ! -e "$default_cache"
test ! -s "$fetch_log"

# Reusing one build directory across menu changes must invalidate rootfs even
# when a newly selected cache file is older than the previous rootfs stamp.
# Exercise the content-stable prerequisite directly without assembling an
# image or acquiring bytes.
stamp_build=$temporary/rootfs-build
stamp_file=$stamp_build/.rootfs-config
arch_image_dir=$temporary/arch-images
arch_stamp=$arch_image_dir/.amd64-rootfs-config
make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$minimal_config" BUILD="$stamp_build" \
	ARCH_IMAGE_DIR="$arch_image_dir" \
	ZEDBSD_USER_PROGRAMS= "$stamp_file"
if grep -Fq 'rtl8822b-firmware' "$stamp_file"; then
	echo 'unselected rootfs stamp contains RTL8822B firmware' >&2
	exit 1
fi
make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$minimal_config" BUILD="$stamp_build" \
	ARCH_IMAGE_DIR="$arch_image_dir" \
	ZEDBSD_USER_PROGRAMS=rtl8822b-firmware "$stamp_file"
grep -Fq 'programs=' "$stamp_file"
grep -Fq 'rtl8822b-firmware' "$stamp_file"
grep -Fq '/lib/firmware/rtw88/rtw8822b_fw.bin=' "$stamp_file"
make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$minimal_config" BUILD="$stamp_build" \
	ARCH_IMAGE_DIR="$arch_image_dir" \
	ZEDBSD_USER_PROGRAMS= "$stamp_file"
if grep -Fq 'rtl8822b-firmware' "$stamp_file"; then
	echo 'deselected rootfs stamp retained RTL8822B firmware' >&2
	exit 1
fi

# Both filesystem encoders feed disk-image assembly directly, so they must
# also depend on the selection stamp.  Otherwise a selected firmware payload
# can survive in an older .img/.ufs after menuconfig deselects it.
make_database=$temporary/make-database
make -C "$repo" --no-print-directory -pn \
	ZEDBSD_CONFIG="$minimal_config" BUILD="$stamp_build" \
	ARCH_IMAGE_DIR="$arch_image_dir" "$arch_image_dir/amd64.ufs" \
	>"$make_database"
for arch_image in amd64.img amd64.ufs; do
	dependency_line=$(grep -F "$arch_image_dir/$arch_image:" \
		"$make_database")
	if ! grep -Fq "$arch_stamp" <<<"$dependency_line"; then
		echo "$arch_image does not depend on the userland selection stamp" >&2
		exit 1
	fi
done

# PC/AT and PC-98 intentionally share the i386 architecture-image pathname.
# Their shared stamp must therefore also be shared and include platform plus
# selection identity; a BUILD-local stamp would allow a stale image when
# switching PC/AT -> PC-98 -> PC/AT.
pcat_config=$temporary/pcat.mk
pc98_config=$temporary/pc98.mk
printf '%s\n' \
	'ZEDBSD_PLATFORM := i386' \
	'ZEDBSD_ARCHITECTURE := i386' \
	'ZEDBSD_BOARD := pcat' \
	'ZEDBSD_VARIANT := default' >"$pcat_config"
printf '%s\n' \
	'ZEDBSD_PLATFORM := pc98' \
	'ZEDBSD_ARCHITECTURE := i386' \
	'ZEDBSD_BOARD := pc98' \
	'ZEDBSD_VARIANT := default' >"$pc98_config"
i386_arch_stamp=$arch_image_dir/.i386-rootfs-config
make -C "$repo" --no-print-directory -s ZEDBSD_CONFIG="$pcat_config" \
	BUILD="$temporary/pcat-build" ARCH_IMAGE_DIR="$arch_image_dir" \
	ZEDBSD_USER_PROGRAMS=rtl8822b-firmware "$i386_arch_stamp"
grep -Fq 'platform=i386;' "$i386_arch_stamp"
grep -Fq 'rtl8822b-firmware' "$i386_arch_stamp"
make -C "$repo" --no-print-directory -s ZEDBSD_CONFIG="$pc98_config" \
	BUILD="$temporary/pc98-build" ARCH_IMAGE_DIR="$arch_image_dir" \
	ZEDBSD_USER_PROGRAMS= "$i386_arch_stamp"
grep -Fq 'platform=pc98;' "$i386_arch_stamp"
if grep -Fq 'rtl8822b-firmware' "$i386_arch_stamp"; then
	echo 'shared i386 architecture stamp retained PC/AT firmware selection' >&2
	exit 1
fi
make -C "$repo" --no-print-directory -s ZEDBSD_CONFIG="$pcat_config" \
	BUILD="$temporary/pcat-build" ARCH_IMAGE_DIR="$arch_image_dir" \
	ZEDBSD_USER_PROGRAMS=rtl8822b-firmware "$i386_arch_stamp"
grep -Fq 'platform=i386;' "$i386_arch_stamp"
grep -Fq 'rtl8822b-firmware' "$i386_arch_stamp"

python3 - "$repo" <<'PY'
import importlib.util
import pathlib
import sys

repo = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("zedbsd_menuconfig", repo / "tools/menuconfig.py")
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)
assert [label for label, _groups in module.PROGRAM_CATEGORIES] == [
    "Base", "X11", "Firmware", "Packages"
]
row = next(row for row in module.user_program_rows() if row[0] == "rtl8822b-firmware")
assert module.applies(row[2], "i386")
assert module.applies(row[2], "amd64")
assert module.applies(row[2], "pc98")
assert not module.applies(row[2], "rpi4")

usb_options = module.CONFIG_DIR / "drivers" / "usb.drivers"
for platform in ("i386", "amd64"):
    keys = {str(option["key"])
            for option in module.option_rows(usb_options, platform)}
    assert "CONFIG_DRIVER_USB_RTL8822BU" in keys
for platform in ("pc98", "rpi4", "sun4u", "x68k"):
    keys = {str(option["key"])
            for option in module.option_rows(usb_options, platform)}
    assert "CONFIG_DRIVER_USB_RTL8822BU" not in keys
    values = module.defaults()
    values["ZEDBSD_PLATFORM"] = platform
    values["CONFIG_DRIVER_USB_RTL8822BU"] = "y"
    module.normalize(values)
    assert values["CONFIG_DRIVER_USB_RTL8822BU"] == "n"
PY

# Use small local bytes to exercise the acquisition/cache algorithm without
# placing a binary fixture in the repository or requiring network service.
revision=q056-fixture-revision
fixture=$temporary/mirror/$revision
mkdir -p "$fixture/rtw88"
printf 'q056 rtl8822b firmware fixture\n' >"$fixture/rtw88/rtw8822b_fw.bin"
printf 'q056 rtlwifi license fixture\n' >"$fixture/LICENCE.rtlwifi_firmware.txt"
printf 'q056 mirror WHENCE fixture distinct from official provenance\n' >"$fixture/WHENCE"

firmware_size=$(wc -c <"$fixture/rtw88/rtw8822b_fw.bin" | tr -d '[:space:]')
license_size=$(wc -c <"$fixture/LICENCE.rtlwifi_firmware.txt" | tr -d '[:space:]')
whence_size=$(wc -c <"$fixture/WHENCE" | tr -d '[:space:]')
firmware_hash=$(sha256sum "$fixture/rtw88/rtw8822b_fw.bin" | awk '{print $1}')
license_hash=$(sha256sum "$fixture/LICENCE.rtlwifi_firmware.txt" | awk '{print $1}')
whence_hash=$(sha256sum "$fixture/WHENCE" | awk '{print $1}')

run_package() {
	local cache_root=$1
	local fetch_command=$2
	FETCH_LOG=$fetch_log make -C "$repo" --no-print-directory \
		ZEDBSD_CONFIG="$absent_config" \
		RTL8822B_FIRMWARE_MIRROR_BASE_URL="file://$temporary/mirror" \
		RTL8822B_FIRMWARE_MIRROR_REVISION="$revision" \
		RTL8822B_FIRMWARE_SIZE="$firmware_size" \
		RTL8822B_FIRMWARE_SHA256="$firmware_hash" \
		RTL8822B_FIRMWARE_LICENSE_SIZE="$license_size" \
		RTL8822B_FIRMWARE_LICENSE_SHA256="$license_hash" \
		RTL8822B_FIRMWARE_MIRROR_WHENCE_SIZE="$whence_size" \
		RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256="$whence_hash" \
		RTL8822B_FIRMWARE_CACHE_ROOT="$cache_root" \
		RTL8822B_FIRMWARE_FETCH="$fetch_command" \
		rtl8822b-firmware-fixture-cache
}

cache_root=$temporary/cache
: >"$fetch_log"
run_package "$cache_root" "$fetcher"
cache=$cache_root/$revision
test "$(wc -l <"$fetch_log" | tr -d '[:space:]')" = 3
test "$(sha256sum "$cache/rtw8822b_fw.bin" | awk '{print $1}')" = "$firmware_hash"
test "$(sha256sum "$cache/LICENCE.rtlwifi_firmware.txt" | awk '{print $1}')" = "$license_hash"
test "$(sha256sum "$cache/WHENCE" | awk '{print $1}')" = "$whence_hash"

# A complete verified cache is sufficient offline and does not call fetch.
: >"$fetch_log"
run_package "$cache_root" "$fail_fetcher"
test ! -s "$fetch_log"

missing_offline_root=$temporary/missing-offline-cache
if run_package "$missing_offline_root" "$fail_fetcher" >"$temporary/missing-offline.log" 2>&1; then
	echo 'missing offline cache unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'unexpected firmware network fetch' "$temporary/missing-offline.log"
test ! -e "$missing_offline_root/$revision"

# Each protected object is independently revalidated.  Existing corruption is
# reported and is not concealed by a replacement download.
snapshot=$temporary/verified-snapshot
cp -a "$cache" "$snapshot"
for corrupt_name in rtw8822b_fw.bin LICENCE.rtlwifi_firmware.txt WHENCE; do
	corrupt_root=$temporary/corrupt-$corrupt_name
	mkdir -p "$corrupt_root"
	cp -a "$snapshot" "$corrupt_root/$revision"
	printf 'corrupt\n' >>"$corrupt_root/$revision/$corrupt_name"
	if run_package "$corrupt_root" "$fail_fetcher" >"$temporary/corrupt.log" 2>&1; then
		echo "corrupt cache unexpectedly accepted: $corrupt_name" >&2
		exit 1
	fi
	grep -Eq 'size mismatch|SHA-256 mismatch' "$temporary/corrupt.log"
done

# A failed selected acquisition never publishes its temporary directory as a
# cache, and a subsequent selected build can retry normally.
partial_root=$temporary/partial-cache
incomplete_mirror=$temporary/incomplete/$revision
mkdir -p "$incomplete_mirror/rtw88"
cp "$fixture/rtw88/rtw8822b_fw.bin" "$incomplete_mirror/rtw88/"
if FETCH_LOG=$fetch_log make -C "$repo" --no-print-directory \
	RTL8822B_FIRMWARE_MIRROR_BASE_URL="file://$temporary/incomplete" \
	RTL8822B_FIRMWARE_MIRROR_REVISION="$revision" \
	RTL8822B_FIRMWARE_SIZE="$firmware_size" \
	RTL8822B_FIRMWARE_SHA256="$firmware_hash" \
	RTL8822B_FIRMWARE_LICENSE_SIZE="$license_size" \
	RTL8822B_FIRMWARE_LICENSE_SHA256="$license_hash" \
	RTL8822B_FIRMWARE_MIRROR_WHENCE_SIZE="$whence_size" \
	RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256="$whence_hash" \
	RTL8822B_FIRMWARE_CACHE_ROOT="$partial_root" \
	RTL8822B_FIRMWARE_FETCH="$fetcher" \
	rtl8822b-firmware-fixture-cache >"$temporary/partial.log" 2>&1; then
	echo 'incomplete acquisition unexpectedly succeeded' >&2
	exit 1
fi
test ! -e "$partial_root/$revision"
test -z "$(find "$partial_root" -mindepth 1 -print -quit)"

# The rootfs assembler receives only the three declared installed files.  The
# mirror WHENCE is verified provenance metadata and is intentionally not
# installed as a fourth payload.
data_mapping=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$absent_config" ZEDBSD_PLATFORM=amd64 \
	ZEDBSD_ARCHITECTURE=amd64 ZEDBSD_BOARD=pcat ZEDBSD_VARIANT=uefi \
	ZEDBSD_USER_PROGRAMS=rtl8822b-firmware \
	--eval='q056-print-firmware-data:;@printf "%s\n" "$(RTL8822B_FIRMWARE_DATA)"' \
	q056-print-firmware-data)
grep -Fq '/lib/firmware/rtw88/rtw8822b_fw.bin=' <<<"$data_mapping"
grep -Fq '/usr/share/licenses/rtl8822b-firmware/LICENCE.rtlwifi_firmware.txt=' <<<"$data_mapping"
grep -Fq '/usr/share/zedbsd/packages/rtl8822b-firmware.manifest=' <<<"$data_mapping"
test "$(grep -o -- '/[^ =]*=' <<<"$data_mapping" | wc -l | tr -d '[:space:]')" = 3

echo 'rtl8822b firmware package: PASS'
