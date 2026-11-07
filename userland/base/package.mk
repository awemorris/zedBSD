# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Common standalone package interface for userland/base.
#

ifndef TOPLEVEL_BUILD

.DEFAULT_GOAL := all

BASE_PACKAGE_MK := $(lastword $(MAKEFILE_LIST))
REPO_ROOT := $(abspath $(dir $(BASE_PACKAGE_MK))/../..)
ZEDBSD_REPO_ROOT := $(REPO_ROOT)
PREFIX ?= /usr/local
DESTDIR ?=
INSTALL ?= install
STANDALONE_CONFIG ?= $(REPO_ROOT)/config.mk

ifeq ($(origin CC),default)
CC := $(REPO_ROOT)/build/llvm/bin/clang
endif
ifeq ($(strip $(CC)),)
CC := $(REPO_ROOT)/build/llvm/bin/clang
endif
ifeq ($(origin NM),default)
NM := $(REPO_ROOT)/build/llvm/bin/llvm-nm
endif
ifeq ($(strip $(NM)),)
NM := $(REPO_ROOT)/build/llvm/bin/llvm-nm
endif
AR ?= $(REPO_ROOT)/build/llvm/bin/llvm-ar

# Acquisition and patch preparation must also work in a freshly cloned source
# tree, before menuconfig has produced config.mk.  Compilation and installation
# retain the existing configured-target requirement.
ifeq ($(strip $(MAKECMDGOALS)),)
include $(STANDALONE_CONFIG)
else ifeq ($(strip $(filter-out download patch,$(MAKECMDGOALS))),)
-include $(STANDALONE_CONFIG)
else
include $(STANDALONE_CONFIG)
endif

USERLAND_PACKAGE_LIFECYCLE := 1
include $(dir $(BASE_PACKAGE_MK))../download.mk

STANDALONE_PLATFORM_DIR = $(strip \
	$(if $(filter sun4u,$(PLATFORM)),sparcv9,\
	$(PLATFORM)))
STANDALONE_TARGET_TRIPLE = $(strip \
	$(if $(filter rpi4 rpi5 arm64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),aarch64-unknown-zedbsd,\
	$(if $(filter vf2 riscv64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),riscv64-unknown-zedbsd,\
	$(if $(filter i386 pcat pc98 pc9821 pc9801,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),i386-unknown-zedbsd,\
	$(if $(filter sun4u sparcv9,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),sparc64-unknown-zedbsd,\
	x86_64-unknown-zedbsd)))))
STANDALONE_ARCH_CFLAGS = $(strip \
	$(if $(filter rpi4 rpi5 arm64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),-march=armv8-a -mno-outline-atomics -DHAL_ARCH_ARM64 -DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64,\
	$(if $(filter vf2 riscv64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),-march=rv64gc -mabi=lp64d -mno-relax -DHAL_ARCH_RISCV64 -DKERN_USER_ABI_RISCV64 -DKERN_USER_ABI_LP64,\
	$(if $(filter i386 pcat pc98 pc9821 pc9801,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),-m32 -march=i686 -DHAL_ARCH_I386 -DKERN_USER_ABI_ILP32,\
	-m64 -march=x86-64 -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64))))
STANDALONE_BINDIR = $(strip $(if $(filter /,$(PREFIX)),\
	/$(STANDALONE_INSTALL_DIR),$(patsubst %/,%,$(PREFIX))/$(STANDALONE_INSTALL_DIR)))
STANDALONE_TERMINFO_DIR = $(strip $(if $(filter /,$(PREFIX)),\
	/lib/terminfo,$(patsubst %/,%,$(PREFIX))/share/terminfo))

define USERLAND_PACKAGE
STANDALONE_NAME := $(1)
STANDALONE_SO_NAME := $(if $(filter shared-library,$(12)),$(if $(2),$(2),$(1)),$(1))
STANDALONE_CLASS := $(5)
STANDALONE_SOURCES := $(6)
STANDALONE_REQUIRE := $(10)
STANDALONE_MODE := $(if $(11),$(11),0755)
STANDALONE_TYPE := $(if $(12),$(12),$(if $(filter library,$(5)),library,$(if $(filter package,$(5)),package,program)))
STANDALONE_DATA := $(13)
STANDALONE_HEADERS := $(14)
STANDALONE_INSTALL_DIR := $(if $(15),$(15),bin)
ifneq ($$(filter package custom,$$(STANDALONE_TYPE)),)
build: $(1)
endif
endef

