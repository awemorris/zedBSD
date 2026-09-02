# Common standalone source-distribution lifecycle for userland items.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The top-level build includes every package Makefile to collect metadata.  It
# owns its aggregate lifecycle targets, so package-local defaults must only be
# visible when an item is invoked directly with `make -C userland/...`.
ifndef ZEDBSD_TOPLEVEL_BUILD

ZEDBSD_USERLAND_DOWNLOAD_TARGETS ?=
ZEDBSD_USERLAND_PATCH_TARGETS ?=

# A package with external bytes also appends its verified acquisition target
# to ZEDBSD_USERLAND_DOWNLOAD_TARGETS.  The repository Makefile consumes the
# declarations after all package Makefiles have registered; standalone items
# keep an explicit local prerequisite because GNU Make expands prerequisites
# when their rule is parsed.

.PHONY: download patch

# In-tree sources have nothing to acquire or patch.  Packages backed by
# immutable external artifacts extend these rules with verified prerequisites.
download: $(ZEDBSD_USERLAND_DOWNLOAD_TARGETS)
	@:

patch: download $(ZEDBSD_USERLAND_PATCH_TARGETS)
	@:

# Aggregator Makefiles have no package-specific build/install implementation.
# Give them the same callable lifecycle while package.mk supplies the concrete
# build and staged-install recipes for actual packages.
ifndef ZEDBSD_USERLAND_PACKAGE_LIFECYCLE
.PHONY: build install

build: patch
	@:

install: build
	@:
endif

endif
