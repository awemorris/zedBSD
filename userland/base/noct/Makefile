# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Noct language runtime package for zedBSD.
#

NOCT_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(NOCT_DIR)../package.mk
include $(NOCT_DIR)../../../mk/build-jobs.mk

REPO_ROOT ?= $(abspath $(NOCT_DIR)../..)
REPO_HASH ?= d6f0f9b4f7debb4a3c8dd3ba0415665a94d561ae
BUILD ?= $(REPO_ROOT)/build/$(PLATFORM)

ARCH ?= $(strip \
	$(if $(filter i386 pc98 pc9821 pc9801,$(PLATFORM)),i386,\
	$(if $(filter arm64 rpi4 rpi5,$(PLATFORM)),arm64,\
	$(if $(filter riscv64 vf2,$(PLATFORM)),riscv64,\
	x86_64))))

ZEDBSD_LLVM_BIN ?= $(REPO_ROOT)/build/llvm/bin
CLANG ?= $(ZEDBSD_LLVM_BIN)/clang

ifeq ($(origin CC),default)
CC := $(CLANG)
endif
CC ?= $(CLANG)

TARGET_TRIPLE ?= $(strip \
	$(if $(filter i386,$(ARCH)),i386-unknown-zedbsd,\
	$(if $(filter arm64,$(ARCH)),aarch64-unknown-zedbsd,\
	$(if $(filter riscv64,$(ARCH)),riscv64-unknown-zedbsd,\
	x86_64-unknown-zedbsd))))

CFLAGS ?= --target=$(TARGET_TRIPLE) --sysroot=$(BUILD)/sysroot
CPPFLAGS ?= -D__ZEDBSD__

NOCT_CPPFLAGS := \
	-I$(REPO_ROOT)/include \
	-I$(REPO_ROOT)/src \
	-I$(REPO_ROOT) \
	-isystem $(BUILD)/sysroot/usr/include \
	$(CPPFLAGS)
NOCT_CFLAGS := $(filter-out -Werror -fno-pic -fno-pie,$(CFLAGS)) -fPIE

# The JIT needs __clear_cache, which compiler-rt implements for riscv64 only on
# Linux and OpenBSD (elsewhere it aborts), so riscv64 runs the interpreter.
NOCT_JIT := $(if $(filter riscv64,$(ARCH)),OFF,ON)

NOCT_SRC := $(abspath $(NOCT_DIR)noct)
NOCT_BUILD_DIR := $(NOCT_SRC)/build-zedbsd-$(ARCH)
NOCT_ARTIFACT := $(NOCT_BUILD_DIR)/noct

NOCT_BIN := $(BUILD)/bin/noct
NOCT_MAN := $(BUILD)/share/man/man1/noct.1

DISTFILE := $(abspath $(NOCT_DIR)distfiles)/NoctLang-$(REPO_HASH).tar.gz
PATCHES := $(sort $(wildcard $(abspath $(NOCT_DIR)patches)/*.patch))
PATCH_STAMP := $(NOCT_SRC)/.patched

$(eval $(call USERLAND_PACKAGE,noct,noct,*,n,package,,y,base,base/noct,desktop/libegl desktop/libglesv2,0755,custom))

PACKAGE_INPUTS += $(NOCT_BIN) $(NOCT_MAN)
PACKAGE_FILES += \
	--file /bin/noct=$(NOCT_BIN) \
	--mode /bin/noct=0755 \
	--file /usr/share/man/man1/noct.1=$(NOCT_MAN) \
	--mode /usr/share/man/man1/noct.1=0644

# The package's own target, which package.mk adds to build for a custom package.
.PHONY: noct
noct: $(NOCT_BIN) $(NOCT_MAN)

patch: $(PATCH_STAMP)

$(DISTFILE):
	@mkdir -p $(dir $@)
	curl --fail --location --output $@ "https://codeload.github.com/awemorris/NoctLang/tar.gz/$(REPO_HASH)"

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
	@mkdir -p $(BUILD)/dynamic
	@test -f $(BUILD)/dynamic/libEGL.so || $(MAKE) -C $(REPO_ROOT)/userland/desktop/libegl
	@test -f $(BUILD)/dynamic/libGLESv2.so || $(MAKE) -C $(REPO_ROOT)/userland/desktop/libglesv2
	@rm -rf $(NOCT_BUILD_DIR)
	@mkdir -p $(NOCT_BUILD_DIR)
	cd $(NOCT_SRC) && \
		CC="$(CC)" \
		CFLAGS="$(strip $(NOCT_CPPFLAGS) $(NOCT_CFLAGS))" \
		CPPFLAGS="$(NOCT_CPPFLAGS)" \
		ZEDBSD_SOURCE_DIR=$(REPO_ROOT) \
		ZEDBSD_SYSROOT=$(BUILD)/sysroot \
		ZEDBSD_LLVM_BIN=$(ZEDBSD_LLVM_BIN) \
		ZEDBSD_DYNAMIC_DIR=$(BUILD)/dynamic \
		cmake -B $(NOCT_BUILD_DIR) \
			-DCMAKE_C_COMPILER="$(CC)" \
			-DCMAKE_ASM_COMPILER="$(CC)" \
			-DCMAKE_C_FLAGS="$(strip $(NOCT_CPPFLAGS) $(NOCT_CFLAGS))" \
			-DCMAKE_ASM_FLAGS="$(strip $(NOCT_CPPFLAGS) $(NOCT_CFLAGS))" \
			-DCMAKE_EXE_LINKER_FLAGS="-L$(BUILD)/dynamic" \
			-DCMAKE_SHARED_LINKER_FLAGS="-L$(BUILD)/dynamic" \
			-DCMAKE_MODULE_LINKER_FLAGS="-L$(BUILD)/dynamic" \
			-DCMAKE_SYSROOT=$(BUILD)/sysroot \
			-DCMAKE_FIND_ROOT_PATH=$(BUILD)/sysroot \
			-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
			-DNOCT_TARGET_ZEDBSD=ON \
			-DNOCT_ENABLE_INSTALL=ON \
			-DNOCT_ENABLE_STATIC=ON \
			-DNOCT_ENABLE_SHARED=OFF \
			-DNOCT_ENABLE_JIT=$(NOCT_JIT) \
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
		cmake --build $(NOCT_BUILD_DIR) --parallel $(ZEDBSD_BUILD_JOBS)

$(NOCT_BIN): $(NOCT_ARTIFACT)
	@mkdir -p $(dir $@)
	cp -f $< $@
	chmod 0755 $@

$(NOCT_MAN): $(PATCH_STAMP)
	@mkdir -p $(dir $@)
	cp -f $(NOCT_SRC)/resources/man/noct.1.man $@
	chmod 0644 $@
