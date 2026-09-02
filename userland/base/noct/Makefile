# Canonical Noct release acquisition and zedBSD target package.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_NOCT_PACKAGE_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_NOCT_PACKAGE_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_NOCT_PACKAGE_MAKEFILE)))
include $(ZEDBSD_NOCT_PACKAGE_DIR)/../package.mk
include $(ZEDBSD_NOCT_PACKAGE_DIR)/version.mk

ZEDBSD_NOCT_ROOT := $(if $(ZEDBSD_TOPLEVEL_BUILD),$(CURDIR),$(ZEDBSD_REPO_ROOT))
ZEDBSD_NOCT_PACKAGE_ROOT := $(abspath $(ZEDBSD_NOCT_ROOT)/userland/base/noct)
ZEDBSD_NOCT_DISTDIR := $(ZEDBSD_NOCT_PACKAGE_ROOT)/distfiles
ZEDBSD_NOCT_DISTFILE := $(ZEDBSD_NOCT_DISTDIR)/$(ZEDBSD_NOCT_ARCHIVE_NAME)
ZEDBSD_NOCT_PATCH := $(ZEDBSD_NOCT_PACKAGE_ROOT)/patches/0001-connect-zedbsd-target-adapter.patch
ZEDBSD_NOCT_FETCH ?= curl --fail --location --silent --show-error
ZEDBSD_NOCT_SHA256 ?= sha256sum

NOCT_SOURCE_DIR := $(if $(ZEDBSD_TOPLEVEL_BUILD),userland/base/noct/noct,$(abspath $(ZEDBSD_NOCT_ROOT)/userland/base/noct/noct))
NOCT_SOURCE_STAMP := $(NOCT_SOURCE_DIR)/.zedbsd-source-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)
NOCT_SOURCE_IDENTITY := $(NOCT_SOURCE_DIR)/.zedbsd-source-identity
NOCT_SOURCE_MANIFEST := $(NOCT_SOURCE_DIR)/.zedbsd-source-manifest
NOCT_HOST_SOURCE_DIR := $(if $(ZEDBSD_TOPLEVEL_BUILD),build/NoctLang,$(abspath $(ZEDBSD_NOCT_ROOT)/build/NoctLang))
NOCT_HOST_SOURCE_STAMP := $(NOCT_HOST_SOURCE_DIR)/.zedbsd-source-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)
NOCT_HOST_SOURCE_IDENTITY := $(NOCT_HOST_SOURCE_DIR)/.zedbsd-source-identity
NOCT_HOST_SOURCE_MANIFEST := $(NOCT_HOST_SOURCE_DIR)/.zedbsd-source-manifest

NOCT_ZEDBSD_BUILD_DIR := $(NOCT_SOURCE_DIR)/build-zedbsd-amd64
NOCT_ZEDBSD_ARTIFACT := $(NOCT_ZEDBSD_BUILD_DIR)/noct
NOCT_PACKAGE_HOLORIS := $(NOCT_SOURCE_DIR)/resources/apps/holoris/holoris.noct

# The first resumed target remains amd64-only and optional. Remacs stays held
# independently until its own application acceptance is scheduled.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,noct,Noct,amd64,n,package,,y,base,base/noct,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	$(BUILD)/bin/noct $(NOCT_PACKAGE_HOLORIS))
ZEDBSD_PACKAGE_FILES += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	--file $(ZEDBSD_PACKAGE_BINDIR)/noct=$(BUILD)/bin/noct \
	--file $(ZEDBSD_PACKAGE_BINDIR)/holoris.nct=$(NOCT_PACKAGE_HOLORIS))

define ZEDBSD_NOCT_VERIFY_ARCHIVE_COMMANDS
	archive="$(1)"; \
	if test ! -f "$$archive" || test -L "$$archive"; then \
		echo "Noct: missing or unsafe archive: $$archive" >&2; \
		exit 1; \
	fi; \
	actual_size=$$(wc -c < "$$archive" | tr -d '[:space:]'); \
	if test "$$actual_size" != '$(ZEDBSD_NOCT_ARCHIVE_SIZE)'; then \
		echo "Noct: archive size mismatch: expected $(ZEDBSD_NOCT_ARCHIVE_SIZE), got $$actual_size" >&2; \
		exit 1; \
	fi; \
	actual_hash=$$('$(ZEDBSD_NOCT_SHA256)' "$$archive" | awk '{print $$1}'); \
	if test "$$actual_hash" != '$(ZEDBSD_NOCT_ARCHIVE_SHA256)'; then \
		echo "Noct: archive SHA-256 mismatch" >&2; \
		exit 1; \
	fi; \
	bad_entry=$$(tar -tzf "$$archive" | awk -v root='$(ZEDBSD_NOCT_ARCHIVE_ROOT)' '\
		$$0 != root && $$0 != root "/" && index($$0, root "/") != 1 { print; exit } \
		$$0 ~ /(^|\/)\.\.($$|\/)/ { print; exit }'); \
	if test -n "$$bad_entry"; then \
		echo "Noct: unsafe archive member: $$bad_entry" >&2; \
		exit 1; \
	fi; \
	bad_type=$$(tar -tvzf "$$archive" | awk 'substr($$0, 1, 1) != "-" && substr($$0, 1, 1) != "d" { print; exit }'); \
	if test -n "$$bad_type"; then \
		echo "Noct: unsupported archive member type: $$bad_type" >&2; \
		exit 1; \
	fi
