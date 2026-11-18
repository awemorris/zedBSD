# zedBSD optional Broadcom/Cypress BCM43455 SDIO WiFi firmware package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The Raspberry Pi 4's on-board WiFi driver (plan/ws206/phase001/phase.md,
# W8 and W9) reads three files after the root is mounted and writes them
# into the BCM43455 (Cypress CYW43455): the minimal firmware 7.45.241 (the
# variant that carries the firmware supplicant, idsup), Raspberry Pi's
# regulatory data (CLM "RPi.4", with the world locale XX), and the Pi 4
# NVRAM.  All three come from one commit of Raspberry Pi's firmware-nonfree
# tree, whose debian/copyright places them under the Cypress licence.  The
# same NVRAM values are also published in linux-firmware under GPL-2.0+, so
# the package installs the plain LICENCE.cypress and the GPL-2 text of the
# official linux-firmware tag 20260410 beside the full debian/copyright and
# the provenance manifest (ws206-p003, the user's answer E4 of 2026-10-13).
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|SOURCE|UPSTREAM-PATH|SIZE|SHA-256, where SOURCE is
# nonfree (RPi-Distro/firmware-nonfree) or linux (linux-firmware).
override BCM43455_FIRMWARE_FIXTURE_GOAL := bcm43455-firmware-fixture-cache
ifneq ($(filter $(BCM43455_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(BCM43455_FIRMWARE_FIXTURE_GOAL))
$(error $(BCM43455_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
BCM43455_FIRMWARE_NONFREE_BASE_URL ?= https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree
BCM43455_FIRMWARE_NONFREE_REVISION ?= 3bab0f823f5b53150b76aab77093adef6655b920
BCM43455_FIRMWARE_LINUX_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
BCM43455_FIRMWARE_LINUX_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
BCM43455_FIRMWARE_LINUX_URL_SUFFIX ?=
BCM43455_FIRMWARE_FILES ?= \
	cyfmac43455-sdio-minimal.bin|nonfree|debian/added-firmware/cypress/cyfmac43455-sdio-minimal.bin|548728|3075cb0bdc4b28ed4f08e01b1a216d0ebc70f4022d9d3272a4a43b3c90456e60 \
	cyfmac43455-sdio.clm_blob|nonfree|debian/added-firmware/cypress/cyfmac43455-sdio.clm_blob|2676|9823842cae9fb9a5dd1e5fb31f595516ec7deee341354bef30bb3026eee29cc1 \
	brcmfmac43455-sdio.txt|nonfree|debian/added-firmware/brcm/brcmfmac43455-sdio.txt|2074|ca709be81a78bdb6932936374f39943acbd7af07fae6151011127599a3ce9e3d \
	copyright|nonfree|debian/copyright|430523|07082fd0bb65e32c73af39a2292dd6c83f169be7427d338ba1fe34f9b86c0e30 \
	LICENCE.cypress|linux|LICENCE.cypress|8914|ae0db6cc4db33941148df0f67de53e76a77b1b5a46b3165edb7040aa2750015f \
	GPL-2|linux|GPL-2|18092|8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
BCM43455_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
BCM43455_FIRMWARE_LICENSE_PROMPT ?= n
BCM43455_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override BCM43455_FIRMWARE_NONFREE_BASE_URL := https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree
override BCM43455_FIRMWARE_NONFREE_REVISION := 3bab0f823f5b53150b76aab77093adef6655b920
override BCM43455_FIRMWARE_LINUX_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override BCM43455_FIRMWARE_LINUX_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override BCM43455_FIRMWARE_LINUX_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override BCM43455_FIRMWARE_FILES := \
	cyfmac43455-sdio-minimal.bin|nonfree|debian/added-firmware/cypress/cyfmac43455-sdio-minimal.bin|548728|3075cb0bdc4b28ed4f08e01b1a216d0ebc70f4022d9d3272a4a43b3c90456e60 \
	cyfmac43455-sdio.clm_blob|nonfree|debian/added-firmware/cypress/cyfmac43455-sdio.clm_blob|2676|9823842cae9fb9a5dd1e5fb31f595516ec7deee341354bef30bb3026eee29cc1 \
	brcmfmac43455-sdio.txt|nonfree|debian/added-firmware/brcm/brcmfmac43455-sdio.txt|2074|ca709be81a78bdb6932936374f39943acbd7af07fae6151011127599a3ce9e3d \
	copyright|nonfree|debian/copyright|430523|07082fd0bb65e32c73af39a2292dd6c83f169be7427d338ba1fe34f9b86c0e30 \
	LICENCE.cypress|linux|LICENCE.cypress|8914|ae0db6cc4db33941148df0f67de53e76a77b1b5a46b3165edb7040aa2750015f \
	GPL-2|linux|GPL-2|18092|8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643
override BCM43455_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/bcm43455)
override BCM43455_FIRMWARE_LICENSE_PROMPT := y
override BCM43455_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override BCM43455_FIRMWARE_VERSION := 1:20260519-1~bpo13+1+rpt1
override BCM43455_FIRMWARE_LINUX_TAG := 20260410
override BCM43455_FIRMWARE_SHA256_COMMAND := sha256sum
override BCM43455_FIRMWARE_FILE_COUNT := 6

BCM43455_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/bcm43455
override BCM43455_FIRMWARE_CACHE_DIR := $(BCM43455_FIRMWARE_CACHE_ROOT)/$(BCM43455_FIRMWARE_NONFREE_REVISION)-$(BCM43455_FIRMWARE_LINUX_REVISION)
override BCM43455_FIRMWARE_CACHE_NAMES := $(foreach entry,$(BCM43455_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override BCM43455_FIRMWARE_CACHE_FILES := $(addprefix $(BCM43455_FIRMWARE_CACHE_DIR)/,$(BCM43455_FIRMWARE_CACHE_NAMES))
override BCM43455_FIRMWARE_MANIFEST := userland/firmware/bcm43455/bcm43455-firmware.manifest
override BCM43455_FIRMWARE_LICENSE_DIR := /usr/share/licenses/bcm43455-firmware

# The three chip files under their upstream names (the driver opens these
# paths), the two plain licences, the firmware-nonfree copyright record that
# names the licence of debian/added-firmware/*/*43455*, and the manifest.
override BCM43455_FIRMWARE_DATA := \
	/lib/firmware/cypress/cyfmac43455-sdio-minimal.bin=$(BCM43455_FIRMWARE_CACHE_DIR)/cyfmac43455-sdio-minimal.bin \
	/lib/firmware/cypress/cyfmac43455-sdio.clm_blob=$(BCM43455_FIRMWARE_CACHE_DIR)/cyfmac43455-sdio.clm_blob \
	/lib/firmware/brcm/brcmfmac43455-sdio.txt=$(BCM43455_FIRMWARE_CACHE_DIR)/brcmfmac43455-sdio.txt \
	$(BCM43455_FIRMWARE_LICENSE_DIR)/LICENCE.cypress=$(BCM43455_FIRMWARE_CACHE_DIR)/LICENCE.cypress \
	$(BCM43455_FIRMWARE_LICENSE_DIR)/GPL-2=$(BCM43455_FIRMWARE_CACHE_DIR)/GPL-2 \
	$(BCM43455_FIRMWARE_LICENSE_DIR)/firmware-nonfree.copyright=$(BCM43455_FIRMWARE_CACHE_DIR)/copyright \
	/usr/share/zedbsd/packages/bcm43455-firmware.manifest=$(BCM43455_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override BCM43455_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,bcm43455-firmware,$(BCM43455_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/bcm43455/bcm43455-firmware.notice,LICENCE.cypress,$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.cypress,4,$(BCM43455_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.cypress,5,$(BCM43455_FIRMWARE_FILES)),$(BCM43455_FIRMWARE_ZEDBSD_URL),$(BCM43455_FIRMWARE_LINUX_BASE_URL)/$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.cypress,3,$(BCM43455_FIRMWARE_FILES))$(BCM43455_FIRMWARE_LINUX_URL_SUFFIX),$(BCM43455_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,bcm43455-firmware,$(filter bcm43455-firmware,$(ZEDBSD_USER_PROGRAMS)),$(BCM43455_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(BCM43455_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error bcm43455-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter bcm43455-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: bcm43455-firmware-license
firmware-licenses-pending: bcm43455-firmware-license-pending
endif
.PHONY: bcm43455-firmware-license bcm43455-firmware-license-pending
bcm43455-firmware-license:
	@if test ! -d '$(BCM43455_FIRMWARE_CACHE_DIR)'; then $(BCM43455_FIRMWARE_LICENSE_COMMAND); fi
bcm43455-firmware-license-pending:
	@if test ! -d '$(BCM43455_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(BCM43455_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,bcm43455-firmware,Broadcom BCM43455 SDIO WiFi firmware,*,n,package,,y,firmware,firmware/bcm43455,,,data,$(BCM43455_FIRMWARE_DATA),,))
override USERLAND_bcm43455-firmware_DATA := $(BCM43455_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += bcm43455-firmware

.PHONY: download bcm43455-firmware bcm43455-firmware-cache \
	bcm43455-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: bcm43455-firmware
endif

bcm43455-firmware: $(BCM43455_FIRMWARE_CACHE_FILES)

bcm43455-firmware-fixture-cache: bcm43455-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
bcm43455-firmware-cache:
	@set -eu; \
	cache='$(BCM43455_FIRMWARE_CACHE_DIR)'; \
	files='$(strip $(BCM43455_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(BCM43455_FIRMWARE_FILE_COUNT)'; then \
			echo "bcm43455-firmware: cache must contain exactly $(BCM43455_FIRMWARE_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$4"; expected_hash="$$5"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "bcm43455-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "bcm43455-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(BCM43455_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "bcm43455-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "bcm43455-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(BCM43455_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(BCM43455_FIRMWARE_LICENSE_COMMAND); \
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
		echo "bcm43455-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.bcm43455.$(BCM43455_FIRMWARE_NONFREE_REVISION).XXXXXX"); \
	repository='$(BCM43455_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(BCM43455_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "bcm43455-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(BCM43455_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	for entry in $$files; do \
		old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
		case "$$2" in \
		nonfree) url='$(BCM43455_FIRMWARE_NONFREE_BASE_URL)/$(BCM43455_FIRMWARE_NONFREE_REVISION)'"/$$3" ;; \
		linux) url='$(BCM43455_FIRMWARE_LINUX_BASE_URL)'"/$$3"'$(BCM43455_FIRMWARE_LINUX_URL_SUFFIX)' ;; \
		*) echo "bcm43455-firmware: unknown source for $$1: $$2" >&2; exit 1 ;; \
		esac; \
		fetch "$$1" "$$url"; \
	done; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(BCM43455_FIRMWARE_CACHE_FILES): bcm43455-firmware-cache
	@test -f "$@"
