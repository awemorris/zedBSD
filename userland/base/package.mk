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
PREFIX ?= /usr/local
DESTDIR ?=
INSTALL ?= install
STANDALONE_CONFIG ?= $(REPO_ROOT)/config.mk

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
	$(if $(filter i386,$(PLATFORM)),pcat,\
	$(if $(filter rpi4 rpi5,$(PLATFORM)),arm64,\
	$(if $(filter sun4u,$(PLATFORM)),sparcv9,\
	$(PLATFORM)))))
STANDALONE_BINDIR = $(strip $(if $(filter /,$(PREFIX)),\
	/$(STANDALONE_INSTALL_DIR),$(patsubst %/,%,$(PREFIX))/$(STANDALONE_INSTALL_DIR)))
STANDALONE_TERMINFO_DIR = $(strip $(if $(filter /,$(PREFIX)),\
	/lib/terminfo,$(patsubst %/,%,$(PREFIX))/share/terminfo))

define USERLAND_PACKAGE
STANDALONE_NAME := $(1)
STANDALONE_SO_NAME := $(if $(2),$(2),$(1))
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
	@if test "$(STANDALONE_TYPE)" = program; then \
		$(MAKE) -C "$(REPO_ROOT)" \
			CONFIG="$(STANDALONE_CONFIG)" \
			USER_PROGRAMS="$(STANDALONE_NAME)" \
			"build/$(PLATFORM)/bin/$(STANDALONE_NAME)"; \
	elif test "$(STANDALONE_TYPE)" = shared-library; then \
		$(MAKE) -C "$(REPO_ROOT)" \
			CONFIG="$(STANDALONE_CONFIG)" \
			USER_PROGRAMS="$(STANDALONE_NAME)" \
			"build/$(STANDALONE_PLATFORM_DIR)/dynamic/$(STANDALONE_SO_NAME).so"; \
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
	fi

install: build
	@if test "$(STANDALONE_TYPE)" = program; then \
		$(INSTALL) -d "$(DESTDIR)$(STANDALONE_BINDIR)"; \
		$(INSTALL) -m "$(STANDALONE_MODE)" \
			"$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR)/bin/$(STANDALONE_NAME)" \
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
	@:

endif
