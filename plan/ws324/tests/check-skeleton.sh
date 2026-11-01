#!/bin/sh
# The shape checks of libmedia's H.265 parse (h265sw-*, h265-dpb, h265-stream, h265-std):
#  1. every h265sw*.h and h265-*.h compiles alone (included twice, so its guard works), with cc and clang,
#     -std=c89 -Wall -Wextra -Werror -Wpedantic (<vulkan/vulkan.h> the host's);
#  2. every h265sw-*.c and h265-*.c compiles with both (same flags), and, when build/llvm exists, against the zedBSD
#     C library headers with the shared clang for x86_64, aarch64 and riscv64 zedBSD (syntax only); h265-std.h and
#     h265-std.c need the tree's Vulkan H.265 headers there and are skipped while they are missing;
#  3. layering: no h265sw file includes an h265- header or a Vulkan header; h265-dpb and h265-stream include no
#     Vulkan header (only h265-std does);
#  4. every function an h265 header declares is defined in a .c, and the cc objects define no external name outside
#     h265sw_ and h265_ (nm);
#  5. the parse is not part of libmedia's build: sources.mk and the Makefile name no h265 file;
#  6. with BASE=<commit>: the headers and the test tools (run-unit.sh, check-skeleton.sh, unit/h265write.h,
#     unit/test-h265write.c, and plan/tools/h264sw/unit/check.h and bitwriter.h they use) are unchanged since that
#     commit, none added or removed (an implementation Phase must not change them; a fault goes back to Q1);
#  7. the shared vcodec*.h are unchanged since the H.264 decoder's base 77ed48ac4 (the parse does not change them).
#   sh plan/ws324/tests/check-skeleton.sh
#   BASE=<sha> sh plan/ws324/tests/check-skeleton.sh
# Last line: check-skeleton: PASS or check-skeleton: FAIL.  Output under build/tmp/h265-skeleton (a fresh run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/h265-skeleton
fresh_out "$out"
lib=userland/desktop/libmedia
flags="-std=c89 -Wall -Wextra -Werror -Wpedantic -D_DEFAULT_SOURCE -I."
headers="$lib/h265sw*.h $lib/h265-*.h"
sources="$lib/h265sw-*.c $lib/h265-*.c"
vcodec_base=77ed48ac4
status=0

# Step 1 and 2 on the host's compilers.
for compiler in cc clang; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "$compiler: skipped (not installed)"
		continue
	fi
	for header in $headers; do
		printf '#include "%s"\n#include "%s"\nint skeleton_header_check;\n' "$header" "$header" > "$out/one.c"
		if ! $compiler $flags -fsyntax-only "$out/one.c" > "$out/header.log" 2>&1; then
			echo "FAILED $compiler $header"
			head -10 "$out/header.log"
			status=1
		fi
	done
	for source in $sources; do
		if ! $compiler $flags -c "$source" -o "$out/$compiler-$(basename "$source" .c).o" > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done

# Step 2 for zedBSD.
zclang=build/llvm/bin/clang
if [ -x "$zclang" ]; then
	vulkan_h265=include/libc/vulkan/vk_video/vulkan_video_codec_h265std_decode.h
	for target in x86_64-unknown-zedbsd aarch64-unknown-zedbsd riscv64-unknown-zedbsd; do
		for file in $headers $sources; do
			case "$file" in
			*/h265-std.h|*/h265-std.c)
				[ -f "$vulkan_h265" ] || continue ;;
			esac
			case "$file" in
			*.h) printf '#include "%s"\n#include "%s"\nint skeleton_header_check;\n' "$file" "$file" > "$out/zone.c"
			    input=$out/zone.c ;;
			*) input=$file ;;
			esac
			if ! "$zclang" --target=$target -nostdinc -I. -Iinclude -isystem include/libc $flags -fsyntax-only \
			    "$input" > "$out/zedbsd.log" 2>&1; then
				echo "FAILED $target $file"
				head -10 "$out/zedbsd.log"
				status=1
			fi
		done
	done
	[ -f "$vulkan_h265" ] || echo "zedBSD: h265-std skipped (no $vulkan_h265 in the tree yet)"