endef

define ZEDBSD_NOCT_VERIFY_ARCHIVE
	@set -eu; \
	$(call ZEDBSD_NOCT_VERIFY_ARCHIVE_COMMANDS,$(ZEDBSD_NOCT_DISTFILE))
endef

$(ZEDBSD_NOCT_DISTFILE):
	@set -eu; \
	mkdir -p '$(ZEDBSD_NOCT_DISTDIR)'; \
	lock='$@.lock'; \
	temporary=; \
	locked=0; \
	cleanup() { \
		if test -n "$$temporary" && test -f "$$temporary"; then rm -f -- "$$temporary"; fi; \
		if test "$$locked" = 1 && test -d "$$lock"; then rmdir -- "$$lock"; fi; \
	}; \
	if ! mkdir "$$lock"; then \
		echo "Noct: archive acquisition already in progress: $@" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp '$(ZEDBSD_NOCT_DISTDIR)/.$(ZEDBSD_NOCT_ARCHIVE_NAME).XXXXXX'); \
	$(ZEDBSD_NOCT_FETCH) --output "$$temporary" '$(ZEDBSD_NOCT_ARCHIVE_URL)'; \
	$(call ZEDBSD_NOCT_VERIFY_ARCHIVE_COMMANDS,$$temporary); \
	mv -- "$$temporary" '$@'; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

.PHONY: noct-download
noct-download: $(ZEDBSD_NOCT_DISTFILE)
	$(ZEDBSD_NOCT_VERIFY_ARCHIVE)

ZEDBSD_USERLAND_DOWNLOAD_TARGETS += noct-download

