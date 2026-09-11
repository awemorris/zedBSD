#!/bin/sh
# WS016-p001 host verification gate (SWAP-T001--T006).
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

"$test_dir/run-swap-manager-test.sh"
"$test_dir/run-backing-claim-test.sh"
"$test_dir/run-swap-drain-test.sh"
"$test_dir/run-swap-commit-resize-test.sh"