.PHONY: all build install clean

all: build

build: patch
	@set -e; \
	dynamic_dir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/dynamic"; \
	sysroot="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/sysroot"; \
	is_dynamic=0; \
	req_libs=""; \
	for req in $(STANDALONE_REQUIRE); do \
		case "$$req" in \
			base/libedit) ;; \
			desktop/xserver) \
				is_dynamic=1; \
				if test -f "$(REPO_ROOT)/userland/desktop/xserver/libGLX/Makefile"; then \
					$(MAKE) -C "$(REPO_ROOT)/userland/desktop/xserver/libGLX"; \
				fi; \
				if test -f "$$dynamic_dir/libGLX.so"; then \
					req_libs="$$req_libs -l:libGLX.so"; \
				fi ;; \
			*lib*) \
				is_dynamic=1; \
				so="$${req##*/}.so"; \
				case "$$req" in \
					desktop/libwayland) so="libwayland-client.so" ;; \
					desktop/libegl) so="libEGL.so" ;; \
					desktop/libglesv2) so="libGLESv2.so" ;; \
					desktop/libGL) so="libGL.so" ;; \
					desktop/libwayland-client) so="libwayland-client.so" ;; \
				esac; \
				if test -f "$(REPO_ROOT)/userland/$$req/Makefile"; then \
					$(MAKE) -C "$(REPO_ROOT)/userland/$$req"; \
				fi; \
				if test -f "$$dynamic_dir/$$so"; then \
					req_libs="$$req_libs -l:$$so"; \
				fi ;; \
		esac; \
	done; \
	if test "$(STANDALONE_TYPE)" = program; then \
		bindir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/$(STANDALONE_INSTALL_DIR)"; \
		workdir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/packages/$(STANDALONE_NAME)"; \
		mkdir -p "$$bindir" "$$workdir"; \
		extra_srcs=""; \
		extra_cflags=""; \
		if test "$(STANDALONE_CLASS)" = "basic"; then \
			test -f "$(REPO_ROOT)/userland/base/common/command.c" && extra_srcs="$$extra_srcs $(REPO_ROOT)/userland/base/common/command.c"; \
			test -f "$(REPO_ROOT)/userland/base/common/pager.c" && extra_srcs="$$extra_srcs $(REPO_ROOT)/userland/base/common/pager.c"; \
		elif test "$(STANDALONE_CLASS)" = "network"; then \
			test -f "$(REPO_ROOT)/userland/base/net/netutil.c" && extra_srcs="$$extra_srcs $(REPO_ROOT)/userland/base/net/netutil.c $(REPO_ROOT)/userland/base/net/dhcp.c"; \
		fi; \
		if test "$(STANDALONE_NAME)" = "sh" || echo "$(STANDALONE_REQUIRE)" | grep -q "base/libedit"; then \
			test -f "$(REPO_ROOT)/userland/base/libedit/readline.c" && extra_srcs="$$extra_srcs $(REPO_ROOT)/userland/base/libedit/readline.c"; \
			extra_cflags="$$extra_cflags -I$(REPO_ROOT)/userland/base/libedit"; \
		fi; \
		srcs=""; \
		for s in $(STANDALONE_SOURCES); do \
			case "$$s" in \
				/*) srcs="$$srcs $$s" ;; \
				*) srcs="$$srcs $(REPO_ROOT)/$$s" ;; \
			esac; \
		done; \
		if test "$$is_dynamic" = 1; then \
			$(CC) --target=$(STANDALONE_TARGET_TRIPLE) --sysroot="$$sysroot" \
				$(STANDALONE_ARCH_CFLAGS) \
				-nostdinc -isystem "$$sysroot/usr/include" \
				-I"$(REPO_ROOT)/include" -I"$(REPO_ROOT)/src" -I"$(REPO_ROOT)" -I. \
				$$extra_cflags \
				-ffreestanding -fno-stack-protector -fPIC -Os \
				-nostdlib -pie -Wl,--no-relax \
				-Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
				-Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
				-Wl,--dynamic-linker=/lib/ld.so \
				"$$sysroot/usr/lib/crt1.o" \
				$$srcs $$extra_srcs \
				-L"$$dynamic_dir" -Wl,-rpath-link,"$$dynamic_dir" \
				$$req_libs -l:libm.so -l:libc.so \
				-o "$$bindir/$(STANDALONE_NAME)"; \
		else \
			$(CC) --target=$(STANDALONE_TARGET_TRIPLE) --sysroot="$$sysroot" \
				$(STANDALONE_ARCH_CFLAGS) \
				-nostdinc -isystem "$$sysroot/usr/include" \
				-I"$(REPO_ROOT)/include" -I"$(REPO_ROOT)/src" -I"$(REPO_ROOT)" -I. \
				$$extra_cflags \
				-ffreestanding -fno-stack-protector -nostdlib -static \
				$$srcs $$extra_srcs \
				-Wl,-T,"$$sysroot/usr/lib/zedbsd/$(STANDALONE_PLATFORM_DIR)/user.ld" \
				"$$sysroot/usr/lib/crt0.o" \
				-Wl,--start-group "$$sysroot/usr/lib/libc.a" \
				"$$sysroot/usr/lib/libm.a" \
				"$$sysroot/usr/lib/libzedbsd-compiler-rt.a" \
				"$$sysroot/usr/lib/libclang_rt.builtins.a" -Wl,--end-group \
				-o "$$bindir/$(STANDALONE_NAME)"; \
			test -z "$$($(NM) -u "$$bindir/$(STANDALONE_NAME)")" || { $(NM) -u "$$bindir/$(STANDALONE_NAME)"; exit 1; }; \
		fi; \
	elif test "$(STANDALONE_TYPE)" = shared-library; then \
		dynamic_dir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/dynamic"; \
		libdir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/lib"; \
		sysroot="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/sysroot"; \
		mkdir -p "$$dynamic_dir" "$$libdir"; \
		srcs=""; \
		for s in $(STANDALONE_SOURCES); do \
			case "$$s" in \
				/*) srcs="$$srcs $$s" ;; \
				*) srcs="$$srcs $(REPO_ROOT)/$$s" ;; \
			esac; \
		done; \
		version_script=""; \
		if test -f exports.map; then \
			version_script="-Wl,--version-script=exports.map"; \
		fi; \
		$(CC) --target=$(STANDALONE_TARGET_TRIPLE) --sysroot="$$sysroot" \
			$(STANDALONE_ARCH_CFLAGS) \
			-fPIC -shared \
			-nostdinc -isystem "$$sysroot/usr/include" \
			-I"$(REPO_ROOT)/include" -I"$(REPO_ROOT)/src" -I"$(REPO_ROOT)" -I. \
			$$srcs \
			-Wl,-soname,"$(STANDALONE_SO_NAME).so" \
			-Wl,--hash-style=both -Bsymbolic-functions \
			-Wl,-z,now,-z,relro,-z,separate-code,-z,stack-size=0x100000 \
			$$version_script \
			-L"$$dynamic_dir" -L"$$sysroot/usr/lib" \
			$$req_libs -l:libc.so -l:libm.so \
			-o "$$dynamic_dir/$(STANDALONE_SO_NAME).so" && \
		cp -f "$$dynamic_dir/$(STANDALONE_SO_NAME).so" "$$libdir/$(STANDALONE_SO_NAME).so"; \
	elif test "$(STANDALONE_TYPE)" = library && \
	    test -n "$(STANDALONE_SOURCES)"; then \
		libdir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/lib"; \
		workdir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/packages/$(STANDALONE_NAME)"; \
		mkdir -p "$$libdir" "$$workdir"; objects=; \
		for source in $(STANDALONE_SOURCES); do \
			object="$$workdir/$$(printf '%s' "$$source" | tr '/.' '__').o"; \
			$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
				-I"$(REPO_ROOT)" \
				-c "$(REPO_ROOT)/$$source" -o "$$object"; \
			objects="$$objects $$object"; \
		done; \
		$(AR) rcs "$$libdir/lib$(STANDALONE_NAME).a" $$objects; \
	fi; \
	for entry in $(STANDALONE_DATA); do \
		destination=$${entry%%=*}; source=$${entry#*=}; \
		case "$$destination" in \
		/lib/terminfo/*) destination="/share/terminfo/$${destination##*/}" ;; \
		esac; \
		case "$$source" in \
		/*) ;; \
		*) source="$(REPO_ROOT)/$$source" ;; \
		esac; \
		target="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)$${destination}"; \
		mkdir -p "$${target%/*}"; \
		cp -f "$$source" "$$target"; \
	done
	@stampdir="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/packages/$(STANDALONE_NAME)"; \
	mkdir -p "$$stampdir" && touch "$$stampdir/.stamp"; \
	pkgdir="$${PWD##*/}"; \
	if test -n "$$pkgdir" && test "$$pkgdir" != "$(STANDALONE_NAME)"; then \
		curdir_stamp="$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/packages/$$pkgdir"; \
		mkdir -p "$$curdir_stamp" && touch "$$curdir_stamp/.stamp"; \
	fi

