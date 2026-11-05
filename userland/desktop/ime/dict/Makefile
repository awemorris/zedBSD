# zedBSD userland package: ime-dict-ja, the Japanese dictionaries of the input method (WS095).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# One SKK dictionary goes into the image as /usr/share/keiland/ime/ja/SKK-JISYO.ja (ws095-p017, 2026-10-05: the
# user's "/usr/share/keiland/ime への移動と、辞書を 1 つにまとめる"; it was /usr/share/kei/ime/ja/ with two files).
# The file has two parts, which merge.sh made from the two files of before (their last revision is in git):
#
#   the supplement  written for zedBSD (formerly SKK-JISYO.kei), looked in first for the everyday words REmacs's
#                   dictionary lacks; the input method weighs its words above the other part's;
#   the system part REmacs's dictionary (formerly SKK-JISYO.X, copied into the tree from
#                   userland/base/emacs/dict/SKK-JISYO.X on 2026-10-04, user: "IME の辞書はコピーして取り込んで
#                   ください。"), from the line ";; ==== part: system ====" on, its header kept as published.
#
# License: Awe Morris holds the copyright of both.  The supplement is zlib; REmacs's dictionary was relicensed
# under zlib for zedBSD on 2026-09-29 ("私が著作権者なので、zlibライセンスで改めてライセンスします。…プロジェクト全体の
# ライセンスファイルの影響下ということで問題ないです。", WS095 decision D1, plan/ws095/design.md section 9.1), and
# REmacs itself is in the tree under zlib since 2026-10-04.  It is covered by the project's own license file, and
# no license file of its own is installed.
ZEDBSD_IME_DICT_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_IME_DICT_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_IME_DICT_MAKEFILE)))
include $(ZEDBSD_IME_DICT_DIR)/../../package.mk

ZEDBSD_IME_DICT_JA := $(ZEDBSD_IME_DICT_DIR)/SKK-JISYO.ja

.PHONY: ime-dict-ja
ime-dict-ja: $(ZEDBSD_IME_DICT_JA)
	@:

# ---------------------------------------------------------------- packaging

# Not chosen by default until the input method itself is a package (ws095-p004),
# which then requires this one.
$(eval $(call USERLAND_PACKAGE,ime-dict-ja,ime-dict-ja - Japanese dictionaries of the input method,*,n,package,,y,\
	desktop,desktop/ime/dict,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter ime-dict-ja,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_IME_DICT_JA))
ZEDBSD_PACKAGE_FILES += $(if $(filter ime-dict-ja,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/share/keiland/ime/ja/SKK-JISYO.ja=$(ZEDBSD_IME_DICT_JA) \
	--mode /usr/share/keiland/ime/ja/SKK-JISYO.ja=0644)
