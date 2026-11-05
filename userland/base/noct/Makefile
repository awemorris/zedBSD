# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Noct language runtime package for zedBSD.
#

NOCT_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(NOCT_DIR)../package.mk
include $(NOCT_DIR)../../../build-jobs.mk

REPO_ROOT ?= $(abspath $(NOCT_DIR)../..)
BUILD ?= $(REPO_ROOT)/build/$(PLATFORM)

ARCH ?= $(strip \
	$(if $(filter i386 pc98,$(PLATFORM)),i386,\
	$(if $(filter arm64 rpi4 rpi5,$(PLATFORM)),arm64,\
	amd64)))

NOCT_SRC := $(abspath $(NOCT_DIR)noct)
NOCT_BUILD_DIR := $(NOCT_SRC)/build-zedbsd-$(ARCH)
NOCT_ARTIFACT := $(NOCT_BUILD_DIR)/noct

NOCT_BIN := $(BUILD)/bin/noct
NOCT_MAN := $(BUILD)/share/man/man1/noct.1

DISTFILE := $(abspath $(NOCT_DIR)distfiles)/NoctLang-82c1753ab3002c2742089898fc5f69c383792af1.tar.gz
PATCHES := $(sort $(wildcard $(abspath $(NOCT_DIR)patches)/*.patch))
PATCH_STAMP := $(NOCT_SRC)/.patched

$(eval $(call USERLAND_PACKAGE,noct,noct,*,n,package,,y,base,base/noct,,0755,custom))

PACKAGE_INPUTS += $(NOCT_BIN) $(NOCT_MAN)
PACKAGE_FILES += \
	--file /bin/noct=$(NOCT_BIN) \
	--mode /bin/noct=0755 \
	--file /usr/share/man/man1/noct.1=$(NOCT_MAN) \
	--mode /usr/share/man/man1/noct.1=0644

build: $(NOCT_BIN) $(NOCT_MAN)

patch: $(PATCH_STAMP)

$(DISTFILE):
	@mkdir -p $(dir $@)
	curl --fail --location --output $@ "https://codeload.github.com/awemorris/NoctLang/tar.gz/82c1753ab3002c2742089898fc5f69c383792af1"

$(PATCH_STAMP): $(DISTFILE) $(PATCHES)
	@rm -rf $(NOCT_SRC)
	@mkdir -p $(NOCT_SRC)
	tar -xzf $(DISTFILE) --strip-components=1 -C $(NOCT_SRC)
	@for p in $(PATCHES); do \
		echo "Applying patch $$p..."; \
		patch -d $(NOCT_SRC) -p1 < "$$p"; \
	done
	@touch $@

$(NOCT_ARTIFACT): $(PATCH_STAMP)
	@rm -rf $(NOCT_BUILD_DIR)
	@mkdir -p $(NOCT_BUILD_DIR)
	cd $(NOCT_SRC) && \
		env -u CFLAGS -u CPPFLAGS -u LDFLAGS \
		ZEDBSD_SOURCE_DIR=$(REPO_ROOT) \
		ZEDBSD_SYSROOT=$(BUILD)/sysroot \
		ZEDBSD_LLVM_BIN=$(REPO_ROOT)/build/llvm/bin \
		ZEDBSD_DYNAMIC_DIR=$(BUILD)/dynamic \
		cmake -B $(NOCT_BUILD_DIR) \
			-DNOCT_TARGET_ZEDBSD=ON \
			-DNOCT_ENABLE_INSTALL=ON \
			-DNOCT_ENABLE_SHARED=ON \
			-DNOCT_ENABLE_JIT=ON \
			-DNOCT_ENABLE_OPTIMIZER=ON \
			-DNOCT_ENABLE_MULTITHREAD=ON \
			-DNOCT_ENABLE_ACCEL=ON \
			-DNOCT_ENABLE_CLI=ON \
			-DNOCT_ENABLE_REPL=ON \
			-DNOCT_ENABLE_BCBACKEND=ON \
			-DNOCT_ENABLE_CBACKEND=ON \
			-DNOCT_ENABLE_ELBACKEND=ON \
			-DNOCT_ENABLE_SCMBACKEND=ON \
			-DNOCT_ENABLE_I18N=ON \
			-DNOCT_ENABLE_I18N_LIBINTL=ON \
			-DNOCT_ENABLE_API=ON \
			-DNOCT_ENABLE_API_SYSTEM=ON \
			-DNOCT_ENABLE_API_CONSOLE=ON \
			-DNOCT_ENABLE_API_FILE=ON \
			-DNOCT_ENABLE_API_THREAD=ON \
			-DNOCT_ENABLE_API_HTTPSERVER=ON \
			-DNOCT_ENABLE_API_TERM=ON \
			-DNOCT_ENABLE_API_BEUI=ON \
			-DNOCT_ENABLE_API_PROCESS=ON
	cd $(NOCT_SRC) && \
		env -u CFLAGS -u CPPFLAGS -u LDFLAGS \
		cmake --build $(NOCT_BUILD_DIR) --parallel $(ZEDBSD_BUILD_JOBS)

$(NOCT_BIN): $(NOCT_ARTIFACT)
	@mkdir -p $(dir $@)
	cp -f $< $@
	chmod 0755 $@

$(NOCT_MAN): $(PATCH_STAMP)
	@mkdir -p $(dir $@)
	cp -f $(NOCT_SRC)/resources/man/noct.1.man $@
	chmod 0644 $@
