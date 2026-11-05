# Verified LLVM 23.1.0 acquisition, patch, and host-tool installation.
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# Determine the LLVM bin directory.
TARGET_LLVM_BIN := $(abspath build/llvm/bin)

# Determine the triple.
ifeq ($(ARCHITECTURE),x86_64)
TARGET_TRIPLE  := x86_64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),i386)
TARGET_TRIPLE  := i386-unknown-zedbsd
else ifeq ($(ARCHITECTURE),arm64)
TARGET_TRIPLE  := aarch64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),armv7)
TARGET_TRIPLE  := armv7-unknown-zedbsd
else ifeq ($(ARCHITECTURE),riscv64)
TARGET_TRIPLE  := riscv64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),riscv32)
TARGET_TRIPLE  := riscv32-unknown-zedbsd
else ifeq ($(ARCHITECTURE),mips64)
TARGET_TRIPLE  := mips64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),mips32)
TARGET_TRIPLE  := mips-unknown-zedbsd
endif

# Determine the sysroot.
TARGET_SYSROOT := $(abspath build/$(ARCHITECTURE)/sysroot)

# Determine the commands.
TARGET_CLANG := $(TARGET_LLVM_BIN)/clang
CC := $(TARGET_CLANG) --target=$(TARGET_TRIPLE) --sysroot=$(TARGET_SYSROOT)
AS := $(CC)
LD := $(TARGET_LLVM_BIN)/ld.lld
AR := $(TARGET_LLVM_BIN)/llvm-ar
RANLIB := $(TARGET_LLVM_BIN)/llvm-ranlib
NM := $(TARGET_LLVM_BIN)/llvm-nm
OBJCOPY := $(TARGET_LLVM_BIN)/llvm-objcopy
OBJDUMP := $(TARGET_LLVM_BIN)/llvm-objdump
READELF := $(TARGET_LLVM_BIN)/llvm-readelf
STRIP := $(TARGET_LLVM_BIN)/llvm-strip

# Determine the LTO flag.
ifeq ($(KERNEL_LTO),full)
KERNEL_LTO_CFLAGS := -flto
else ifeq ($(KERNEL_LTO),thin)
KERNEL_LTO_CFLAGS := -flto=thin
else ifeq ($(KERNEL_LTO),none)
KERNEL_LTO_CFLAGS :=
else
$(error KERNEL_LTO must be full, thin or none, not '$(KERNEL_LTO)')
endif

# The kernel objects are compiled again when the LTO flags change:
# this content-stable stamp, a prerequisite of each of them, is
# rewritten only when they differ.
KERNEL_LTO_STAMP := $(BUILD)/.kernel-lto
.PHONY: FORCE_KERNEL_LTO
FORCE_KERNEL_LTO:
$(KERNEL_LTO_STAMP): FORCE_KERNEL_LTO
	@mkdir -p $(dir $@)
	@value='lto=$(KERNEL_LTO_CFLAGS)'; \
	if ! test -f $@ || ! grep -Fqx -- "$$value" $@; then \
		printf '%s\n' "$$value" > $@.tmp; \
		mv $@.tmp $@; \
	fi


LLVM_MAKEFILE := $(lastword $(MAKEFILE_LIST))
LLVM_DIR := $(patsubst %/,%,$(dir $(LLVM_MAKEFILE)))
include $(LLVM_DIR)/version.mk

LLVM_ROOT := $(if $(TOPLEVEL_BUILD),$(CURDIR),$(REPO_ROOT))
LLVM_PACKAGE_ROOT := $(abspath $(LLVM_ROOT)/toolchain/llvm)
LLVM_DISTDIR := $(LLVM_PACKAGE_ROOT)/distfiles
LLVM_DISTFILE := $(LLVM_DISTDIR)/$(LLVM_ARCHIVE_NAME)
LLVM_PATCH := $(LLVM_PACKAGE_ROOT)/patches/0001-add-zedbsd-x86-target.patch
LLVM_SOURCE := $(abspath $(LLVM_ROOT)/build/llvm-source)
LLVM_SOURCE_STAMP := $(LLVM_SOURCE)/.zedbsd-source-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)
LLVM_SOURCE_IDENTITY := $(LLVM_SOURCE)/.zedbsd-source-identity
LLVM_SOURCE_MANIFEST := $(LLVM_SOURCE)/.zedbsd-source-manifest

# Each check below records its result in a stamp whose prerequisite is what
# the check examined. A build waits on the record, so an archive or a source
# tree that has not changed since it passed is not examined again.
LLVM_ARCHIVE_VERIFIED := \
	$(LLVM_DISTDIR)/.zedbsd-archive-verified-$(LLVM_ARCHIVE_NAME)
LLVM_SOURCE_VERIFIED := \
	$(LLVM_SOURCE)/.zedbsd-source-verified-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)
LLVM_LICENSE := $(LLVM_SOURCE)/LICENSE.TXT
LLVM_BUILD := $(abspath $(LLVM_ROOT)/build/llvm-build)
LLVM_INSTALL := $(abspath $(LLVM_ROOT)/build/llvm)
LLVM_INSTALL_STAMP := $(LLVM_INSTALL)/.zedbsd-install-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)
LLVM_DISTRIBUTION_COMPONENTS := clang;clang-resource-headers;lld;llvm-ar;llvm-ranlib;llvm-nm;llvm-objcopy;llvm-objdump;llvm-readelf;llvm-strip
LLVM_INSTALLED_TOOL_NAMES := clang clang++ ld.lld lld-link \
	llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-objdump llvm-readelf \
	llvm-strip
