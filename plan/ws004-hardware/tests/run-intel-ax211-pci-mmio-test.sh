#!/bin/sh
set -eu

repository=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ax211-pci-mmio.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc=${CC:-cc}
flags="-std=c11 -D_POSIX_C_SOURCE=200809L -DINTEL_AX211_PCI_MMIO_HOST_TEST -Wall -Wextra -Werror"
production_flags="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror"
includes="-I$repository/include -I$repository/src/drivers"
source="$repository/src/drivers/intel-ax211-pci-mmio.c"
fixture="$repository/plan/ws004-hardware/tests/intel-ax211-pci-mmio-test.c"

$cc $flags -O2 $includes "$source" "$fixture" -o "$build_dir/ordinary"
"$build_dir/ordinary"
$cc $flags -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$includes "$source" "$fixture" -o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/sanitize"
$cc $flags -O0 -fanalyzer $includes "$source" "$fixture" \
	-o "$build_dir/analyzer"
"$build_dir/analyzer"
$cc $flags -D__amd64__ -fsyntax-only $includes "$source" "$fixture"
$cc $flags -D__i386__ -fsyntax-only $includes "$source" "$fixture"
$cc $production_flags -D__amd64__ -fsyntax-only $includes "$source"

echo "intel ax211 PCI MMIO: ordinary, ASan/UBSan, analyzer, host amd64/i386 and production amd64 syntax PASS"
