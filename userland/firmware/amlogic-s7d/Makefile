# zedBSD optional Amlogic S7D (S905X5M) bootloader package for the ODROID-C5
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The ODROID-C5's boot ROM loads its bootloader from the SD card's sectors
# after the MBR: Amlogic's signed and encrypted boot stages (BL2, BL30, BL31,
# BL32) and Hardkernel's U-Boot (BL33), all in one area.  zedBSD neither
# builds nor changes it.  The source is Hardkernel's official ODROID-C5 card
# image, kept unmodified (under its own name, with Amlogic's license) in the
# zedBSD firmware repository; the fetch checks it, decompresses it, takes its
# sectors 1 to 6538 and checks those too, and the image build
# (platform/arm64/odroid-c5.mk) writes them from sector 1, where
# Hardkernel's image has them.  The root file system gets only the license,
# the notice and the manifest, not the bootloader itself.
#
# A build cannot fetch Hardkernel's image from Hardkernel, so the firmware
# repository is the only network source.  Without it the two files are
# placed by hand in $(AMLOGIC_S7D_BOOTLOADER_BY_HAND_DIR) (the notice says
# how) and are checked the same way.
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(ZEDBSD_TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-URL|SIZE|SHA-256; an UPSTREAM-URL of
# "by-hand" means the file has no upstream a build can fetch, and the copy
# placed by hand in $(AMLOGIC_S7D_BOOTLOADER_BY_HAND_DIR) is the fallback.
# The area entry is AREA-NAME|SOURCE-NAME|FIRST-SECTOR|LAST-SECTOR|SIZE|
# SHA-256: the bootloader the image build writes, cut from the uncompressed
# SOURCE-NAME.
override AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL := amlogic-s7d-bootloader-fixture-cache
ifneq ($(filter $(AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL))
$(error $(AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
AMLOGIC_S7D_BOOTLOADER_REVISION ?= odroidc5-20260610
AMLOGIC_S7D_BOOTLOADER_FILES ?= \
	ubuntu-22.04-gnome-desktop-odroidc5-20260610-bsp-boot.img.xz|by-hand|37760380|04b9817d4e5e27ac4cc46dc4ad48fbc164e9a6c6f42eb1b0ba8d7a927accc5f2 \
	LICENSE.amlogic|by-hand|1273|5e8f7ad650a0af4a14ad5641817323f654d0e1b8dc2019aa6f0944ba9108ff65
AMLOGIC_S7D_BOOTLOADER_AREA ?= odroidc5-20260610-bootloader.bin|ubuntu-22.04-gnome-desktop-odroidc5-20260610-bsp-boot.img.xz|1|6538|3347456|6c5316ed320c78c9c2a67665cdee1089339be25bee625cf1eaffb0a6277c6562
# The package directory of the zedBSD firmware repository; empty leaves
# only the files placed by hand (userland/firmware/zedbsd-firmware.mk).
AMLOGIC_S7D_BOOTLOADER_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
AMLOGIC_S7D_BOOTLOADER_LICENSE_PROMPT ?= n
AMLOGIC_S7D_BOOTLOADER_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override AMLOGIC_S7D_BOOTLOADER_REVISION := odroidc5-20260610
override AMLOGIC_S7D_BOOTLOADER_FILES := \
	ubuntu-22.04-gnome-desktop-odroidc5-20260610-bsp-boot.img.xz|by-hand|37760380|04b9817d4e5e27ac4cc46dc4ad48fbc164e9a6c6f42eb1b0ba8d7a927accc5f2 \
	LICENSE.amlogic|by-hand|1273|5e8f7ad650a0af4a14ad5641817323f654d0e1b8dc2019aa6f0944ba9108ff65
override AMLOGIC_S7D_BOOTLOADER_AREA := odroidc5-20260610-bootloader.bin|ubuntu-22.04-gnome-desktop-odroidc5-20260610-bsp-boot.img.xz|1|6538|3347456|6c5316ed320c78c9c2a67665cdee1089339be25bee625cf1eaffb0a6277c6562
override AMLOGIC_S7D_BOOTLOADER_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/amlogic-s7d)
override AMLOGIC_S7D_BOOTLOADER_LICENSE_PROMPT := y
override AMLOGIC_S7D_BOOTLOADER_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override AMLOGIC_S7D_BOOTLOADER_SHA256_COMMAND := sha256sum
# The fetched files and the area cut from the image.
override AMLOGIC_S7D_BOOTLOADER_FILE_COUNT := 3

AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT ?= $(if $(ZEDBSD_TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/amlogic-s7d
# The cache holds the file set of one revision.
override AMLOGIC_S7D_BOOTLOADER_CACHE_DIR := $(AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT)/$(AMLOGIC_S7D_BOOTLOADER_REVISION)
# Where the files are placed by hand when the firmware repository is not
# available: the official image as Hardkernel distributes it and the license
# text, under the names in AMLOGIC_S7D_BOOTLOADER_FILES.
override AMLOGIC_S7D_BOOTLOADER_BY_HAND_DIR := $(AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT)/by-hand
override AMLOGIC_S7D_BOOTLOADER_AREA_NAME := $(word 1,$(subst |, ,$(AMLOGIC_S7D_BOOTLOADER_AREA)))
override AMLOGIC_S7D_BOOTLOADER_CACHE_NAMES := $(foreach entry,$(AMLOGIC_S7D_BOOTLOADER_FILES),$(word 1,$(subst |, ,$(entry)))) $(AMLOGIC_S7D_BOOTLOADER_AREA_NAME)
override AMLOGIC_S7D_BOOTLOADER_CACHE_FILES := $(addprefix $(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)/,$(AMLOGIC_S7D_BOOTLOADER_CACHE_NAMES))
# The file the image build writes from the SD card's sector 1.
override AMLOGIC_S7D_BOOTLOADER_IMAGE := $(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)/$(AMLOGIC_S7D_BOOTLOADER_AREA_NAME)
override AMLOGIC_S7D_BOOTLOADER_NOTICE := $(ZEDBSD_FIRMWARE_TREE)/amlogic-s7d/amlogic-s7d-bootloader.notice
override AMLOGIC_S7D_BOOTLOADER_MANIFEST := userland/firmware/amlogic-s7d/amlogic-s7d-bootloader.manifest
override AMLOGIC_S7D_BOOTLOADER_LICENSE_DIR := /usr/share/licenses/amlogic-s7d-bootloader

override AMLOGIC_S7D_BOOTLOADER_DATA := \
	$(AMLOGIC_S7D_BOOTLOADER_LICENSE_DIR)/LICENSE.amlogic=$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)/LICENSE.amlogic \
	$(AMLOGIC_S7D_BOOTLOADER_LICENSE_DIR)/amlogic-s7d-bootloader.notice=userland/firmware/amlogic-s7d/amlogic-s7d-bootloader.notice \
	/usr/share/zedbsd/packages/amlogic-s7d-bootloader.manifest=$(AMLOGIC_S7D_BOOTLOADER_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk):
# the notice, which says the file is for the ODROID-C5 only, and Amlogic's
# license text.  The text has no upstream; without the firmware repository
# it is read from the copy placed by hand.
override AMLOGIC_S7D_BOOTLOADER_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,amlogic-s7d-bootloader,$(AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT),$(AMLOGIC_S7D_BOOTLOADER_NOTICE),LICENSE.amlogic,$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.amlogic,3,$(AMLOGIC_S7D_BOOTLOADER_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.amlogic,4,$(AMLOGIC_S7D_BOOTLOADER_FILES)),$(AMLOGIC_S7D_BOOTLOADER_ZEDBSD_URL),file://$(abspath $(AMLOGIC_S7D_BOOTLOADER_BY_HAND_DIR))/LICENSE.amlogic,$(AMLOGIC_S7D_BOOTLOADER_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,amlogic-s7d-bootloader,$(filter amlogic-s7d-bootloader,$(ZEDBSD_USER_PROGRAMS)),$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)),)
ifneq ($(shell $(AMLOGIC_S7D_BOOTLOADER_LICENSE_COMMAND) && echo accepted),accepted)
$(error amlogic-s7d-bootloader: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter amlogic-s7d-bootloader,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: amlogic-s7d-bootloader-license
firmware-licenses-pending: amlogic-s7d-bootloader-license-pending
endif
.PHONY: amlogic-s7d-bootloader-license amlogic-s7d-bootloader-license-pending
amlogic-s7d-bootloader-license:
	@if test ! -d '$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)'; then $(AMLOGIC_S7D_BOOTLOADER_LICENSE_COMMAND); fi
amlogic-s7d-bootloader-license-pending:
	@if test ! -d '$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(AMLOGIC_S7D_BOOTLOADER_LICENSE_COMMAND); fi

$(eval $(call ZEDBSD_USERLAND_PACKAGE,amlogic-s7d-bootloader,Amlogic S905X5M bootloader (BL2-BL32 and U-Boot) for ODROID-C5,*,n,package,,y,firmware,firmware/amlogic-s7d,,,data,$(AMLOGIC_S7D_BOOTLOADER_DATA),,))
override USERLAND_amlogic-s7d-bootloader_DATA := $(AMLOGIC_S7D_BOOTLOADER_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += amlogic-s7d-bootloader

.PHONY: download amlogic-s7d-bootloader amlogic-s7d-bootloader-cache \
	amlogic-s7d-bootloader-fixture-cache

ifndef ZEDBSD_TOPLEVEL_BUILD
download: amlogic-s7d-bootloader
endif

amlogic-s7d-bootloader: $(AMLOGIC_S7D_BOOTLOADER_CACHE_FILES)

amlogic-s7d-bootloader-fixture-cache: amlogic-s7d-bootloader-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file,
# and the area cut from the image, passes exact type, size, and SHA-256
# checks.
amlogic-s7d-bootloader-cache:
	@set -euf; \
	cache='$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)'; \
	files='$(strip $(AMLOGIC_S7D_BOOTLOADER_FILES))'; \
	area='$(strip $(AMLOGIC_S7D_BOOTLOADER_AREA))'; \
	by_hand='$(AMLOGIC_S7D_BOOTLOADER_BY_HAND_DIR)'; \
	verify_file() { \
		file="$$1"; expected_size="$$2"; expected_hash="$$3"; \
		if test ! -f "$$file" || test -L "$$file"; then \
			echo "amlogic-s7d-bootloader: missing or unsafe cache file: $$file" >&2; \
			return 1; \
		fi; \
		actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
		if test "$$actual_size" != "$$expected_size"; then \
			echo "amlogic-s7d-bootloader: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
			return 1; \
		fi; \
		actual_hash=$$('$(AMLOGIC_S7D_BOOTLOADER_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
		if test "$$actual_hash" != "$$expected_hash"; then \
			echo "amlogic-s7d-bootloader: SHA-256 mismatch for $$file" >&2; \
			return 1; \
		fi; \
	}; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(AMLOGIC_S7D_BOOTLOADER_FILE_COUNT)'; then \
			echo "amlogic-s7d-bootloader: cache must contain exactly $(AMLOGIC_S7D_BOOTLOADER_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			verify_file "$$directory/$$1" "$$3" "$$4" || return 1; \
		done; \
		old_ifs=$$IFS; IFS='|'; set -- $$area; IFS=$$old_ifs; \
		verify_file "$$directory/$$1" "$$5" "$$6" || return 1; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "amlogic-s7d-bootloader: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(AMLOGIC_S7D_BOOTLOADER_LICENSE_PROMPT)' = y; then \
		$(AMLOGIC_S7D_BOOTLOADER_LICENSE_COMMAND); \
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
		echo "amlogic-s7d-bootloader: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.amlogic-s7d.$(AMLOGIC_S7D_BOOTLOADER_REVISION).XXXXXX"); \
	repository='$(AMLOGIC_S7D_BOOTLOADER_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(AMLOGIC_S7D_BOOTLOADER_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test "$$2" != by-hand; then \
			if test -n "$$repository"; then \
				echo "amlogic-s7d-bootloader: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
			fi; \
			$(AMLOGIC_S7D_BOOTLOADER_FETCH) --output "$$temporary/$$1" "$$2"; \
			return; \
		fi; \
		if test -f "$$by_hand/$$1" && test ! -L "$$by_hand/$$1"; then \
			echo "amlogic-s7d-bootloader: using $$by_hand/$$1, placed by hand" >&2; \
			cp -- "$$by_hand/$$1" "$$temporary/$$1"; \
			return; \
		fi; \
		echo "amlogic-s7d-bootloader: $$1 has no upstream a build can fetch; the zedBSD firmware repository ($${repository:-not set}) did not give it, and it was not placed by hand in $$by_hand." >&2; \
		echo "  Place it there as $(AMLOGIC_S7D_BOOTLOADER_NOTICE) says, or build without the package." >&2; \
		return 1; \
	}; \
	for entry in $$files; do \
		old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
		fetch "$$1" "$$2"; \
		verify_file "$$temporary/$$1" "$$3" "$$4"; \
	done; \
	old_ifs=$$IFS; IFS='|'; set -- $$area; IFS=$$old_ifs; \
	xz -dc -- "$$temporary/$$2" | head -c "$$(( ($$4 + 1) * 512 ))" | \
		tail -c "$$(( ($$4 - $$3 + 1) * 512 ))" > "$$temporary/$$1"; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(AMLOGIC_S7D_BOOTLOADER_CACHE_FILES): amlogic-s7d-bootloader-cache
	@test -f "$@"
