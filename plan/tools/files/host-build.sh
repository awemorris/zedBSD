#!/bin/sh
# ws071: builds files' host tests with the host's C compiler into build/ws071-host/.
#
# The drawing (canvas, text, icons), the interface and the model of files are built
# without Wayland and Vulkan (window.c, present.c, menu.c and titlebar.c stay out); libtruetype is built
# from its sources.  The test programs:
#   files-render   draws scenes of the interface into PPM pictures (host-render.c)
#   files-model    checks the model in temporary directories (host-model.c)
#
#   plan/tools/files/host-build.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=build/ws071-host
src=userland/desktop/files
mkdir -p "$out/include" "$out/obj"
mkdir -p "$out/include/truetype"
ln -sf "$(pwd)/userland/desktop/include/truetype/truetype.h" "$out/include/truetype/truetype.h"
mkdir -p "$out/include/keiland"
ln -sf "$(pwd)/userland/desktop/include/keiland/keiland.h" "$out/include/keiland/keiland.h"
ln -sf "$(pwd)/include/libc/sha2.h" "$out/include/sha2.h"
ln -sf "$(pwd)/include/libc/pdf.h" "$out/include/pdf.h"
ln -sf "$(pwd)/include/libc/md5.h" "$out/include/md5.h"
ln -sf "$(pwd)/include/libc/sha1.h" "$out/include/sha1.h"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
cc=${CC:-cc}
flags="-O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D_GNU_SOURCE -I$out/include -I$src -I."

# The libraries the program uses, from their sources.
objects=""
for file in userland/desktop/libtruetype/face.c userland/desktop/libtruetype/cmap.c \
    userland/desktop/libtruetype/outline.c userland/desktop/libtruetype/render.c \
    userland/desktop/libtruetype/glyph.c userland/desktop/libtruetype/design.c userland/desktop/libtruetype/companion.c \
    userland/desktop/libtruetype/color.c userland/desktop/libtruetype/contour.c; do
	object="$out/obj/truetype-$(basename "$file" .c).o"
	"$cc" $flags -Wno-error -Iuserland/desktop/libtruetype -c "$file" -o "$object"
	objects="$objects $object"
done
# The C library's SHA-2 (the information's checksum), which the host's C library does not have.
"$cc" $flags -c src/libc/openbsd-sha2.c -o "$out/obj/libc-sha2.o"
objects="$objects $out/obj/libc-sha2.o"
# The thumbnails (ws168-p004): keiland-preview's callers' side with the making in this process (freebsd/spawn.c: no
# child and no installed program on the host), its decoding and scaling, and what they decode with: libz-compat,
# libpng-compat, libjpeg-compat, libgif-compat, the decoding shared with Image Viewer, libpdf and the C library's MD5.
for file in userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libz-compat/deflate.c \
    userland/base/libpng-compat/read.c \
    userland/base/libjpeg-compat/decompress.c userland/base/libjpeg-compat/error.c userland/base/libjpeg-compat/huffman.c \
    userland/base/libjpeg-compat/idct.c userland/base/libjpeg-compat/marker.c userland/base/libjpeg-compat/memory.c \
    userland/base/libjpeg-compat/source.c userland/base/libgif-compat/decode.c userland/base/libgif-compat/lzw.c \
    userland/desktop/picture/picture.c userland/desktop/preview/client.c userland/desktop/preview/freebsd/spawn.c \
    userland/desktop/preview/make.c userland/desktop/preview/decode.c userland/desktop/preview/scale.c; do
	object="$out/obj/compat-$(basename "$(dirname "$file")")-$(basename "$file" .c).o"
	"$cc" $flags -c "$file" -o "$object"
	objects="$objects $object"
