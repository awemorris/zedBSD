# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) sysroot rules
#
# The sysroot holds the public headers the kernel compiles against; there
# is no C library to put beside them.
#

PLATFORM_MACPPC64_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT ?= $(abspath $(PLATFORM_MACPPC64_DIR)/../..)

# The sysroot is in the build directory, where the packages look for it.
SYSROOT     := $(abspath $(BUILD))/sysroot
SYSROOT_INC := $(SYSROOT)/usr/include
SYSROOT_LIB := $(SYSROOT)/usr/lib

PUBLIC_HEADERS := $(shell find include/libc include/uapi userland/desktop/include -type f ! -name '*~' 2>/dev/null | LC_ALL=C sort)

HEADERS_INSTALLED_STAMP := $(SYSROOT)/.headers-installed

.PHONY: sysroot-platform
sysroot-platform: $(HEADERS_INSTALLED_STAMP)

# Install public headers into sysroot
$(HEADERS_INSTALLED_STAMP): $(PUBLIC_HEADERS)
	@mkdir -p $(SYSROOT_INC)
	@for header in $(PUBLIC_HEADERS); do \
		case "$$header" in \
		include/libc/*) relative="$${header#include/libc/}" ;; \
		include/uapi/*) relative="$${header#include/}" ;; \
		userland/desktop/include/*) relative="$${header#userland/desktop/include/}" ;; \
		*) continue ;; \
		esac; \
		mkdir -p "$$(dirname "$(SYSROOT_INC)/$$relative")"; \
		cp -f "$$header" "$(SYSROOT_INC)/$$relative"; \
	done
	@touch $@
