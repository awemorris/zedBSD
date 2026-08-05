#!/usr/bin/env bash
set -euo pipefail

# Reuse the G2a drawing workload on a PC-9821.  The automatic display HAL
# must select Core-Graph and expose its 640x480 Cirrus surface.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${BOOTS_ARCH:-pc98}"
build="${BOOTS_BUILD_DIR:-$repo/build/$arch}"
BOOTS_BEUI_MACHINE=pc9821 \
BOOTS_BEUI_EXPECT_HEIGHT=480 \
BOOTS_BEUI_BACKEND_NAME=Core-Graph/Cirrus \
BOOTS_BEUI_TEST_TAG=cirrus \
	"$repo/scripts/test-beui-gdc.sh"
