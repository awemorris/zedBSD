#!/bin/sh
# BUG-266: host test of the run log's forwarding of the Type-C hooks (display/diagnostics.c), ASan/UBSan.
#   sh plan/bugs/BUG-266/tests/run-host-trace-tc.sh [OUT]   (default build/bug266/trace-tc)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../../.."
. plan/ws084/tests/display-host-lib.sh
OUT=${1:-build/bug266/trace-tc}
mkdir -p "$(dirname "$OUT")"
I915_HOST_EXTRA_PRODUCTION="src/drivers/gpu/i915/perf.c ${I915_HOST_EXTRA_PRODUCTION:-}"
i915_display_host_build "$OUT" plan/bugs/BUG-266/tests/host-trace-tc.c plan/ws084/tests/native-decide-stubs.c
"$OUT"
