#!/bin/sh
# Builds and runs plan/tools/browser-win32/scheme-test.c: the color scheme a libbrowser view's pages are shown in
# (browser_view_set_color_scheme, the prefers-color-scheme media queries and matchMedia).  The engine's
# objects come from the WS074 host build (plan/ws074/tests/host-build.sh VARIANT, run here first).
#   sh plan/tools/browser-win32/scheme-test.sh [plain|asan]
# The output goes to a new directory of build/tmp/browser-win32-scheme (fresh_out).
# Last line: scheme-test: PASS or scheme-test: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
variant=${1:-plain}
case $variant in
plain|asan) ;;
*) echo "scheme-test: unknown variant $variant" >&2; exit 2 ;;
esac
out=build/tmp/browser-win32-scheme
fresh_out "$out"
run=$(cd "$out" && pwd -P)

# The engine, built by the WS074 host build.
if ! timeout 3600 sh plan/ws074/tests/host-build.sh "$variant" > "$run/host-build.log" 2>&1; then
	tail -20 "$run/host-build.log"
	echo "scheme-test: FAIL (the host build failed, log $run/host-build.log)"
	exit 1
fi
host=build/ws074-host/$variant
flags=$(cat "$host/flags" | cut -d' ' -f2-)
engine=$(ls "$host"/obj/*.o | grep -v -e '/browser_main\.o$' -e '/browser_main-posix\.o$' -e '/host-shell\.o$' -e '/browser-os-error\.o$')
if ! timeout 600 ${CC:-cc} $flags -Iplan/tools/browser-win32/unit -o "$run/scheme-test" plan/tools/browser-win32/scheme-test.c \
    $engine -lvulkan -lm -ldl -lpthread > "$run/compile.log" 2>&1 || [ -s "$run/compile.log" ]; then
	cat "$run/compile.log"
	echo "scheme-test: FAIL (the test did not compile cleanly)"
	exit 1
fi

# The page: the body's color in each scheme, and what the script sees.
cat > "$run/page.html" <<'PAGE'
<!DOCTYPE html><html><head><style>
body { color: #102030; background: #ffffff; }
@media (prefers-color-scheme: dark) { body { color: #e0e0f0; background: #101010; } }
</style></head><body><p>scheme</p><script>
console.log("dark=" + matchMedia("(prefers-color-scheme: dark)").matches +
    " light=" + matchMedia("(prefers-color-scheme: light)").matches +
    " color=" + getComputedStyle(document.body).color);
</script></body></html>
PAGE

# The test.
ASAN_OPTIONS=detect_stack_use_after_return=0 timeout 300 "$run/scheme-test" "$run/page.html" > "$run/test.log" 2>&1
code=$?
sync
cat "$run/test.log"
if [ "$code" = 0 ] && tail -1 "$run/test.log" | grep -q '^test-scheme: PASS'; then
	echo "scheme-test: PASS"
else
	echo "scheme-test: FAIL (exit $code, log $run/test.log)"
	exit 1
fi
