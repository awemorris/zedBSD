#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intelax211-firmware.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT

for command_name in awk curl find git grep make mktemp python3 sha256sum wc; do
	command -v "$command_name" >/dev/null || {
		echo "missing host command: $command_name" >&2
		exit 1
	}
done

package_makefile=$repo/userland/firmware/intelax211/Makefile
manifest=$repo/userland/firmware/intelax211/intelax211-firmware.manifest
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

if git -C "$repo" ls-files | grep -E \
	'/(iwlwifi-so-a0-gf-a0-89\.ucode|iwlwifi-so-a0-gf-a0\.pnvm)$' \
	>/dev/null; then
	echo 'AX211 firmware bytes must not be tracked' >&2
	exit 1
fi

# Static package metadata and the manifest must describe the q061 exact bytes,
# official immutable commit, complete license/WHENCE, and visible version
# discrepancy.
grep -Fq 'override INTEL_AX211_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120' "$package_makefile"
grep -Fq 'override INTEL_AX211_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120' "$package_makefile"
grep -Fq 'acquisition-tag=20260410' "$manifest"
grep -Fq 'acquisition-tag-object=4585dd5a5f0cee08990d754701d8866d9e9266e6' "$manifest"
grep -Fq 'acquisition-revision=dc85ccedc9c973682fbcf4d628ca61174bcc3120' "$manifest"
grep -Fq 'acquisition-firmware-size=1736748' "$manifest"
grep -Fq 'acquisition-firmware-sha256=c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514' "$manifest"
grep -Fq 'acquisition-pnvm-size=55176' "$manifest"
grep -Fq 'acquisition-pnvm-sha256=efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a' "$manifest"
grep -Fq 'acquisition-license-size=2046' "$manifest"
grep -Fq 'acquisition-license-sha256=16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5' "$manifest"
grep -Fq 'acquisition-whence-size=425450' "$manifest"
grep -Fq 'acquisition-whence-sha256=c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527' "$manifest"
grep -Fq 'firmware-runtime-version=89.735b75a4.0' "$manifest"
grep -Fq 'whence-recorded-version=86.735b75a4.0' "$manifest"
grep -Fq 'version-discrepancy=retained' "$manifest"

# Production acquisition identity, accepted bytes, verifier, manifest, and
# rootfs mapping must ignore command-line substitutions.
locked_metadata=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$absent_config" ZEDBSD_PLATFORM=amd64 \
	ZEDBSD_ARCHITECTURE=amd64 ZEDBSD_BOARD=pcat ZEDBSD_VARIANT=uefi \
	INTEL_AX211_FIRMWARE_BASE_URL=https://example.invalid/attacker \
	INTEL_AX211_FIRMWARE_REVISION=attacker \
	INTEL_AX211_FIRMWARE_URL_SUFFIX='?id=attacker' \
	INTEL_AX211_FIRMWARE_UCODE_SIZE=1 \
	INTEL_AX211_FIRMWARE_UCODE_SHA256=deadbeef \
	INTEL_AX211_FIRMWARE_PNVM_SIZE=1 \
	INTEL_AX211_FIRMWARE_PNVM_SHA256=deadbeef \
	INTEL_AX211_FIRMWARE_LICENSE_SIZE=1 \
	INTEL_AX211_FIRMWARE_LICENSE_SHA256=deadbeef \
	INTEL_AX211_FIRMWARE_WHENCE_SIZE=1 \
	INTEL_AX211_FIRMWARE_WHENCE_SHA256=deadbeef \
	INTEL_AX211_FIRMWARE_FETCH=false \
	INTEL_AX211_FIRMWARE_SHA256_COMMAND=false \
	INTEL_AX211_FIRMWARE_MANIFEST=/tmp/attacker-manifest \
	INTEL_AX211_FIRMWARE_DATA=/tmp/attacker-data \
	'USERLAND_intelax211-firmware_DATA=/tmp/attacker-userland-data' \
	--eval='q062-print-locked-metadata:;@printf "%s\n" "$(INTEL_AX211_FIRMWARE_BASE_URL)|$(INTEL_AX211_FIRMWARE_REVISION)|$(INTEL_AX211_FIRMWARE_URL_SUFFIX)|$(INTEL_AX211_FIRMWARE_UCODE_SIZE)|$(INTEL_AX211_FIRMWARE_UCODE_SHA256)|$(INTEL_AX211_FIRMWARE_PNVM_SIZE)|$(INTEL_AX211_FIRMWARE_PNVM_SHA256)|$(INTEL_AX211_FIRMWARE_LICENSE_SIZE)|$(INTEL_AX211_FIRMWARE_LICENSE_SHA256)|$(INTEL_AX211_FIRMWARE_WHENCE_SIZE)|$(INTEL_AX211_FIRMWARE_WHENCE_SHA256)|$(INTEL_AX211_FIRMWARE_FETCH)|$(INTEL_AX211_FIRMWARE_SHA256_COMMAND)|$(INTEL_AX211_FIRMWARE_MANIFEST)|$(INTEL_AX211_FIRMWARE_DATA)|$(USERLAND_intelax211-firmware_DATA)"' \
	q062-print-locked-metadata)
