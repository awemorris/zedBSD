#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
noct=${NOCT:-$repo_dir/build/NoctLang/build-static/noct}
checker=$repo_dir/tools/build/check-rtl8822b-tables.noct
imported=$repo_dir/src/drivers/rtl8822b-tables.inc
binary_license=$repo_dir/userland/base/licenses/rtl8822b-tables/LICENSE
commit=0b8db87da54178717d302ca5dc09285ad4922abc
base=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain

cleanup_dir=
cleanup()
{
	if [ -n "$cleanup_dir" ] && [ -d "$cleanup_dir" ]; then
		find "$cleanup_dir" -type f -delete
		rmdir "$cleanup_dir"
	fi
}
trap cleanup EXIT HUP INT TERM

if [ -n "${RTL8822B_TABLE_SOURCE:-}" ] ||
    [ -n "${RTL8822B_CHIP_SOURCE:-}" ] ||
    [ -n "${RTL8822B_LICENSE_SOURCE:-}" ]; then
	: "${RTL8822B_TABLE_SOURCE:?all three source overrides are required}"
	: "${RTL8822B_CHIP_SOURCE:?all three source overrides are required}"
	: "${RTL8822B_LICENSE_SOURCE:?all three source overrides are required}"
	table_source=$RTL8822B_TABLE_SOURCE
	chip_source=$RTL8822B_CHIP_SOURCE
	license_source=$RTL8822B_LICENSE_SOURCE
else
	cleanup_dir=$(mktemp -d)
	table_source=$cleanup_dir/rtw8822b_table.c
	chip_source=$cleanup_dir/rtw8822b.c
	license_source=$cleanup_dir/BSD-3-Clause
	curl -LfsS "$base/drivers/net/wireless/realtek/rtw88/rtw8822b_table.c?id=$commit" \
	    -o "$table_source"
	curl -LfsS "$base/drivers/net/wireless/realtek/rtw88/rtw8822b.c?id=$commit" \
	    -o "$chip_source"
	curl -LfsS "$base/LICENSES/preferred/BSD-3-Clause?id=$commit" \
	    -o "$license_source"
fi

"$noct" --path="$repo_dir/tools/build" "$checker" \
    --source-table "$table_source" \
    --source-chip "$chip_source" \
    --source-license "$license_source" \
    --import "$imported" \
    --binary-license "$binary_license"

selected_license=$(make -s -C "$repo_dir" \
    CONFIG_DRIVER_USB_RTL8822BU=y \
    --eval='q057-print-table-license:;@printf "%s\n" "$(filter rtl8822b-tables-license,$(ZEDBSD_USER_PROGRAMS))|$(USERLAND_rtl8822b-tables-license_DATA)"' \
    q057-print-table-license)
expected_license='rtl8822b-tables-license|/usr/share/licenses/rtl8822b-tables/LICENSE=userland/base/licenses/rtl8822b-tables/LICENSE'
if [ "$selected_license" != "$expected_license" ]; then
	echo "RTL8822B table notice is not mandatory in driver-enabled images" >&2
	exit 1
fi
