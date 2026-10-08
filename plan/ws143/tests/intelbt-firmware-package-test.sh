#!/usr/bin/env bash
# The intelbt-firmware package (ws143-p003 i02): locked acquisition identity,
# default-off, hermetic fixture acquisition and cache verification, and the
# installed mapping (six files, eight copies under WHENCE's linked names, the
# licence, WHENCE, and the manifest).  Host only; no network.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=/dev/null
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/intelbt-firmware-test"
temporary=$fresh_dir

for command_name in awk find git grep make mktemp python3 sha256sum wc; do
	command -v "$command_name" >/dev/null || {
		echo "missing host command: $command_name" >&2
		exit 1
	}
done

package_makefile=$repo/userland/firmware/intelbt/Makefile
manifest=$repo/userland/firmware/intelbt/intelbt-firmware.manifest
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

if git -C "$repo" ls-files | grep -E '(^|/)ibt-[0-9a-f-]+\.(sfi|ddc)$' >/dev/null; then
	echo 'Intel Bluetooth firmware bytes must not be tracked' >&2
	exit 1
fi

# The manifest names the official immutable commit and every accepted byte.
grep -Fq 'override INTELBT_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120' "$package_makefile"
grep -Fq 'override INTELBT_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120' "$package_makefile"
grep -Fq 'acquisition-tag=20260410' "$manifest"
grep -Fq 'acquisition-revision=dc85ccedc9c973682fbcf4d628ca61174bcc3120' "$manifest"
grep -Fq 'whence-block=BT_Solar_REL82122_23.50.26053.82122' "$manifest"
grep -Fq 'acquisition-file=intel/ibt-0040-0041.sfi size=720988 sha256=8dcfb3a7c1592c11c3e80505d21ee78dbde3de0c830d8e3e856371c37717b62c' "$manifest"
grep -Fq 'acquisition-file=intel/ibt-0041-0041.sfi size=713448 sha256=2d891022ded1d2d8b208cc5b7632f3098c761ae099024de971e4158dd5681a9c' "$manifest"
grep -Fq 'acquisition-file=intel/ibt-1040-0041.sfi size=720988 sha256=8dcfb3a7c1592c11c3e80505d21ee78dbde3de0c830d8e3e856371c37717b62c' "$manifest"
grep -Fq 'acquisition-license-sha256=5181b0b51efc79d5acb2c9bb92042878fdbad97a92114d4ab5e32e2b5b52fce4' "$manifest"
grep -Fq 'acquisition-whence-sha256=c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527' "$manifest"
grep -Fq 'redistribution=unmodified-binary-only' "$manifest"
test "$(grep -c '^acquisition-file=' "$manifest")" = 6