grep -Fq 'https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain|dc85ccedc9c973682fbcf4d628ca61174bcc3120|?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120|1736748|c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514|55176|efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a|2046|16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527|curl --fail --location --silent --show-error|sha256sum|userland/firmware/intelax211/intelax211-firmware.manifest|' <<<"$locked_metadata"
grep -Fq '/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode=' <<<"$locked_metadata"
grep -Fq '/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm=' <<<"$locked_metadata"
if grep -Eq 'attacker|deadbeef|/tmp/attacker|\|false\|' <<<"$locked_metadata"; then
	echo 'production AX211 metadata accepted a command-line substitution' >&2
	exit 1
fi

# Package discovery and default configuration are metadata-only and cannot
# select or acquire the firmware.
default_cache=$temporary/default-cache
default_config=$temporary/default.mk
make -C "$repo" --no-print-directory ZEDBSD_CONFIG="$absent_config" \
	INTEL_AX211_FIRMWARE_CACHE_ROOT="$default_cache" \
	list-user-programs >"$temporary/programs"
grep -Fq 'intelax211-firmware|Intel AX211 firmware|amd64|n|firmware|firmware/intelax211|' "$temporary/programs"
ZEDBSD_CONFIG="$absent_config" \
	python3 "$repo/tools/menuconfig.py" --defaults --output "$default_config"
if grep '^ZEDBSD_USER_PROGRAMS' "$default_config" | \
	grep -Fq 'intelax211-firmware'; then
	echo 'AX211 firmware must be default-off' >&2
	exit 1
fi
test ! -e "$default_cache"
default_data=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$default_config" \
	--eval='q062-print-default-data:;@printf "%s\n" "$(ZEDBSD_USERLAND_DATA_INPUTS)"' \
	q062-print-default-data)
if grep -Fq 'iwlwifi-so-a0-gf-a0' <<<"$default_data"; then
	echo 'ordinary default image depends on AX211 firmware' >&2
	exit 1
fi
test ! -e "$default_cache"

# The fixture override exists only for its single hermetic goal.
if make -C "$repo" --no-print-directory ZEDBSD_CONFIG="$absent_config" \
	intelax211-firmware-fixture-cache list-user-programs \
	>"$temporary/mixed-goal.log" 2>&1; then
	echo 'fixture goal unexpectedly accepted a second goal' >&2
	exit 1
fi
grep -Fq 'must be the only requested goal' "$temporary/mixed-goal.log"

revision=q062-fixture-revision
fixture=$temporary/mirror
mkdir -p "$fixture/intel/iwlwifi"
printf 'q062 AX211 firmware fixture\n' \
	>"$fixture/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode"
printf 'q062 AX211 PNVM fixture\n' \
	>"$fixture/intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm"
printf 'q062 Intel firmware license fixture\n' \
	>"$fixture/LICENCE.iwlwifi_firmware"
printf 'q062 WHENCE fixture with version discrepancy retained\n' \
	>"$fixture/WHENCE"

