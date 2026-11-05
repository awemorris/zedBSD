# Raspberry Pi 4 boot firmware
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The SD card's FAT partition carries the Pi's GPU firmware (start4.elf and
# fixup4.dat), the board's device tree, the one overlay platform/arm64/config.txt
# names, and the Broadcom licence of the firmware.  They come from one commit
# of github.com/raspberrypi/firmware, fetched file by file into a verified
# cache like the firmware packages (userland/firmware/), from the zedBSD
# firmware repository first when one is set (userland/firmware/zedbsd-firmware.mk).
# The device tree's source, GPL-2.0, is arch/arm/boot/dts/broadcom/
# bcm2711-rpi-4-b.dts and arch/arm/boot/dts/overlays/disable-bt-overlay.dts
# of raspberrypi/linux at the commit the firmware commit's extra/git_hash
# names, 89586905b8603e545cce9089a81f5f35d65bc998.
include userland/firmware/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-PATH|SIZE|SHA-256.  A cache name may
# have one directory, which the cache keeps (the image builder copies
# overlays/ as it is).
override RPI4_BOOT_FIRMWARE_FIXTURE_GOAL := rpi4-boot-firmware-fixture-cache
ifneq ($(filter $(RPI4_BOOT_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(RPI4_BOOT_FIRMWARE_FIXTURE_GOAL))
$(error $(RPI4_BOOT_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
RPI4_BOOT_FIRMWARE_BASE_URL ?= https://raw.githubusercontent.com/raspberrypi/firmware
RPI4_BOOT_FIRMWARE_REVISION ?= 3d301dd924bcd758a4c8cb19fe8531031f033f43
RPI4_BOOT_FIRMWARE_FILES ?= \
	start4.elf|boot/start4.elf|2298048|6d540ddf77623976ba60de630e9300a5a099f7710e11ee85a7bf076f5931f4bc \
	fixup4.dat|boot/fixup4.dat|5512|5818b069814c7f051d4c9246307dced983bcbdcc6a90d9e5491fc5c68159d077 \
	bcm2711-rpi-4-b.dtb|boot/bcm2711-rpi-4-b.dtb|56407|75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb \
	overlays/disable-bt.dtbo|boot/overlays/disable-bt.dtbo|1073|ea69d22dedc607fee75eec57d8a4cc0f0eab93cd75393e61a64c49fbac912d02 \
	LICENCE.broadcom|boot/LICENCE.broadcom|1594|c7283ff51f863d93a275c66e3b4cb08021a5dd4d8c1e7acc47d872fbe52d3d6b
# The directory of the zedBSD firmware repository; empty fetches from the
# upstream alone.
RPI4_BOOT_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
RPI4_BOOT_FIRMWARE_LICENSE_PROMPT ?= n
RPI4_BOOT_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
override RPI4_BOOT_FIRMWARE_BASE_URL := https://raw.githubusercontent.com/raspberrypi/firmware
override RPI4_BOOT_FIRMWARE_REVISION := 3d301dd924bcd758a4c8cb19fe8531031f033f43
override RPI4_BOOT_FIRMWARE_FILES := \
	start4.elf|boot/start4.elf|2298048|6d540ddf77623976ba60de630e9300a5a099f7710e11ee85a7bf076f5931f4bc \
	fixup4.dat|boot/fixup4.dat|5512|5818b069814c7f051d4c9246307dced983bcbdcc6a90d9e5491fc5c68159d077 \
	bcm2711-rpi-4-b.dtb|boot/bcm2711-rpi-4-b.dtb|56407|75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb \
	overlays/disable-bt.dtbo|boot/overlays/disable-bt.dtbo|1073|ea69d22dedc607fee75eec57d8a4cc0f0eab93cd75393e61a64c49fbac912d02 \
	LICENCE.broadcom|boot/LICENCE.broadcom|1594|c7283ff51f863d93a275c66e3b4cb08021a5dd4d8c1e7acc47d872fbe52d3d6b
override RPI4_BOOT_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/rpi4-boot)
override RPI4_BOOT_FIRMWARE_LICENSE_PROMPT := y
override RPI4_BOOT_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override RPI4_BOOT_FIRMWARE_SHA256_COMMAND := sha256sum
# Five files and the overlays directory.
override RPI4_BOOT_FIRMWARE_ENTRY_COUNT := 6

RPI4_BOOT_FIRMWARE_CACHE_ROOT ?= build/sources/firmware/rpi4-boot
override RPI4_BOOT_FIRMWARE_DIR := $(RPI4_BOOT_FIRMWARE_CACHE_ROOT)/$(RPI4_BOOT_FIRMWARE_REVISION)
override RPI4_BOOT_FIRMWARE_CACHE_NAMES := $(foreach entry,$(RPI4_BOOT_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override RPI4_BOOT_FIRMWARE_CACHE_FILES := $(addprefix $(RPI4_BOOT_FIRMWARE_DIR)/,$(RPI4_BOOT_FIRMWARE_CACHE_NAMES))
override RPI4_BOOT_FIRMWARE_DTB := $(RPI4_BOOT_FIRMWARE_DIR)/bcm2711-rpi-4-b.dtb

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override RPI4_BOOT_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,rpi4-boot-firmware,$(RPI4_BOOT_FIRMWARE_CACHE_ROOT),platform/arm64/rpi4-boot-firmware.notice,LICENCE.broadcom,$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.broadcom,3,$(RPI4_BOOT_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.broadcom,4,$(RPI4_BOOT_FIRMWARE_FILES)),$(RPI4_BOOT_FIRMWARE_ZEDBSD_URL),$(RPI4_BOOT_FIRMWARE_BASE_URL)/$(RPI4_BOOT_FIRMWARE_REVISION)/$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.broadcom,2,$(RPI4_BOOT_FIRMWARE_FILES)),$(RPI4_BOOT_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,rpi4-boot-firmware,y,$(RPI4_BOOT_FIRMWARE_DIR)),)
ifneq ($(shell $(RPI4_BOOT_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error rpi4-boot-firmware: the firmware license was not accepted; see above)
endif
endif
firmware-licenses: rpi4-boot-firmware-license
firmware-licenses-pending: rpi4-boot-firmware-license-pending
.PHONY: rpi4-boot-firmware-license rpi4-boot-firmware-license-pending
rpi4-boot-firmware-license:
	@if test ! -d '$(RPI4_BOOT_FIRMWARE_DIR)'; then $(RPI4_BOOT_FIRMWARE_LICENSE_COMMAND); fi
rpi4-boot-firmware-license-pending:
	@if test ! -d '$(RPI4_BOOT_FIRMWARE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(RPI4_BOOT_FIRMWARE_LICENSE_COMMAND); fi

.PHONY: download rpi4-boot-firmware rpi4-boot-firmware-cache \
	rpi4-boot-firmware-fixture-cache

download: rpi4-boot-firmware

rpi4-boot-firmware: $(RPI4_BOOT_FIRMWARE_CACHE_FILES)

rpi4-boot-firmware-fixture-cache: rpi4-boot-firmware-cache

# A build revalidates the complete cache.  Existing corruption is an error
# rather than an implicit update.  A missing cache is acquired in a sibling
# directory and published with one rename only after every file passes exact
# type, size, and SHA-256 checks.
rpi4-boot-firmware-cache:
	@set -eu; \
	cache='$(RPI4_BOOT_FIRMWARE_DIR)'; \
	files='$(strip $(RPI4_BOOT_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(RPI4_BOOT_FIRMWARE_ENTRY_COUNT)'; then \
			echo "rpi4-boot-firmware: cache must contain exactly $(RPI4_BOOT_FIRMWARE_ENTRY_COUNT) entries: $$directory" >&2; \
			return 1; \
		fi; \
		if test ! -d "$$directory/overlays" || test -L "$$directory/overlays"; then \
			echo "rpi4-boot-firmware: missing or unsafe cache directory: $$directory/overlays" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "rpi4-boot-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "rpi4-boot-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(RPI4_BOOT_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "rpi4-boot-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "rpi4-boot-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(RPI4_BOOT_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(RPI4_BOOT_FIRMWARE_LICENSE_COMMAND); \
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
		echo "rpi4-boot-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.rpi4-boot.$(RPI4_BOOT_FIRMWARE_REVISION).XXXXXX"); \
	mkdir "$$temporary/overlays"; \
	repository='$(RPI4_BOOT_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(RPI4_BOOT_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "rpi4-boot-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(RPI4_BOOT_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(RPI4_BOOT_FIRMWARE_BASE_URL)/$(RPI4_BOOT_FIRMWARE_REVISION)'; \
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

$(RPI4_BOOT_FIRMWARE_CACHE_FILES): rpi4-boot-firmware-cache
	@test -f "$@"
