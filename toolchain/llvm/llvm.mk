# Verified LLVM 23.1.0 acquisition, patch, and host-tool installation.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_LLVM_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_LLVM_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_LLVM_MAKEFILE)))
include $(ZEDBSD_LLVM_DIR)/version.mk

ZEDBSD_LLVM_ROOT := $(if $(ZEDBSD_TOPLEVEL_BUILD),$(CURDIR),$(ZEDBSD_REPO_ROOT))
ZEDBSD_LLVM_PACKAGE_ROOT := $(abspath $(ZEDBSD_LLVM_ROOT)/toolchain/llvm)
ZEDBSD_LLVM_DISTDIR := $(ZEDBSD_LLVM_PACKAGE_ROOT)/distfiles
ZEDBSD_LLVM_DISTFILE := $(ZEDBSD_LLVM_DISTDIR)/$(ZEDBSD_LLVM_ARCHIVE_NAME)
ZEDBSD_LLVM_PATCH := $(ZEDBSD_LLVM_PACKAGE_ROOT)/patches/0001-add-zedbsd-x86-target.patch
ZEDBSD_LLVM_SOURCE := $(abspath $(ZEDBSD_LLVM_ROOT)/build/llvm-source)
ZEDBSD_LLVM_SOURCE_STAMP := $(ZEDBSD_LLVM_SOURCE)/.zedbsd-source-$(ZEDBSD_LLVM_VERSION)-$(ZEDBSD_LLVM_PATCH_LEVEL)
ZEDBSD_LLVM_SOURCE_IDENTITY := $(ZEDBSD_LLVM_SOURCE)/.zedbsd-source-identity
ZEDBSD_LLVM_SOURCE_MANIFEST := $(ZEDBSD_LLVM_SOURCE)/.zedbsd-source-manifest
ZEDBSD_LLVM_LICENSE := $(ZEDBSD_LLVM_SOURCE)/LICENSE.TXT
ZEDBSD_LLVM_BUILD := $(abspath $(ZEDBSD_LLVM_ROOT)/build/llvm-build)
ZEDBSD_LLVM_INSTALL := $(abspath $(ZEDBSD_LLVM_ROOT)/build/llvm)
ZEDBSD_LLVM_INSTALL_STAMP := $(ZEDBSD_LLVM_INSTALL)/.zedbsd-install-$(ZEDBSD_LLVM_VERSION)-$(ZEDBSD_LLVM_PATCH_LEVEL)
ZEDBSD_LLVM_DISTRIBUTION_COMPONENTS := clang;clang-resource-headers;lld;llvm-ar;llvm-ranlib;llvm-nm;llvm-objcopy;llvm-objdump;llvm-readelf;llvm-strip
ZEDBSD_LLVM_INSTALLED_TOOL_NAMES := clang clang++ ld.lld lld-link \
	llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-objdump llvm-readelf \
	llvm-strip
ZEDBSD_LLVM_INSTALLED_TOOLS := $(addprefix $(ZEDBSD_LLVM_INSTALL)/bin/,\
	$(ZEDBSD_LLVM_INSTALLED_TOOL_NAMES))
ZEDBSD_LLVM_INSTALLED_LICENSE := \
	$(ZEDBSD_LLVM_INSTALL)/share/licenses/llvm/LICENSE.TXT
ZEDBSD_LLVM_BUILD_PROFILE := x86-release-c4-l2-noanalyzer-noobjcrw-dist
ZEDBSD_LLVM_CONFIG_STAMP := $(ZEDBSD_LLVM_BUILD)/.zedbsd-config-$(ZEDBSD_LLVM_VERSION)-$(ZEDBSD_LLVM_PATCH_LEVEL)-$(ZEDBSD_LLVM_BUILD_PROFILE)
ZEDBSD_LLVM_BUILD_STAMP := $(ZEDBSD_LLVM_BUILD)/.zedbsd-build-$(ZEDBSD_LLVM_VERSION)-$(ZEDBSD_LLVM_PATCH_LEVEL)-$(ZEDBSD_LLVM_BUILD_PROFILE)
ZEDBSD_LLVM_HOST_CC ?= $(if $(HOSTCC),$(HOSTCC),cc)
ZEDBSD_LLVM_HOST_CXX ?= $(if $(HOSTCXX),$(HOSTCXX),c++)
ZEDBSD_LLVM_FETCH ?= curl --fail --location --silent --show-error
ZEDBSD_LLVM_SHA256 ?= sha256sum
ZEDBSD_LLVM_CACHE_URL := https://github.com/awemorris/zedBSD/releases/download/$(ZEDBSD_LLVM_CACHE_TAG)/$(ZEDBSD_LLVM_CACHE_ASSET)
ZEDBSD_LLVM_CACHE_ARCHIVE := $(abspath $(ZEDBSD_LLVM_ROOT)/build/releases/$(ZEDBSD_LLVM_CACHE_ASSET))