ucode_size=$(wc -c <"$fixture/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode" | tr -d '[:space:]')
pnvm_size=$(wc -c <"$fixture/intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm" | tr -d '[:space:]')
license_size=$(wc -c <"$fixture/LICENCE.iwlwifi_firmware" | tr -d '[:space:]')
whence_size=$(wc -c <"$fixture/WHENCE" | tr -d '[:space:]')
ucode_hash=$(sha256sum "$fixture/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode" | awk '{print $1}')
pnvm_hash=$(sha256sum "$fixture/intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm" | awk '{print $1}')
license_hash=$(sha256sum "$fixture/LICENCE.iwlwifi_firmware" | awk '{print $1}')
whence_hash=$(sha256sum "$fixture/WHENCE" | awk '{print $1}')

run_package() {
	local cache_root=$1
	local fetch_command=$2
	FETCH_LOG=$fetch_log make -C "$repo" --no-print-directory \
		ZEDBSD_CONFIG="$absent_config" \
		INTEL_AX211_FIRMWARE_BASE_URL="file://$fixture" \
		INTEL_AX211_FIRMWARE_REVISION="$revision" \
		INTEL_AX211_FIRMWARE_URL_SUFFIX= \
		INTEL_AX211_FIRMWARE_UCODE_SIZE="$ucode_size" \
		INTEL_AX211_FIRMWARE_UCODE_SHA256="$ucode_hash" \
		INTEL_AX211_FIRMWARE_PNVM_SIZE="$pnvm_size" \
		INTEL_AX211_FIRMWARE_PNVM_SHA256="$pnvm_hash" \
		INTEL_AX211_FIRMWARE_LICENSE_SIZE="$license_size" \
		INTEL_AX211_FIRMWARE_LICENSE_SHA256="$license_hash" \
		INTEL_AX211_FIRMWARE_WHENCE_SIZE="$whence_size" \
		INTEL_AX211_FIRMWARE_WHENCE_SHA256="$whence_hash" \
		INTEL_AX211_FIRMWARE_CACHE_ROOT="$cache_root" \
		INTEL_AX211_FIRMWARE_FETCH="$fetch_command" \
		intelax211-firmware-fixture-cache
}

cache_root=$temporary/cache
: >"$fetch_log"
run_package "$cache_root" "$fetcher"
cache=$cache_root/$revision
test "$(wc -l <"$fetch_log" | tr -d '[:space:]')" = 4
test "$(sha256sum "$cache/iwlwifi-so-a0-gf-a0-89.ucode" | awk '{print $1}')" = "$ucode_hash"
test "$(sha256sum "$cache/iwlwifi-so-a0-gf-a0.pnvm" | awk '{print $1}')" = "$pnvm_hash"
test "$(sha256sum "$cache/LICENCE.iwlwifi_firmware" | awk '{print $1}')" = "$license_hash"
test "$(sha256sum "$cache/WHENCE" | awk '{print $1}')" = "$whence_hash"

# A complete cache is reusable offline.  A missing cache, unsafe path,
# unexpected file, or any independently corrupt object must fail without
# silently replacing existing state.
: >"$fetch_log"
run_package "$cache_root" "$fail_fetcher"
test ! -s "$fetch_log"

missing_root=$temporary/missing
if run_package "$missing_root" "$fail_fetcher" \
	>"$temporary/missing.log" 2>&1; then
	echo 'missing offline AX211 cache unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'unexpected firmware network fetch' "$temporary/missing.log"
test ! -e "$missing_root/$revision"

snapshot=$temporary/snapshot
cp -a "$cache" "$snapshot"
for corrupt_name in \
	iwlwifi-so-a0-gf-a0-89.ucode \
	iwlwifi-so-a0-gf-a0.pnvm \
	LICENCE.iwlwifi_firmware WHENCE; do
	corrupt_root=$temporary/corrupt-$corrupt_name
	mkdir -p "$corrupt_root"
	cp -a "$snapshot" "$corrupt_root/$revision"
	printf 'corrupt\n' >>"$corrupt_root/$revision/$corrupt_name"
	if run_package "$corrupt_root" "$fail_fetcher" \
		>"$temporary/corrupt.log" 2>&1; then
		echo "corrupt AX211 cache unexpectedly accepted: $corrupt_name" >&2
		exit 1
	fi
	grep -Eq 'size mismatch|SHA-256 mismatch' "$temporary/corrupt.log"
