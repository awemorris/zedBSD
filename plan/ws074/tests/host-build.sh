#!/bin/sh
# ws074: builds browser's engine and its host tests with the host's C compiler.
#
#   sh plan/ws074/tests/host-build.sh [plain|asan]     (default plain)
#
# Every engine source listed in userland/desktop/libbrowser/Makefile and main.c from
# userland/desktop/browser/Makefile are built, not the shell/ directory (the zdesktop window);
# plan/ws074/tests/host-shell.c stands in for it.
# The GPU renderer (paint/vulkan.c) links the host's libvulkan (Debian's libvulkan-dev; lavapipe
# draws when there is no GPU).
# The outputs, in build/ws074-host/<variant>/:
#   browser   the program with its headless modes (the same main.c as on zedBSD)
#   browser-probe      the second program over the engine (userland/tests/browser-probe)
#   host-NAME          each plan/ws074/tests/host-NAME.c unit test, linked with the engine
# BROWSER_HOST_BUILD selects a separate output root (default build/ws074-host).
# The asan variant adds -fsanitize=address,undefined; the runners use it to find crashes.  Run it with
# ASAN_OPTIONS=detect_stack_use_after_return=0: the collector scans the real stack, and the sanitizer's
# separate stacks for address-taken locals would hide cells from it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
base=${BROWSER_HOST_BUILD:-build/ws074-host}
out=$base/$variant
src=userland/desktop/libbrowser
app=userland/desktop/browser
cc=${CC:-cc}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fPIC -I$src -I$app -Iplan/ws074/tests -I$base/include -I."
case $variant in
plain) ;;
asan) flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
*) echo "host-build: unknown variant $variant" >&2; exit 2 ;;
esac
mkdir -p "$out/obj"
# A changed compiler or flags invalidate cached objects without deleting shared output.
# (Compared in the shell, not through a scratch file, so that nothing is removed: 2026-10-06 user, deleting is Q1's.)
if [ ! -f "$out/flags" ] || [ "$(cat "$out/flags")" != "$cc $flags" ]; then
	printf '%s\n' "$cc $flags" > "$out/flags"
fi

# The engine's sources, from the package's list (the window's directory stays out).
sources=$(sh plan/ws074/tests/list-sources.sh | grep -v '/shell/')
objects=""
engine=""

# The base libraries the engine links on zedBSD, built from their sources (their public headers
# are linked into $base/include, since the host's C library does not have them).
mkdir -p $base/include
mkdir -p $base/include/truetype
ln -sf "$(pwd)/userland/desktop/include/truetype/truetype.h" $base/include/truetype/truetype.h
# The engine's own public header (libbrowser, ws074-p057), which the host's C library lacks too.
mkdir -p $base/include/browser
ln -sf "$(pwd)/userland/desktop/include/browser/browser.h" $base/include/browser/browser.h
for file in userland/desktop/libtruetype/face.c userland/desktop/libtruetype/cmap.c userland/desktop/libtruetype/outline.c \
    userland/desktop/libtruetype/render.c userland/desktop/libtruetype/glyph.c userland/desktop/libtruetype/design.c userland/desktop/libtruetype/companion.c; do
	object=$out/obj/truetype-$(basename "$file" .c).o
	if [ ! -f "$object" ] || [ "$file" -nt "$object" ] || [ "$out/flags" -nt "$object" ]; then
		"$cc" $flags -Wno-error -Iuserland/desktop/libtruetype -c "$file" -o "$object"
	fi
	engine="$engine $object"
	objects="$objects $object"
done

# The image libraries (ws074-p021): libjpeg-compat, libpng-compat with libz-compat, libgif-compat.
ln -sfn "$(pwd)/include/libc/compat" $base/include/compat
for file in userland/base/libjpeg-compat/*.c userland/base/libpng-compat/*.c userland/base/libz-compat/*.c \
    userland/base/libgif-compat/*.c; do
	library=$(basename "$(dirname "$file")")
	object=$out/obj/$library-$(basename "$file" .c).o
	if [ ! -f "$object" ] || [ "$file" -nt "$object" ] || [ "$out/flags" -nt "$object" ]; then
		"$cc" $flags -Wno-error -I"$(dirname "$file")" -c "$file" -o "$object"
	fi
	engine="$engine $object"
	objects="$objects $object"
done

# libmedia (ws121-p002), which the engine's <video> and <audio> play through: its engine, the container reader and
# Video Player's decoding add-in (FFmpeg opened with dlopen); it plays no sound (WS191).
for file in userland/desktop/libmedia/engine.c userland/desktop/videoplayer/codec.c userland/desktop/videoplayer/bitstream.c \
    userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c \
    userland/desktop/mediafile/mkv.c; do
	object=$out/obj/media-$(basename "$(dirname "$file")")-$(basename "$file" .c).o
	if [ ! -f "$object" ] || [ "$file" -nt "$object" ] || [ "$out/flags" -nt "$object" ]; then
		"$cc" $flags -c "$file" -o "$object"
	fi
	engine="$engine $object"
	objects="$objects $object"
done

for file in $sources; do
	object=$out/obj/$(printf '%s' "${file#userland/desktop/}" | tr '/' '_' | sed 's/\.c$/.o/')
	if [ ! -f "$object" ] || [ "$file" -nt "$object" ] || [ "$out/flags" -nt "$object" ] || [ -n "$(find "$src" "$app" userland/desktop/include -name '*.h' -newer "$object" | head -1)" ]; then
		"$cc" $flags -c "$file" -o "$object"
	fi
	objects="$objects $object"
	case $file in
	*/main.c) ;;
	*) engine="$engine $object" ;;
	esac
done

# The stand-in for the window.
"$cc" $flags -c plan/ws074/tests/host-shell.c -o "$out/obj/host-shell.o"
# The host component has the same public exports as the production library.
"$cc" $flags -shared -Wl,-z,defs -Wl,-soname,libbrowser.so \
    -Wl,--version-script=userland/desktop/libbrowser/exports.map -o "$out/libbrowser.so" $engine -lvulkan -lm -ldl -lpthread
"$cc" $flags -o "$out/browser" "$out/obj/browser_main.o" "$out/obj/host-shell.o" \
    -L"$out" -Wl,-rpath,'$ORIGIN' -l:libbrowser.so -lvulkan -lm
echo "built $out/browser"

# The second program over the engine (ws074-p057), built from <browser/browser.h> only (on the host it links the
# engine's shared library, as on the target).
"$cc" $flags -I$base/include -o "$out/browser-probe" userland/tests/browser-probe/main.c -L"$out" -Wl,-rpath,'$ORIGIN' -l:libbrowser.so -lm
echo "built $out/browser-probe"

# The unit tests.
for test in plan/ws074/tests/host-*.c; do
	name=$(basename "$test" .c)
	[ "$name" = host-shell ] && continue
	"$cc" $flags -o "$out/$name" "$test" $engine "$out/obj/host-shell.o" -lvulkan -lm -ldl -lpthread
	echo "built $out/$name"
done
