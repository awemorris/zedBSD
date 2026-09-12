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
# Only the cross-build wiring stays local; the rest is upstream now.
ZEDBSD_NOCT_PATCHES := \
	$(ZEDBSD_NOCT_PACKAGE_ROOT)/patches/0001-connect-zedbsd-target-adapter.patch \
	$(ZEDBSD_NOCT_PACKAGE_ROOT)/patches/0002-use-project-llvm-sysroot.patch
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

# Records of the archive and source-tree checks, kept beside what each one
# examined so that a build repeats a check only after that thing changes.
ZEDBSD_NOCT_ARCHIVE_VERIFIED := \
	$(ZEDBSD_NOCT_DISTDIR)/.zedbsd-archive-verified-$(ZEDBSD_NOCT_ARCHIVE_NAME)
NOCT_SOURCE_VERIFIED := \
	$(NOCT_SOURCE_DIR)/.zedbsd-source-verified-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)
NOCT_HOST_SOURCE_VERIFIED := \
	$(NOCT_HOST_SOURCE_DIR)/.zedbsd-source-verified-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)

# The interpreter built for the build machine. The build's own scripts
# run on it, so it exists before any target program does; $(NOCT) is how
# the rest of the tree asks for it.
NOCT_HOST_BUILD_DIR := $(NOCT_HOST_SOURCE_DIR)/build-static
NOCT_HOST_ARTIFACT := $(NOCT_HOST_BUILD_DIR)/noct
NOCT_HOST_STATE_DIR := $(if $(ZEDBSD_TOPLEVEL_BUILD),build/host-noct-state,\
	$(abspath $(ZEDBSD_NOCT_ROOT)/build/host-noct-state))
NOCT_HOST_BUILD_STAMP := \
	$(NOCT_HOST_STATE_DIR)/built-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)-process
NOCT_HOST_CMAKE_OPTIONS := -DNOCT_ENABLE_API_PROCESS=ON \
	-DNOCT_VERSION=$(ZEDBSD_NOCT_VERSION)
NOCT ?= $(abspath $(NOCT_HOST_ARTIFACT))

NOCT_ZEDBSD_ARCH := $(if $(filter i386,$(ZEDBSD_ARCHITECTURE)),i386,amd64)
NOCT_ZEDBSD_SYSROOT = $(if $(filter i386,$(NOCT_ZEDBSD_ARCH)),$(ZEDBSD_SYSROOT_I386),$(ZEDBSD_SYSROOT_AMD64))
NOCT_ZEDBSD_CMAKE_OPTIONS := $(if $(filter i386,$(NOCT_ZEDBSD_ARCH)),--toolchain $(ZEDBSD_NOCT_PACKAGE_ROOT)/zedbsd-i386.cmake -DNOCT_ENABLE_JIT=OFF,)
NOCT_ZEDBSD_BUILD_DIR := $(NOCT_SOURCE_DIR)/build-zedbsd-$(NOCT_ZEDBSD_ARCH)
NOCT_ZEDBSD_ARTIFACT := $(NOCT_ZEDBSD_BUILD_DIR)/noct

# cmake leaves the interpreter's timestamp alone when it has nothing to
# do, so the build records itself here instead of in its own output.
NOCT_ZEDBSD_BUILD_STAMP := \
	$(NOCT_ZEDBSD_BUILD_DIR)/.zedbsd-built-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)

# The sysroot the interpreter links against, named by the stamp that says
# it is complete. sysroot.mk is included after this package, so its own
# variables are not available here yet; the top level settled the path for
# the selected architecture before any package was read.
NOCT_ZEDBSD_SYSROOT_STAMP := $(if $(ZEDBSD_TARGET_SYSROOT),\
	$(ZEDBSD_TARGET_SYSROOT)/.zedbsd-sysroot-complete,)
NOCT_PACKAGE_HOLORIS := $(NOCT_SOURCE_DIR)/resources/apps/holoris/holoris.noct

# i386 uses the interpreter for the PC98 i486 baseline. Remacs stays held
# independently until its own application acceptance is scheduled.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,noct,noct,amd64 pc98,n,package,,y,base,base/noct,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	$(BUILD)/bin/noct $(NOCT_PACKAGE_HOLORIS))
ZEDBSD_PACKAGE_FILES += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	--file /bin/noct=$(BUILD)/bin/noct \
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
	@touch '$(ZEDBSD_NOCT_ARCHIVE_VERIFIED)'

$(ZEDBSD_NOCT_ARCHIVE_VERIFIED): $(ZEDBSD_NOCT_DISTFILE)
	$(ZEDBSD_NOCT_VERIFY_ARCHIVE)
	@touch '$@'

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
	for patch_file in $(ZEDBSD_NOCT_PATCHES); do \
		(cd "$$tree" && patch --batch --forward --fuzz=0 -p1 < "$$patch_file"); \
	done; \
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

$(NOCT_SOURCE_STAMP): $(ZEDBSD_NOCT_PATCHES) \
		| $(ZEDBSD_NOCT_ARCHIVE_VERIFIED)
	$(call ZEDBSD_NOCT_EXTRACT_SOURCE,$(NOCT_SOURCE_DIR))