install: build
	@if test "$(STANDALONE_TYPE)" = program; then \
		$(INSTALL) -d "$(DESTDIR)$(STANDALONE_BINDIR)"; \
		$(INSTALL) -m "$(STANDALONE_MODE)" \
			"$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/$(STANDALONE_INSTALL_DIR)/$(STANDALONE_NAME)" \
			"$(DESTDIR)$(STANDALONE_BINDIR)/$(STANDALONE_NAME)"; \
	elif test "$(STANDALONE_TYPE)" = shared-library; then \
		libdir="$(if $(filter /,$(PREFIX)),/lib,$(patsubst %/,%,$(PREFIX))/lib)"; \
		$(INSTALL) -d "$(DESTDIR)$$libdir"; \
		$(INSTALL) -m 0644 \
			"$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/dynamic/$(STANDALONE_SO_NAME).so" \
			"$(DESTDIR)$$libdir/$(STANDALONE_SO_NAME).so"; \
	elif test "$(STANDALONE_TYPE)" = library && \
	    test -n "$(STANDALONE_SOURCES)"; then \
		libdir="$(if $(filter /,$(PREFIX)),/lib,$(patsubst %/,%,$(PREFIX))/lib)"; \
		$(INSTALL) -d "$(DESTDIR)$$libdir"; \
		$(INSTALL) -m 0644 "$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/lib/lib$(STANDALONE_NAME).a" \
			"$(DESTDIR)$$libdir/lib$(STANDALONE_NAME).a"; \
	fi
	@set -e; for header in $(STANDALONE_HEADERS); do \
		includedir="$(if $(filter /,$(PREFIX)),/include,$(patsubst %/,%,$(PREFIX))/include)"; \
		$(INSTALL) -d "$(DESTDIR)$$includedir"; \
		$(INSTALL) -m 0644 "$(REPO_ROOT)/$$header" \
			"$(DESTDIR)$$includedir/$${header##*/}"; \
	done
	@set -e; for entry in $(STANDALONE_DATA); do \
		destination=$${entry%%=*}; source=$${entry#*=}; \
		case "$$destination" in \
		/lib/terminfo/*) destination="$(STANDALONE_TERMINFO_DIR)/$${destination##*/}" ;; \
		esac; \
		case "$$source" in \
		/*) ;; \
		*) source="$(REPO_ROOT)/$$source" ;; \
		esac; \
		$(INSTALL) -d "$(DESTDIR)$${destination%/*}"; \
		$(INSTALL) -m 0644 "$$source" \
			"$(DESTDIR)$$destination"; \
	done

clean:
	@rm -f "$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/packages/$(STANDALONE_NAME)/.stamp" \
		"$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/packages/$${PWD##*/}/.stamp"

endif