LLVM_INSTALLED_TOOLS := $(addprefix $(LLVM_INSTALL)/bin/,\
	$(LLVM_INSTALLED_TOOL_NAMES))
LLVM_INSTALLED_LICENSE := \
	$(LLVM_INSTALL)/share/licenses/llvm/LICENSE.TXT
# How much of the machine the LLVM build may use.  The defaults suit a small
# one: a link of an LLVM tool can need several gigabytes, so a host with
# little memory must not run many at once.  A large host raises them on the
# command line, for example
#
#   make LLVM_COMPILE_JOBS=64 LLVM_LINK_JOBS=8 toolchain
#
# The chosen numbers are part of the build profile, and therefore of the
# configuration stamp, because they are set when the build tree is configured
# rather than when it is built.
include $(LLVM_DIR)/../../build-jobs.mk
LLVM_COMPILE_JOBS ?= $(BUILD_JOBS)
LLVM_LINK_JOBS ?= $(BUILD_LINK_JOBS)
LLVM_BUILD_PROFILE := \
	x86-release-c$(LLVM_COMPILE_JOBS)-l$(LLVM_LINK_JOBS)-noanalyzer-noobjcrw-dist
LLVM_CONFIG_IDENTITY := $(LLVM_BUILD)/.zedbsd-config-identity
LLVM_CONFIG_STAMP := $(LLVM_BUILD)/.zedbsd-config-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)-$(LLVM_BUILD_PROFILE)
LLVM_BUILD_STAMP := $(LLVM_BUILD)/.zedbsd-build-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)-$(LLVM_BUILD_PROFILE)
LLVM_HOST_CC ?= $(if $(HOSTCC),$(HOSTCC),cc)
LLVM_HOST_CXX ?= $(if $(HOSTCXX),$(HOSTCXX),c++)
LLVM_FETCH ?= curl --fail --location --silent --show-error
LLVM_SHA256 ?= sha256sum
LLVM_CACHE_URL := https://github.com/awemorris/zedBSD/releases/download/$(LLVM_CACHE_TAG)/$(LLVM_CACHE_ASSET)
LLVM_CACHE_ARCHIVE := $(abspath $(LLVM_ROOT)/build/releases/$(LLVM_CACHE_ASSET))

# build/llvm, build/llvm-source and build/llvm-build may be links into another
# checkout's build, which is how a second working tree reuses a toolchain it
# did not build.  Reading them is safe.  Writing is not: the other checkout,
# and every tree that links to it, would change underneath (BUG-089).  So a
# rule that is about to write into one of them first asks where the tree
# really is, and stops with the reason when that is outside this checkout.  A
# link that stays inside the checkout (build/llvm -> llvm-zedbsd8) belongs to
# it.  LLVM_ALLOW_FOREIGN=yes is the explicit permission to write into
# a tree elsewhere anyway.
LLVM_ALLOW_FOREIGN ?= no