define ZEDBSD_LLVM_VERIFY_ARCHIVE_COMMANDS
	archive="$(1)"; \
	if test ! -f "$$archive" || test -L "$$archive"; then \
		echo "LLVM: missing or unsafe archive: $$archive" >&2; exit 1; \
	fi; \
	actual_size=$$(wc -c < "$$archive" | tr -d '[:space:]'); \
	if test "$$actual_size" != '$(ZEDBSD_LLVM_ARCHIVE_SIZE)'; then \
		echo "LLVM: archive size mismatch: expected $(ZEDBSD_LLVM_ARCHIVE_SIZE), got $$actual_size" >&2; exit 1; \
	fi; \
	actual_hash=$$('$(ZEDBSD_LLVM_SHA256)' "$$archive" | awk '{print $$1}'); \
	if test "$$actual_hash" != '$(ZEDBSD_LLVM_ARCHIVE_SHA256)'; then \
		echo "LLVM: archive SHA-256 mismatch" >&2; exit 1; \
	fi; \
	bad_entry=$$(tar -tJf "$$archive" | awk -v root='$(ZEDBSD_LLVM_ARCHIVE_ROOT)' '\
		$$0 != root && $$0 != root "/" && index($$0, root "/") != 1 { print; exit } \
		$$0 ~ /(^|\/)\.\.($$|\/)/ || $$0 ~ /^\// { print; exit }'); \
	if test -n "$$bad_entry"; then \
		echo "LLVM: unsafe archive member: $$bad_entry" >&2; exit 1; \
	fi; \
	bad_type=$$(tar -tvJf "$$archive" | awk 'index("-dlh", substr($$0, 1, 1)) == 0 { print; exit }'); \
	if test -n "$$bad_type"; then \
		echo "LLVM: unsupported archive member type: $$bad_type" >&2; exit 1; \
	fi; \
	bad_link=$$(tar -tvJf "$$archive" | awk -v root='$(ZEDBSD_LLVM_ARCHIVE_ROOT)' '\
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

define ZEDBSD_LLVM_VERIFY_ARCHIVE
	@set -eu; $(call ZEDBSD_LLVM_VERIFY_ARCHIVE_COMMANDS,$(ZEDBSD_LLVM_DISTFILE))
endef

$(ZEDBSD_LLVM_DISTFILE):
	@set -eu; \
	mkdir -p '$(ZEDBSD_LLVM_DISTDIR)'; \
	lock='$@.lock'; temporary=; locked=0; \
	cleanup() { \
		if test -n "$$temporary" && test -f "$$temporary"; then find "$$temporary" -delete; fi; \
		if test "$$locked" = 1 && test -d "$$lock"; then rmdir -- "$$lock"; fi; \
	}; \
	if ! mkdir "$$lock"; then echo "LLVM: archive acquisition already in progress: $@" >&2; exit 1; fi; \
	locked=1; trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp '$(ZEDBSD_LLVM_DISTDIR)/.$(ZEDBSD_LLVM_ARCHIVE_NAME).XXXXXX'); \
	$(ZEDBSD_LLVM_FETCH) --output "$$temporary" '$(ZEDBSD_LLVM_ARCHIVE_URL)'; \
	$(call ZEDBSD_LLVM_VERIFY_ARCHIVE_COMMANDS,$$temporary); \
	mv -- "$$temporary" '$@'; temporary=; rmdir -- "$$lock"; locked=0; \
	trap - EXIT HUP INT TERM

.PHONY: llvm-download
llvm-download: $(ZEDBSD_LLVM_DISTFILE)
	$(ZEDBSD_LLVM_VERIFY_ARCHIVE)