# The manifest and the Makefile's locked list agree on every size and hash.
while read -r path size hash; do
	name=${path#intel/}
	grep -Fq "$name|$path|$size|$hash" "$package_makefile" || {
		echo "Makefile and manifest disagree on $path" >&2
		exit 1
	}
done < <(sed -n 's/^acquisition-file=\([^ ]*\) size=\([0-9]*\) sha256=\([0-9a-f]*\)$/\1 \2 \3/p' "$manifest")

# Production acquisition identity, accepted bytes, verifier, manifest, and
# rootfs mapping ignore command-line substitutions.
locked_metadata=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$absent_config" ZEDBSD_PLATFORM=amd64 \
	ZEDBSD_ARCHITECTURE=amd64 ZEDBSD_BOARD=pcat ZEDBSD_VARIANT=uefi \
	INTELBT_FIRMWARE_BASE_URL=https://example.invalid/attacker \
	INTELBT_FIRMWARE_REVISION=attacker \
	INTELBT_FIRMWARE_URL_SUFFIX='?id=attacker' \
	INTELBT_FIRMWARE_FILES='x|attacker|1|deadbeef' \
	INTELBT_FIRMWARE_FETCH=false \
	INTELBT_FIRMWARE_SHA256_COMMAND=false \
	INTELBT_FIRMWARE_MANIFEST=/tmp/attacker-manifest \
	INTELBT_FIRMWARE_DATA=/tmp/attacker-data \
	'USERLAND_intelbt-firmware_DATA=/tmp/attacker-userland-data' \
	--eval='intelbt-print-locked-metadata:;@printf "%s\n" "$(INTELBT_FIRMWARE_BASE_URL)|$(INTELBT_FIRMWARE_REVISION)|$(INTELBT_FIRMWARE_URL_SUFFIX)|$(INTELBT_FIRMWARE_FETCH)|$(INTELBT_FIRMWARE_SHA256_COMMAND)|$(INTELBT_FIRMWARE_MANIFEST)|$(strip $(INTELBT_FIRMWARE_FILES))|$(INTELBT_FIRMWARE_DATA)|$(USERLAND_intelbt-firmware_DATA)"' \
	intelbt-print-locked-metadata)
grep -Fq 'https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain|dc85ccedc9c973682fbcf4d628ca61174bcc3120|?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120|curl --fail --location --silent --show-error|sha256sum|userland/firmware/intelbt/intelbt-firmware.manifest|' <<<"$locked_metadata"
grep -Fq 'ibt-0040-0041.sfi|intel/ibt-0040-0041.sfi|720988|8dcfb3a7c1592c11c3e80505d21ee78dbde3de0c830d8e3e856371c37717b62c' <<<"$locked_metadata"
grep -Fq '/lib/firmware/intel/ibt-0040-4150.sfi=' <<<"$locked_metadata"
if grep -Eq 'attacker|deadbeef|\|false\|' <<<"$locked_metadata"; then
	echo 'production intelbt metadata accepted a command-line substitution' >&2
	exit 1
fi

# Package discovery and the default configuration cannot select or acquire it.
default_cache=$temporary/default-cache
default_config=$temporary/default.mk
make -C "$repo" --no-print-directory ZEDBSD_CONFIG="$absent_config" \
	INTELBT_FIRMWARE_CACHE_ROOT="$default_cache" \
	list-user-programs >"$temporary/programs"
grep -Fq 'intelbt-firmware|Intel Bluetooth firmware|amd64|n|firmware|firmware/intelbt|' "$temporary/programs"
ZEDBSD_CONFIG="$absent_config" \
	python3 "$repo/tools/menuconfig.py" --defaults --output "$default_config"
if grep '^ZEDBSD_USER_PROGRAMS' "$default_config" | grep -Fq 'intelbt-firmware'; then
	echo 'intelbt firmware must be default-off' >&2
	exit 1
fi
test ! -e "$default_cache"
default_data=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$default_config" \
	--eval='intelbt-print-default-data:;@printf "%s\n" "$(ZEDBSD_USERLAND_DATA_INPUTS)"' \
	intelbt-print-default-data)
if grep -Eq 'ibt-[0-9]' <<<"$default_data"; then
	echo 'ordinary default image depends on intelbt firmware' >&2
	exit 1
fi
test ! -e "$default_cache"

# The fixture override exists only for its single hermetic goal.
if make -C "$repo" --no-print-directory ZEDBSD_CONFIG="$absent_config" \
	intelbt-firmware-fixture-cache list-user-programs \
	>"$temporary/mixed-goal.log" 2>&1; then
	echo 'fixture goal unexpectedly accepted a second goal' >&2
	exit 1
fi
grep -Fq 'must be the only requested goal' "$temporary/mixed-goal.log"

revision=intelbt-fixture-revision
fixture=$temporary/mirror
mkdir -p "$fixture/intel"
names='ibt-0040-0041.sfi ibt-0040-0041.ddc ibt-0041-0041.sfi ibt-0041-0041.ddc ibt-1040-0041.sfi ibt-1040-0041.ddc'
fixture_files=
for name in $names; do
	printf 'intelbt fixture %s\n' "$name" >"$fixture/intel/$name"
	fixture_files="$fixture_files $name|intel/$name|$(wc -c <"$fixture/intel/$name" | tr -d '[:space:]')|$(sha256sum "$fixture/intel/$name" | awk '{print $1}')"
done
printf 'intelbt licence fixture\n' >"$fixture/LICENCE.ibt_firmware"
printf 'intelbt WHENCE fixture\n' >"$fixture/WHENCE"
for name in LICENCE.ibt_firmware WHENCE; do
	fixture_files="$fixture_files $name|$name|$(wc -c <"$fixture/$name" | tr -d '[:space:]')|$(sha256sum "$fixture/$name" | awk '{print $1}')"
done

run_package() {
	local cache_root=$1
	local fetch_command=$2
	local base=${3:-file://$fixture}
	FETCH_LOG=$fetch_log make -C "$repo" --no-print-directory \
		ZEDBSD_CONFIG="$absent_config" \
		INTELBT_FIRMWARE_BASE_URL="$base" \
		INTELBT_FIRMWARE_REVISION="$revision" \
		INTELBT_FIRMWARE_URL_SUFFIX= \
		INTELBT_FIRMWARE_FILES="$fixture_files" \
		INTELBT_FIRMWARE_CACHE_ROOT="$cache_root" \
		INTELBT_FIRMWARE_FETCH="$fetch_command" \
		intelbt-firmware-fixture-cache
}

cache_root=$temporary/cache
: >"$fetch_log"
run_package "$cache_root" "$fetcher"
cache=$cache_root/$revision
test "$(wc -l <"$fetch_log" | tr -d '[:space:]')" = 8
for name in $names; do
	cmp -s "$cache/$name" "$fixture/intel/$name"
done
cmp -s "$cache/LICENCE.ibt_firmware" "$fixture/LICENCE.ibt_firmware"
cmp -s "$cache/WHENCE" "$fixture/WHENCE"

# A complete cache is reusable offline.  A missing cache, unsafe path,
# unexpected file, or any corrupt object fails without replacing state.
: >"$fetch_log"
run_package "$cache_root" "$fail_fetcher"
test ! -s "$fetch_log"

missing_root=$temporary/missing
if run_package "$missing_root" "$fail_fetcher" >"$temporary/missing.log" 2>&1; then
	echo 'missing offline intelbt cache unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'unexpected firmware network fetch' "$temporary/missing.log"
test ! -e "$missing_root/$revision"

snapshot=$temporary/snapshot
cp -a "$cache" "$snapshot"
for corrupt_name in $names LICENCE.ibt_firmware WHENCE; do
	corrupt_root=$temporary/corrupt-$corrupt_name
	mkdir -p "$corrupt_root"
	cp -a "$snapshot" "$corrupt_root/$revision"
	printf 'corrupt\n' >>"$corrupt_root/$revision/$corrupt_name"
	if run_package "$corrupt_root" "$fail_fetcher" >"$temporary/corrupt.log" 2>&1; then
		echo "corrupt intelbt cache unexpectedly accepted: $corrupt_name" >&2
		exit 1
	fi
	grep -Eq 'size mismatch|SHA-256 mismatch' "$temporary/corrupt.log"
done

# Same size, different bytes: only the hash can catch it.
flip_root=$temporary/flip
mkdir -p "$flip_root"
cp -a "$snapshot" "$flip_root/$revision"
printf 'intelbt fixture ibt-0040-0041.sfX\n' >"$flip_root/$revision/ibt-0040-0041.sfi"
if run_package "$flip_root" "$fail_fetcher" >"$temporary/flip.log" 2>&1; then
	echo 'same-size corrupt intelbt cache unexpectedly accepted' >&2
	exit 1
fi
grep -Fq 'SHA-256 mismatch' "$temporary/flip.log"

extra_root=$temporary/extra
mkdir -p "$extra_root"
cp -a "$snapshot" "$extra_root/$revision"
: >"$extra_root/$revision/unexpected"
if run_package "$extra_root" "$fail_fetcher" >"$temporary/extra.log" 2>&1; then
	echo 'intelbt cache with an extra object unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'cache must contain exactly 8 files' "$temporary/extra.log"

link_root=$temporary/link-file
mkdir -p "$link_root/$revision"
cp -a "$snapshot/." "$link_root/$revision/"
mv "$link_root/$revision/WHENCE" "$link_root/WHENCE.real"
ln -s ../WHENCE.real "$link_root/$revision/WHENCE"
if run_package "$link_root" "$fail_fetcher" >"$temporary/link-file.log" 2>&1; then
	echo 'intelbt cache with a symlinked file unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'missing or unsafe cache file' "$temporary/link-file.log"

unsafe_root=$temporary/unsafe
mkdir -p "$unsafe_root"
ln -s "$snapshot" "$unsafe_root/$revision"
if run_package "$unsafe_root" "$fail_fetcher" >"$temporary/unsafe.log" 2>&1; then
	echo 'symlink intelbt cache unexpectedly succeeded' >&2
	exit 1
fi
grep -Fq 'unsafe cache path' "$temporary/unsafe.log"

# A partial fetch publishes nothing.
partial_fixture=$temporary/partial-mirror
partial_root=$temporary/partial-cache
mkdir -p "$partial_fixture/intel"
cp "$fixture/intel/ibt-0040-0041.sfi" "$partial_fixture/intel/"
if run_package "$partial_root" "$fetcher" "file://$partial_fixture" >"$temporary/partial.log" 2>&1; then
	echo 'partial intelbt acquisition unexpectedly succeeded' >&2
	exit 1
fi
test ! -e "$partial_root/$revision"
test -z "$(find "$partial_root" -mindepth 1 -print -quit)"

# The selected rootfs mapping: six files, eight linked names as copies of
# the file WHENCE links them to, the licence, WHENCE, and the manifest.
data_mapping=$(make -C "$repo" --no-print-directory -s \
	ZEDBSD_CONFIG="$absent_config" ZEDBSD_PLATFORM=amd64 \
	ZEDBSD_ARCHITECTURE=amd64 ZEDBSD_BOARD=pcat ZEDBSD_VARIANT=uefi \
	ZEDBSD_USER_PROGRAMS=intelbt-firmware \
	--eval='intelbt-print-firmware-data:;@printf "%s\n" "$(INTELBT_FIRMWARE_DATA)"' \
	intelbt-print-firmware-data)
test "$(grep -o -- '/[^ =]*=' <<<"$data_mapping" | wc -l | tr -d '[:space:]')" = 17
for name in $names; do
	grep -Fq "/lib/firmware/intel/$name=" <<<"$data_mapping"
done
whence=
for candidate in intelbt intelax211; do
	candidate=$repo/build/sources/firmware/$candidate/dc85ccedc9c973682fbcf4d628ca61174bcc3120/WHENCE
	if test -f "$candidate"; then
		whence=$candidate
		break
	fi
done
expected_links=$temporary/links.expected
cat >"$expected_links" <<'EOF'
ibt-0040-4150.sfi ibt-0040-0041.sfi
ibt-0040-4150.ddc ibt-0040-0041.ddc
ibt-1040-4150.sfi ibt-1040-0041.sfi
ibt-1040-4150.ddc ibt-1040-0041.ddc
ibt-0040-1050.sfi ibt-0040-0041.sfi
ibt-0040-1050.ddc ibt-0040-0041.ddc
ibt-1040-1050.sfi ibt-1040-0041.sfi
ibt-1040-1050.ddc ibt-1040-0041.ddc
EOF
while read -r link target; do
	tr ' ' '\n' <<<"$data_mapping" | grep -Eq "^/lib/firmware/intel/$link=.*/$target\$" || {
		echo "linked name $link is not a copy of $target" >&2
		exit 1
	}
done <"$expected_links"
grep -Fq '/usr/share/licenses/intelbt-firmware/LICENCE.ibt_firmware=' <<<"$data_mapping"
grep -Fq '/usr/share/licenses/intelbt-firmware/WHENCE=' <<<"$data_mapping"
grep -Fq '/usr/share/zedbsd/packages/intelbt-firmware.manifest=' <<<"$data_mapping"

# When a verified WHENCE of this commit (this package's or the AX211
# package's cache) is at hand, its Solar block is exactly what is installed.
if test -n "$whence"; then
	test "$(sha256sum "$whence" | awk '{print $1}')" = c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
	block=$temporary/solar.block
	awk '/^File: intel\/ibt-0041-0041.sfi$/ {on = 1} on {print} /^Version: BT_Solar_REL82122/ {exit}' "$whence" >"$block"
	test "$(grep -c '^File: ' "$block")" = 6
	test "$(grep -c '^Link: ' "$block")" = 8
	for name in $names; do
		grep -Fqx "File: intel/$name" "$block"
	done
	while read -r link target; do
		grep -Fqx "Link: intel/$link -> $target" "$block"
	done <"$expected_links"
	echo 'intelbt firmware package: WHENCE Solar block matches'
else
	echo 'intelbt firmware package: WHENCE cross-check skipped (no verified cache)'
fi

echo 'intelbt firmware package: PASS'
