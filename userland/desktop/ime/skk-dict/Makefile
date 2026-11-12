# zedBSD userland package: ime-dict-skk, the dictionaries of the input method's SKK engine (WS154).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# REmacs's two SKK dictionaries, copied into the tree from userland/base/emacs/dict/ on 2026-10-05 and kept apart from
# them and from the Japanese engine's dictionary (2026-10-04 user: "Emacsの辞書は重複して持ってください。別々に管理し
# ます。"); they go into the image as /usr/share/keiland/ime/skk/SKK-JISYO.X (the fuller) and SKK-JISYO.remacs (the
# compact one), which the SKK engine looks in after the user's dictionary, in that order.
#
# License: Awe Morris holds the copyright; REmacs and its dictionaries are under zlib (relicensed for zedBSD on
# 2026-09-29, WS095 decision D1, and REmacs is in the tree under zlib since 2026-10-04), covered by the project's own
# license file.  No entry is copied from SKK-JISYO.L (the files' headers say so).
ZEDBSD_IME_DICT_SKK_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_IME_DICT_SKK_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_IME_DICT_SKK_MAKEFILE)))
include $(ZEDBSD_IME_DICT_SKK_DIR)/../../package.mk

ZEDBSD_IME_DICT_SKK := $(ZEDBSD_IME_DICT_SKK_DIR)/SKK-JISYO.X $(ZEDBSD_IME_DICT_SKK_DIR)/SKK-JISYO.remacs

.PHONY: ime-dict-skk
ime-dict-skk: $(ZEDBSD_IME_DICT_SKK)
	@:

# ---------------------------------------------------------------- packaging

# The input method (keiland-ime) requires it since WS154 p004, so every image with the input method has it.
$(eval $(call USERLAND_PACKAGE,ime-dict-skk,ime-dict-skk - Dictionaries of the SKK input method,*,n,package,,y,\
	desktop,desktop/ime/skk-dict,,0644,package,\
	/usr/share/keiland/ime/skk/SKK-JISYO.X=$(abspath $(ZEDBSD_IME_DICT_SKK_DIR)/SKK-JISYO.X) \
	/usr/share/keiland/ime/skk/SKK-JISYO.remacs=$(abspath $(ZEDBSD_IME_DICT_SKK_DIR)/SKK-JISYO.remacs)))
