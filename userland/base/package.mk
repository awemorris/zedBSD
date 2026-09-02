# Common standalone package interface for userland/base.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifndef ZEDBSD_TOPLEVEL_BUILD

.DEFAULT_GOAL := all

ZEDBSD_BASE_PACKAGE_MK := $(lastword $(MAKEFILE_LIST))
ZEDBSD_REPO_ROOT := $(abspath $(dir $(ZEDBSD_BASE_PACKAGE_MK))/../..)
PREFIX ?= /usr/local
DESTDIR ?=
INSTALL ?= install
CC ?= cc
AR ?= ar
ZEDBSD_STANDALONE_CONFIG ?= $(ZEDBSD_REPO_ROOT)/config.mk

# Acquisition and patch preparation must also work in a freshly cloned source
# tree, before menuconfig has produced config.mk.  Compilation and installation
# retain the existing configured-target requirement.
ifeq ($(strip $(MAKECMDGOALS)),)
include $(ZEDBSD_STANDALONE_CONFIG)
else ifeq ($(strip $(filter-out download patch,$(MAKECMDGOALS))),)
-include $(ZEDBSD_STANDALONE_CONFIG)
else
include $(ZEDBSD_STANDALONE_CONFIG)
endif

ZEDBSD_USERLAND_PACKAGE_LIFECYCLE := 1
include $(dir $(ZEDBSD_BASE_PACKAGE_MK))../download.mk

ZEDBSD_STANDALONE_PLATFORM_DIR = $(strip \
	$(if $(filter i386,$(ZEDBSD_PLATFORM)),pcat,\
	$(if $(filter rpi4,$(ZEDBSD_PLATFORM)),arm64,\
	$(if $(filter sun4u,$(ZEDBSD_PLATFORM)),sparcv9,\
	$(ZEDBSD_PLATFORM)))))
ZEDBSD_STANDALONE_BINDIR = $(strip $(if $(filter /,$(PREFIX)),\
	/$(ZEDBSD_STANDALONE_INSTALL_DIR),$(patsubst %/,%,$(PREFIX))/$(ZEDBSD_STANDALONE_INSTALL_DIR)))
ZEDBSD_STANDALONE_TERMINFO_DIR = $(strip $(if $(filter /,$(PREFIX)),\
	/lib/terminfo,$(patsubst %/,%,$(PREFIX))/share/terminfo))

define ZEDBSD_USERLAND_PACKAGE
ZEDBSD_STANDALONE_NAME := $(1)
ZEDBSD_STANDALONE_CLASS := $(5)
ZEDBSD_STANDALONE_SOURCES := $(6)
ZEDBSD_STANDALONE_REQUIRE := $(10)
ZEDBSD_STANDALONE_MODE := $(if $(11),$(11),0755)
ZEDBSD_STANDALONE_TYPE := $(if $(12),$(12),$(if $(filter library,$(5)),library,program))
ZEDBSD_STANDALONE_DATA := $(13)
ZEDBSD_STANDALONE_HEADERS := $(14)
ZEDBSD_STANDALONE_INSTALL_DIR := $(if $(15),$(15),bin)
endef

.PHONY: all build install clean

all: build

build: patch
	@if test "$(ZEDBSD_STANDALONE_TYPE)" = program; then \
		$(MAKE) -C "$(ZEDBSD_REPO_ROOT)" \
			ZEDBSD_CONFIG="$(ZEDBSD_STANDALONE_CONFIG)" \
			ZEDBSD_USER_PROGRAMS="$(ZEDBSD_STANDALONE_NAME)" \
			"build/$(ZEDBSD_STANDALONE_PLATFORM_DIR)/bin/$(ZEDBSD_STANDALONE_NAME)"; \
	elif test "$(ZEDBSD_STANDALONE_TYPE)" = library && \
	    test -n "$(ZEDBSD_STANDALONE_SOURCES)"; then \
		mkdir -p build; objects=; \
		for source in $(ZEDBSD_STANDALONE_SOURCES); do \
			object=build/$$(printf '%s' "$$source" | tr '/.' '__').o; \
			$(CC) -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
				-I"$(ZEDBSD_REPO_ROOT)" \
				-c "$(ZEDBSD_REPO_ROOT)/$$source" -o "$$object"; \
			objects="$$objects $$object"; \
		done; \
		$(AR) rcs "build/lib$(ZEDBSD_STANDALONE_NAME).a" $$objects; \
	fi

install: build
	@if test "$(ZEDBSD_STANDALONE_TYPE)" = program; then \
		$(INSTALL) -d "$(DESTDIR)$(ZEDBSD_STANDALONE_BINDIR)"; \
		$(INSTALL) -m "$(ZEDBSD_STANDALONE_MODE)" \
			"$(ZEDBSD_REPO_ROOT)/build/$(ZEDBSD_STANDALONE_PLATFORM_DIR)/bin/$(ZEDBSD_STANDALONE_NAME)" \
			"$(DESTDIR)$(ZEDBSD_STANDALONE_BINDIR)/$(ZEDBSD_STANDALONE_NAME)"; \
	elif test "$(ZEDBSD_STANDALONE_TYPE)" = library && \
	    test -n "$(ZEDBSD_STANDALONE_SOURCES)"; then \
		libdir="$(if $(filter /,$(PREFIX)),/lib,$(patsubst %/,%,$(PREFIX))/lib)"; \
		$(INSTALL) -d "$(DESTDIR)$$libdir"; \
		$(INSTALL) -m 0644 "build/lib$(ZEDBSD_STANDALONE_NAME).a" \
			"$(DESTDIR)$$libdir/lib$(ZEDBSD_STANDALONE_NAME).a"; \
	fi
	@set -e; for header in $(ZEDBSD_STANDALONE_HEADERS); do \
		includedir="$(if $(filter /,$(PREFIX)),/include,$(patsubst %/,%,$(PREFIX))/include)"; \
		$(INSTALL) -d "$(DESTDIR)$$includedir"; \
		$(INSTALL) -m 0644 "$(ZEDBSD_REPO_ROOT)/$$header" \
			"$(DESTDIR)$$includedir/$${header##*/}"; \
	done
	@set -e; for entry in $(ZEDBSD_STANDALONE_DATA); do \
		destination=$${entry%%=*}; source=$${entry#*=}; \
		case "$$destination" in \
		/lib/terminfo/*) destination="$(ZEDBSD_STANDALONE_TERMINFO_DIR)/$${destination##*/}" ;; \
		esac; \
		case "$$source" in \
		/*) ;; \
		*) source="$(ZEDBSD_REPO_ROOT)/$$source" ;; \
		esac; \
		$(INSTALL) -d "$(DESTDIR)$${destination%/*}"; \
		$(INSTALL) -m 0644 "$$source" \
			"$(DESTDIR)$$destination"; \
	done

clean:
	@:

endif
