#!/usr/bin/env bash
# POSIX R2 remaining-work integration test. Copyright (C) 2026 Awe Morris.
# SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export ZEDBSD_POSIX_TEST_ELF=POSIX-R2-REMAINING.ELF
export ZEDBSD_POSIX_TEST_MARKER=R2R:01-08:PASS
export ZEDBSD_POSIX_TEST_FAILURE=POSIX_R2R_FAIL:
export ZEDBSD_POSIX_TEST_LABEL="POSIX R2 remaining"
exec "$repo/scripts/test-posix-r2.sh" "${1:-all}"
