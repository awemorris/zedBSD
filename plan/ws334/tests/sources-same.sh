#!/bin/sh
# The source lists moved into sources.mk (libbrowser, libmedia, libtruetype, the compat libraries,
# libkeiland, the browser app) expand to the same words as before the move: each Makefile of the zedBSD,
# Linux and FreeBSD builds is read by GNU make in a copy of its directory (with an empty package.mk), once
# from BASE and once from the working tree, and the lists are printed with $(info).  The one intended
# difference: libkeiland/Makefile.linux now reads the zedBSD list, so its order follows it (the same
# files).  Also: plan/ws074/tests/list-sources.sh prints the same lines from both trees.
#   sh plan/ws334/tests/sources-same.sh
#   BASE=<commit> sh plan/ws334/tests/sources-same.sh
# BASE defaults to the parent of the commit that added libbrowser/sources.mk (HEAD before it is committed).
# Last line: sources-same: PASS or sources-same: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws334-sources-same
fresh_out "$out"
if [ -z "${BASE:-}" ]; then
	added=$(git log -1 --format=%H --diff-filter=A -- userland/desktop/libbrowser/sources.mk)
	if [ -n "$added" ]; then BASE=$added^; else BASE=HEAD; fi
fi
echo "base: $(git rev-parse --short "$BASE")"
status=0

# <makefile> <variables>: the lists each Makefile defines.
checks='userland/desktop/libbrowser/Makefile LIBBROWSER_DIR,LIBBROWSER_SOURCES,LIBBROWSER_IMAGE_DATA
userland/desktop/libmedia/Makefile LIBMEDIA_SOURCES,LIBMEDIA_IMAGE_DATA
userland/desktop/libtruetype/Makefile LIBTRUETYPE_SOURCES,LIBTRUETYPE_IMAGE_DATA
userland/desktop/libtruetype/Makefile.linux KEILAND_LINUX_TRUETYPE_SOURCES
userland/desktop/libtruetype/Makefile.freebsd KEILAND_FREEBSD_TRUETYPE_SOURCES
userland/desktop/libpng-compat/Makefile LIBPNG_COMPAT_SOURCES,LIBPNG_COMPAT_IMAGE_DATA
userland/desktop/libpng-compat/Makefile.linux KEILAND_LINUX_PNG_COMPAT_SOURCES
userland/desktop/libpng-compat/Makefile.freebsd KEILAND_FREEBSD_PNG_COMPAT_SOURCES
userland/desktop/libjpeg-compat/Makefile LIBJPEG_COMPAT_SOURCES,LIBJPEG_COMPAT_IMAGE_DATA
userland/desktop/libjpeg-compat/Makefile.linux KEILAND_LINUX_JPEG_COMPAT_SOURCES
userland/desktop/libjpeg-compat/Makefile.freebsd KEILAND_FREEBSD_JPEG_COMPAT_SOURCES
userland/desktop/libgif-compat/Makefile LIBGIF_COMPAT_SOURCES,LIBGIF_COMPAT_IMAGE_DATA
userland/desktop/libgif-compat/Makefile.linux KEILAND_LINUX_GIF_COMPAT_SOURCES
userland/desktop/libgif-compat/Makefile.freebsd KEILAND_FREEBSD_GIF_COMPAT_SOURCES
userland/base/libz-compat/Makefile LIBZ_COMPAT_SOURCES,LIBZ_COMPAT_IMAGE_DATA
userland/base/libz-compat/Makefile.linux KEILAND_LINUX_Z_COMPAT_SOURCES
userland/base/libz-compat/Makefile.freebsd KEILAND_FREEBSD_Z_COMPAT_SOURCES
userland/desktop/libkeiland/Makefile LIBKEILAND_SOURCES,LIBKEILAND_IMAGE_DATA
userland/desktop/libkeiland/Makefile.linux KEILAND_LINUX_KEILAND_SOURCES
userland/desktop/browser/Makefile KEILAND_BROWSER_DIR,KEILAND_BROWSER_SOURCES,KEILAND_BROWSER_DATA'

# The two trees: the Makefiles (and sources.mk) of BASE and of the working tree, with empty package.mk files.
for side in before after; do
	mkdir -p "$out/$side/plan/ws074/tests" "$out/$side/userland/desktop" "$out/$side/userland/base"
	: > "$out/$side/userland/desktop/package.mk"
	: > "$out/$side/userland/base/package.mk"