# $(1) = the tree about to be written, $(2) = what the rule would do to it.
define LLVM_REQUIRE_OWNED
	owned_tree='$(1)'; \
	owned_root=$$(realpath -m -- '$(LLVM_ROOT)'); \
	owned_real=$$(realpath -m -- "$$owned_tree"); \
	case "$$owned_real/" in \
	("$$owned_root"/*) ;; \
	(*) \
		if test '$(LLVM_ALLOW_FOREIGN)' != yes; then \
			echo "LLVM: refusing to $(2) in $$owned_tree:" >&2; \
			echo "LLVM: it resolves to $$owned_real, outside this tree ($$owned_root)," >&2; \
			echo "LLVM: so it belongs to another checkout and is shared with it." >&2; \
			echo "LLVM: Run the toolchain build in the checkout that owns it, remove the" >&2; \
			echo "LLVM: link to build one here, or pass LLVM_ALLOW_FOREIGN=yes." >&2; \
			exit 1; \
		fi ;; \
	esac
endef

# The same question asked while make reads this file, so that a rule which
# could only be refused is defined as the refusal, and nothing is fetched,
# extracted or built first for a write that would then be stopped.
LLVM_OWNED = $(if $(filter yes,$(LLVM_ALLOW_FOREIGN)),yes,$(strip $(shell \
	case "$$(realpath -m -- '$(1)')/" in \
	("$$(realpath -m -- '$(LLVM_ROOT)')"/*) echo yes ;; \
	esac)))
LLVM_SOURCE_OWNED := $(call LLVM_OWNED,$(LLVM_SOURCE))
LLVM_BUILD_OWNED := $(call LLVM_OWNED,$(LLVM_BUILD))
LLVM_INSTALL_OWNED := $(call LLVM_OWNED,$(LLVM_INSTALL))

# A refusal always runs: a stale stamp in a tree elsewhere must not pass for
# an up-to-date one merely because the file is there.
.PHONY: FORCE_LLVM_REFUSAL
FORCE_LLVM_REFUSAL:

# What an extracted source tree records about itself.  A tree whose record
# matches this is the pinned release plus the current patch, whenever and
# wherever it was extracted, so it is judged by these contents rather than by
# how its time compares with the patch file's: a new checkout gives the patch
# a new time without changing a byte of it.
LLVM_PATCH_SHA256 := $(firstword $(shell sha256sum '$(LLVM_PATCH)' 2>/dev/null))
LLVM_SOURCE_IDENTITY_LINES := \
	'version=$(LLVM_VERSION)' \
	'tag=$(LLVM_TAG)' \
	'archive-sha256=$(LLVM_ARCHIVE_SHA256)' \
	'patch-sha256=$(LLVM_PATCH_SHA256)' \
	'patch-level=$(LLVM_PATCH_LEVEL)'
LLVM_SOURCE_ACCEPTED := $(strip $(shell \
	identity='$(LLVM_SOURCE_IDENTITY)'; \
	if test -n '$(LLVM_PATCH_SHA256)' && \
	   test -f '$(LLVM_SOURCE_STAMP)' && test -f "$$identity" && \
	   test "$$(cat "$$identity")" = \
		"$$(printf '%s\n' $(LLVM_SOURCE_IDENTITY_LINES))"; \
	then echo yes; fi))

define LLVM_VERIFY_ARCHIVE_COMMANDS
	archive="$(1)"; \
	if test ! -f "$$archive" || test -L "$$archive"; then \
		echo "LLVM: missing or unsafe archive: $$archive" >&2; exit 1; \
	fi; \
	actual_size=$$(wc -c < "$$archive" | tr -d '[:space:]'); \
	if test "$$actual_size" != '$(LLVM_ARCHIVE_SIZE)'; then \
		echo "LLVM: archive size mismatch: expected $(LLVM_ARCHIVE_SIZE), got $$actual_size" >&2; exit 1; \
	fi; \
	actual_hash=$$('$(LLVM_SHA256)' "$$archive" | awk '{print $$1}'); \
	if test "$$actual_hash" != '$(LLVM_ARCHIVE_SHA256)'; then \
		echo "LLVM: archive SHA-256 mismatch" >&2; exit 1; \
	fi; \
	bad_entry=$$(tar -tJf "$$archive" | awk -v root='$(LLVM_ARCHIVE_ROOT)' '\
		$$0 != root && $$0 != root "/" && index($$0, root "/") != 1 { print; exit } \
		$$0 ~ /(^|\/)\.\.($$|\/)/ || $$0 ~ /^\// { print; exit }'); \
	if test -n "$$bad_entry"; then \
		echo "LLVM: unsafe archive member: $$bad_entry" >&2; exit 1; \
	fi; \
	bad_type=$$(tar -tvJf "$$archive" | awk 'index("-dlh", substr($$0, 1, 1)) == 0 { print; exit }'); \
	if test -n "$$bad_type"; then \
		echo "LLVM: unsupported archive member type: $$bad_type" >&2; exit 1; \
	fi; \
	bad_link=$$(tar -tvJf "$$archive" | awk -v root='$(LLVM_ARCHIVE_ROOT)' '\
		function outside(path, target, base, joined, n, i, part, depth) { \
			if (target ~ /^\//) return 1; \
			if (index(target, root "/") == 1) joined = target; \
			else { base = path; sub(/[^\/]*$$/, "", base); joined = base target; } \
			n = split(joined, part, "/"); depth = 0; \
			for (i = 1; i <= n; ++i) { \
				if (part[i] == "" || part[i] == ".") continue; \
				if (part[i] == "..") { if (--depth < 1) return 1; } else ++depth; \
			} return 0; \
		} \
		substr($$0,1,1) == "l" { if (outside($$6,$$8)) { print; exit } } \
		substr($$0,1,1) == "h" { if (outside($$6,$$9)) { print; exit } }'); \
	if test -n "$$bad_link"; then \
		echo "LLVM: archive link escapes its release root: $$bad_link" >&2; exit 1; \
	fi
endef

define LLVM_VERIFY_ARCHIVE
	@set -eu; $(call LLVM_VERIFY_ARCHIVE_COMMANDS,$(LLVM_DISTFILE))
endef

$(LLVM_DISTFILE):
	@set -eu; \
	mkdir -p '$(LLVM_DISTDIR)'; \
	lock='$@.lock'; temporary=; locked=0; \
	cleanup() { \
		if test -n "$$temporary" && test -f "$$temporary"; then find "$$temporary" -delete; fi; \
		if test "$$locked" = 1 && test -d "$$lock"; then rmdir -- "$$lock"; fi; \
	}; \
	if ! mkdir "$$lock"; then echo "LLVM: archive acquisition already in progress: $@" >&2; exit 1; fi; \
	locked=1; trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp '$(LLVM_DISTDIR)/.$(LLVM_ARCHIVE_NAME).XXXXXX'); \
	$(LLVM_FETCH) --output "$$temporary" '$(LLVM_ARCHIVE_URL)'; \
	$(call LLVM_VERIFY_ARCHIVE_COMMANDS,$$temporary); \
	mv -- "$$temporary" '$@'; temporary=; rmdir -- "$$lock"; locked=0; \
	trap - EXIT HUP INT TERM

.PHONY: llvm-download
llvm-download: $(LLVM_DISTFILE)
	$(LLVM_VERIFY_ARCHIVE)
	@touch '$(LLVM_ARCHIVE_VERIFIED)'

# The record is older than the archive whenever the file is replaced, so a
# new archive is checked and an unchanged one is not decompressed again.
$(LLVM_ARCHIVE_VERIFIED): $(LLVM_DISTFILE)
	$(LLVM_VERIFY_ARCHIVE)
	@touch '$@'

.PHONY: FORCE_LLVM_SOURCE
FORCE_LLVM_SOURCE:

# An accepted tree is used as it is, and needs neither the archive nor a
# fresh extraction.  Any other tree is replaced from the verified archive.
ifeq ($(LLVM_SOURCE_ACCEPTED),yes)
$(LLVM_SOURCE_STAMP):
	@test -f '$@'
else ifneq ($(LLVM_SOURCE_OWNED),yes)
$(LLVM_SOURCE_STAMP): FORCE_LLVM_REFUSAL
	@echo 'LLVM: $(LLVM_SOURCE) does not hold LLVM $(LLVM_VERSION) with the current patch.' >&2
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_SOURCE),extract the LLVM source)
else
$(LLVM_SOURCE_STAMP): FORCE_LLVM_SOURCE \
		| $(LLVM_ARCHIVE_VERIFIED)
	@set -eu; \
	source='$(LLVM_SOURCE)'; parent=$${source%/*}; \
	if test -e "$$source"; then \
		if test -d "$$source" && test ! -L "$$source" && \
		   test -f "$$source/.zedbsd-source-identity" && \
		   test -f "$$source/.zedbsd-source-manifest" && \
		   grep -Eq '^version=[0-9]+\.[0-9]+\.[0-9]+$$' "$$source/.zedbsd-source-identity" && \
		   grep -Eq '^archive-sha256=[0-9a-f]{64}$$' "$$source/.zedbsd-source-identity" && \
		   grep -Eq '^patch-level=zedbsd[0-9]+$$' "$$source/.zedbsd-source-identity" && \
		   find "$$source" -maxdepth 1 -type f -name '.zedbsd-source-*' \
			! -name '.zedbsd-source-identity' ! -name '.zedbsd-source-manifest' \
			-print -quit | grep . >/dev/null; then \
			find "$$source" -depth -delete; \
		else \
			echo "LLVM: refusing to replace an unrecognized source tree: $$source" >&2; exit 1; \
		fi; \
	fi; \
	mkdir -p "$$parent"; lock="$$source.lock"; temporary=; locked=0; \
	cleanup() { \
		if test -n "$$temporary" && test -d "$$temporary"; then find "$$temporary" -depth -delete; fi; \
		if test "$$locked" = 1 && test -d "$$lock"; then rmdir -- "$$lock"; fi; \
	}; \
	if ! mkdir "$$lock"; then echo "LLVM: source extraction already in progress: $$source" >&2; exit 1; fi; \
	locked=1; trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.llvm-$(LLVM_VERSION).XXXXXX"); \
	tar -xJf '$(LLVM_DISTFILE)' -C "$$temporary"; \
	tree="$$temporary/$(LLVM_ARCHIVE_ROOT)"; \
	if test ! -d "$$tree" || test -L "$$tree"; then echo "LLVM: expected archive root is missing" >&2; exit 1; fi; \
	(cd "$$tree" && patch --batch --forward --fuzz=0 -p1 < '$(LLVM_PATCH)'); \
	printf '%s\n' \
		'version=$(LLVM_VERSION)' \
		'tag=$(LLVM_TAG)' \
		'archive-sha256=$(LLVM_ARCHIVE_SHA256)' \
		'patch-sha256='$$(sha256sum '$(LLVM_PATCH)' | awk '{print $$1}') \
		'patch-level=$(LLVM_PATCH_LEVEL)' > "$$tree/.zedbsd-source-identity"; \
	(cd "$$tree" && find . -type f ! -name '.zedbsd-source-*' -print0 | \
		LC_ALL=C sort -z | xargs -0 sha256sum) > "$$tree/.zedbsd-source-manifest"; \
	touch "$$tree/.zedbsd-source-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)"; \
	mv -- "$$tree" "$$source"; rmdir -- "$$temporary"; temporary=; \
	rmdir -- "$$lock"; locked=0; trap - EXIT HUP INT TERM
endif

define LLVM_VERIFY_SOURCE_COMMANDS
	source='$(LLVM_SOURCE)'; identity='$(LLVM_SOURCE_IDENTITY)'; manifest='$(LLVM_SOURCE_MANIFEST)'; \
	test -f "$$identity" && test ! -L "$$identity" && test -f "$$manifest" && test ! -L "$$manifest" || \
		{ echo "LLVM: source identity is missing or unsafe" >&2; exit 1; }; \
	expected=$$(printf '%s\n' \
		'version=$(LLVM_VERSION)' \
		'tag=$(LLVM_TAG)' \
		'archive-sha256=$(LLVM_ARCHIVE_SHA256)' \
		'patch-sha256='$$(sha256sum '$(LLVM_PATCH)' | awk '{print $$1}') \
		'patch-level=$(LLVM_PATCH_LEVEL)'); \
	test "$$(cat "$$identity")" = "$$expected" || { echo "LLVM: source identity mismatch" >&2; exit 1; }; \
	temporary=$$(mktemp "$${source%/*}/.llvm-manifest.XXXXXX"); \
	trap 'find "'"$$temporary"'" -delete' EXIT HUP INT TERM; \
	(cd "$$source" && find . -type f ! -name '.zedbsd-source-*' -print0 | \
		LC_ALL=C sort -z | xargs -0 sha256sum) > "$$temporary"; \
	cmp -s "$$manifest" "$$temporary" || { echo "LLVM: extracted source differs from the verified release plus patch" >&2; exit 1; }; \
	find "$$temporary" -delete; trap - EXIT HUP INT TERM
endef

.PHONY: llvm-source llvm-source-verify
llvm-source: $(LLVM_SOURCE_STAMP)
llvm-source-verify: $(LLVM_SOURCE_STAMP)
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_SOURCE),record a verification)
	@set -eu; $(LLVM_VERIFY_SOURCE_COMMANDS)
	@touch '$(LLVM_SOURCE_VERIFIED)'

# Hashing every extracted file is slow, so the build waits on the record of
# that check and repeats it only after a fresh extraction.
$(LLVM_SOURCE_VERIFIED): $(LLVM_SOURCE_STAMP)
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_SOURCE),record a verification)
	@set -eu; $(LLVM_VERIFY_SOURCE_COMMANDS)
	@touch '$@'

.PHONY: FORCE_LLVM_CONFIG_IDENTITY
FORCE_LLVM_CONFIG_IDENTITY:

# Everything the generated build tree depends on, in one line.
define LLVM_CONFIG_IDENTITY_COMMANDS
	host_cc=$$(command -v '$(LLVM_HOST_CC)'); \
	host_cxx=$$(command -v '$(LLVM_HOST_CXX)'); \
	test -n "$$host_cc" && test -n "$$host_cxx" || \
		{ echo 'LLVM: host C/C++ compiler is unavailable' >&2; exit 1; }; \
	host_cc_version=$$("$$host_cc" --version | sed -n '1p'); \
	host_cxx_version=$$("$$host_cxx" --version | sed -n '1p'); \
	identity="version=$(LLVM_VERSION) patch=$(LLVM_PATCH_LEVEL) host-cc=$$host_cc ($$host_cc_version) host-cxx=$$host_cxx ($$host_cxx_version) projects=clang,lld targets=PowerPC,AArch64,RISCV,X86,Mips,ARM build=Release compile-jobs=4 link-jobs=2 analyzer=off objc-rewriter=off distribution=$(LLVM_DISTRIBUTION_COMPONENTS)"
endef

# The record is compared while make reads this file, so a tree configured
# with the same identity is left alone, and make -n does not report a
# reconfiguration that the build would not do.
LLVM_CONFIG_ACCEPTED := $(strip $(shell \
	$(LLVM_CONFIG_IDENTITY_COMMANDS); \
	if test -f '$(LLVM_CONFIG_IDENTITY)' && \
	   test "$$(cat '$(LLVM_CONFIG_IDENTITY)')" = "$$identity"; \
	then echo yes; fi 2>/dev/null))

# The record is rewritten only when it differs from what the tree was
# configured with, so the stamps below see a new file exactly when cmake has
# to run again, and not merely because a line of this makefile was edited.
ifeq ($(LLVM_CONFIG_ACCEPTED),yes)
$(LLVM_CONFIG_IDENTITY):
	@test -f '$@'
else ifneq ($(LLVM_BUILD_OWNED),yes)
$(LLVM_CONFIG_IDENTITY): FORCE_LLVM_REFUSAL
	@echo 'LLVM: $(LLVM_BUILD) was configured for another LLVM, patch level or host compiler.' >&2
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_BUILD),configure the LLVM build tree)
else
$(LLVM_CONFIG_IDENTITY): FORCE_LLVM_CONFIG_IDENTITY
	@set -eu; \
	$(LLVM_CONFIG_IDENTITY_COMMANDS); \
	if test -f '$@' && test "$$(cat '$@')" = "$$identity"; then exit 0; fi; \
	mkdir -p '$(LLVM_BUILD)'; \
	if test -f '$@'; then \
		echo 'LLVM: reconfiguring generated build tree for the current bounded-memory profile'; \
	fi; \
	printf '%s\n' "$$identity" > '$@.tmp'; \
	mv -- '$@.tmp' '$@'
endif

# A build tree elsewhere is only read, through the accepted records above;
# configuring or building in it is refused.
ifneq ($(LLVM_BUILD_OWNED),yes)
$(LLVM_CONFIG_STAMP): FORCE_LLVM_REFUSAL
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_BUILD),configure the LLVM build tree)

$(LLVM_BUILD_STAMP): FORCE_LLVM_REFUSAL
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_BUILD),build LLVM)
else
$(LLVM_CONFIG_STAMP): $(LLVM_CONFIG_IDENTITY) \
		| $(LLVM_SOURCE_VERIFIED)
	cmake -S '$(LLVM_SOURCE)/llvm' -B '$(LLVM_BUILD)' -G Ninja \
		-UCLANG_ENABLE_ARCMT \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX='$(LLVM_INSTALL)' \
		-DCMAKE_C_COMPILER='$(LLVM_HOST_CC)' \
		-DCMAKE_CXX_COMPILER='$(LLVM_HOST_CXX)' \
		-DLLVM_ENABLE_PROJECTS='clang;lld;lldb' \
		-DLLDB_ENABLE_PYTHON=OFF \
		-DLLDB_ENABLE_LUA=OFF \
		-DLLDB_ENABLE_LIBEDIT=OFF \
		-DLLDB_ENABLE_CURSES=OFF \
		-DLLDB_ENABLE_LZMA=OFF \
		-DLLDB_ENABLE_LIBXML2=OFF \
		-DLLDB_INCLUDE_TESTS=OFF \
		-DLLVM_TARGETS_TO_BUILD='PowerPC;AArch64;RISCV;X86;Mips;ARM' \
		-DLLVM_ENABLE_TERMINFO=OFF \
		-DLLVM_ENABLE_ZLIB=OFF \
		-DLLVM_ENABLE_ZSTD=OFF \
		-DLLVM_ENABLE_LIBXML2=OFF \
		-DLLVM_ENABLE_LIBEDIT=OFF \
		-DLLVM_ENABLE_BINDINGS=OFF \
		-DCLANG_ENABLE_STATIC_ANALYZER=OFF \
		-DCLANG_ENABLE_OBJC_REWRITER=OFF \
		-DLLVM_INCLUDE_EXAMPLES=OFF \
		-DLLVM_INCLUDE_BENCHMARKS=OFF \
		-DLLVM_INCLUDE_TESTS=ON \
		-DLLVM_BUILD_TOOLS=ON \
		-DLLVM_INSTALL_UTILS=ON \
		-DLLVM_DISTRIBUTION_COMPONENTS='$(LLVM_DISTRIBUTION_COMPONENTS)' \
		-DLLVM_PARALLEL_COMPILE_JOBS=$(LLVM_COMPILE_JOBS) \
		-DLLVM_PARALLEL_LINK_JOBS=$(LLVM_LINK_JOBS)
	@touch '$@'

$(LLVM_BUILD_STAMP): $(LLVM_CONFIG_STAMP) \
		$(LLVM_CONFIG_IDENTITY)
	cmake --build '$(LLVM_BUILD)' --target distribution \
		--parallel $(BUILD_JOBS)
	cmake --build '$(LLVM_BUILD)' --target lldb-tblgen \
		--parallel $(BUILD_JOBS)
	@touch '$@'
endif

.PHONY: llvm-configure llvm-build
llvm-configure: $(LLVM_CONFIG_STAMP)
llvm-build: $(LLVM_BUILD_STAMP)

# The generators the cross build of the clang package runs on the host.  A
# source build of the toolchain makes them; with the binary cache (which holds
# only the installed tools) this builds just them, a small part of LLVM.
LLVM_NATIVE_TOOLS := llvm-min-tblgen llvm-tblgen clang-tblgen lldb-tblgen
LLVM_NATIVE_STAMP := $(LLVM_BUILD)/.zedbsd-native-tools

# Generators built after the build tree was last configured for this LLVM
# release, patch level and host compiler are the right ones, whichever
# checkout built them and however many jobs it used: the job counts name the
# configuration stamp but do not change what the generators do.
LLVM_NATIVE_ACCEPTED := $(strip $(shell \
	test '$(LLVM_CONFIG_ACCEPTED)' = yes || exit 0; \
	test -f '$(LLVM_NATIVE_STAMP)' || exit 0; \
	test ! '$(LLVM_NATIVE_STAMP)' -ot '$(LLVM_CONFIG_IDENTITY)' || exit 0; \
	for tool in $(LLVM_NATIVE_TOOLS); do \
		test -x '$(LLVM_BUILD)/bin/'"$$tool" || exit 0; \
	done; \
	echo yes))

ifeq ($(LLVM_NATIVE_ACCEPTED),yes)
$(LLVM_NATIVE_STAMP):
	@test -f '$@'
else ifneq ($(LLVM_BUILD_OWNED),yes)
$(LLVM_NATIVE_STAMP): FORCE_LLVM_REFUSAL
	@echo 'LLVM: $(LLVM_BUILD) holds no host generators for this configuration.' >&2
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_BUILD),build the host generators)
else
$(LLVM_NATIVE_STAMP): $(LLVM_CONFIG_STAMP) \
		$(LLVM_CONFIG_IDENTITY)
	cmake --build '$(LLVM_BUILD)' --target $(LLVM_NATIVE_TOOLS) \
		--parallel $(BUILD_JOBS)
	@touch '$@'
endif

.PHONY: llvm-native-tools
llvm-native-tools: $(LLVM_NATIVE_STAMP)

# An installation that already carries this version and patch identity is the
# accepted result, however it got there: a source build, or the verified binary
# cache. Recording that here is what keeps `make toolchain-cache` meaningful --
# otherwise the stamp's prerequisite (a configured build tree the cache never
# creates) sends every later target through a full LLVM source build.
LLVM_INSTALL_ACCEPTED := $(strip $(shell \
	if test -f '$(LLVM_INSTALL)/.zedbsd-install-identity' && \
	   test "$$(cat '$(LLVM_INSTALL)/.zedbsd-install-identity')" = \
		'version=$(LLVM_VERSION) patch=$(LLVM_PATCH_LEVEL)'; \
	then echo yes; fi))

ifeq ($(LLVM_INSTALL_ACCEPTED),yes)
$(LLVM_INSTALL_STAMP):
	@for tool in $(LLVM_INSTALLED_TOOL_NAMES); do \
		test -x '$(LLVM_INSTALL)/bin/'"$$tool" || { \
			echo "LLVM: accepted installation is missing a tool: $$tool" >&2; \
			exit 1; \
		}; \
	done
	@test -f '$(LLVM_INSTALL)/share/licenses/llvm/LICENSE.TXT' || { \
		echo 'LLVM: accepted installation is missing its license' >&2; exit 1; }
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_INSTALL),record the accepted installation)
	@touch '$@'
else ifneq ($(LLVM_INSTALL_OWNED)$(LLVM_BUILD_OWNED),yesyes)
# A toolchain is missing or stale where build/llvm points, and it would be
# installed into another checkout's tree, or from another checkout's build
# tree (whose install prefix is that checkout's build/llvm).  Say so before
# anything is extracted, configured or built for an installation that would
# then be refused.
$(LLVM_INSTALL_STAMP): FORCE_LLVM_REFUSAL
	@echo 'LLVM: $(LLVM_INSTALL) holds no toolchain for version=$(LLVM_VERSION) patch=$(LLVM_PATCH_LEVEL).' >&2
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_INSTALL),install the LLVM toolchain)
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_BUILD),install from the build tree)
else
# The source is extracted here, before the sub-make starts: the sysroot's
# builtins also need it, and two makes extracting one tree at once fail on
# its lock (a fresh -j build, as in CI).
$(LLVM_INSTALL_STAMP): $(LLVM_CONFIG_IDENTITY) \
		| $(LLVM_SOURCE_VERIFIED)
	@$(MAKE) --no-print-directory llvm-build
	@set -eu; \
	if test -d '$(LLVM_INSTALL)' && \
	   test -n "$$(find '$(LLVM_INSTALL)' -mindepth 1 -print -quit)" && \
	   test ! -f '$(LLVM_INSTALL)/.zedbsd-install-identity'; then \
		echo 'LLVM: refusing to overwrite an unmanaged build/llvm tree' >&2; exit 1; \
	fi; \
	if test -f '$(LLVM_INSTALL)/.zedbsd-install-identity' && \
	   test "$$(cat '$(LLVM_INSTALL)/.zedbsd-install-identity')" != 'version=$(LLVM_VERSION) patch=$(LLVM_PATCH_LEVEL)'; then \
		echo 'LLVM: replacing the recognized generated installation for the new patch identity'; \
		find '$(LLVM_INSTALL)' -depth -delete; \
	fi
	cmake --build '$(LLVM_BUILD)' --target install-distribution \
		--parallel $(BUILD_JOBS)
	@mkdir -p '$(LLVM_INSTALL)/share/licenses/llvm'
	@cp '$(LLVM_LICENSE)' \
		'$(LLVM_INSTALL)/share/licenses/llvm/LICENSE.TXT'
	@printf '%s\n' 'version=$(LLVM_VERSION) patch=$(LLVM_PATCH_LEVEL)' > '$(LLVM_INSTALL)/.zedbsd-install-identity'
	@for tool in $(LLVM_INSTALLED_TOOL_NAMES); do \
		test -x '$(LLVM_INSTALL)/bin/'"$$tool" || { echo "LLVM: installed tool is missing: $$tool" >&2; exit 1; }; \
	done
	@test -f '$(LLVM_INSTALL)/share/licenses/llvm/LICENSE.TXT'
	@touch '$@'
endif

$(LLVM_INSTALLED_TOOLS): | $(LLVM_INSTALL_STAMP)
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_INSTALL),repair the LLVM toolchain)
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_BUILD),repair the toolchain from the build tree)
	@echo 'LLVM: repairing a missing tool in the generated installation: $(@F)'
	cmake --build '$(LLVM_BUILD)' --target install-distribution \
		--parallel $(BUILD_JOBS)
	@test -x '$@' || { echo 'LLVM: repair did not restore $(@F)' >&2; exit 1; }

$(LLVM_INSTALLED_LICENSE): | $(LLVM_INSTALL_STAMP)
	@set -eu; $(call LLVM_REQUIRE_OWNED,$(LLVM_INSTALL),repair the LLVM toolchain)
	@mkdir -p '$(@D)'
	@cp '$(LLVM_LICENSE)' '$@'

.PHONY: llvm-toolchain
llvm-toolchain: $(LLVM_INSTALLED_TOOLS) $(LLVM_INSTALLED_LICENSE)
	@'$(LLVM_INSTALL)/bin/clang' --version | grep -F 'clang version $(LLVM_VERSION)'
	@'$(LLVM_INSTALL)/bin/ld.lld' --version | grep -F 'LLD $(LLVM_VERSION)'

.PHONY: llvm-host-archive
# The archive's modes are made owner-writable, so a locked shared tree
# (plan/tools/toolchain-lock.sh makes its directories read-only) does not
# leave read-only directories that the cache's extraction cannot move.
llvm-host-archive: llvm-toolchain
	@set -eu; \
		mkdir -p '$(@D)' '$(dir $(LLVM_CACHE_ARCHIVE))'; \
		temporary=$$(mktemp '$(dir $(LLVM_CACHE_ARCHIVE)).$(LLVM_CACHE_ASSET).XXXXXX'); \
		trap 'find "'"'$$temporary'"'" -delete' EXIT HUP INT TERM; \
		tar -C '$(dir $(LLVM_INSTALL))' --sort=name --mtime='@0' \
			--owner=0 --group=0 --numeric-owner --mode='u+w' \
			-cf - '$(notdir $(LLVM_INSTALL))' | \
			gzip -n > "$$temporary"; \
		mv -- "$$temporary" '$(LLVM_CACHE_ARCHIVE)'; \
		trap - EXIT HUP INT TERM; \
		'$(LLVM_SHA256)' '$(LLVM_CACHE_ARCHIVE)'

.PHONY: toolchain-cache
toolchain-cache:
	@set -eu; \
		if test "$$(uname -s)" != Linux || test "$$(uname -m)" != x86_64; then \
			echo 'toolchain-cache: the rev-0 binary cache supports x86_64 Linux only' >&2; exit 1; \
		fi; \
		if test '$(LLVM_CACHE_SHA256)' = PENDING; then \
			echo 'toolchain-cache: the release archive digest has not been published' >&2; exit 1; \
		fi; \
		destination='$(LLVM_INSTALL)'; \
		if test -f "$$destination/.zedbsd-install-identity" && \
		   test "$$(cat "$$destination/.zedbsd-install-identity")" = \
			'version=$(LLVM_VERSION) patch=$(LLVM_PATCH_LEVEL)'; then \
			complete=1; \
			for tool in $(LLVM_INSTALLED_TOOL_NAMES); do \
				test -x "$$destination/bin/$$tool" || complete=0; \
			done; \
			test -f "$$destination/share/licenses/llvm/LICENSE.TXT" || complete=0; \
			"$$destination/bin/clang" --version 2>/dev/null | \
				grep -F 'clang version $(LLVM_VERSION)' >/dev/null || complete=0; \
			"$$destination/bin/ld.lld" --version 2>/dev/null | \
				grep -F 'LLD $(LLVM_VERSION)' >/dev/null || complete=0; \
			if test "$$complete" = 1; then \
				if test ! -f '$(LLVM_INSTALL_STAMP)'; then \
					$(call LLVM_REQUIRE_OWNED,$(LLVM_INSTALL),record the accepted installation); \
					touch '$(LLVM_INSTALL_STAMP)'; \
				fi; \
				echo 'toolchain-cache: accepted build/llvm is already present'; exit 0; \
			fi; \
		fi; \
		$(call LLVM_REQUIRE_OWNED,$(LLVM_INSTALL),install the LLVM toolchain cache); \
		mkdir -p '$(dir $(LLVM_CACHE_ARCHIVE))'; \
		archive='$(LLVM_CACHE_ARCHIVE)'; download=; temporary=; tree=; \
		cleanup() { \
			if test -n "$$download" && test -f "$$download"; then find "$$download" -delete; fi; \
			if test -n "$$temporary" && test -d "$$temporary"; then find "$$temporary" -depth -delete; fi; \
		}; \
		trap cleanup EXIT HUP INT TERM; \
		if test ! -f "$$archive"; then \
			download=$$(mktemp '$(dir $(LLVM_CACHE_ARCHIVE)).$(LLVM_CACHE_ASSET).XXXXXX'); \
			$(LLVM_FETCH) --output "$$download" '$(LLVM_CACHE_URL)'; \
			actual=$$('$(LLVM_SHA256)' "$$download" | awk '{print $$1}'); \
			test "$$actual" = '$(LLVM_CACHE_SHA256)' || \
				{ echo 'toolchain-cache: downloaded archive SHA-256 mismatch' >&2; exit 1; }; \
			mv -- "$$download" "$$archive"; download=; \
		fi; \
		actual=$$('$(LLVM_SHA256)' "$$archive" | awk '{print $$1}'); \
		test "$$actual" = '$(LLVM_CACHE_SHA256)' || \
			{ echo 'toolchain-cache: cached archive SHA-256 mismatch' >&2; exit 1; }; \
		bad=$$(tar -tzf "$$archive" | awk '\
			$$0 != "llvm" && $$0 != "llvm/" && index($$0,"llvm/") != 1 { print; exit } \
			$$0 ~ /(^|\/)\.\.($$|\/)/ || $$0 ~ /^\// { print; exit }'); \
		test -z "$$bad" || { echo "toolchain-cache: unsafe archive member: $$bad" >&2; exit 1; }; \
		mkdir -p '$(dir $(LLVM_INSTALL))'; \
		temporary=$$(mktemp -d '$(dir $(LLVM_INSTALL)).llvm-cache.XXXXXX'); \
		tar -xzf "$$archive" -C "$$temporary" --no-same-owner --no-same-permissions; \
		tree="$$temporary/llvm"; \
		test -f "$$tree/.zedbsd-install-identity" && \
		test "$$(cat "$$tree/.zedbsd-install-identity")" = \
			'version=$(LLVM_VERSION) patch=$(LLVM_PATCH_LEVEL)' || \
			{ echo 'toolchain-cache: extracted install identity mismatch' >&2; exit 1; }; \
		for tool in $(LLVM_INSTALLED_TOOL_NAMES); do \
			test -x "$$tree/bin/$$tool" || \
				{ echo "toolchain-cache: extracted tool is missing: $$tool" >&2; exit 1; }; \
		done; \
		test -f "$$tree/share/licenses/llvm/LICENSE.TXT" || \
			{ echo 'toolchain-cache: LLVM license is missing' >&2; exit 1; }; \
		"$$tree/bin/clang" --version | grep -F 'clang version $(LLVM_VERSION)' >/dev/null; \
		"$$tree/bin/ld.lld" --version | grep -F 'LLD $(LLVM_VERSION)' >/dev/null; \
		if test -e "$$destination"; then \
			test -f "$$destination/.zedbsd-install-identity" || \
				{ echo 'toolchain-cache: refusing to replace unmanaged build/llvm' >&2; exit 1; }; \
			find "$$destination" -depth -delete; \
		fi; \
		mv -- "$$tree" "$$destination"; tree=; \
		touch '$(LLVM_INSTALL_STAMP)'; \
		rmdir -- "$$temporary"; temporary=; trap - EXIT HUP INT TERM; \
		echo 'toolchain-cache: installed verified rev-0 LLVM cache'
