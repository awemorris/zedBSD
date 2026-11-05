# Raspberry Pi 5 boot files
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The Pi 5's bootloader is in the board's EEPROM and reads config.txt, the
# kernel and the device tree from the SD card's FAT partition; no GPU
# firmware goes there.  The partition carries the two device trees the
# bootloader picks from by the chip's stepping (bcm2712-rpi-5-b.dtb for C1,
# bcm2712d0-rpi-5-b.dtb for D0) and their license, fetched file by file from
# the same raspberrypi/firmware commit as the Pi 4's (platform/arm64/firmware.mk)
# into a verified cache, from the zedBSD firmware repository's rpi5-boot/
# first (userland/firmware/zedbsd-firmware.mk).
include userland/firmware/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-PATH|SIZE|SHA-256.
override RPI5_BOOT_FIRMWARE_BASE_URL := https://raw.githubusercontent.com/raspberrypi/firmware
override RPI5_BOOT_FIRMWARE_REVISION := 3d301dd924bcd758a4c8cb19fe8531031f033f43
override RPI5_BOOT_FIRMWARE_FILES := \
	bcm2712-rpi-5-b.dtb|boot/bcm2712-rpi-5-b.dtb|78703|40a2fbe9c29e8b9a4912cf726a943068defb779fc052ec38e457a79c58abca00 \
	bcm2712d0-rpi-5-b.dtb|boot/bcm2712d0-rpi-5-b.dtb|78707|03d80fc8196bfd113f86534c60ee2cb35ba95ae182e34a42f66caf7fa15f48ba \
	COPYING.linux|boot/COPYING.linux|18693|af8067302947c01fd9eee72befa54c7e3ef8a48fecde7fd71277f2290b2bf0f7
override RPI5_BOOT_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/rpi5-boot)
override RPI5_BOOT_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
override RPI5_BOOT_FIRMWARE_SHA256_COMMAND := sha256sum
# Three files.
override RPI5_BOOT_FIRMWARE_ENTRY_COUNT := 3

RPI5_BOOT_FIRMWARE_CACHE_ROOT ?= build/sources/firmware/rpi5-boot
override RPI5_BOOT_FIRMWARE_DIR := $(RPI5_BOOT_FIRMWARE_CACHE_ROOT)/$(RPI5_BOOT_FIRMWARE_REVISION)
override RPI5_BOOT_FIRMWARE_CACHE_NAMES := $(foreach entry,$(RPI5_BOOT_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override RPI5_BOOT_FIRMWARE_CACHE_FILES := $(addprefix $(RPI5_BOOT_FIRMWARE_DIR)/,$(RPI5_BOOT_FIRMWARE_CACHE_NAMES))
override RPI5_BOOT_FIRMWARE_DTB := $(RPI5_BOOT_FIRMWARE_DIR)/bcm2712-rpi-5-b.dtb
override RPI5_BOOT_FIRMWARE_D0_DTB := $(RPI5_BOOT_FIRMWARE_DIR)/bcm2712d0-rpi-5-b.dtb

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override RPI5_BOOT_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,rpi5-boot-firmware,$(RPI5_BOOT_FIRMWARE_CACHE_ROOT),platform/arm64/rpi5-boot-firmware.notice,COPYING.linux,$(call ZEDBSD_FIRMWARE_FIELD,COPYING.linux,3,$(RPI5_BOOT_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,COPYING.linux,4,$(RPI5_BOOT_FIRMWARE_FILES)),$(RPI5_BOOT_FIRMWARE_ZEDBSD_URL),$(RPI5_BOOT_FIRMWARE_BASE_URL)/$(RPI5_BOOT_FIRMWARE_REVISION)/$(call ZEDBSD_FIRMWARE_FIELD,COPYING.linux,2,$(RPI5_BOOT_FIRMWARE_FILES)),$(RPI5_BOOT_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,rpi5-boot-firmware,y,$(RPI5_BOOT_FIRMWARE_DIR)),)
ifneq ($(shell $(RPI5_BOOT_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error rpi5-boot-firmware: the license was not accepted; see above)
endif
endif
firmware-licenses: rpi5-boot-firmware-license
firmware-licenses-pending: rpi5-boot-firmware-license-pending
.PHONY: rpi5-boot-firmware-license rpi5-boot-firmware-license-pending
rpi5-boot-firmware-license:
	@if test ! -d '$(RPI5_BOOT_FIRMWARE_DIR)'; then $(RPI5_BOOT_FIRMWARE_LICENSE_COMMAND); fi
rpi5-boot-firmware-license-pending:
	@if test ! -d '$(RPI5_BOOT_FIRMWARE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(RPI5_BOOT_FIRMWARE_LICENSE_COMMAND); fi

.PHONY: download rpi5-boot-firmware rpi5-boot-firmware-cache

download: rpi5-boot-firmware

rpi5-boot-firmware: $(RPI5_BOOT_FIRMWARE_CACHE_FILES)

# A build revalidates the complete cache.  Existing corruption is an error
# rather than an implicit update.  A missing cache is acquired in a sibling
# directory and published with one rename only after every file passes exact
# type, size, and SHA-256 checks.
rpi5-boot-firmware-cache:
	@set -eu; \
	cache='$(RPI5_BOOT_FIRMWARE_DIR)'; \
	files='$(strip $(RPI5_BOOT_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(RPI5_BOOT_FIRMWARE_ENTRY_COUNT)'; then \
			echo "rpi5-boot-firmware: cache must contain exactly $(RPI5_BOOT_FIRMWARE_ENTRY_COUNT) entries: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "rpi5-boot-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "rpi5-boot-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(RPI5_BOOT_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "rpi5-boot-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "rpi5-boot-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	$(RPI5_BOOT_FIRMWARE_LICENSE_COMMAND); \
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
		echo "rpi5-boot-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.rpi5-boot.$(RPI5_BOOT_FIRMWARE_REVISION).XXXXXX"); \
	repository='$(RPI5_BOOT_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(RPI5_BOOT_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "rpi5-boot-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(RPI5_BOOT_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(RPI5_BOOT_FIRMWARE_BASE_URL)/$(RPI5_BOOT_FIRMWARE_REVISION)'; \
	for entry in $$files; do \
		old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
		fetch "$$1" "$$base/$$2"; \
	done; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(RPI5_BOOT_FIRMWARE_CACHE_FILES): rpi5-boot-firmware-cache
	@test -f "$@"
