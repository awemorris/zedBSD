# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

REQUIRE = lang/noct
$(eval $(call ZEDBSD_USERLAND_PACKAGE,remacs,Remacs,i386 pc98 amd64,n,package,,y,packages/editors,editors/remacs,$(REQUIRE)))
REQUIRE =

REMACS_REPOSITORY ?= https://github.com/awemorris/remacs.git
REMACS_GIT_REF ?= main
REMACS_SOURCE_DIR := build/sources/remacs
REMACS_SOURCE_STAMP := $(REMACS_SOURCE_DIR)/.zedbsd-source
REMACS_HOST_NOCT_DIR := build/host-noct-package
REMACS_HOST_NOCT := $(REMACS_HOST_NOCT_DIR)/noct
REMACS_NAP := $(BUILD)/packages/editors/remacs/remacs.nap
REMACS_DICTIONARY := $(REMACS_SOURCE_DIR)/dict/SKK-JISYO.remacs

# These recursive additions are evaluated after config.mk dependencies have
# been expanded.  Merely opening menuconfig therefore performs no network I/O.
ZEDBSD_PACKAGE_INPUTS += $(if $(filter remacs,$(ZEDBSD_USER_PROGRAMS)),\
	$(REMACS_NAP) $(REMACS_DICTIONARY))
ZEDBSD_PACKAGE_FILES += $(if $(filter remacs,$(ZEDBSD_USER_PROGRAMS)),\
	--file $(ZEDBSD_PACKAGE_BINDIR)/remacs.nap=$(REMACS_NAP) \
	--file /home/skkjisyo.dic=$(REMACS_DICTIONARY))

$(REMACS_SOURCE_STAMP):
	@mkdir -p $(dir $(REMACS_SOURCE_DIR))
	git clone --depth 1 --branch $(REMACS_GIT_REF) \
		$(REMACS_REPOSITORY) $(REMACS_SOURCE_DIR)
	@git -C $(REMACS_SOURCE_DIR) rev-parse HEAD > $@

$(REMACS_HOST_NOCT): build/sources/noct/.zedbsd-source
	cmake -S $(NOCT_SOURCE_DIR) \
		-B $(REMACS_HOST_NOCT_DIR) -DCMAKE_BUILD_TYPE=Release \
		-DNOCT_ENABLE_STATIC=ON -DNOCT_ENABLE_CLI=ON \
		-DNOCT_ENABLE_JIT=ON -DNOCT_ENABLE_API=ON \
		-DNOCT_ENABLE_API_SYSTEM=ON -DNOCT_ENABLE_API_CONSOLE=ON \
		-DNOCT_ENABLE_API_FILE=ON -DNOCT_ENABLE_API_TERM=ON \
		-DNOCT_ENABLE_REPL=ON -DNOCT_ENABLE_BCBACKEND=ON \
		-DNOCT_ENABLE_OPTIMIZER=ON -DNOCT_ENABLE_INSTALL=OFF
	cmake --build $(REMACS_HOST_NOCT_DIR) --target noctcli --parallel

$(REMACS_NAP): $(REMACS_SOURCE_STAMP) $(REMACS_HOST_NOCT)
	$(MAKE) -C $(REMACS_SOURCE_DIR) NOCT=$(abspath $(REMACS_HOST_NOCT)) all
	@mkdir -p $(dir $@)
	cp -f $(REMACS_SOURCE_DIR)/build/remacs.nap $@

.PHONY: remacs
remacs: $(REMACS_NAP)
