# zedBSD userland package: noto-color-emoji (Google's Noto Color Emoji, the colour emoji font).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# ws102-p019: the font the desktop's text falls back to for emoji, drawn in
# colour (libtruetype's truetype_color_glyph reads its CBDT bitmaps, and
# userland/desktop/picture/color-glyph.c decodes and scales them).  The CBDT
# build is used (PNG images at 109 pixels per em, about 10 MB) rather than
# the COLRv1 one (vector paints, about 5 MB), whose gradients, transforms
# and compositing would need far more code in libtruetype
# (plan/ws102/phase019/phase.md).
#
# The font is the release's file as published; nothing converts it.  It is
# under the SIL Open Font License 1.1, whose text (the release's
# fonts/LICENSE) sits beside this Makefile.  It is installed as
# /usr/share/fonts/keiland-emoji.ttf, the path libkeiland's text and the
# compositor open.
#
# To update: take fonts/NotoColorEmoji.ttf of a newer tag of
# https://github.com/googlefonts/noto-emoji, record its size and SHA-256,
# and check its license file is still OFL-1.1.
NOTO_COLOR_EMOJI_MAKEFILE := $(lastword $(MAKEFILE_LIST))
NOTO_COLOR_EMOJI_DIR := $(patsubst %/,%,$(dir $(NOTO_COLOR_EMOJI_MAKEFILE)))
include $(NOTO_COLOR_EMOJI_DIR)/../../../base/package.mk
include $(NOTO_COLOR_EMOJI_DIR)/../../external.mk

# Noto Emoji v2.047 (Unicode 16.0), fonts/NotoColorEmoji.ttf.
EXT_noto-color-emoji_VERSION := 2.047
EXT_noto-color-emoji_ARCHIVE := NotoColorEmoji-2.047.ttf
EXT_noto-color-emoji_URL := https://github.com/googlefonts/noto-emoji/raw/v2.047/fonts/NotoColorEmoji.ttf
EXT_noto-color-emoji_SIZE := 10643852
EXT_noto-color-emoji_SHA256 := 39ee3c587e10e89669b9ff32703261d10d5f9c4dd5ad147b6b5a1c5200591817

$(eval $(call EXTERNAL_FILE,noto-color-emoji))

NOTO_COLOR_EMOJI_LICENSE := $(abspath $(NOTO_COLOR_EMOJI_DIR)/OFL-1.1.txt)

.PHONY: noto-color-emoji
noto-color-emoji: $(EXT_noto-color-emoji_ARCHIVE_VERIFIED)
	@:

build: noto-color-emoji

# ---------------------------------------------------------------- packaging

$(eval $(call USERLAND_PACKAGE,noto-color-emoji,Colour emoji font,*,n,package,,y,\
	packages/fonts,fonts/noto-color-emoji,,0644,custom,\
	/usr/share/fonts/keiland-emoji.ttf=$(EXT_noto-color-emoji_DISTFILE) \
	/usr/share/licenses/noto-color-emoji/OFL-1.1.txt=$(NOTO_COLOR_EMOJI_LICENSE)))

# The copy into the image waits for the check of the download, not for
# the bare download.
$(STANDALONE_BUILD_DIR)/usr/share/fonts/keiland-emoji.ttf: $(EXT_noto-color-emoji_ARCHIVE_VERIFIED)
