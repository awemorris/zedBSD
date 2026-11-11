# zedBSD optional Broadcom/Cypress UART Bluetooth firmware package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# bluetoothd (userland/base/bluetoothd) sends the records of
# /lib/firmware/brcm/BCM4345C0.hcd as HCI commands to the Bluetooth part of
# the Raspberry Pi 4's BCM43455 after a reset (plan/ws215/phase001/phase.md,
# B14 and section 4).  The patch file comes from Raspberry Pi's
# bluez-firmware tree, which carries it under the Cypress Wireless
# Connectivity Devices EULA; that tree keeps the licence only as a saved
# HTML page, so the plain-text LICENCE.cypress comes from the official
# linux-firmware tag 20260410 (the same blob, checked in ws215-p004).
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|SOURCE|UPSTREAM-PATH|SIZE|SHA-256, where SOURCE is
# bluez (RPi-Distro/bluez-firmware) or linux (linux-firmware).
override BRCMBT_FIRMWARE_FIXTURE_GOAL := brcmbt-firmware-fixture-cache
ifneq ($(filter $(BRCMBT_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(BRCMBT_FIRMWARE_FIXTURE_GOAL))
$(error $(BRCMBT_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
BRCMBT_FIRMWARE_BLUEZ_BASE_URL ?= https://raw.githubusercontent.com/RPi-Distro/bluez-firmware
BRCMBT_FIRMWARE_BLUEZ_REVISION ?= adb4d55c8f193ff16ece74dd72fd6e8801632103
BRCMBT_FIRMWARE_LINUX_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
BRCMBT_FIRMWARE_LINUX_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
BRCMBT_FIRMWARE_LINUX_URL_SUFFIX ?=
BRCMBT_FIRMWARE_FILES ?= \
	BCM4345C0.hcd|bluez|debian/firmware/broadcom/BCM4345C0.hcd|63806|51c45e77ddad91a19e96dc8fb75295b2087c279940df2634b23baf71b6dea42c \
	copyright|bluez|debian/copyright|23518|79576e5ea6740845bf71ce96ad4e31cd49797c8e1a259d64670f48554b8eb67c \
	LICENCE.cypress|linux|LICENCE.cypress|8914|ae0db6cc4db33941148df0f67de53e76a77b1b5a46b3165edb7040aa2750015f
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
BRCMBT_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
BRCMBT_FIRMWARE_LICENSE_PROMPT ?= n
BRCMBT_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override BRCMBT_FIRMWARE_BLUEZ_BASE_URL := https://raw.githubusercontent.com/RPi-Distro/bluez-firmware
override BRCMBT_FIRMWARE_BLUEZ_REVISION := adb4d55c8f193ff16ece74dd72fd6e8801632103
override BRCMBT_FIRMWARE_LINUX_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override BRCMBT_FIRMWARE_LINUX_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override BRCMBT_FIRMWARE_LINUX_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override BRCMBT_FIRMWARE_FILES := \
	BCM4345C0.hcd|bluez|debian/firmware/broadcom/BCM4345C0.hcd|63806|51c45e77ddad91a19e96dc8fb75295b2087c279940df2634b23baf71b6dea42c \
	copyright|bluez|debian/copyright|23518|79576e5ea6740845bf71ce96ad4e31cd49797c8e1a259d64670f48554b8eb67c \
	LICENCE.cypress|linux|LICENCE.cypress|8914|ae0db6cc4db33941148df0f67de53e76a77b1b5a46b3165edb7040aa2750015f
override BRCMBT_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/brcmbt)
override BRCMBT_FIRMWARE_LICENSE_PROMPT := y
override BRCMBT_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override BRCMBT_FIRMWARE_VERSION := 1.2-9+rpt4
override BRCMBT_FIRMWARE_LINUX_TAG := 20260410
override BRCMBT_FIRMWARE_SHA256_COMMAND := sha256sum
override BRCMBT_FIRMWARE_FILE_COUNT := 3

BRCMBT_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/brcmbt
override BRCMBT_FIRMWARE_CACHE_DIR := $(BRCMBT_FIRMWARE_CACHE_ROOT)/$(BRCMBT_FIRMWARE_BLUEZ_REVISION)-$(BRCMBT_FIRMWARE_LINUX_REVISION)
override BRCMBT_FIRMWARE_CACHE_NAMES := $(foreach entry,$(BRCMBT_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override BRCMBT_FIRMWARE_CACHE_FILES := $(addprefix $(BRCMBT_FIRMWARE_CACHE_DIR)/,$(BRCMBT_FIRMWARE_CACHE_NAMES))
override BRCMBT_FIRMWARE_MANIFEST := userland/firmware/brcmbt/brcmbt-firmware.manifest
override BRCMBT_FIRMWARE_DIR := /lib/firmware/brcm
override BRCMBT_FIRMWARE_LICENSE_DIR := /usr/share/licenses/brcmbt-firmware

# The patch file under the name bluetoothd's chip table gives it, the plain
# licence, the bluez-firmware copyright record naming the licence of
# debian/firmware/broadcom/*, and the provenance manifest.
override BRCMBT_FIRMWARE_DATA := \
	$(BRCMBT_FIRMWARE_DIR)/BCM4345C0.hcd=$(BRCMBT_FIRMWARE_CACHE_DIR)/BCM4345C0.hcd \
	$(BRCMBT_FIRMWARE_LICENSE_DIR)/LICENCE.cypress=$(BRCMBT_FIRMWARE_CACHE_DIR)/LICENCE.cypress \
	$(BRCMBT_FIRMWARE_LICENSE_DIR)/bluez-firmware.copyright=$(BRCMBT_FIRMWARE_CACHE_DIR)/copyright \
	/usr/share/zedbsd/packages/brcmbt-firmware.manifest=$(BRCMBT_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override BRCMBT_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,brcmbt-firmware,$(BRCMBT_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/brcmbt/brcmbt-firmware.notice,LICENCE.cypress,$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.cypress,4,$(BRCMBT_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.cypress,5,$(BRCMBT_FIRMWARE_FILES)),$(BRCMBT_FIRMWARE_ZEDBSD_URL),$(BRCMBT_FIRMWARE_LINUX_BASE_URL)/$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.cypress,3,$(BRCMBT_FIRMWARE_FILES))$(BRCMBT_FIRMWARE_LINUX_URL_SUFFIX),$(BRCMBT_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,brcmbt-firmware,$(filter brcmbt-firmware,$(ZEDBSD_USER_PROGRAMS)),$(BRCMBT_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(BRCMBT_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error brcmbt-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter brcmbt-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: brcmbt-firmware-license
firmware-licenses-pending: brcmbt-firmware-license-pending
endif
.PHONY: brcmbt-firmware-license brcmbt-firmware-license-pending
brcmbt-firmware-license:
	@if test ! -d '$(BRCMBT_FIRMWARE_CACHE_DIR)'; then $(BRCMBT_FIRMWARE_LICENSE_COMMAND); fi
brcmbt-firmware-license-pending:
	@if test ! -d '$(BRCMBT_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(BRCMBT_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,brcmbt-firmware,Broadcom UART Bluetooth firmware,*,n,package,,y,firmware,firmware/brcmbt,,,data,$(BRCMBT_FIRMWARE_DATA),,))
override USERLAND_brcmbt-firmware_DATA := $(BRCMBT_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += brcmbt-firmware

.PHONY: download brcmbt-firmware brcmbt-firmware-cache \
	brcmbt-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: brcmbt-firmware
endif

brcmbt-firmware: $(BRCMBT_FIRMWARE_CACHE_FILES)

brcmbt-firmware-fixture-cache: brcmbt-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
brcmbt-firmware-cache:
	@set -eu; \
	cache='$(BRCMBT_FIRMWARE_CACHE_DIR)'; \
	files='$(strip $(BRCMBT_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(BRCMBT_FIRMWARE_FILE_COUNT)'; then \
			echo "brcmbt-firmware: cache must contain exactly $(BRCMBT_FIRMWARE_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$4"; expected_hash="$$5"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "brcmbt-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "brcmbt-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(BRCMBT_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "brcmbt-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "brcmbt-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(BRCMBT_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(BRCMBT_FIRMWARE_LICENSE_COMMAND); \
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
		echo "brcmbt-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.brcmbt.$(BRCMBT_FIRMWARE_BLUEZ_REVISION).XXXXXX"); \
	repository='$(BRCMBT_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(BRCMBT_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "brcmbt-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(BRCMBT_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	for entry in $$files; do \
		old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
		case "$$2" in \
		bluez) url='$(BRCMBT_FIRMWARE_BLUEZ_BASE_URL)/$(BRCMBT_FIRMWARE_BLUEZ_REVISION)'"/$$3" ;; \
		linux) url='$(BRCMBT_FIRMWARE_LINUX_BASE_URL)'"/$$3"'$(BRCMBT_FIRMWARE_LINUX_URL_SUFFIX)' ;; \
		*) echo "brcmbt-firmware: unknown source for $$1: $$2" >&2; exit 1 ;; \
		esac; \
		fetch "$$1" "$$url"; \
	done; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(BRCMBT_FIRMWARE_CACHE_FILES): brcmbt-firmware-cache
	@test -f "$@"
