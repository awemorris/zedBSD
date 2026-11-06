# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# REmacs (github.com/awemorris/remacs) is a GNU Emacs clone written in
# Noct.  REmacs is part of this tree, under the zlib license of its
# author.
#

REQUIRE = base/noct

REMACS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(REMACS_DIR)../../../build-jobs.mk
include $(REMACS_DIR)../package.mk
include $(REMACS_DIR)../noct/version.mk

$(eval $(call USERLAND_PACKAGE,emacs,emacs,*,n,package,,y,base,base/emacs,$(REQUIRE),0755,emacs))

REPO_ROOT ?= $(abspath $(REMACS_DIR)../..)
BUILD ?= $(REPO_ROOT)/build/$(PLATFORM)

REMACS_SOURCE_DIR := $(REPO_ROOT)/userland/base/emacs

REMACS_SOURCES := \
	$(wildcard $(REMACS_SOURCE_DIR)/editor/*.noct) \
	$(REMACS_SOURCE_DIR)/src/napi.def $(REMACS_SOURCE_DIR)/tools/gen-napi.py \
	$(REMACS_SOURCE_DIR)/tools/build-nap.sh $(REMACS_SOURCE_DIR)/Makefile.remacs

REMACS_WORK_DIR := $(BUILD)/packages/base/emacs/work

NOCT_HOST_ARTIFACT     ?= $(REPO_ROOT)/build/NoctLang/build/noct

REMACS_NAP             := $(BUILD)/packages/base/emacs/remacs.nap
REMACS_BIN             := $(BUILD)/bin/emacs
REMACS_DICTIONARY      := $(REMACS_SOURCE_DIR)/dict/SKK-JISYO.remacs
REMACS_DICTIONARY_X    := $(REMACS_SOURCE_DIR)/dict/SKK-JISYO.X

PACKAGE_INPUTS += \
	$(if $(filter emacs,$(ZEDBSD_USER_PROGRAMS)), \
		$(REMACS_NAP) $(REMACS_BIN) $(REMACS_DICTIONARY) $(REMACS_DICTIONARY_X))

PACKAGE_FILES += \
	$(if $(filter emacs,$(ZEDBSD_USER_PROGRAMS)),\
		--file /bin/emacs=$(REMACS_BIN) \
		--mode /bin/emacs=0755 \
		--file /usr/share/remacs/skkjisyo.dic=$(REMACS_DICTIONARY) \
		--mode /usr/share/remacs/skkjisyo.dic=0644 \
		--file /usr/share/remacs/skkjisyo-x.dic=$(REMACS_DICTIONARY_X) \
		--mode /usr/share/remacs/skkjisyo-x.dic=0644)

PACKAGE_LINKS += \
	$(if $(filter emacs,$(ZEDBSD_USER_PROGRAMS)),\
		/usr/bin/noct=/bin/noct)

$(REMACS_NAP): $(REMACS_SOURCES)
	@rm -rf $(REMACS_WORK_DIR)
	@mkdir -p $(REMACS_WORK_DIR)
	cp -R $(REMACS_SOURCE_DIR)/editor $(REMACS_SOURCE_DIR)/src $(REMACS_SOURCE_DIR)/tools $(REMACS_SOURCE_DIR)/Makefile.remacs $(REMACS_WORK_DIR)/
	$(MAKE) -C $(REMACS_WORK_DIR) -f Makefile.remacs NOCT=$(abspath $(NOCT_HOST_ARTIFACT)) build/remacs.nap
	@mkdir -p $(dir $@)
	cp -f $(REMACS_WORK_DIR)/build/remacs.nap $@
	@head -c 16 $@ | grep -qx '#!/usr/bin/noct' || { echo 'remacs: $@ does not start with #!/usr/bin/noct' >&2; rm -f $@; exit 1; }
	@mkdir -p $(dir $(REMACS_BIN))
	cp -f $@ $(REMACS_BIN)
	chmod 0755 $(REMACS_BIN)

.PHONY: emacs
emacs: $(REMACS_NAP)

build: emacs
