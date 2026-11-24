# zedBSD userland package: unifont (GNU Unifont, the bitmap-style TrueType font).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# GNU Unifont 15.0.06 (Japanese JIS glyph variant, unifont_jp-15.0.06.ttf):
# the last upstream release providing prebuilt TrueType (glyf) outlines,
# which libtruetype rasterizes crisply at 16px and integer multiples.
# Dual-licensed under SIL OFL 1.1 and GNU GPLv2+ with the GNU Font
# Embedding Exception; redistributed here under SIL OFL 1.1, whose text
# sits beside this Makefile.
#
# Installed as /usr/share/fonts/unifont.ttf for the X11 server's fixed
# core fonts.
UNIFONT_MAKEFILE := $(lastword $(MAKEFILE_LIST))
UNIFONT_DIR := $(patsubst %/,%,$(dir $(UNIFONT_MAKEFILE)))
include $(UNIFONT_DIR)/../../../base/package.mk
include $(UNIFONT_DIR)/../../external.mk

# GNU Unifont 15.0.06 (unifont_jp-15.0.06.ttf).
EXT_unifont_VERSION := 15.0.06
EXT_unifont_ARCHIVE := unifont_jp-15.0.06.ttf
EXT_unifont_URL := https://ftp.gnu.org/gnu/unifont/unifont-15.0.06/unifont_jp-15.0.06.ttf
EXT_unifont_SIZE := 12366012
EXT_unifont_SHA256 := 7a5697ebb9d1451896371d294cc14a42b8f3c5158ce481acc81c2af6307ba160

$(eval $(call EXTERNAL_FILE,unifont))

UNIFONT_LICENSE := $(abspath $(UNIFONT_DIR)/OFL-1.1.txt)

.PHONY: unifont
unifont: $(EXT_unifont_ARCHIVE_VERIFIED)
	@:

build: unifont

# ---------------------------------------------------------------- packaging

$(eval $(call USERLAND_PACKAGE,unifont,GNU Unifont core font,*,n,package,,y,\
	packages/fonts,fonts/unifont,,0644,custom,\
	/usr/share/fonts/unifont.ttf=$(EXT_unifont_DISTFILE) \
	/usr/share/licenses/unifont/OFL-1.1.txt=$(UNIFONT_LICENSE)))

# The copy into the image waits for the check of the download, not for
# the bare download.
$(STANDALONE_BUILD_DIR)/usr/share/fonts/unifont.ttf: $(EXT_unifont_ARCHIVE_VERIFIED)
