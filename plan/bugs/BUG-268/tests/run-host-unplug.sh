#!/bin/sh
# BUG-268: host test of the window's end at the unplug of the moved output's display (display/present.c), ASan/UBSan.
#   sh plan/bugs/BUG-268/tests/run-host-unplug.sh [OUT]   (default build/bug268/unplug)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../../.."
. plan/ws084/tests/display-host-lib.sh
OUT=${1:-build/bug268/unplug}
mkdir -p "$(dirname "$OUT")"
I915_HOST_EXTRA_PRODUCTION="src/drivers/gpu/i915/perf.c ${I915_HOST_EXTRA_PRODUCTION:-}"
i915_display_host_build "$OUT" plan/bugs/BUG-268/tests/host-unplug.c plan/bugs/BUG-268/tests/unplug-stubs.c
"$OUT"