define ZEDBSD_NOCT_EXTRACT_SOURCE
	@set -eu; \
	source='$(1)'; \
	parent=$${source%/*}; \
	if test -e "$$source"; then \
		echo "Noct: refusing to replace existing source tree: $$source" >&2; \
		exit 1; \
	fi; \
	mkdir -p "$$parent"; \
	lock="$$source.lock"; \
	temporary=; \
	locked=0; \
	cleanup() { \
		if test -n "$$temporary" && test -d "$$temporary"; then rm -rf -- "$$temporary"; fi; \
		if test "$$locked" = 1 && test -d "$$lock"; then rmdir -- "$$lock"; fi; \
	}; \
	if ! mkdir "$$lock"; then \
		echo "Noct: source extraction already in progress: $$source" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.noct-$(ZEDBSD_NOCT_VERSION).XXXXXX"); \
	tar -xzf '$(ZEDBSD_NOCT_DISTFILE)' -C "$$temporary"; \
	tree="$$temporary/$(ZEDBSD_NOCT_ARCHIVE_ROOT)"; \
	if test ! -d "$$tree" || test -L "$$tree"; then \
		echo "Noct: expected archive root is missing" >&2; \
		exit 1; \
	fi; \
	(cd "$$tree" && patch --batch --forward --fuzz=0 -p1 < '$(ZEDBSD_NOCT_PATCH)'); \
	printf '%s\n' \
		'version=$(ZEDBSD_NOCT_VERSION)' \
		'tag=$(ZEDBSD_NOCT_TAG)' \
		'commit=$(ZEDBSD_NOCT_TAG_COMMIT)' \
		'archive-sha256=$(ZEDBSD_NOCT_ARCHIVE_SHA256)' \
		'patch-level=$(ZEDBSD_NOCT_PATCH_LEVEL)' \
		> "$$tree/.zedbsd-source-identity"; \
	(cd "$$tree" && \
		find . -type f ! -path './build-*/*' ! -name '.zedbsd-source-*' -print | \
		LC_ALL=C sort | while IFS= read -r file; do sha256sum "$$file"; done) \
		> "$$tree/.zedbsd-source-manifest"; \
	touch "$$tree/.zedbsd-source-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)"; \
	mv -- "$$tree" "$$source"; \
	rmdir -- "$$temporary"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM
endef

define ZEDBSD_NOCT_VERIFY_SOURCE
	@set -eu; \
	source='$(1)'; \
	identity="$$source/.zedbsd-source-identity"; \
	manifest="$$source/.zedbsd-source-manifest"; \
	if test ! -f "$$identity" || test -L "$$identity" || \
	   test ! -f "$$manifest" || test -L "$$manifest"; then \
		echo "Noct: source identity is missing or unsafe: $$source" >&2; \
		exit 1; \
	fi; \
	expected=$$(printf '%s\n' \
		'version=$(ZEDBSD_NOCT_VERSION)' \
		'tag=$(ZEDBSD_NOCT_TAG)' \
		'commit=$(ZEDBSD_NOCT_TAG_COMMIT)' \
		'archive-sha256=$(ZEDBSD_NOCT_ARCHIVE_SHA256)' \
		'patch-level=$(ZEDBSD_NOCT_PATCH_LEVEL)'); \
	actual=$$(cat "$$identity"); \
	if test "$$actual" != "$$expected"; then \
		echo "Noct: source identity mismatch: $$source" >&2; \
		exit 1; \
	fi; \
	parent=$${source%/*}; \
	temporary=$$(mktemp "$$parent/.noct-manifest.XXXXXX"); \
	trap 'rm -f -- "$$temporary"' EXIT HUP INT TERM; \
	(cd "$$source" && \
		find . -type f ! -path './build-*/*' ! -name '.zedbsd-source-*' -print | \
		LC_ALL=C sort | while IFS= read -r file; do sha256sum "$$file"; done) \
		> "$$temporary"; \
	if ! cmp -s "$$manifest" "$$temporary"; then \
		echo "Noct: extracted source differs from the verified release plus patch: $$source" >&2; \
		exit 1; \
	fi; \
	rm -f -- "$$temporary"; \
	trap - EXIT HUP INT TERM
endef

$(NOCT_SOURCE_STAMP): $(ZEDBSD_NOCT_PATCH) | noct-download
	$(call ZEDBSD_NOCT_EXTRACT_SOURCE,$(NOCT_SOURCE_DIR))

$(NOCT_HOST_SOURCE_STAMP): $(ZEDBSD_NOCT_PATCH) | noct-download
	$(call ZEDBSD_NOCT_EXTRACT_SOURCE,$(NOCT_HOST_SOURCE_DIR))

.PHONY: noct-source noct-target-source-verify noct-host-source \
	noct-host-source-verify noct-zedbsd
noct-source: $(NOCT_SOURCE_STAMP)

noct-target-source-verify: $(NOCT_SOURCE_STAMP)
	$(call ZEDBSD_NOCT_VERIFY_SOURCE,$(NOCT_SOURCE_DIR))

noct-host-source: $(NOCT_HOST_SOURCE_STAMP)

noct-host-source-verify: $(NOCT_HOST_SOURCE_STAMP)
	$(call ZEDBSD_NOCT_VERIFY_SOURCE,$(NOCT_HOST_SOURCE_DIR))

ifndef ZEDBSD_TOPLEVEL_BUILD
download: noct-download
endif
patch: noct-target-source-verify

$(NOCT_PACKAGE_HOLORIS): | noct-target-source-verify
	@test -f '$@'

noct-zedbsd: noct-target-source-verify
	cd '$(NOCT_SOURCE_DIR)' && \
		ZEDBSD_SOURCE_DIR='$(ZEDBSD_NOCT_ROOT)' cmake --preset zedbsd-amd64 \
		-DNOCT_VERSION='$(ZEDBSD_NOCT_VERSION)'
	cd '$(NOCT_SOURCE_DIR)' && \
		ZEDBSD_SOURCE_DIR='$(ZEDBSD_NOCT_ROOT)' \
		cmake --build --preset zedbsd-amd64 --parallel 16
	@test -x '$(NOCT_ZEDBSD_ARTIFACT)'

ifeq ($(ZEDBSD_ARCHITECTURE),amd64)
$(BUILD)/bin/noct: noct-zedbsd
	@mkdir -p $(dir $@)
	cp -f '$(NOCT_ZEDBSD_ARTIFACT)' '$@'
	@cmp -s '$(NOCT_ZEDBSD_ARTIFACT)' '$@' || { \
		echo 'Installed Noct differs from the canonical CMake artifact' >&2; \
		exit 1; \
	}
endif

ifndef ZEDBSD_TOPLEVEL_BUILD
.PHONY: noct-install-holoris
install: noct-install-holoris

noct-install-holoris: build
	$(INSTALL) -d "$(DESTDIR)$(ZEDBSD_STANDALONE_BINDIR)"
	$(INSTALL) -m 0644 "$(NOCT_PACKAGE_HOLORIS)" \
		"$(DESTDIR)$(ZEDBSD_STANDALONE_BINDIR)/holoris.nct"
endif
