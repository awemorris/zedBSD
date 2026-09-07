#!/bin/sh
# Historical entry point; the unified formatter owns this gate.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
exec sh "$repo/plan/ws024-unified-ufs/tests/run-formatter-fault-host.sh"
