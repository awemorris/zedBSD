# zedBSD optional Imagination PowerVR firmware package (BXE-4-32, 36.50.54.182)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The powervr driver of the VisionFive 2 (StarFive JH7110; plan/ws269/design.md
# sections 3.3 and 4) boots this Rogue firmware 1.1.OS@6976702.  It is not in
# kernel.org's linux-firmware; the upstream is Imagination's fork, branch
# powervr, pinned here to commit 8a58f818, and the zedBSD firmware repository
# keeps the same bytes below powervr/ (plan/ws269/phase002/phase.md).
# LICENSE.powervr forbids reverse engineering, decompilation and disassembly:
# investigate the firmware only through its trace buffer, its interface's
# replies and register values (plan/ws269/design.md section 0.2).
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

override POWERVR_FIRMWARE_FIXTURE_GOAL := powervr-firmware-fixture-cache
ifneq ($(filter $(POWERVR_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(POWERVR_FIRMWARE_FIXTURE_GOAL))
$(error $(POWERVR_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic package fixture may replace acquisition metadata.  This
# target never assembles a root filesystem, and a later production target
# revalidates its own immutable cache identity.
POWERVR_FIRMWARE_MIRROR_BASE_URL ?= https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw
POWERVR_FIRMWARE_MIRROR_REVISION ?= 8a58f81883f7be458daa34e418cc4079f995b279
POWERVR_FIRMWARE_SIZE ?= 131072
POWERVR_FIRMWARE_SHA256 ?= 59127d3a02e48876310a520b7dc355f7430d3748d2940caaa33f39237d517abb
POWERVR_FIRMWARE_LICENSE_SIZE ?= 2190
POWERVR_FIRMWARE_LICENSE_SHA256 ?= 1c9aa6bd6703a7ce1cdb879542fa1d8aca115a327bd819b193c971de9c53f402
POWERVR_FIRMWARE_MIRROR_WHENCE_SIZE ?= 341319
POWERVR_FIRMWARE_MIRROR_WHENCE_SHA256 ?= ec86d7649ff9e80e728b129dd725db1728f8a441cf342f1715c0a76dcf8146c3
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
POWERVR_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
POWERVR_FIRMWARE_LICENSE_PROMPT ?= n
else
# These values define the selected production package.  `override` is
# intentional: accepting command-line substitutions would allow different
# bytes to be installed while the committed manifest still named this pinned
# revision and digest.
override POWERVR_FIRMWARE_MIRROR_BASE_URL := https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw
override POWERVR_FIRMWARE_MIRROR_REVISION := 8a58f81883f7be458daa34e418cc4079f995b279
override POWERVR_FIRMWARE_SIZE := 131072
override POWERVR_FIRMWARE_SHA256 := 59127d3a02e48876310a520b7dc355f7430d3748d2940caaa33f39237d517abb
override POWERVR_FIRMWARE_LICENSE_SIZE := 2190
override POWERVR_FIRMWARE_LICENSE_SHA256 := 1c9aa6bd6703a7ce1cdb879542fa1d8aca115a327bd819b193c971de9c53f402
override POWERVR_FIRMWARE_MIRROR_WHENCE_SIZE := 341319
override POWERVR_FIRMWARE_MIRROR_WHENCE_SHA256 := ec86d7649ff9e80e728b129dd725db1728f8a441cf342f1715c0a76dcf8146c3
override POWERVR_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/powervr)
override POWERVR_FIRMWARE_LICENSE_PROMPT := y
endif

override POWERVR_FIRMWARE_UPSTREAM_PATH := powervr/rogue_36.50.54.182_v1.fw
override POWERVR_FIRMWARE_LICENSE_UPSTREAM_PATH := LICENSE.powervr
override POWERVR_FIRMWARE_WHENCE_UPSTREAM_PATH := WHENCE
override POWERVR_FIRMWARE_VERSION := 1.1.OS@6976702

POWERVR_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
override POWERVR_FIRMWARE_SHA256_COMMAND := sha256sum
POWERVR_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/powervr
override POWERVR_FIRMWARE_CACHE_DIR := $(POWERVR_FIRMWARE_CACHE_ROOT)/$(POWERVR_FIRMWARE_MIRROR_REVISION)
override POWERVR_FIRMWARE_CACHE_BLOB := $(POWERVR_FIRMWARE_CACHE_DIR)/rogue_36.50.54.182_v1.fw
override POWERVR_FIRMWARE_CACHE_LICENSE := $(POWERVR_FIRMWARE_CACHE_DIR)/LICENSE.powervr
override POWERVR_FIRMWARE_CACHE_WHENCE := $(POWERVR_FIRMWARE_CACHE_DIR)/WHENCE
override POWERVR_FIRMWARE_MANIFEST := userland/firmware/powervr/powervr-firmware.manifest

override POWERVR_FIRMWARE_DATA := \
	/lib/firmware/powervr/rogue_36.50.54.182_v1.fw=$(POWERVR_FIRMWARE_CACHE_BLOB) \
	/usr/share/licenses/powervr-firmware/LICENSE.powervr=$(POWERVR_FIRMWARE_CACHE_LICENSE) \
	/usr/share/licenses/powervr-firmware/WHENCE=$(POWERVR_FIRMWARE_CACHE_WHENCE) \
	/usr/share/zedbsd/packages/powervr-firmware.manifest=$(POWERVR_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override POWERVR_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,powervr-firmware,$(POWERVR_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/powervr/powervr-firmware.notice,LICENSE.powervr,$(POWERVR_FIRMWARE_LICENSE_SIZE),$(POWERVR_FIRMWARE_LICENSE_SHA256),$(POWERVR_FIRMWARE_ZEDBSD_URL),$(POWERVR_FIRMWARE_MIRROR_BASE_URL)/$(POWERVR_FIRMWARE_MIRROR_REVISION)/$(POWERVR_FIRMWARE_LICENSE_UPSTREAM_PATH),$(POWERVR_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,powervr-firmware,$(filter powervr-firmware,$(ZEDBSD_USER_PROGRAMS)),$(POWERVR_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(POWERVR_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error powervr-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter powervr-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: powervr-firmware-license
firmware-licenses-pending: powervr-firmware-license-pending
endif
.PHONY: powervr-firmware-license powervr-firmware-license-pending
powervr-firmware-license:
	@if test ! -d '$(POWERVR_FIRMWARE_CACHE_DIR)'; then $(POWERVR_FIRMWARE_LICENSE_COMMAND); fi
powervr-firmware-license-pending:
	@if test ! -d '$(POWERVR_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(POWERVR_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,powervr-firmware,Imagination PowerVR BXE-4-32 GPU firmware for VisionFive 2,*,n,package,,y,firmware,firmware/powervr,,,data,$(POWERVR_FIRMWARE_DATA),,))
override USERLAND_powervr-firmware_DATA := $(POWERVR_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += powervr-firmware

.PHONY: download powervr-firmware powervr-firmware-cache \
	powervr-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: powervr-firmware
endif

powervr-firmware: $(POWERVR_FIRMWARE_CACHE_BLOB) \
	$(POWERVR_FIRMWARE_CACHE_LICENSE) $(POWERVR_FIRMWARE_CACHE_WHENCE)

powervr-firmware-fixture-cache: powervr-firmware-cache

# Every selected build revalidates the cache.  A corrupt existing cache is an
# error: silently replacing it would conceal local damage or an upstream
# substitution.  A missing cache is acquired into a sibling temporary
# directory and becomes visible only after all three files pass validation.
powervr-firmware-cache:
	@set -eu; \
	cache='$(POWERVR_FIRMWARE_CACHE_DIR)'; \
	verify_file() { \
		file="$$1"; expected_size="$$2"; expected_hash="$$3"; \
		if test ! -f "$$file" || test -L "$$file"; then \
			echo "powervr-firmware: missing or unsafe cache file: $$file" >&2; \
			return 1; \
		fi; \
		actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
		if test "$$actual_size" != "$$expected_size"; then \
			echo "powervr-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
			return 1; \
		fi; \
		actual_hash=$$('$(POWERVR_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
		if test "$$actual_hash" != "$$expected_hash"; then \
			echo "powervr-firmware: SHA-256 mismatch for $$file" >&2; \
			return 1; \
		fi; \
	}; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != 3; then \
			echo "powervr-firmware: cache must contain exactly three files: $$directory" >&2; \
			return 1; \
		fi; \
		verify_file "$$directory/rogue_36.50.54.182_v1.fw" '$(POWERVR_FIRMWARE_SIZE)' '$(POWERVR_FIRMWARE_SHA256)'; \
		verify_file "$$directory/LICENSE.powervr" '$(POWERVR_FIRMWARE_LICENSE_SIZE)' '$(POWERVR_FIRMWARE_LICENSE_SHA256)'; \
		verify_file "$$directory/WHENCE" '$(POWERVR_FIRMWARE_MIRROR_WHENCE_SIZE)' '$(POWERVR_FIRMWARE_MIRROR_WHENCE_SHA256)'; \
	}; \
	if test -e "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "powervr-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(POWERVR_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(POWERVR_FIRMWARE_LICENSE_COMMAND); \
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
		echo "powervr-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.powervr.$(POWERVR_FIRMWARE_MIRROR_REVISION).XXXXXX"); \
	repository='$(POWERVR_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(POWERVR_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "powervr-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(POWERVR_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(POWERVR_FIRMWARE_MIRROR_BASE_URL)/$(POWERVR_FIRMWARE_MIRROR_REVISION)'; \
	fetch rogue_36.50.54.182_v1.fw "$$base/$(POWERVR_FIRMWARE_UPSTREAM_PATH)"; \
	fetch LICENSE.powervr "$$base/$(POWERVR_FIRMWARE_LICENSE_UPSTREAM_PATH)"; \
	fetch WHENCE "$$base/$(POWERVR_FIRMWARE_WHENCE_UPSTREAM_PATH)"; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(POWERVR_FIRMWARE_CACHE_BLOB) \
$(POWERVR_FIRMWARE_CACHE_LICENSE) \
$(POWERVR_FIRMWARE_CACHE_WHENCE): powervr-firmware-cache
	@test -f "$@"