done

extra_root=$temporary/extra
mkdir -p "$extra_root"
cp -a "$snapshot" "$extra_root/$revision"
: >"$extra_root/$revision/unexpected"
if run_package "$extra_root" "$fail_fetcher" \
	>"$temporary/extra.log" 2>&1; then
	echo 'AX211 cache with an extra object unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'cache must contain exactly four files' "$temporary/extra.log"

unsafe_root=$temporary/unsafe
mkdir -p "$unsafe_root"
ln -s "$snapshot" "$unsafe_root/$revision"
if run_package "$unsafe_root" "$fail_fetcher" \
	>"$temporary/unsafe.log" 2>&1; then
	echo 'symlink AX211 cache unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'unsafe cache path' "$temporary/unsafe.log"

# A partial fetch is cleaned without publishing the revision directory.
partial_fixture=$temporary/partial-mirror
partial_root=$temporary/partial-cache
mkdir -p "$partial_fixture/intel/iwlwifi"
cp "$fixture/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode" \
	"$partial_fixture/intel/iwlwifi/"
if FETCH_LOG=$fetch_log make -C "$repo" --no-print-directory \
	ZEDBSD_CONFIG="$absent_config" \
	INTEL_AX211_FIRMWARE_BASE_URL="file://$partial_fixture" \
	INTEL_AX211_FIRMWARE_REVISION="$revision" \
	INTEL_AX211_FIRMWARE_URL_SUFFIX= \
	INTEL_AX211_FIRMWARE_UCODE_SIZE="$ucode_size" \
	INTEL_AX211_FIRMWARE_UCODE_SHA256="$ucode_hash" \
	INTEL_AX211_FIRMWARE_PNVM_SIZE="$pnvm_size" \
	INTEL_AX211_FIRMWARE_PNVM_SHA256="$pnvm_hash" \
	INTEL_AX211_FIRMWARE_LICENSE_SIZE="$license_size" \
	INTEL_AX211_FIRMWARE_LICENSE_SHA256="$license_hash" \
	INTEL_AX211_FIRMWARE_WHENCE_SIZE="$whence_size" \
	INTEL_AX211_FIRMWARE_WHENCE_SHA256="$whence_hash" \
	INTEL_AX211_FIRMWARE_CACHE_ROOT="$partial_root" \
	INTEL_AX211_FIRMWARE_FETCH="$fetcher" \
	intelax211-firmware-fixture-cache >"$temporary/partial.log" 2>&1; then
	echo 'partial AX211 acquisition unexpectedly succeeded' >&2
	exit 1
fi
test ! -e "$partial_root/$revision"
test -z "$(find "$partial_root" -mindepth 1 -print -quit)"

# The selected rootfs mapping installs exactly two device files plus the full
# Intel license, full WHENCE, and static provenance manifest.
data_mapping=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$absent_config" ZEDBSD_PLATFORM=amd64 \
	ZEDBSD_ARCHITECTURE=amd64 ZEDBSD_BOARD=pcat ZEDBSD_VARIANT=uefi \
	ZEDBSD_USER_PROGRAMS=intelax211-firmware \
	--eval='q062-print-firmware-data:;@printf "%s\n" "$(INTEL_AX211_FIRMWARE_DATA)"' \
	q062-print-firmware-data)
grep -Fq '/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode=' <<<"$data_mapping"
grep -Fq '/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm=' <<<"$data_mapping"
grep -Fq '/usr/share/licenses/intelax211-firmware/LICENCE.iwlwifi_firmware=' <<<"$data_mapping"
grep -Fq '/usr/share/licenses/intelax211-firmware/WHENCE=' <<<"$data_mapping"
grep -Fq '/usr/share/zedbsd/packages/intelax211-firmware.manifest=' <<<"$data_mapping"
test "$(grep -o -- '/[^ =]*=' <<<"$data_mapping" | wc -l | tr -d '[:space:]')" = 5

echo 'intelax211 firmware package: PASS'
