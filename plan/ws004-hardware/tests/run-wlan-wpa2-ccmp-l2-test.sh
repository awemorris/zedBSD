#!/bin/sh
# HW-T33 WPA2-Personal/CCMP L2 aggregate verification gate.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
temporary_root=${TMPDIR:-"$repo_root/build/q058-tmp"}

mkdir -p "$temporary_root"
TMPDIR=$(CDPATH= cd -- "$temporary_root" && pwd)
export TMPDIR

# Keep this list fail-fast and explicit: each component owns its ordinary,
# sanitizer, analyzer, and ABI checks, where applicable.
"$test_dir/run-wlan-crypto-test.sh"
"$test_dir/run-wlan-wpa2-codec-test.sh"
"$test_dir/run-wlan-wpa2-engine-test.sh"
"$test_dir/run-wlan-l2-test.sh"
"$test_dir/run-rtl8822b-security-test.sh"
"$test_dir/run-wlan-ccmp-reference-test.sh"
"$test_dir/run-wlan-common-core-test.sh"
"$test_dir/run-usb-rtl8822bu-driver-test.sh"

echo 'HW-T33 WPA2-Personal/CCMP L2 aggregate: PASS'
