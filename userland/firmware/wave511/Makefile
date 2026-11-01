# zedBSD optional Chips&Media Wave511 video decoder firmware package (0002d46a)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The VisionFive 2's Wave511 video decoder (src/drivers/vpu, the board's glue
# in src/drivers/platform/visionfive2/vf2-vpu.c) boots this program,
# /lib/firmware/wave511/chagall.bin, on the decoder's own processor.  It is
# StarFive's soft_3rdpart repository's wave511/firmware/chagall.bin with the
# LICENSE.txt beside it (Chips&Media, binary redistribution only, no reverse
# engineering): investigate the firmware only through its command replies
# and register values.
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(ZEDBSD_TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-PATH|SIZE|SHA-256.
override WAVE511_FIRMWARE_FIXTURE_GOAL := wave511-firmware-fixture-cache
ifneq ($(filter $(WAVE511_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(WAVE511_FIRMWARE_FIXTURE_GOAL))
$(error $(WAVE511_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
WAVE511_FIRMWARE_BASE_URL ?= https://raw.githubusercontent.com/starfive-tech/soft_3rdpart/b60da16be1b36453aa46599889c28310fd148fb8
WAVE511_FIRMWARE_REVISION ?= b60da16be1b36453aa46599889c28310fd148fb8
WAVE511_FIRMWARE_URL_SUFFIX ?=
WAVE511_FIRMWARE_FILES ?= \
	chagall.bin|wave511/firmware/chagall.bin|949120|aad0383bc9ea7694b7038e903710fe5ba8571a74769c909fecc99623b2ec252e \
	LICENSE.txt|wave511/firmware/LICENSE.txt|1175|8c15e1830a2e7638ca982f22db27c226df3f4b05771f8b28369549e0298cea2c
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
WAVE511_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
WAVE511_FIRMWARE_LICENSE_PROMPT ?= n
WAVE511_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override WAVE511_FIRMWARE_BASE_URL := https://raw.githubusercontent.com/starfive-tech/soft_3rdpart/b60da16be1b36453aa46599889c28310fd148fb8
override WAVE511_FIRMWARE_REVISION := b60da16be1b36453aa46599889c28310fd148fb8
override WAVE511_FIRMWARE_URL_SUFFIX :=
override WAVE511_FIRMWARE_FILES := \
	chagall.bin|wave511/firmware/chagall.bin|949120|aad0383bc9ea7694b7038e903710fe5ba8571a74769c909fecc99623b2ec252e \
	LICENSE.txt|wave511/firmware/LICENSE.txt|1175|8c15e1830a2e7638ca982f22db27c226df3f4b05771f8b28369549e0298cea2c
override WAVE511_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/wave511)
override WAVE511_FIRMWARE_LICENSE_PROMPT := y
override WAVE511_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override WAVE511_FIRMWARE_VERSION := 0002d46a
override WAVE511_FIRMWARE_SHA256_COMMAND := sha256sum
override WAVE511_FIRMWARE_FILE_COUNT := 2

WAVE511_FIRMWARE_CACHE_ROOT ?= $(if $(ZEDBSD_TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/wave511
# The cache holds the file set of one revision.
override WAVE511_FIRMWARE_CACHE_DIR := $(WAVE511_FIRMWARE_CACHE_ROOT)/$(WAVE511_FIRMWARE_REVISION)
override WAVE511_FIRMWARE_CACHE_NAMES := $(foreach entry,$(WAVE511_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override WAVE511_FIRMWARE_CACHE_FILES := $(addprefix $(WAVE511_FIRMWARE_CACHE_DIR)/,$(WAVE511_FIRMWARE_CACHE_NAMES))
override WAVE511_FIRMWARE_MANIFEST := userland/firmware/wave511/wave511-firmware.manifest
override WAVE511_FIRMWARE_DIR := /lib/firmware/wave511
override WAVE511_FIRMWARE_LICENSE_DIR := /usr/share/licenses/wave511-firmware

# The installed names, below the directory the driver reads them from.
override WAVE511_FIRMWARE_DATA := \
	$(WAVE511_FIRMWARE_DIR)/chagall.bin=$(WAVE511_FIRMWARE_CACHE_DIR)/chagall.bin \
	$(WAVE511_FIRMWARE_LICENSE_DIR)/LICENSE.txt=$(WAVE511_FIRMWARE_CACHE_DIR)/LICENSE.txt \
	/usr/share/zedbsd/packages/wave511-firmware.manifest=$(WAVE511_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override WAVE511_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,wave511-firmware,$(WAVE511_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/wave511/wave511-firmware.notice,LICENSE.txt,$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.txt,3,$(WAVE511_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.txt,4,$(WAVE511_FIRMWARE_FILES)),$(WAVE511_FIRMWARE_ZEDBSD_URL),$(WAVE511_FIRMWARE_BASE_URL)/$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.txt,2,$(WAVE511_FIRMWARE_FILES))$(WAVE511_FIRMWARE_URL_SUFFIX),$(WAVE511_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,wave511-firmware,$(filter wave511-firmware,$(ZEDBSD_USER_PROGRAMS)),$(WAVE511_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(WAVE511_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error wave511-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter wave511-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: wave511-firmware-license
firmware-licenses-pending: wave511-firmware-license-pending
endif
.PHONY: wave511-firmware-license wave511-firmware-license-pending
wave511-firmware-license:
	@if test ! -d '$(WAVE511_FIRMWARE_CACHE_DIR)'; then $(WAVE511_FIRMWARE_LICENSE_COMMAND); fi
wave511-firmware-license-pending:
	@if test ! -d '$(WAVE511_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(WAVE511_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call ZEDBSD_USERLAND_PACKAGE,wave511-firmware,Chips&Media Wave511 video decoder firmware for VisionFive 2,*,n,package,,y,firmware,firmware/wave511,,,data,$(WAVE511_FIRMWARE_DATA),,))
override USERLAND_wave511-firmware_DATA := $(WAVE511_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += wave511-firmware

.PHONY: download wave511-firmware wave511-firmware-cache \
	wave511-firmware-fixture-cache

ifndef ZEDBSD_TOPLEVEL_BUILD
download: wave511-firmware
endif

wave511-firmware: $(WAVE511_FIRMWARE_CACHE_FILES)

wave511-firmware-fixture-cache: wave511-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
wave511-firmware-cache:
	@set -eu; \
	cache='$(WAVE511_FIRMWARE_CACHE_DIR)'; \
	files='$(strip $(WAVE511_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(WAVE511_FIRMWARE_FILE_COUNT)'; then \
			echo "wave511-firmware: cache must contain exactly $(WAVE511_FIRMWARE_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "wave511-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "wave511-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(WAVE511_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "wave511-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "wave511-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(WAVE511_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(WAVE511_FIRMWARE_LICENSE_COMMAND); \
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
		echo "wave511-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.wave511.$(WAVE511_FIRMWARE_REVISION).XXXXXX"); \
	repository='$(WAVE511_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(WAVE511_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "wave511-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(WAVE511_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(WAVE511_FIRMWARE_BASE_URL)'; \
	suffix='$(WAVE511_FIRMWARE_URL_SUFFIX)'; \
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

$(WAVE511_FIRMWARE_CACHE_FILES): wave511-firmware-cache
	@test -f "$@"
