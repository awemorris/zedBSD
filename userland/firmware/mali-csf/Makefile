# zedBSD optional Arm Mali CSF GPU firmware package (linux-firmware 20260410)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The mali driver (src/drivers/gpu/mali) loads this program into the
# microcontroller of a Mali-G310 (Valhall, arch 10), the Command Stream
# Frontend's firmware, from /lib/firmware/arm/mali/arch<major>.<minor>/.
# linux-firmware ships the same bytes for arch 10.8, 10.10 and 10.12, so the
# one verified file is installed under each of the three names.
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-PATH|SIZE|SHA-256.
override MALI_CSF_FIRMWARE_FIXTURE_GOAL := mali-csf-firmware-fixture-cache
ifneq ($(filter $(MALI_CSF_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(MALI_CSF_FIRMWARE_FIXTURE_GOAL))
$(error $(MALI_CSF_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
MALI_CSF_FIRMWARE_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
MALI_CSF_FIRMWARE_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
MALI_CSF_FIRMWARE_URL_SUFFIX ?=
MALI_CSF_FIRMWARE_FILES ?= \
	mali_csffw.bin|arm/mali/arch10.8/mali_csffw.bin|282624|a27847ea11f8efb3136340c3ba8aab413ae25145eeb4a7f64ff5edd829a2405b \
	LICENCE.mali_csffw|LICENCE.mali_csffw|9690|ebedc86d1767186a66dcb59ce8dcb97c5fd9ac10de2adaacad226714e4712f6d \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
MALI_CSF_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
MALI_CSF_FIRMWARE_LICENSE_PROMPT ?= n
MALI_CSF_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override MALI_CSF_FIRMWARE_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override MALI_CSF_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override MALI_CSF_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override MALI_CSF_FIRMWARE_FILES := \
	mali_csffw.bin|arm/mali/arch10.8/mali_csffw.bin|282624|a27847ea11f8efb3136340c3ba8aab413ae25145eeb4a7f64ff5edd829a2405b \
	LICENCE.mali_csffw|LICENCE.mali_csffw|9690|ebedc86d1767186a66dcb59ce8dcb97c5fd9ac10de2adaacad226714e4712f6d \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
override MALI_CSF_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/mali-csf)
override MALI_CSF_FIRMWARE_LICENSE_PROMPT := y
override MALI_CSF_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override MALI_CSF_FIRMWARE_TAG := 20260410
override MALI_CSF_FIRMWARE_TAG_OBJECT := 4585dd5a5f0cee08990d754701d8866d9e9266e6
override MALI_CSF_FIRMWARE_VERSION := 20260410
override MALI_CSF_FIRMWARE_SHA256_COMMAND := sha256sum
override MALI_CSF_FIRMWARE_FILE_COUNT := 3

MALI_CSF_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/mali-csf
# The cache holds the file set of one revision.
override MALI_CSF_FIRMWARE_CACHE_DIR := $(MALI_CSF_FIRMWARE_CACHE_ROOT)/$(MALI_CSF_FIRMWARE_REVISION)
override MALI_CSF_FIRMWARE_CACHE_NAMES := $(foreach entry,$(MALI_CSF_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override MALI_CSF_FIRMWARE_CACHE_FILES := $(addprefix $(MALI_CSF_FIRMWARE_CACHE_DIR)/,$(MALI_CSF_FIRMWARE_CACHE_NAMES))
override MALI_CSF_FIRMWARE_MANIFEST := userland/firmware/mali-csf/mali-csf-firmware.manifest
override MALI_CSF_FIRMWARE_DIR := /lib/firmware/arm/mali
override MALI_CSF_FIRMWARE_LICENSE_DIR := /usr/share/licenses/mali-csf-firmware

# The installed names, below the directory the driver reads them from;
# the one program under the three arch 10 names linux-firmware gives it.
override MALI_CSF_FIRMWARE_DATA := \
	$(MALI_CSF_FIRMWARE_DIR)/arch10.8/mali_csffw.bin=$(MALI_CSF_FIRMWARE_CACHE_DIR)/mali_csffw.bin \
	$(MALI_CSF_FIRMWARE_DIR)/arch10.10/mali_csffw.bin=$(MALI_CSF_FIRMWARE_CACHE_DIR)/mali_csffw.bin \
	$(MALI_CSF_FIRMWARE_DIR)/arch10.12/mali_csffw.bin=$(MALI_CSF_FIRMWARE_CACHE_DIR)/mali_csffw.bin \
	$(MALI_CSF_FIRMWARE_LICENSE_DIR)/LICENCE.mali_csffw=$(MALI_CSF_FIRMWARE_CACHE_DIR)/LICENCE.mali_csffw \
	$(MALI_CSF_FIRMWARE_LICENSE_DIR)/WHENCE=$(MALI_CSF_FIRMWARE_CACHE_DIR)/WHENCE \
	/usr/share/zedbsd/packages/mali-csf-firmware.manifest=$(MALI_CSF_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override MALI_CSF_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,mali-csf-firmware,$(MALI_CSF_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/mali-csf/mali-csf-firmware.notice,LICENCE.mali_csffw,$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.mali_csffw,3,$(MALI_CSF_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.mali_csffw,4,$(MALI_CSF_FIRMWARE_FILES)),$(MALI_CSF_FIRMWARE_ZEDBSD_URL),$(MALI_CSF_FIRMWARE_BASE_URL)/$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.mali_csffw,2,$(MALI_CSF_FIRMWARE_FILES))$(MALI_CSF_FIRMWARE_URL_SUFFIX),$(MALI_CSF_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,mali-csf-firmware,$(filter mali-csf-firmware,$(ZEDBSD_USER_PROGRAMS)),$(MALI_CSF_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(MALI_CSF_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error mali-csf-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter mali-csf-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: mali-csf-firmware-license
firmware-licenses-pending: mali-csf-firmware-license-pending
endif
.PHONY: mali-csf-firmware-license mali-csf-firmware-license-pending
mali-csf-firmware-license:
	@if test ! -d '$(MALI_CSF_FIRMWARE_CACHE_DIR)'; then $(MALI_CSF_FIRMWARE_LICENSE_COMMAND); fi
mali-csf-firmware-license-pending:
	@if test ! -d '$(MALI_CSF_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(MALI_CSF_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,mali-csf-firmware,Arm Mali-G310 CSF GPU firmware for ODROID-C5,*,n,package,,y,firmware,firmware/mali-csf,,,data,$(MALI_CSF_FIRMWARE_DATA),,))
override USERLAND_mali-csf-firmware_DATA := $(MALI_CSF_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += mali-csf-firmware

.PHONY: download mali-csf-firmware mali-csf-firmware-cache \
	mali-csf-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: mali-csf-firmware
endif

mali-csf-firmware: $(MALI_CSF_FIRMWARE_CACHE_FILES)

mali-csf-firmware-fixture-cache: mali-csf-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
mali-csf-firmware-cache:
	@set -eu; \
	cache='$(MALI_CSF_FIRMWARE_CACHE_DIR)'; \
	files='$(strip $(MALI_CSF_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(MALI_CSF_FIRMWARE_FILE_COUNT)'; then \
			echo "mali-csf-firmware: cache must contain exactly $(MALI_CSF_FIRMWARE_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "mali-csf-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "mali-csf-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(MALI_CSF_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "mali-csf-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "mali-csf-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(MALI_CSF_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(MALI_CSF_FIRMWARE_LICENSE_COMMAND); \
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
		echo "mali-csf-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.mali-csf.$(MALI_CSF_FIRMWARE_REVISION).XXXXXX"); \
	repository='$(MALI_CSF_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(MALI_CSF_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "mali-csf-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(MALI_CSF_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(MALI_CSF_FIRMWARE_BASE_URL)'; \
	suffix='$(MALI_CSF_FIRMWARE_URL_SUFFIX)'; \
	for entry in $$files; do \
		old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
		fetch "$$1" "$$base/$$2$$suffix"; \
	done; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(MALI_CSF_FIRMWARE_CACHE_FILES): mali-csf-firmware-cache
	@test -f "$@"