done
for file in userland/base/libpdf/*.c src/libc/openbsd-digest.c; do
	object="$out/obj/pdf-$(basename "$file" .c).o"
	"$cc" $flags -Wno-error -w -c "$file" -o "$object"
	objects="$objects $object"
done
if [ -f userland/desktop/libkeiland/recent.c ]; then
	"$cc" $flags -c userland/desktop/libkeiland/recent.c -o "$out/obj/zdesktop-recent.o"
	objects="$objects $out/obj/zdesktop-recent.o"
fi

# libkeiland's overlay scroll bar (files/ui-scrollbar.c uses it since ws127-p002), which needs no canvas.
"$cc" $flags -c userland/desktop/libkeiland/ui/scroll-bar.c -o "$out/obj/keiui-scroll-bar.o"
objects="$objects $out/obj/keiui-scroll-bar.o"

# libkeiland's canvas, text and icons (files draws with them since ws090-p009), the colour glyphs its text draws emoji with,
# and its widgets (the field of the name being changed, ws090-p010; the sidebar, the buttons, the cards and the chip, ws090-p023).
for file in userland/desktop/libkeiland/ui/canvas.c userland/desktop/libkeiland/ui/text.c userland/desktop/libkeiland/ui/icons.c \
    userland/desktop/libkeiland/ui/icons-line.c userland/desktop/picture/color-glyph.c \
    userland/desktop/libkeiland/ui/ui.c userland/desktop/libkeiland/ui/field.c userland/desktop/libkeiland/ui/input.c \
    userland/desktop/libkeiland/ui/theme.c userland/desktop/libkeiland/ui/scroll.c userland/desktop/libkeiland/ui/text-touch.c \
    userland/desktop/libkeiland/ui/widgets.c userland/desktop/libkeiland/ui/cards.c userland/desktop/libkeiland/ui/list.c userland/desktop/libkeiland/ui/views.c \
    plan/tools/files/host-appearance.c; do
	object="$out/obj/keiui-$(basename "$file" .c).o"
	"$cc" $flags -Iuserland/desktop/libkeiland/ui -c "$file" -o "$object"
	objects="$objects $object"
done

# The desktop's settings (files/apps.c keeps the chosen ways there since WS135): libkeiland's cache and application
# files and the settings' table, under the stand-in for kl_settings_* without Wayland (plan/tools/settings/host-kl-settings.c).
for file in userland/desktop/libkeiland/settings-cache.c userland/desktop/libkeiland/settings-app.c \
    userland/desktop/settings-keys/settings-keys.c plan/tools/settings/host-kl-settings.c; do
	object="$out/obj/keiland-$(basename "$file" .c).o"
	"$cc" $flags -Iuserland/desktop/libkeiland -c "$file" -o "$object"
	objects="$objects $object"
done

# libkeiland's gesture, scroller and motion (files/touch.c uses them since ws081-p010; ws093-p003).
for file in userland/desktop/libkeiland/gesture.c userland/desktop/libkeiland/scroll.c userland/desktop/libkeiland/motion.c; do
	object="$out/obj/keiland-$(basename "$file" .c).o"
	"$cc" $flags -c "$file" -o "$object"
	objects="$objects $object"
done

# libkeiland's translations (files' words go through kl_tr since ws158-p004; without catalogs the English shows).
"$cc" $flags -Iuserland/desktop/libkeiland -c userland/desktop/libkeiland/translate.c -o "$out/obj/keiland-translate.o"
objects="$objects $out/obj/keiland-translate.o"

# files without the window, the presenter, the menus, the titlebar and the glass.
# The mounts are the desktop's answer (mounts.c, ws188-p004): none without a compositor.
for file in $src/*.c; do
	case $(basename "$file") in
	main.c|window.c|present.c|menu.c|titlebar.c|glass.c|dnd.c|canvas.c|text.c|icons.c) continue ;;
	esac
	object="$out/obj/files-$(basename "$file" .c).o"
	"$cc" $flags -c "$file" -o "$object"
	objects="$objects $object"
done

# The Kei mark (ws035-p108), shared with the compositor.
"$cc" $flags -c userland/desktop/artwork/mark.c -o "$out/obj/artwork-mark.o"
objects="$objects $out/obj/artwork-mark.o"

# The test programs.
for test in render model; do
	if [ -f "plan/tools/files/host-$test.c" ]; then
		"$cc" $flags -c "plan/tools/files/host-$test.c" -o "$out/obj/host-$test.o"
		extra=
		if [ "$test" = render ]; then
			"$cc" $flags -c plan/tools/files/host-glass.c -o "$out/obj/host-glass.o"
			extra="$out/obj/host-glass.o"
		fi
		"$cc" -o "$out/files-$test" "$out/obj/host-$test.o" $extra $objects -lm -ldl
		echo "built $out/files-$test"
	fi
done