$(NOCT_HOST_SOURCE_STAMP): $(ZEDBSD_NOCT_PATCHES) \
		| $(ZEDBSD_NOCT_ARCHIVE_VERIFIED)
	$(call ZEDBSD_NOCT_EXTRACT_SOURCE,$(NOCT_HOST_SOURCE_DIR))

.PHONY: noct-source noct-target-source-verify noct-host-source \
	noct-host-source-verify noct-zedbsd
noct-source: $(NOCT_SOURCE_STAMP)

noct-target-source-verify: $(NOCT_SOURCE_STAMP)
	$(call ZEDBSD_NOCT_VERIFY_SOURCE,$(NOCT_SOURCE_DIR))
	@touch '$(NOCT_SOURCE_VERIFIED)'

noct-host-source: $(NOCT_HOST_SOURCE_STAMP)

noct-host-source-verify: $(NOCT_HOST_SOURCE_STAMP)
	$(call ZEDBSD_NOCT_VERIFY_SOURCE,$(NOCT_HOST_SOURCE_DIR))
	@touch '$(NOCT_HOST_SOURCE_VERIFIED)'

$(NOCT_SOURCE_VERIFIED): $(NOCT_SOURCE_STAMP)
	$(call ZEDBSD_NOCT_VERIFY_SOURCE,$(NOCT_SOURCE_DIR))
	@touch '$@'

$(NOCT_HOST_SOURCE_VERIFIED): $(NOCT_HOST_SOURCE_STAMP)
	$(call ZEDBSD_NOCT_VERIFY_SOURCE,$(NOCT_HOST_SOURCE_DIR))
	@touch '$@'

ifndef ZEDBSD_TOPLEVEL_BUILD
download: noct-download
endif
patch: noct-target-source-verify

$(NOCT_PACKAGE_HOLORIS): | $(NOCT_SOURCE_VERIFIED)
	@test -f '$@'

# Entered only when the record of the last build is older than what the
# interpreter is built from, so an unchanged tree costs nothing and
# prints nothing.
$(NOCT_ZEDBSD_BUILD_STAMP): $(NOCT_SOURCE_VERIFIED) \
		$(ZEDBSD_NOCT_PACKAGE_MAKEFILE) $(NOCT_ZEDBSD_SYSROOT_STAMP)
	cd '$(NOCT_SOURCE_DIR)' && \
		ZEDBSD_SOURCE_DIR='$(ZEDBSD_NOCT_ROOT)' \
		ZEDBSD_SYSROOT='$(NOCT_ZEDBSD_SYSROOT)' \
		ZEDBSD_LLVM_BIN='$(ZEDBSD_LLVM_INSTALL)/bin' \
		cmake --preset zedbsd-amd64 -B build-zedbsd-$(NOCT_ZEDBSD_ARCH) $(NOCT_ZEDBSD_CMAKE_OPTIONS) \
		-DNOCT_VERSION='$(ZEDBSD_NOCT_VERSION)'
	cd '$(NOCT_SOURCE_DIR)' && \
		ZEDBSD_SOURCE_DIR='$(ZEDBSD_NOCT_ROOT)' \
		ZEDBSD_SYSROOT='$(NOCT_ZEDBSD_SYSROOT)' \
		ZEDBSD_LLVM_BIN='$(ZEDBSD_LLVM_INSTALL)/bin' \
		cmake --build build-zedbsd-$(NOCT_ZEDBSD_ARCH) --parallel 16
	@test -x '$(NOCT_ZEDBSD_ARTIFACT)'
	@touch '$@'

noct-zedbsd: $(NOCT_ZEDBSD_BUILD_STAMP)

$(NOCT_HOST_ARTIFACT): $(NOCT_HOST_SOURCE_STAMP) \
		| $(NOCT_HOST_SOURCE_VERIFIED)
	@mkdir -p '$(NOCT_HOST_STATE_DIR)'
	cd '$(NOCT_HOST_SOURCE_DIR)' && cmake --preset static \
		$(NOCT_HOST_CMAKE_OPTIONS)
	cd '$(NOCT_HOST_SOURCE_DIR)' && \
		cmake --build --preset static --parallel 16
	@test -x '$@'
	@touch '$(NOCT_HOST_BUILD_STAMP)'

$(NOCT_HOST_BUILD_STAMP): $(NOCT_HOST_ARTIFACT)
	@test -x '$(NOCT_HOST_ARTIFACT)'
	@mkdir -p '$(@D)'
	@touch '$@'

.PHONY: noct-toolchain-smoke
noct-toolchain-smoke: $(NOCT_HOST_BUILD_STAMP) \
		plan/ws010/tests/toolchain-smoke.noct
	$(NOCT) plan/ws010/tests/toolchain-smoke.noct \
		$(abspath build/noct-toolchain-smoke.txt)

ifneq ($(filter amd64 i386,$(ZEDBSD_ARCHITECTURE)),)
$(BUILD)/bin/noct: $(NOCT_ZEDBSD_BUILD_STAMP)
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
