# zedBSD optional Intel i915 display DMC firmware package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

override I915_FIRMWARE_FIXTURE_GOAL := i915-firmware-fixture-cache
ifneq ($(filter $(I915_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(I915_FIRMWARE_FIXTURE_GOAL))
$(error $(I915_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
I915_FIRMWARE_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
I915_FIRMWARE_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
I915_FIRMWARE_URL_SUFFIX ?=
I915_FIRMWARE_ADLP_DMC_SIZE ?= 79088
I915_FIRMWARE_ADLP_DMC_SHA256 ?= 3516de2e134ddcf3b319c75d2e437779fecbd58cbb77234bd6f297c544e92ccb
I915_FIRMWARE_TGL_DMC_SIZE ?= 19760
I915_FIRMWARE_TGL_DMC_SHA256 ?= 3c013ef0ad96ba73aee8e5bd04a8e27cc9b1c6e9183b1a83ce124485f325afca
I915_FIRMWARE_ADLP_FALLBACK_DMC_SIZE ?= 77084
I915_FIRMWARE_ADLP_FALLBACK_DMC_SHA256 ?= 2da482ea46a40e54c9ca3b54185959177f393eff98ece21acdac7eb6cacb0fcb
I915_FIRMWARE_ADLS_DMC_SIZE ?= 18704
I915_FIRMWARE_ADLS_DMC_SHA256 ?= 96e426ed7e80f0cc6644ada1928f4d02b163ff59b14b2339aa28a374d65a5b19
I915_FIRMWARE_ICL_DMC_SIZE ?= 25952
I915_FIRMWARE_ICL_DMC_SHA256 ?= aafad1967679baa36e971e5331da73eea31c68239416432df83e71332755dc01
I915_FIRMWARE_RKL_DMC_SIZE ?= 18476
I915_FIRMWARE_RKL_DMC_SHA256 ?= 139ce0eb873a13679a4685463cdacf6d247073ce286c8a5bca443ef1d17ee2ac
I915_FIRMWARE_LICENSE_SIZE ?= 2080
I915_FIRMWARE_LICENSE_SHA256 ?= 8542aeabf2761935122d693561e16766ce1bcc2b0d003204f9040b7d6d929f2e
I915_FIRMWARE_WHENCE_SIZE ?= 425450
I915_FIRMWARE_WHENCE_SHA256 ?= c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
I915_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
I915_FIRMWARE_LICENSE_PROMPT ?= n
I915_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.  The package holds every DMC program the
# driver names: the platforms' own (display-tables-xd.h) and the older Alder
# Lake-P one it falls back to (dmc-shell.c).  The Alder Lake-P and Tiger Lake
# bytes are the ones the driver was brought up with (the former embedded
# reference blobs).
override I915_FIRMWARE_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override I915_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override I915_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override I915_FIRMWARE_ADLP_DMC_SIZE := 79088
override I915_FIRMWARE_ADLP_DMC_SHA256 := 3516de2e134ddcf3b319c75d2e437779fecbd58cbb77234bd6f297c544e92ccb
override I915_FIRMWARE_TGL_DMC_SIZE := 19760
override I915_FIRMWARE_TGL_DMC_SHA256 := 3c013ef0ad96ba73aee8e5bd04a8e27cc9b1c6e9183b1a83ce124485f325afca
override I915_FIRMWARE_ADLP_FALLBACK_DMC_SIZE := 77084
override I915_FIRMWARE_ADLP_FALLBACK_DMC_SHA256 := 2da482ea46a40e54c9ca3b54185959177f393eff98ece21acdac7eb6cacb0fcb
override I915_FIRMWARE_ADLS_DMC_SIZE := 18704
override I915_FIRMWARE_ADLS_DMC_SHA256 := 96e426ed7e80f0cc6644ada1928f4d02b163ff59b14b2339aa28a374d65a5b19
override I915_FIRMWARE_ICL_DMC_SIZE := 25952
override I915_FIRMWARE_ICL_DMC_SHA256 := aafad1967679baa36e971e5331da73eea31c68239416432df83e71332755dc01
override I915_FIRMWARE_RKL_DMC_SIZE := 18476
override I915_FIRMWARE_RKL_DMC_SHA256 := 139ce0eb873a13679a4685463cdacf6d247073ce286c8a5bca443ef1d17ee2ac
override I915_FIRMWARE_LICENSE_SIZE := 2080
override I915_FIRMWARE_LICENSE_SHA256 := 8542aeabf2761935122d693561e16766ce1bcc2b0d003204f9040b7d6d929f2e
override I915_FIRMWARE_WHENCE_SIZE := 425450
override I915_FIRMWARE_WHENCE_SHA256 := c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
override I915_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/i915)
override I915_FIRMWARE_LICENSE_PROMPT := y
override I915_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override I915_FIRMWARE_TAG := 20260410
override I915_FIRMWARE_TAG_OBJECT := 4585dd5a5f0cee08990d754701d8866d9e9266e6
override I915_FIRMWARE_ADLP_DMC_PATH := i915/adlp_dmc.bin
override I915_FIRMWARE_TGL_DMC_PATH := i915/tgl_dmc_ver2_12.bin
override I915_FIRMWARE_ADLP_FALLBACK_DMC_PATH := i915/adlp_dmc_ver2_16.bin
override I915_FIRMWARE_ADLS_DMC_PATH := i915/adls_dmc_ver2_01.bin
override I915_FIRMWARE_ICL_DMC_PATH := i915/icl_dmc_ver1_09.bin
override I915_FIRMWARE_RKL_DMC_PATH := i915/rkl_dmc_ver2_03.bin
override I915_FIRMWARE_LICENSE_PATH := LICENSE.i915
override I915_FIRMWARE_WHENCE_PATH := WHENCE
override I915_FIRMWARE_VERSION := 20260410
override I915_FIRMWARE_SHA256_COMMAND := sha256sum

I915_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/i915
# The cache of one revision holds one set of files.  The set grew from two
# DMC programs to every one the driver names on 2026-10-27, so the larger set
# has a directory of its own and a verified cache of the two-program set is
# left alone rather than reported as corrupt.
override I915_FIRMWARE_CACHE_SET := all-dmc
override I915_FIRMWARE_CACHE_DIR := $(I915_FIRMWARE_CACHE_ROOT)/$(I915_FIRMWARE_REVISION)-$(I915_FIRMWARE_CACHE_SET)
override I915_FIRMWARE_CACHE_ADLP_DMC := $(I915_FIRMWARE_CACHE_DIR)/adlp_dmc.bin
override I915_FIRMWARE_CACHE_TGL_DMC := $(I915_FIRMWARE_CACHE_DIR)/tgl_dmc_ver2_12.bin
override I915_FIRMWARE_CACHE_ADLP_FALLBACK_DMC := $(I915_FIRMWARE_CACHE_DIR)/adlp_dmc_ver2_16.bin
override I915_FIRMWARE_CACHE_ADLS_DMC := $(I915_FIRMWARE_CACHE_DIR)/adls_dmc_ver2_01.bin
override I915_FIRMWARE_CACHE_ICL_DMC := $(I915_FIRMWARE_CACHE_DIR)/icl_dmc_ver1_09.bin
override I915_FIRMWARE_CACHE_RKL_DMC := $(I915_FIRMWARE_CACHE_DIR)/rkl_dmc_ver2_03.bin
override I915_FIRMWARE_CACHE_LICENSE := $(I915_FIRMWARE_CACHE_DIR)/LICENSE.i915
override I915_FIRMWARE_CACHE_WHENCE := $(I915_FIRMWARE_CACHE_DIR)/WHENCE
override I915_FIRMWARE_MANIFEST := userland/firmware/i915/i915-firmware.manifest

override I915_FIRMWARE_DATA := \
	/lib/firmware/i915/adlp_dmc.bin=$(I915_FIRMWARE_CACHE_ADLP_DMC) \
	/lib/firmware/i915/tgl_dmc_ver2_12.bin=$(I915_FIRMWARE_CACHE_TGL_DMC) \
	/lib/firmware/i915/adlp_dmc_ver2_16.bin=$(I915_FIRMWARE_CACHE_ADLP_FALLBACK_DMC) \
	/lib/firmware/i915/adls_dmc_ver2_01.bin=$(I915_FIRMWARE_CACHE_ADLS_DMC) \
	/lib/firmware/i915/icl_dmc_ver1_09.bin=$(I915_FIRMWARE_CACHE_ICL_DMC) \
	/lib/firmware/i915/rkl_dmc_ver2_03.bin=$(I915_FIRMWARE_CACHE_RKL_DMC) \
	/usr/share/licenses/i915-firmware/LICENSE.i915=$(I915_FIRMWARE_CACHE_LICENSE) \
	/usr/share/licenses/i915-firmware/WHENCE=$(I915_FIRMWARE_CACHE_WHENCE) \
	/usr/share/zedbsd/packages/i915-firmware.manifest=$(I915_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override I915_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,i915-firmware,$(I915_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/i915/i915-firmware.notice,$(I915_FIRMWARE_LICENSE_PATH),$(I915_FIRMWARE_LICENSE_SIZE),$(I915_FIRMWARE_LICENSE_SHA256),$(I915_FIRMWARE_ZEDBSD_URL),$(I915_FIRMWARE_BASE_URL)/$(I915_FIRMWARE_LICENSE_PATH)$(I915_FIRMWARE_URL_SUFFIX),$(I915_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,i915-firmware,$(filter i915-firmware,$(ZEDBSD_USER_PROGRAMS)),$(I915_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(I915_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error i915-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter i915-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: i915-firmware-license
firmware-licenses-pending: i915-firmware-license-pending
endif
.PHONY: i915-firmware-license i915-firmware-license-pending
i915-firmware-license:
	@if test ! -d '$(I915_FIRMWARE_CACHE_DIR)'; then $(I915_FIRMWARE_LICENSE_COMMAND); fi
i915-firmware-license-pending:
	@if test ! -d '$(I915_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(I915_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,i915-firmware,Intel i915 display DMC firmware,amd64,n,package,,y,firmware,firmware/i915,,,data,$(I915_FIRMWARE_DATA),,))
override USERLAND_i915-firmware_DATA := $(I915_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += i915-firmware

.PHONY: download i915-firmware i915-firmware-cache \
	i915-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: i915-firmware
endif

i915-firmware: $(I915_FIRMWARE_CACHE_ADLP_DMC) \
	$(I915_FIRMWARE_CACHE_TGL_DMC) \
	$(I915_FIRMWARE_CACHE_ADLP_FALLBACK_DMC) \
	$(I915_FIRMWARE_CACHE_ADLS_DMC) \
	$(I915_FIRMWARE_CACHE_ICL_DMC) \
	$(I915_FIRMWARE_CACHE_RKL_DMC) \
	$(I915_FIRMWARE_CACHE_LICENSE) \
	$(I915_FIRMWARE_CACHE_WHENCE)

i915-firmware-fixture-cache: i915-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after all eight files
# pass exact type, size, and SHA-256 checks.
i915-firmware-cache:
	@set -eu; \
	cache='$(I915_FIRMWARE_CACHE_DIR)'; \
	verify_file() { \
		file="$$1"; expected_size="$$2"; expected_hash="$$3"; \
		if test ! -f "$$file" || test -L "$$file"; then \
			echo "i915-firmware: missing or unsafe cache file: $$file" >&2; \
			return 1; \
		fi; \
		actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
		if test "$$actual_size" != "$$expected_size"; then \
			echo "i915-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
			return 1; \
		fi; \
		actual_hash=$$('$(I915_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
		if test "$$actual_hash" != "$$expected_hash"; then \
			echo "i915-firmware: SHA-256 mismatch for $$file" >&2; \
			return 1; \
		fi; \
	}; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != 8; then \
			echo "i915-firmware: cache must contain exactly eight files: $$directory" >&2; \
			return 1; \
		fi; \
		verify_file "$$directory/adlp_dmc.bin" '$(I915_FIRMWARE_ADLP_DMC_SIZE)' '$(I915_FIRMWARE_ADLP_DMC_SHA256)'; \
		verify_file "$$directory/tgl_dmc_ver2_12.bin" '$(I915_FIRMWARE_TGL_DMC_SIZE)' '$(I915_FIRMWARE_TGL_DMC_SHA256)'; \
		verify_file "$$directory/adlp_dmc_ver2_16.bin" '$(I915_FIRMWARE_ADLP_FALLBACK_DMC_SIZE)' '$(I915_FIRMWARE_ADLP_FALLBACK_DMC_SHA256)'; \
		verify_file "$$directory/adls_dmc_ver2_01.bin" '$(I915_FIRMWARE_ADLS_DMC_SIZE)' '$(I915_FIRMWARE_ADLS_DMC_SHA256)'; \
		verify_file "$$directory/icl_dmc_ver1_09.bin" '$(I915_FIRMWARE_ICL_DMC_SIZE)' '$(I915_FIRMWARE_ICL_DMC_SHA256)'; \
		verify_file "$$directory/rkl_dmc_ver2_03.bin" '$(I915_FIRMWARE_RKL_DMC_SIZE)' '$(I915_FIRMWARE_RKL_DMC_SHA256)'; \
		verify_file "$$directory/LICENSE.i915" '$(I915_FIRMWARE_LICENSE_SIZE)' '$(I915_FIRMWARE_LICENSE_SHA256)'; \
		verify_file "$$directory/WHENCE" '$(I915_FIRMWARE_WHENCE_SIZE)' '$(I915_FIRMWARE_WHENCE_SHA256)'; \
	}; \
	if test -e "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "i915-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(I915_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(I915_FIRMWARE_LICENSE_COMMAND); \
	fi; \
	parent=$${cache%/*}; \
	mkdir -p "$$parent"; \
	lock="$$cache.lock"; \
	locked=0; \
	temporary=; \
	cleanup() { \
		if test -n "$$temporary" && test -d "$$temporary"; then \
			rm -rf -- "$$temporary"; \
		fi; \
		if test "$$locked" = 1 && test -d "$$lock"; then \
			rmdir -- "$$lock"; \
		fi; \
	}; \
	if ! mkdir "$$lock"; then \
		echo "i915-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.i915.$(I915_FIRMWARE_REVISION).XXXXXX"); \
	repository='$(I915_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(I915_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "i915-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(I915_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(I915_FIRMWARE_BASE_URL)'; \
	suffix='$(I915_FIRMWARE_URL_SUFFIX)'; \
	fetch adlp_dmc.bin "$$base/$(I915_FIRMWARE_ADLP_DMC_PATH)$$suffix"; \
	fetch tgl_dmc_ver2_12.bin "$$base/$(I915_FIRMWARE_TGL_DMC_PATH)$$suffix"; \
	fetch adlp_dmc_ver2_16.bin "$$base/$(I915_FIRMWARE_ADLP_FALLBACK_DMC_PATH)$$suffix"; \
	fetch adls_dmc_ver2_01.bin "$$base/$(I915_FIRMWARE_ADLS_DMC_PATH)$$suffix"; \
	fetch icl_dmc_ver1_09.bin "$$base/$(I915_FIRMWARE_ICL_DMC_PATH)$$suffix"; \
	fetch rkl_dmc_ver2_03.bin "$$base/$(I915_FIRMWARE_RKL_DMC_PATH)$$suffix"; \
	fetch LICENSE.i915 "$$base/$(I915_FIRMWARE_LICENSE_PATH)$$suffix"; \
	fetch WHENCE "$$base/$(I915_FIRMWARE_WHENCE_PATH)$$suffix"; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(I915_FIRMWARE_CACHE_ADLP_DMC) \
$(I915_FIRMWARE_CACHE_TGL_DMC) \
$(I915_FIRMWARE_CACHE_ADLP_FALLBACK_DMC) \
$(I915_FIRMWARE_CACHE_ADLS_DMC) \
$(I915_FIRMWARE_CACHE_ICL_DMC) \
$(I915_FIRMWARE_CACHE_RKL_DMC) \
$(I915_FIRMWARE_CACHE_LICENSE) \
$(I915_FIRMWARE_CACHE_WHENCE): i915-firmware-cache
	@test -f "$@"
