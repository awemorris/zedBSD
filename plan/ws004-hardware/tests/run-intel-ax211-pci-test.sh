#!/bin/sh
set -eu

repository=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ax211-pci.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
includes="-I$repository/include -I$repository/include/uapi"
fixture="$repository/plan/ws004-hardware/tests/intel-ax211-pci-test.c"

# Exercises the exact identity, BAR/CSR inspection, and every unwind stage.
# shellcheck disable=SC2086
$compiler $warnings $includes "$fixture" -o "$temporary/ordinary"
"$temporary/ordinary"

# Checks the same ownership paths for host memory and arithmetic defects.
# shellcheck disable=SC2086
$compiler $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined $includes "$fixture" \
	-o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=1 "$temporary/sanitize"

# Applies GCC's interprocedural ownership analysis to the production unit.
# shellcheck disable=SC2086
$compiler $warnings -O0 -fanalyzer $includes "$fixture" \
	-o "$temporary/analyzer"
"$temporary/analyzer"

echo 'intel ax211 pci: ordinary, sanitizer, analyzer PASS'
