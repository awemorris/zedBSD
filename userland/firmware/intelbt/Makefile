# zedBSD optional Intel Bluetooth (AX211 "Solar") firmware package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# bluetoothd (userland/base/bluetoothd) loads /lib/firmware/intel/ibt-*.sfi
# and .ddc into an Intel controller that starts in its bootloader
# (plan/ws143/design.md section 3).  This package carries the whole Solar
# block of linux-firmware's WHENCE (BT_Solar_REL82122_23.50.26053.82122):
# three .sfi/.ddc pairs and the eight names WHENCE links to them.  The image
# builder copies files only, so each linked name is installed as a second
# copy of the same verified cache file.
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

# Each entry is CACHE-NAME|UPSTREAM-PATH|SIZE|SHA-256.
override INTELBT_FIRMWARE_FIXTURE_GOAL := intelbt-firmware-fixture-cache
ifneq ($(filter $(INTELBT_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(INTELBT_FIRMWARE_FIXTURE_GOAL))
$(error $(INTELBT_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
INTELBT_FIRMWARE_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
INTELBT_FIRMWARE_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
INTELBT_FIRMWARE_URL_SUFFIX ?=
INTELBT_FIRMWARE_FILES ?= \
	ibt-0040-0041.sfi|intel/ibt-0040-0041.sfi|720988|8dcfb3a7c1592c11c3e80505d21ee78dbde3de0c830d8e3e856371c37717b62c \
	ibt-0040-0041.ddc|intel/ibt-0040-0041.ddc|9|fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 \
	ibt-0041-0041.sfi|intel/ibt-0041-0041.sfi|713448|2d891022ded1d2d8b208cc5b7632f3098c761ae099024de971e4158dd5681a9c \
	ibt-0041-0041.ddc|intel/ibt-0041-0041.ddc|9|fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 \
	ibt-1040-0041.sfi|intel/ibt-1040-0041.sfi|720988|8dcfb3a7c1592c11c3e80505d21ee78dbde3de0c830d8e3e856371c37717b62c \
	ibt-1040-0041.ddc|intel/ibt-1040-0041.ddc|9|fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 \
	LICENCE.ibt_firmware|LICENCE.ibt_firmware|2040|5181b0b51efc79d5acb2c9bb92042878fdbad97a92114d4ab5e32e2b5b52fce4 \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
INTELBT_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override INTELBT_FIRMWARE_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override INTELBT_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override INTELBT_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override INTELBT_FIRMWARE_FILES := \
	ibt-0040-0041.sfi|intel/ibt-0040-0041.sfi|720988|8dcfb3a7c1592c11c3e80505d21ee78dbde3de0c830d8e3e856371c37717b62c \
	ibt-0040-0041.ddc|intel/ibt-0040-0041.ddc|9|fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 \
	ibt-0041-0041.sfi|intel/ibt-0041-0041.sfi|713448|2d891022ded1d2d8b208cc5b7632f3098c761ae099024de971e4158dd5681a9c \
	ibt-0041-0041.ddc|intel/ibt-0041-0041.ddc|9|fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 \
	ibt-1040-0041.sfi|intel/ibt-1040-0041.sfi|720988|8dcfb3a7c1592c11c3e80505d21ee78dbde3de0c830d8e3e856371c37717b62c \
	ibt-1040-0041.ddc|intel/ibt-1040-0041.ddc|9|fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 \
	LICENCE.ibt_firmware|LICENCE.ibt_firmware|2040|5181b0b51efc79d5acb2c9bb92042878fdbad97a92114d4ab5e32e2b5b52fce4 \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
override INTELBT_FIRMWARE_FETCH := curl --fail --location --silent --show-error
endif

override INTELBT_FIRMWARE_TAG := 20260410
override INTELBT_FIRMWARE_TAG_OBJECT := 4585dd5a5f0cee08990d754701d8866d9e9266e6
override INTELBT_FIRMWARE_VERSION := 23.50.26053.82122
override INTELBT_FIRMWARE_SHA256_COMMAND := sha256sum
override INTELBT_FIRMWARE_FILE_COUNT := 8

INTELBT_FIRMWARE_CACHE_ROOT ?= $(if $(ZEDBSD_TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/intelbt
override INTELBT_FIRMWARE_CACHE_DIR := $(INTELBT_FIRMWARE_CACHE_ROOT)/$(INTELBT_FIRMWARE_REVISION)
override INTELBT_FIRMWARE_CACHE_NAMES := $(foreach entry,$(INTELBT_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override INTELBT_FIRMWARE_CACHE_FILES := $(addprefix $(INTELBT_FIRMWARE_CACHE_DIR)/,$(INTELBT_FIRMWARE_CACHE_NAMES))
override INTELBT_FIRMWARE_MANIFEST := userland/firmware/intelbt/intelbt-firmware.manifest
override INTELBT_FIRMWARE_DIR := /lib/firmware/intel
override INTELBT_FIRMWARE_LICENSE_DIR := /usr/share/licenses/intelbt-firmware

# The installed names: each file, then each name WHENCE links to one of them.
override INTELBT_FIRMWARE_DATA := \
	$(INTELBT_FIRMWARE_DIR)/ibt-0040-0041.sfi=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0040-0041.sfi \
	$(INTELBT_FIRMWARE_DIR)/ibt-0040-0041.ddc=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0040-0041.ddc \
	$(INTELBT_FIRMWARE_DIR)/ibt-0041-0041.sfi=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0041-0041.sfi \
	$(INTELBT_FIRMWARE_DIR)/ibt-0041-0041.ddc=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0041-0041.ddc \
	$(INTELBT_FIRMWARE_DIR)/ibt-1040-0041.sfi=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-1040-0041.sfi \
	$(INTELBT_FIRMWARE_DIR)/ibt-1040-0041.ddc=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-1040-0041.ddc \
	$(INTELBT_FIRMWARE_DIR)/ibt-0040-4150.sfi=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0040-0041.sfi \
	$(INTELBT_FIRMWARE_DIR)/ibt-0040-4150.ddc=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0040-0041.ddc \
	$(INTELBT_FIRMWARE_DIR)/ibt-1040-4150.sfi=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-1040-0041.sfi \
	$(INTELBT_FIRMWARE_DIR)/ibt-1040-4150.ddc=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-1040-0041.ddc \
	$(INTELBT_FIRMWARE_DIR)/ibt-0040-1050.sfi=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0040-0041.sfi \
	$(INTELBT_FIRMWARE_DIR)/ibt-0040-1050.ddc=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-0040-0041.ddc \
	$(INTELBT_FIRMWARE_DIR)/ibt-1040-1050.sfi=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-1040-0041.sfi \
	$(INTELBT_FIRMWARE_DIR)/ibt-1040-1050.ddc=$(INTELBT_FIRMWARE_CACHE_DIR)/ibt-1040-0041.ddc \
	$(INTELBT_FIRMWARE_LICENSE_DIR)/LICENCE.ibt_firmware=$(INTELBT_FIRMWARE_CACHE_DIR)/LICENCE.ibt_firmware \
	$(INTELBT_FIRMWARE_LICENSE_DIR)/WHENCE=$(INTELBT_FIRMWARE_CACHE_DIR)/WHENCE \
	/usr/share/zedbsd/packages/intelbt-firmware.manifest=$(INTELBT_FIRMWARE_MANIFEST)

$(eval $(call ZEDBSD_USERLAND_PACKAGE,intelbt-firmware,Intel Bluetooth firmware,amd64,n,package,,y,firmware,firmware/intelbt,,,data,$(INTELBT_FIRMWARE_DATA),,))
override USERLAND_intelbt-firmware_DATA := $(INTELBT_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += intelbt-firmware

.PHONY: download intelbt-firmware intelbt-firmware-cache \
	intelbt-firmware-fixture-cache

ifndef ZEDBSD_TOPLEVEL_BUILD
download: intelbt-firmware
endif

intelbt-firmware: $(INTELBT_FIRMWARE_CACHE_FILES)

intelbt-firmware-fixture-cache: intelbt-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
intelbt-firmware-cache:
	@set -eu; \
	cache='$(INTELBT_FIRMWARE_CACHE_DIR)'; \
	files='$(strip $(INTELBT_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(INTELBT_FIRMWARE_FILE_COUNT)'; then \
			echo "intelbt-firmware: cache must contain exactly $(INTELBT_FIRMWARE_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "intelbt-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "intelbt-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(INTELBT_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "intelbt-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "intelbt-firmware: unsafe cache path: $$cache" >&2; \
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
		echo "intelbt-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.intelbt.$(INTELBT_FIRMWARE_REVISION).XXXXXX"); \
	base='$(INTELBT_FIRMWARE_BASE_URL)'; \
	suffix='$(INTELBT_FIRMWARE_URL_SUFFIX)'; \
	for entry in $$files; do \
		old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
		$(INTELBT_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$base/$$2$$suffix"; \
	done; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(INTELBT_FIRMWARE_CACHE_FILES): intelbt-firmware-cache
	@test -f "$@"
