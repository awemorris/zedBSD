#!/bin/sh
set -eu

repository=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ax211-pci.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${CC:-cc}
warnings="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror"
includes="-I$repository/plan/ws004-hardware/tests/host-include -I$repository/include -I$repository/include/uapi"
fixture="$repository/plan/ws004-hardware/tests/intel-ax211-pci-test.c"
private_sources="$repository/src/drivers/intel-ax211-assoc.c \
$repository/src/drivers/intel-ax211-bss.c \
$repository/src/drivers/intel-ax211-key.c \
$repository/src/drivers/intel-ax211-tx.c \
$repository/src/drivers/intel-ax211-tx-ring.c"

# Exercises persistent attach, post-refresh publication, and checked teardown.
# shellcheck disable=SC2086
$compiler $warnings $includes "$fixture" $private_sources \
	-o "$temporary/ordinary"
"$temporary/ordinary"

# Checks the same ownership paths for host memory and arithmetic defects.
# shellcheck disable=SC2086
$compiler $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined $includes "$fixture" \
	$private_sources \
	-o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=1 "$temporary/sanitize"

# Applies GCC's interprocedural ownership analysis to the complete fixture.
# shellcheck disable=SC2086
$compiler $warnings -O0 -fanalyzer $includes "$fixture" $private_sources \
	-o "$temporary/analyzer"
"$temporary/analyzer"

# Checks the standalone production translation unit against both ABI widths.
target_includes="-nostdinc -I$repository/libc/include -I$repository/include/uapi -I$repository/include -I$repository/src"
# shellcheck disable=SC2086
$compiler $warnings $target_includes -m64 -fsyntax-only \
	"$repository/src/drivers/pci-intel-ax211.c"
# shellcheck disable=SC2086
$compiler $warnings $target_includes -m32 -fsyntax-only \
	"$repository/src/drivers/pci-intel-ax211.c"

echo 'intel ax211 pci: ordinary, sanitizer, analyzer, amd64/i386 syntax PASS'