else
	echo "zedBSD clang: skipped (no $zclang)"
fi

# Step 3: the include lines only (the comments name Vulkan freely).
if grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](h265-|vulkan|vk_video)' $lib/h265sw*.h $lib/h265sw-*.c; then
	echo "FAILED an h265sw file includes an h265- or Vulkan header (above)"
	status=1
fi
if grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](vulkan|vk_video)|^[[:space:]]*#[[:space:]]*include[[:space:]]*"h265-std\.h"' \
    $lib/h265-dpb.h $lib/h265-dpb.c $lib/h265-stream.h $lib/h265-stream.c; then
	echo "FAILED h265-dpb or h265-stream includes a Vulkan header or h265-std.h (above)"
	status=1
fi

# Step 4: every declared function has a definition; the objects keep to the namespace.
grep -h -o -E '^(int|void|unsigned) h265[a-z0-9_]*\(' $headers | sed -E 's/^[a-z]+ //; s/\($//' | sort -u > "$out/declared.txt"
grep -h -o -E '^h265[a-z0-9_]*\($' $sources | sed -E 's/\($//' | sort -u > "$out/defined.txt"
missing=$(comm -23 "$out/declared.txt" "$out/defined.txt")
if [ ! -s "$out/declared.txt" ]; then
	echo "FAILED no declaration found in the headers (the pattern no longer matches them)"
	status=1
elif [ -n "$missing" ]; then
	echo "FAILED declared but not defined:"
	echo "$missing"
	status=1
fi
if ls "$out"/cc-*.o > /dev/null 2>&1; then
	if nm -g --defined-only "$out"/cc-*.o > "$out/names.txt" 2> "$out/names.log"; then
		awk 'NF >= 3 { print $3 }' "$out/names.txt" | sort -u > "$out/names.list"
		if ! grep -q '^h265sw_' "$out/names.list" || ! grep -q '^h265_' "$out/names.list"; then
			echo "FAILED nm shows no h265sw_ or h265_ name (output not understood)"
			status=1
		fi
		bad=$(grep -v -E '^(h265sw_|h265_)' "$out/names.list")
		if [ -n "$bad" ]; then
			echo "FAILED external names outside h265sw_ and h265_:"
			echo "$bad" | head -20
			status=1
		fi
	else
		echo "FAILED nm on the cc objects"
		status=1
	fi
fi

# Step 5: not in libmedia's build.
if grep -n 'h265' $lib/sources.mk $lib/Makefile; then
	echo "FAILED libmedia's build names an h265 file (above)"
	status=1
fi

# Step 6: the frozen headers and tools, as quoted pathspecs so an added or removed file is seen too.
if [ -n "${BASE:-}" ]; then
	frozen="userland/desktop/libmedia/h265*.h plan/ws324/tests/run-unit.sh plan/ws324/tests/check-skeleton.sh
plan/ws324/tests/unit/h265write.h plan/ws324/tests/unit/test-h265write.c plan/tools/h264sw/unit/check.h
plan/tools/h264sw/unit/bitwriter.h"
	set -f
	if ! git diff --quiet "$BASE" -- $frozen; then
		echo "FAILED headers or test tools changed since $BASE:"
		git diff --stat "$BASE" -- $frozen
		status=1
	fi
	set +f
fi

# Step 7: the shared vcodec headers.
if ! git diff --quiet "$vcodec_base" -- 'userland/desktop/libmedia/vcodec*.h'; then
	echo "FAILED vcodec headers changed since $vcodec_base:"
	git diff --stat "$vcodec_base" -- 'userland/desktop/libmedia/vcodec*.h'
	status=1
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"
