# zedBSD optional Amlogic S7D (S905X5M) bootloader package for the ODROID-C5
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The ODROID-C5's boot ROM loads its bootloader from the SD card's sectors
# after the MBR: Amlogic's signed and encrypted boot stages (BL2, BL30, BL31,
# BL32) and Hardkernel's U-Boot (BL33), all in one file.  zedBSD neither
# builds nor changes it.  The file is Hardkernel's Yocto layer's prebuilt
# u-boot.bin.signed, pinned to one commit; the image build
# (platform/arm64/odroid-c5.mk) writes it from sector 1, as that layer's
# odroid-c5.wks does.  The root file system gets only its license texts and
# the manifest, not the bootloader itself.
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(ZEDBSD_TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-URL|SIZE|SHA-256.
override AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL := amlogic-s7d-bootloader-fixture-cache
ifneq ($(filter $(AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL))
$(error $(AMLOGIC_S7D_BOOTLOADER_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
AMLOGIC_S7D_BOOTLOADER_REVISION ?= 548e2f037e01c626e3fc964fa298464df148fd51
AMLOGIC_S7D_BOOTLOADER_FILES ?= \
	u-boot.bin.signed|https://raw.githubusercontent.com/mdrjr/meta-odroid-aml/548e2f037e01c626e3fc964fa298464df148fd51/recipes-bsp/u-boot-odroid-c5/files/u-boot.bin.signed|3379200|8c2e66919a05921d79e130cc00a977a07cf8df6b34153e66c18c3241c0809e3c \
	u-boot-odroid-c5.bb|https://raw.githubusercontent.com/mdrjr/meta-odroid-aml/548e2f037e01c626e3fc964fa298464df148fd51/recipes-bsp/u-boot-odroid-c5/u-boot-odroid-c5.bb|803|93711ea64b8037d0f3c9aa9a1602d36c54bbdb96cddaf14b254e525a84a05fec \
	GPL-2|https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/GPL-2?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120|18092|8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
AMLOGIC_S7D_BOOTLOADER_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
AMLOGIC_S7D_BOOTLOADER_LICENSE_PROMPT ?= n
AMLOGIC_S7D_BOOTLOADER_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override AMLOGIC_S7D_BOOTLOADER_REVISION := 548e2f037e01c626e3fc964fa298464df148fd51
override AMLOGIC_S7D_BOOTLOADER_FILES := \
	u-boot.bin.signed|https://raw.githubusercontent.com/mdrjr/meta-odroid-aml/548e2f037e01c626e3fc964fa298464df148fd51/recipes-bsp/u-boot-odroid-c5/files/u-boot.bin.signed|3379200|8c2e66919a05921d79e130cc00a977a07cf8df6b34153e66c18c3241c0809e3c \
	u-boot-odroid-c5.bb|https://raw.githubusercontent.com/mdrjr/meta-odroid-aml/548e2f037e01c626e3fc964fa298464df148fd51/recipes-bsp/u-boot-odroid-c5/u-boot-odroid-c5.bb|803|93711ea64b8037d0f3c9aa9a1602d36c54bbdb96cddaf14b254e525a84a05fec \
	GPL-2|https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/GPL-2?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120|18092|8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643
override AMLOGIC_S7D_BOOTLOADER_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/amlogic-s7d)
override AMLOGIC_S7D_BOOTLOADER_LICENSE_PROMPT := y
override AMLOGIC_S7D_BOOTLOADER_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override AMLOGIC_S7D_BOOTLOADER_SHA256_COMMAND := sha256sum
override AMLOGIC_S7D_BOOTLOADER_FILE_COUNT := 3

AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT ?= $(if $(ZEDBSD_TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/amlogic-s7d
# The cache holds the file set of one revision.
override AMLOGIC_S7D_BOOTLOADER_CACHE_DIR := $(AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT)/$(AMLOGIC_S7D_BOOTLOADER_REVISION)
override AMLOGIC_S7D_BOOTLOADER_CACHE_NAMES := $(foreach entry,$(AMLOGIC_S7D_BOOTLOADER_FILES),$(word 1,$(subst |, ,$(entry))))
override AMLOGIC_S7D_BOOTLOADER_CACHE_FILES := $(addprefix $(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)/,$(AMLOGIC_S7D_BOOTLOADER_CACHE_NAMES))
# The file the image build writes from the SD card's sector 1.
override AMLOGIC_S7D_BOOTLOADER_IMAGE := $(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)/u-boot.bin.signed
override AMLOGIC_S7D_BOOTLOADER_NOTICE := $(ZEDBSD_FIRMWARE_TREE)/amlogic-s7d/amlogic-s7d-bootloader.notice
override AMLOGIC_S7D_BOOTLOADER_MANIFEST := userland/firmware/amlogic-s7d/amlogic-s7d-bootloader.manifest
override AMLOGIC_S7D_BOOTLOADER_LICENSE_DIR := /usr/share/licenses/amlogic-s7d-bootloader

override AMLOGIC_S7D_BOOTLOADER_DATA := \
	$(AMLOGIC_S7D_BOOTLOADER_LICENSE_DIR)/GPL-2=$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)/GPL-2 \
	$(AMLOGIC_S7D_BOOTLOADER_LICENSE_DIR)/u-boot-odroid-c5.bb=$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)/u-boot-odroid-c5.bb \
	$(AMLOGIC_S7D_BOOTLOADER_LICENSE_DIR)/amlogic-s7d-bootloader.notice=userland/firmware/amlogic-s7d/amlogic-s7d-bootloader.notice \
	/usr/share/zedbsd/packages/amlogic-s7d-bootloader.manifest=$(AMLOGIC_S7D_BOOTLOADER_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk):
# the notice, which says what is known of Amlogic's terms, and the GPL-2.0
# text of U-Boot.
override AMLOGIC_S7D_BOOTLOADER_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,amlogic-s7d-bootloader,$(AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT),$(AMLOGIC_S7D_BOOTLOADER_NOTICE),GPL-2,$(call ZEDBSD_FIRMWARE_FIELD,GPL-2,3,$(AMLOGIC_S7D_BOOTLOADER_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,GPL-2,4,$(AMLOGIC_S7D_BOOTLOADER_FILES)),$(AMLOGIC_S7D_BOOTLOADER_ZEDBSD_URL),$(call ZEDBSD_FIRMWARE_FIELD,GPL-2,2,$(AMLOGIC_S7D_BOOTLOADER_FILES)),$(AMLOGIC_S7D_BOOTLOADER_FETCH))
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
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
amlogic-s7d-bootloader-cache:
	@set -euf; \
	cache='$(AMLOGIC_S7D_BOOTLOADER_CACHE_DIR)'; \
	files='$(strip $(AMLOGIC_S7D_BOOTLOADER_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(AMLOGIC_S7D_BOOTLOADER_FILE_COUNT)'; then \
			echo "amlogic-s7d-bootloader: cache must contain exactly $(AMLOGIC_S7D_BOOTLOADER_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
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
		done; \
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
		if test -n "$$repository"; then \
			echo "amlogic-s7d-bootloader: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(AMLOGIC_S7D_BOOTLOADER_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	for entry in $$files; do \
		old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
		fetch "$$1" "$$2"; \
	done; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(AMLOGIC_S7D_BOOTLOADER_CACHE_FILES): amlogic-s7d-bootloader-cache
	@test -f "$@"
