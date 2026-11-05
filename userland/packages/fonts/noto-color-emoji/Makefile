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
ZEDBSD_NOTO_COLOR_EMOJI_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_NOTO_COLOR_EMOJI_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_NOTO_COLOR_EMOJI_MAKEFILE)))
include $(ZEDBSD_NOTO_COLOR_EMOJI_DIR)/../../../base/package.mk
include $(ZEDBSD_NOTO_COLOR_EMOJI_DIR)/../../external.mk

# Noto Emoji v2.047 (Unicode 16.0), fonts/NotoColorEmoji.ttf.
ZEDBSD_EXT_noto-color-emoji_VERSION := 2.047
ZEDBSD_EXT_noto-color-emoji_ARCHIVE := NotoColorEmoji-2.047.ttf
ZEDBSD_EXT_noto-color-emoji_URL := https://github.com/googlefonts/noto-emoji/raw/v2.047/fonts/NotoColorEmoji.ttf
ZEDBSD_EXT_noto-color-emoji_SIZE := 10643852
ZEDBSD_EXT_noto-color-emoji_SHA256 := 39ee3c587e10e89669b9ff32703261d10d5f9c4dd5ad147b6b5a1c5200591817

$(eval $(call ZEDBSD_EXTERNAL_FILE,noto-color-emoji))

ZEDBSD_NOTO_COLOR_EMOJI_LICENSE := $(ZEDBSD_NOTO_COLOR_EMOJI_DIR)/OFL-1.1.txt

.PHONY: noto-color-emoji
noto-color-emoji: $(ZEDBSD_EXT_noto-color-emoji_ARCHIVE_VERIFIED)
	@:

# ---------------------------------------------------------------- packaging

$(eval $(call USERLAND_PACKAGE,noto-color-emoji,Colour emoji font,*,n,package,,y,\
	packages/fonts,fonts/noto-color-emoji,))

# The verified record is the input, so the image waits for the check rather
# than for the bare download.  The download keeps the fetcher's private mode,
# so the modes are set: every program that draws text reads it.
ZEDBSD_PACKAGE_INPUTS += $(if $(filter noto-color-emoji,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_EXT_noto-color-emoji_ARCHIVE_VERIFIED) $(ZEDBSD_NOTO_COLOR_EMOJI_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter noto-color-emoji,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/share/fonts/keiland-emoji.ttf=$(ZEDBSD_EXT_noto-color-emoji_DISTFILE) \
	--file /usr/share/licenses/noto-color-emoji/OFL-1.1.txt=$(ZEDBSD_NOTO_COLOR_EMOJI_LICENSE) \
	--mode /usr/share/fonts/keiland-emoji.ttf=0644 \
	--mode /usr/share/licenses/noto-color-emoji/OFL-1.1.txt=0644)