$(ZEDBSD_LLVM_SOURCE_STAMP): $(ZEDBSD_LLVM_PATCH) | llvm-download
	@set -eu; \
	source='$(ZEDBSD_LLVM_SOURCE)'; parent=$${source%/*}; \
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
	temporary=$$(mktemp -d "$$parent/.llvm-$(ZEDBSD_LLVM_VERSION).XXXXXX"); \
	tar -xJf '$(ZEDBSD_LLVM_DISTFILE)' -C "$$temporary"; \
	tree="$$temporary/$(ZEDBSD_LLVM_ARCHIVE_ROOT)"; \
	if test ! -d "$$tree" || test -L "$$tree"; then echo "LLVM: expected archive root is missing" >&2; exit 1; fi; \
	(cd "$$tree" && patch --batch --forward --fuzz=0 -p1 < '$(ZEDBSD_LLVM_PATCH)'); \
	printf '%s\n' \
		'version=$(ZEDBSD_LLVM_VERSION)' \
		'tag=$(ZEDBSD_LLVM_TAG)' \
		'archive-sha256=$(ZEDBSD_LLVM_ARCHIVE_SHA256)' \
		'patch-sha256='$$(sha256sum '$(ZEDBSD_LLVM_PATCH)' | awk '{print $$1}') \
		'patch-level=$(ZEDBSD_LLVM_PATCH_LEVEL)' > "$$tree/.zedbsd-source-identity"; \
	(cd "$$tree" && find . -type f ! -name '.zedbsd-source-*' -print0 | \
		LC_ALL=C sort -z | xargs -0 sha256sum) > "$$tree/.zedbsd-source-manifest"; \
	touch "$$tree/.zedbsd-source-$(ZEDBSD_LLVM_VERSION)-$(ZEDBSD_LLVM_PATCH_LEVEL)"; \
	mv -- "$$tree" "$$source"; rmdir -- "$$temporary"; temporary=; \
	rmdir -- "$$lock"; locked=0; trap - EXIT HUP INT TERM

define ZEDBSD_LLVM_VERIFY_SOURCE_COMMANDS
	source='$(ZEDBSD_LLVM_SOURCE)'; identity='$(ZEDBSD_LLVM_SOURCE_IDENTITY)'; manifest='$(ZEDBSD_LLVM_SOURCE_MANIFEST)'; \
	test -f "$$identity" && test ! -L "$$identity" && test -f "$$manifest" && test ! -L "$$manifest" || \
		{ echo "LLVM: source identity is missing or unsafe" >&2; exit 1; }; \
	expected=$$(printf '%s\n' \
		'version=$(ZEDBSD_LLVM_VERSION)' \
		'tag=$(ZEDBSD_LLVM_TAG)' \
		'archive-sha256=$(ZEDBSD_LLVM_ARCHIVE_SHA256)' \
		'patch-sha256='$$(sha256sum '$(ZEDBSD_LLVM_PATCH)' | awk '{print $$1}') \
		'patch-level=$(ZEDBSD_LLVM_PATCH_LEVEL)'); \
	test "$$(cat "$$identity")" = "$$expected" || { echo "LLVM: source identity mismatch" >&2; exit 1; }; \
	temporary=$$(mktemp "$${source%/*}/.llvm-manifest.XXXXXX"); \
	trap 'find "'"$$temporary"'" -delete' EXIT HUP INT TERM; \
	(cd "$$source" && find . -type f ! -name '.zedbsd-source-*' -print0 | \
		LC_ALL=C sort -z | xargs -0 sha256sum) > "$$temporary"; \
	cmp -s "$$manifest" "$$temporary" || { echo "LLVM: extracted source differs from the verified release plus patch" >&2; exit 1; }; \
	find "$$temporary" -delete; trap - EXIT HUP INT TERM
endef

.PHONY: llvm-source llvm-source-verify
llvm-source: $(ZEDBSD_LLVM_SOURCE_STAMP)
llvm-source-verify: $(ZEDBSD_LLVM_SOURCE_STAMP)
	@set -eu; $(ZEDBSD_LLVM_VERIFY_SOURCE_COMMANDS)

$(ZEDBSD_LLVM_CONFIG_STAMP): $(ZEDBSD_LLVM_MAKEFILE) | llvm-source-verify
	@set -eu; \
	mkdir -p '$(ZEDBSD_LLVM_BUILD)'; \
	host_cc=$$(command -v '$(ZEDBSD_LLVM_HOST_CC)'); \
	host_cxx=$$(command -v '$(ZEDBSD_LLVM_HOST_CXX)'); \
	test -n "$$host_cc" && test -n "$$host_cxx" || \
		{ echo 'LLVM: host C/C++ compiler is unavailable' >&2; exit 1; }; \
	host_cc_version=$$("$$host_cc" --version | sed -n '1p'); \
	host_cxx_version=$$("$$host_cxx" --version | sed -n '1p'); \
	identity="version=$(ZEDBSD_LLVM_VERSION) patch=$(ZEDBSD_LLVM_PATCH_LEVEL) host-cc=$$host_cc ($$host_cc_version) host-cxx=$$host_cxx ($$host_cxx_version) projects=clang,lld targets=X86 build=Release compile-jobs=4 link-jobs=2 analyzer=off objc-rewriter=off distribution=$(ZEDBSD_LLVM_DISTRIBUTION_COMPONENTS)"; \
	if test -f '$(ZEDBSD_LLVM_BUILD)/.zedbsd-config-identity' && \
	   test "$$(cat '$(ZEDBSD_LLVM_BUILD)/.zedbsd-config-identity')" != "$$identity"; then \
		echo 'LLVM: reconfiguring generated build tree for the current bounded-memory profile'; \
	fi; \
	printf '%s\n' "$$identity" > '$(ZEDBSD_LLVM_BUILD)/.zedbsd-config-identity'
	cmake -S '$(ZEDBSD_LLVM_SOURCE)/llvm' -B '$(ZEDBSD_LLVM_BUILD)' -G Ninja \
		-UCLANG_ENABLE_ARCMT \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX='$(ZEDBSD_LLVM_INSTALL)' \
		-DCMAKE_C_COMPILER='$(ZEDBSD_LLVM_HOST_CC)' \
		-DCMAKE_CXX_COMPILER='$(ZEDBSD_LLVM_HOST_CXX)' \
		-DLLVM_ENABLE_PROJECTS='clang;lld' \
		-DLLVM_TARGETS_TO_BUILD=X86 \
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
		-DLLVM_DISTRIBUTION_COMPONENTS='$(ZEDBSD_LLVM_DISTRIBUTION_COMPONENTS)' \
		-DLLVM_PARALLEL_COMPILE_JOBS=4 \
		-DLLVM_PARALLEL_LINK_JOBS=2
	@touch '$@'

.PHONY: llvm-configure llvm-build
llvm-configure: $(ZEDBSD_LLVM_CONFIG_STAMP)

$(ZEDBSD_LLVM_BUILD_STAMP): $(ZEDBSD_LLVM_CONFIG_STAMP) $(ZEDBSD_LLVM_MAKEFILE)
	cmake --build '$(ZEDBSD_LLVM_BUILD)' --target distribution --parallel 16
	@touch '$@'

llvm-build: $(ZEDBSD_LLVM_BUILD_STAMP)

$(ZEDBSD_LLVM_INSTALL_STAMP): $(ZEDBSD_LLVM_MAKEFILE)
	@$(MAKE) --no-print-directory llvm-build
	@set -eu; \
	if test -d '$(ZEDBSD_LLVM_INSTALL)' && \
	   test -n "$$(find '$(ZEDBSD_LLVM_INSTALL)' -mindepth 1 -print -quit)" && \
	   test ! -f '$(ZEDBSD_LLVM_INSTALL)/.zedbsd-install-identity'; then \
		echo 'LLVM: refusing to overwrite an unmanaged build/llvm tree' >&2; exit 1; \
	fi; \
	if test -f '$(ZEDBSD_LLVM_INSTALL)/.zedbsd-install-identity' && \
	   test "$$(cat '$(ZEDBSD_LLVM_INSTALL)/.zedbsd-install-identity')" != 'version=$(ZEDBSD_LLVM_VERSION) patch=$(ZEDBSD_LLVM_PATCH_LEVEL)'; then \
		echo 'LLVM: replacing the recognized generated installation for the new patch identity'; \
		find '$(ZEDBSD_LLVM_INSTALL)' -depth -delete; \
	fi
	cmake --build '$(ZEDBSD_LLVM_BUILD)' --target install-distribution \
		--parallel 1
	@mkdir -p '$(ZEDBSD_LLVM_INSTALL)/share/licenses/llvm'
	@cp '$(ZEDBSD_LLVM_LICENSE)' \
		'$(ZEDBSD_LLVM_INSTALL)/share/licenses/llvm/LICENSE.TXT'
	@printf '%s\n' 'version=$(ZEDBSD_LLVM_VERSION) patch=$(ZEDBSD_LLVM_PATCH_LEVEL)' > '$(ZEDBSD_LLVM_INSTALL)/.zedbsd-install-identity'
	@for tool in $(ZEDBSD_LLVM_INSTALLED_TOOL_NAMES); do \
		test -x '$(ZEDBSD_LLVM_INSTALL)/bin/'"$$tool" || { echo "LLVM: installed tool is missing: $$tool" >&2; exit 1; }; \
	done
	@test -f '$(ZEDBSD_LLVM_INSTALL)/share/licenses/llvm/LICENSE.TXT'
	@touch '$@'

$(ZEDBSD_LLVM_INSTALLED_TOOLS): | $(ZEDBSD_LLVM_INSTALL_STAMP)
	@echo 'LLVM: repairing a missing tool in the generated installation: $(@F)'
	cmake --build '$(ZEDBSD_LLVM_BUILD)' --target install-distribution \
		--parallel 1
	@test -x '$@' || { echo 'LLVM: repair did not restore $(@F)' >&2; exit 1; }

$(ZEDBSD_LLVM_INSTALLED_LICENSE): | $(ZEDBSD_LLVM_INSTALL_STAMP)
	@mkdir -p '$(@D)'
	@cp '$(ZEDBSD_LLVM_LICENSE)' '$@'

.PHONY: llvm-toolchain
llvm-toolchain: $(ZEDBSD_LLVM_INSTALLED_TOOLS) $(ZEDBSD_LLVM_INSTALLED_LICENSE)
	@'$(ZEDBSD_LLVM_INSTALL)/bin/clang' --version | grep -F 'clang version $(ZEDBSD_LLVM_VERSION)'
	@'$(ZEDBSD_LLVM_INSTALL)/bin/ld.lld' --version | grep -F 'LLD $(ZEDBSD_LLVM_VERSION)'

.PHONY: llvm-host-archive
llvm-host-archive: llvm-toolchain
	@set -eu; \
		mkdir -p '$(@D)' '$(dir $(ZEDBSD_LLVM_CACHE_ARCHIVE))'; \
		temporary=$$(mktemp '$(dir $(ZEDBSD_LLVM_CACHE_ARCHIVE)).$(ZEDBSD_LLVM_CACHE_ASSET).XXXXXX'); \
		trap 'find "'"'$$temporary'"'" -delete' EXIT HUP INT TERM; \
		tar -C '$(dir $(ZEDBSD_LLVM_INSTALL))' --sort=name --mtime='@0' \
			--owner=0 --group=0 --numeric-owner -cf - '$(notdir $(ZEDBSD_LLVM_INSTALL))' | \
			gzip -n > "$$temporary"; \
		mv -- "$$temporary" '$(ZEDBSD_LLVM_CACHE_ARCHIVE)'; \
		trap - EXIT HUP INT TERM; \
		'$(ZEDBSD_LLVM_SHA256)' '$(ZEDBSD_LLVM_CACHE_ARCHIVE)'

.PHONY: toolchain-cache
toolchain-cache:
	@set -eu; \
		if test "$$(uname -s)" != Linux || test "$$(uname -m)" != x86_64; then \
			echo 'toolchain-cache: the rev-0 binary cache supports x86_64 Linux only' >&2; exit 1; \
		fi; \
		if test '$(ZEDBSD_LLVM_CACHE_SHA256)' = PENDING; then \
			echo 'toolchain-cache: the release archive digest has not been published' >&2; exit 1; \
		fi; \
		destination='$(ZEDBSD_LLVM_INSTALL)'; \
		if test -f "$$destination/.zedbsd-install-identity" && \
		   test "$$(cat "$$destination/.zedbsd-install-identity")" = \
			'version=$(ZEDBSD_LLVM_VERSION) patch=$(ZEDBSD_LLVM_PATCH_LEVEL)'; then \
			complete=1; \
			for tool in $(ZEDBSD_LLVM_INSTALLED_TOOL_NAMES); do \
				test -x "$$destination/bin/$$tool" || complete=0; \
			done; \
			test -f "$$destination/share/licenses/llvm/LICENSE.TXT" || complete=0; \
			"$$destination/bin/clang" --version 2>/dev/null | \
				grep -F 'clang version $(ZEDBSD_LLVM_VERSION)' >/dev/null || complete=0; \
			"$$destination/bin/ld.lld" --version 2>/dev/null | \
				grep -F 'LLD $(ZEDBSD_LLVM_VERSION)' >/dev/null || complete=0; \
			if test "$$complete" = 1; then \
				touch '$(ZEDBSD_LLVM_INSTALL_STAMP)'; \
				echo 'toolchain-cache: accepted build/llvm is already present'; exit 0; \
			fi; \
		fi; \
		mkdir -p '$(dir $(ZEDBSD_LLVM_CACHE_ARCHIVE))'; \
		archive='$(ZEDBSD_LLVM_CACHE_ARCHIVE)'; download=; temporary=; tree=; \
		cleanup() { \
			if test -n "$$download" && test -f "$$download"; then find "$$download" -delete; fi; \
			if test -n "$$temporary" && test -d "$$temporary"; then find "$$temporary" -depth -delete; fi; \
		}; \
		trap cleanup EXIT HUP INT TERM; \
		if test ! -f "$$archive"; then \
			download=$$(mktemp '$(dir $(ZEDBSD_LLVM_CACHE_ARCHIVE)).$(ZEDBSD_LLVM_CACHE_ASSET).XXXXXX'); \
			$(ZEDBSD_LLVM_FETCH) --output "$$download" '$(ZEDBSD_LLVM_CACHE_URL)'; \
			actual=$$('$(ZEDBSD_LLVM_SHA256)' "$$download" | awk '{print $$1}'); \
			test "$$actual" = '$(ZEDBSD_LLVM_CACHE_SHA256)' || \
				{ echo 'toolchain-cache: downloaded archive SHA-256 mismatch' >&2; exit 1; }; \
			mv -- "$$download" "$$archive"; download=; \
		fi; \
		actual=$$('$(ZEDBSD_LLVM_SHA256)' "$$archive" | awk '{print $$1}'); \
		test "$$actual" = '$(ZEDBSD_LLVM_CACHE_SHA256)' || \
			{ echo 'toolchain-cache: cached archive SHA-256 mismatch' >&2; exit 1; }; \
		bad=$$(tar -tzf "$$archive" | awk '\
			$$0 != "llvm" && $$0 != "llvm/" && index($$0,"llvm/") != 1 { print; exit } \
			$$0 ~ /(^|\/)\.\.($$|\/)/ || $$0 ~ /^\// { print; exit }'); \
		test -z "$$bad" || { echo "toolchain-cache: unsafe archive member: $$bad" >&2; exit 1; }; \
		mkdir -p '$(dir $(ZEDBSD_LLVM_INSTALL))'; \
		temporary=$$(mktemp -d '$(dir $(ZEDBSD_LLVM_INSTALL)).llvm-cache.XXXXXX'); \
		tar -xzf "$$archive" -C "$$temporary" --no-same-owner --no-same-permissions; \
		tree="$$temporary/llvm"; \
		test -f "$$tree/.zedbsd-install-identity" && \
		test "$$(cat "$$tree/.zedbsd-install-identity")" = \
			'version=$(ZEDBSD_LLVM_VERSION) patch=$(ZEDBSD_LLVM_PATCH_LEVEL)' || \
			{ echo 'toolchain-cache: extracted install identity mismatch' >&2; exit 1; }; \
		for tool in $(ZEDBSD_LLVM_INSTALLED_TOOL_NAMES); do \
			test -x "$$tree/bin/$$tool" || \
				{ echo "toolchain-cache: extracted tool is missing: $$tool" >&2; exit 1; }; \
		done; \
		test -f "$$tree/share/licenses/llvm/LICENSE.TXT" || \
			{ echo 'toolchain-cache: LLVM license is missing' >&2; exit 1; }; \
		"$$tree/bin/clang" --version | grep -F 'clang version $(ZEDBSD_LLVM_VERSION)' >/dev/null; \
		"$$tree/bin/ld.lld" --version | grep -F 'LLD $(ZEDBSD_LLVM_VERSION)' >/dev/null; \
		if test -e "$$destination"; then \
			test -f "$$destination/.zedbsd-install-identity" || \
				{ echo 'toolchain-cache: refusing to replace unmanaged build/llvm' >&2; exit 1; }; \
			find "$$destination" -depth -delete; \
		fi; \
		mv -- "$$tree" "$$destination"; tree=; \
		touch '$(ZEDBSD_LLVM_INSTALL_STAMP)'; \
		rmdir -- "$$temporary"; temporary=; trap - EXIT HUP INT TERM; \
		echo 'toolchain-cache: installed verified rev-0 LLVM cache'
