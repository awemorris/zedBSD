#!/usr/bin/env bash
set -euo pipefail

# Reuse the G2a drawing workload on a PC-9821.  The automatic display HAL
# must select Core-Graph and expose its 640x480 Cirrus surface.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
ZEDBSD_BEUI_MACHINE=pc9821 \
ZEDBSD_BEUI_CPU=486 \
ZEDBSD_BEUI_MEMORY=64M \
ZEDBSD_BEUI_EXPECT_HEIGHT=480 \
ZEDBSD_BEUI_BACKEND_NAME=Core-Graph/Cirrus \
ZEDBSD_BEUI_TEST_TAG=cirrus \
	"$repo/scripts/test-beui-gdc.sh"
