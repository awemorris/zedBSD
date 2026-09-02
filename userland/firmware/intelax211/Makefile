# zedBSD optional Intel AX211 firmware package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

override INTEL_AX211_FIRMWARE_FIXTURE_GOAL := intelax211-firmware-fixture-cache
ifneq ($(filter $(INTEL_AX211_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(INTEL_AX211_FIRMWARE_FIXTURE_GOAL))
$(error $(INTEL_AX211_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
INTEL_AX211_FIRMWARE_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
INTEL_AX211_FIRMWARE_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
INTEL_AX211_FIRMWARE_URL_SUFFIX ?=
INTEL_AX211_FIRMWARE_UCODE_SIZE ?= 1736748
INTEL_AX211_FIRMWARE_UCODE_SHA256 ?= c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514
INTEL_AX211_FIRMWARE_PNVM_SIZE ?= 55176
INTEL_AX211_FIRMWARE_PNVM_SHA256 ?= efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a
INTEL_AX211_FIRMWARE_LICENSE_SIZE ?= 2046
INTEL_AX211_FIRMWARE_LICENSE_SHA256 ?= 16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5
INTEL_AX211_FIRMWARE_WHENCE_SIZE ?= 425450
INTEL_AX211_FIRMWARE_WHENCE_SHA256 ?= c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
INTEL_AX211_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override INTEL_AX211_FIRMWARE_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override INTEL_AX211_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override INTEL_AX211_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override INTEL_AX211_FIRMWARE_UCODE_SIZE := 1736748
override INTEL_AX211_FIRMWARE_UCODE_SHA256 := c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514
override INTEL_AX211_FIRMWARE_PNVM_SIZE := 55176
override INTEL_AX211_FIRMWARE_PNVM_SHA256 := efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a
override INTEL_AX211_FIRMWARE_LICENSE_SIZE := 2046
override INTEL_AX211_FIRMWARE_LICENSE_SHA256 := 16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5
override INTEL_AX211_FIRMWARE_WHENCE_SIZE := 425450
override INTEL_AX211_FIRMWARE_WHENCE_SHA256 := c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
override INTEL_AX211_FIRMWARE_FETCH := curl --fail --location --silent --show-error
endif

override INTEL_AX211_FIRMWARE_TAG := 20260410
override INTEL_AX211_FIRMWARE_TAG_OBJECT := 4585dd5a5f0cee08990d754701d8866d9e9266e6
override INTEL_AX211_FIRMWARE_UCODE_PATH := intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode
override INTEL_AX211_FIRMWARE_PNVM_PATH := intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm
override INTEL_AX211_FIRMWARE_LICENSE_PATH := LICENCE.iwlwifi_firmware
override INTEL_AX211_FIRMWARE_WHENCE_PATH := WHENCE
override INTEL_AX211_FIRMWARE_VERSION := 89.735b75a4.0
override INTEL_AX211_FIRMWARE_WHENCE_VERSION := 86.735b75a4.0
override INTEL_AX211_FIRMWARE_SHA256_COMMAND := sha256sum

INTEL_AX211_FIRMWARE_CACHE_ROOT ?= $(if $(ZEDBSD_TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/intelax211
override INTEL_AX211_FIRMWARE_CACHE_DIR := $(INTEL_AX211_FIRMWARE_CACHE_ROOT)/$(INTEL_AX211_FIRMWARE_REVISION)
override INTEL_AX211_FIRMWARE_CACHE_UCODE := $(INTEL_AX211_FIRMWARE_CACHE_DIR)/iwlwifi-so-a0-gf-a0-89.ucode
override INTEL_AX211_FIRMWARE_CACHE_PNVM := $(INTEL_AX211_FIRMWARE_CACHE_DIR)/iwlwifi-so-a0-gf-a0.pnvm
override INTEL_AX211_FIRMWARE_CACHE_LICENSE := $(INTEL_AX211_FIRMWARE_CACHE_DIR)/LICENCE.iwlwifi_firmware
override INTEL_AX211_FIRMWARE_CACHE_WHENCE := $(INTEL_AX211_FIRMWARE_CACHE_DIR)/WHENCE
override INTEL_AX211_FIRMWARE_MANIFEST := userland/firmware/intelax211/intelax211-firmware.manifest

override INTEL_AX211_FIRMWARE_DATA := \
	/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode=$(INTEL_AX211_FIRMWARE_CACHE_UCODE) \
	/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm=$(INTEL_AX211_FIRMWARE_CACHE_PNVM) \
	/usr/share/licenses/intelax211-firmware/LICENCE.iwlwifi_firmware=$(INTEL_AX211_FIRMWARE_CACHE_LICENSE) \
	/usr/share/licenses/intelax211-firmware/WHENCE=$(INTEL_AX211_FIRMWARE_CACHE_WHENCE) \
	/usr/share/zedbsd/packages/intelax211-firmware.manifest=$(INTEL_AX211_FIRMWARE_MANIFEST)

$(eval $(call ZEDBSD_USERLAND_PACKAGE,intelax211-firmware,Intel AX211 firmware,amd64,n,package,,y,firmware,firmware/intelax211,,,data,$(INTEL_AX211_FIRMWARE_DATA),,))
override USERLAND_intelax211-firmware_DATA := $(INTEL_AX211_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += intelax211-firmware

.PHONY: download intelax211-firmware intelax211-firmware-cache \
	intelax211-firmware-fixture-cache

ifndef ZEDBSD_TOPLEVEL_BUILD
download: intelax211-firmware
endif

intelax211-firmware: $(INTEL_AX211_FIRMWARE_CACHE_UCODE) \
	$(INTEL_AX211_FIRMWARE_CACHE_PNVM) \
	$(INTEL_AX211_FIRMWARE_CACHE_LICENSE) \
	$(INTEL_AX211_FIRMWARE_CACHE_WHENCE)

intelax211-firmware-fixture-cache: intelax211-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after all four files
# pass exact type, size, and SHA-256 checks.
intelax211-firmware-cache:
	@set -eu; \
	cache='$(INTEL_AX211_FIRMWARE_CACHE_DIR)'; \
	verify_file() { \
		file="$$1"; expected_size="$$2"; expected_hash="$$3"; \
		if test ! -f "$$file" || test -L "$$file"; then \
			echo "intelax211-firmware: missing or unsafe cache file: $$file" >&2; \
			return 1; \
		fi; \
		actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
		if test "$$actual_size" != "$$expected_size"; then \
			echo "intelax211-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
			return 1; \
		fi; \
		actual_hash=$$('$(INTEL_AX211_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
		if test "$$actual_hash" != "$$expected_hash"; then \
			echo "intelax211-firmware: SHA-256 mismatch for $$file" >&2; \
			return 1; \
		fi; \
	}; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != 4; then \
			echo "intelax211-firmware: cache must contain exactly four files: $$directory" >&2; \
			return 1; \
		fi; \
		verify_file "$$directory/iwlwifi-so-a0-gf-a0-89.ucode" '$(INTEL_AX211_FIRMWARE_UCODE_SIZE)' '$(INTEL_AX211_FIRMWARE_UCODE_SHA256)'; \
		verify_file "$$directory/iwlwifi-so-a0-gf-a0.pnvm" '$(INTEL_AX211_FIRMWARE_PNVM_SIZE)' '$(INTEL_AX211_FIRMWARE_PNVM_SHA256)'; \
		verify_file "$$directory/LICENCE.iwlwifi_firmware" '$(INTEL_AX211_FIRMWARE_LICENSE_SIZE)' '$(INTEL_AX211_FIRMWARE_LICENSE_SHA256)'; \
		verify_file "$$directory/WHENCE" '$(INTEL_AX211_FIRMWARE_WHENCE_SIZE)' '$(INTEL_AX211_FIRMWARE_WHENCE_SHA256)'; \
	}; \
	if test -e "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "intelax211-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
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
		echo "intelax211-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.intelax211.$(INTEL_AX211_FIRMWARE_REVISION).XXXXXX"); \
	base='$(INTEL_AX211_FIRMWARE_BASE_URL)'; \
	suffix='$(INTEL_AX211_FIRMWARE_URL_SUFFIX)'; \
	$(INTEL_AX211_FIRMWARE_FETCH) --output "$$temporary/iwlwifi-so-a0-gf-a0-89.ucode" "$$base/$(INTEL_AX211_FIRMWARE_UCODE_PATH)$$suffix"; \
	$(INTEL_AX211_FIRMWARE_FETCH) --output "$$temporary/iwlwifi-so-a0-gf-a0.pnvm" "$$base/$(INTEL_AX211_FIRMWARE_PNVM_PATH)$$suffix"; \
	$(INTEL_AX211_FIRMWARE_FETCH) --output "$$temporary/LICENCE.iwlwifi_firmware" "$$base/$(INTEL_AX211_FIRMWARE_LICENSE_PATH)$$suffix"; \
	$(INTEL_AX211_FIRMWARE_FETCH) --output "$$temporary/WHENCE" "$$base/$(INTEL_AX211_FIRMWARE_WHENCE_PATH)$$suffix"; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(INTEL_AX211_FIRMWARE_CACHE_UCODE) \
$(INTEL_AX211_FIRMWARE_CACHE_PNVM) \
$(INTEL_AX211_FIRMWARE_CACHE_LICENSE) \
$(INTEL_AX211_FIRMWARE_CACHE_WHENCE): intelax211-firmware-cache
	@test -f "$@"