done
echo "$checks" | while read -r makefile vars; do
	dir=$(dirname -- "$makefile")
	mkdir -p "$out/before/$dir" "$out/after/$dir"
	git show "$BASE:$makefile" > "$out/before/$makefile"
	cp -- "$makefile" "$out/after/$makefile"
	if git cat-file -e "$BASE:$dir/sources.mk" 2> /dev/null; then
		git show "$BASE:$dir/sources.mk" > "$out/before/$dir/sources.mk"
	fi
	[ -f "$dir/sources.mk" ] && cp -- "$dir/sources.mk" "$out/after/$dir/sources.mk"
done
git show "$BASE:plan/ws074/tests/list-sources.sh" > "$out/before/plan/ws074/tests/list-sources.sh"
cp -- plan/ws074/tests/list-sources.sh "$out/after/plan/ws074/tests/list-sources.sh"

cat > "$out/print.mk" <<'EOF'
comma := ,
include $(MK)
$(foreach v,$(subst $(comma), ,$(VARS)),$(info $(v)=$(strip $($(v)))))
.PHONY: none
none: ;
EOF

# expand <tree> <makefile> <variables>: the variables' values, one per line.
expand() {
	env -u MAKEFLAGS -u MAKELEVEL -u MFLAGS make -s -r -R --no-print-directory -C "$1" \
	    -f "$(pwd)/$out/print.mk" MK="$2" VARS="$3" BUILD=build/amd64 ZEDBSD_TOPLEVEL_BUILD=y none
}

n=0
echo "$checks" | {
	while read -r makefile vars; do
		if ! expand "$out/before" "$makefile" "$vars" > "$out/one.before" 2> "$out/one.log" ||
		    ! expand "$out/after" "$makefile" "$vars" > "$out/one.after" 2>> "$out/one.log" ||
		    [ -s "$out/one.log" ]; then
			echo "FAILED make on $makefile:"
			head -5 "$out/one.log"
			status=1
		elif [ "$makefile" = userland/desktop/libkeiland/Makefile.linux ]; then
			# The same files, in the zedBSD list's order.
			tr ' ' '\n' < "$out/one.before" | sort > "$out/set.before"
			tr ' ' '\n' < "$out/one.after" | sort > "$out/set.after"
			expand "$out/after" userland/desktop/libkeiland/Makefile LIBKEILAND_SOURCES |
			    sed 's/^LIBKEILAND_SOURCES=/KEILAND_LINUX_KEILAND_SOURCES=/' > "$out/one.zedbsd"
			if ! cmp -s "$out/set.before" "$out/set.after"; then
				echo "FAILED $makefile: the set of files changed"
				diff "$out/set.before" "$out/set.after" | head -10
				status=1
			elif ! cmp -s "$out/one.after" "$out/one.zedbsd"; then
				echo "FAILED $makefile: not the zedBSD list"
				status=1
			else
				echo "same set, zedBSD order: $makefile"
			fi
		elif ! cmp -s "$out/one.before" "$out/one.after"; then
			echo "FAILED $makefile differs:"
			diff "$out/one.before" "$out/one.after" | head -10
			status=1
		elif grep -q '=$' "$out/one.after"; then
			echo "FAILED $makefile: an empty list"
			grep '=$' "$out/one.after"
			status=1
		else
			echo "same: $makefile ($(wc -w < "$out/one.after") words)"
		fi
		n=$((n + 1))
	done
	echo "makefiles: $n"

	if ! sh "$out/before/plan/ws074/tests/list-sources.sh" > "$out/list.before" ||
	    ! sh "$out/after/plan/ws074/tests/list-sources.sh" > "$out/list.after" ||
	    ! sh plan/ws074/tests/list-sources.sh > "$out/list.tree"; then
		echo "FAILED list-sources.sh"
		status=1
	elif ! cmp -s "$out/list.before" "$out/list.after" || ! cmp -s "$out/list.after" "$out/list.tree"; then
		echo "FAILED list-sources.sh prints other lines:"
		diff "$out/list.before" "$out/list.after" | head -10
		status=1
	elif [ ! -s "$out/list.after" ]; then
		echo "FAILED list-sources.sh printed nothing"
		status=1
	else
		echo "same: plan/ws074/tests/list-sources.sh ($(wc -l < "$out/list.after") lines)"
	fi

	if [ "$status" = 0 ]; then echo "sources-same: PASS"; else echo "sources-same: FAIL"; fi
	exit "$status"
}
