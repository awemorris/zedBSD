# zedBSD optional NVIDIA GSP-RM firmware package (Turing, Ampere, Ada; 570.144)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The nvrtx driver (src/drivers/gpu/nvrtx, plan/ws037/nvrtx-design.md
# section 9, plan/ws242/phase001/phase.md) boots NVIDIA's GSP-RM on a
# Turing, Ampere or Ada GPU from these files: the GSP-RM images (one for
# Turing, one for Ampere and Ada), the RISC-V bootloaders, the SEC2 booter
# load and unload images, and the falcon bootloader that runs the VBIOS
# FWSEC on Turing.  WHENCE links tu104, tu106 and tu117 to tu102 and tu116,
# ga103 to ga107 to ga102, ad103 to ad107 to ad102, and the Ada GSP-RM image
# to ga102's; the image builder installs files only, so every file is
# installed once and the driver maps each chip to the directories it reads
# instead of carrying copies.
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-PATH|SIZE|SHA-256.
override NVIDIA_GSP_FIRMWARE_FIXTURE_GOAL := nvidia-gsp-firmware-fixture-cache
ifneq ($(filter $(NVIDIA_GSP_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(NVIDIA_GSP_FIRMWARE_FIXTURE_GOAL))
$(error $(NVIDIA_GSP_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
NVIDIA_GSP_FIRMWARE_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
NVIDIA_GSP_FIRMWARE_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
NVIDIA_GSP_FIRMWARE_URL_SUFFIX ?=
NVIDIA_GSP_FIRMWARE_FILES ?= \
	gsp-570.144.bin|nvidia/tu102/gsp/gsp-570.144.bin|28542040|3052aee2872182a14d8d7c069e3a14fe4642405894b24692c4aca4101dfb1809 \
	bootloader-570.144.bin|nvidia/tu102/gsp/bootloader-570.144.bin|4196|12e987b636c2f00fa40f42fd95097515c0817b158119c584049a37faf38f8f96 \
	booter_load-570.144.bin|nvidia/tu102/gsp/booter_load-570.144.bin|59272|7bb181f544a942299bbf4642da6d9bb58bfed8459725094e2ace56647cd3ec8c \
	booter_unload-570.144.bin|nvidia/tu102/gsp/booter_unload-570.144.bin|39304|5c7f1aa37d045cc20bb81584a830e8dfa4483f1cd63943de83a58c4be19a1782 \
	bl.bin|nvidia/tu102/acr/bl.bin|1280|01326cc997716bebb0eab9fe1f463fffa0ee586079e4ca0e5bb096ab6fcfedab \
	tu116-booter_load-570.144.bin|nvidia/tu116/gsp/booter_load-570.144.bin|59016|9bd01804b4b91d92904e7735b025e07a3c935bc6fb92e3833e85c297546025b9 \
	tu116-booter_unload-570.144.bin|nvidia/tu116/gsp/booter_unload-570.144.bin|39048|bf4a2b778722dde57850839bb9f7651ae29592ce85d6df4160d2d228ff430156 \
	ga102-gsp-570.144.bin|nvidia/ga102/gsp/gsp-570.144.bin|63571696|a8c3ebeed280323aedb51c061f321e73379cce7a9ae643a33dd03915df027f7f \
	ga102-bootloader-570.144.bin|nvidia/ga102/gsp/bootloader-570.144.bin|24684|82428f532240727e95bb3083fbaaba9b2cc7b937314323f2d546ce7245f27fad \
	ga102-booter_load-570.144.bin|nvidia/ga102/gsp/booter_load-570.144.bin|61304|4497e3eff7e95c774b8a569d17b27c08c9650158d10b229d2be81cdcad9a085b \
	ga102-booter_unload-570.144.bin|nvidia/ga102/gsp/booter_unload-570.144.bin|41080|8e63db5b78d7d3e349f20a2d11099c3d7109081393cb09ffc0a28133324ae009 \
	ad102-bootloader-570.144.bin|nvidia/ad102/gsp/bootloader-570.144.bin|36972|65ab2e6b6e0fca95365c4deac79a34582abcfeb15b6ae234138f22e7183118a8 \
	ad102-booter_load-570.144.bin|nvidia/ad102/gsp/booter_load-570.144.bin|57720|8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a \
	ad102-booter_unload-570.144.bin|nvidia/ad102/gsp/booter_unload-570.144.bin|41592|975b85a14ded8e430d30f000c3c1afdd55c15dee04f35ff9dfd876acd7e67186 \
	LICENCE.nvidia|LICENCE.nvidia|6212|bc5225a57f49c5249dcf238e4ae6437811677a2a8a7f579c3d839e058653ee44 \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
NVIDIA_GSP_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
NVIDIA_GSP_FIRMWARE_LICENSE_PROMPT ?= n
NVIDIA_GSP_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override NVIDIA_GSP_FIRMWARE_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override NVIDIA_GSP_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override NVIDIA_GSP_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override NVIDIA_GSP_FIRMWARE_FILES := \
	gsp-570.144.bin|nvidia/tu102/gsp/gsp-570.144.bin|28542040|3052aee2872182a14d8d7c069e3a14fe4642405894b24692c4aca4101dfb1809 \
	bootloader-570.144.bin|nvidia/tu102/gsp/bootloader-570.144.bin|4196|12e987b636c2f00fa40f42fd95097515c0817b158119c584049a37faf38f8f96 \
	booter_load-570.144.bin|nvidia/tu102/gsp/booter_load-570.144.bin|59272|7bb181f544a942299bbf4642da6d9bb58bfed8459725094e2ace56647cd3ec8c \
	booter_unload-570.144.bin|nvidia/tu102/gsp/booter_unload-570.144.bin|39304|5c7f1aa37d045cc20bb81584a830e8dfa4483f1cd63943de83a58c4be19a1782 \
	bl.bin|nvidia/tu102/acr/bl.bin|1280|01326cc997716bebb0eab9fe1f463fffa0ee586079e4ca0e5bb096ab6fcfedab \
	tu116-booter_load-570.144.bin|nvidia/tu116/gsp/booter_load-570.144.bin|59016|9bd01804b4b91d92904e7735b025e07a3c935bc6fb92e3833e85c297546025b9 \
	tu116-booter_unload-570.144.bin|nvidia/tu116/gsp/booter_unload-570.144.bin|39048|bf4a2b778722dde57850839bb9f7651ae29592ce85d6df4160d2d228ff430156 \
	ga102-gsp-570.144.bin|nvidia/ga102/gsp/gsp-570.144.bin|63571696|a8c3ebeed280323aedb51c061f321e73379cce7a9ae643a33dd03915df027f7f \
	ga102-bootloader-570.144.bin|nvidia/ga102/gsp/bootloader-570.144.bin|24684|82428f532240727e95bb3083fbaaba9b2cc7b937314323f2d546ce7245f27fad \
	ga102-booter_load-570.144.bin|nvidia/ga102/gsp/booter_load-570.144.bin|61304|4497e3eff7e95c774b8a569d17b27c08c9650158d10b229d2be81cdcad9a085b \
	ga102-booter_unload-570.144.bin|nvidia/ga102/gsp/booter_unload-570.144.bin|41080|8e63db5b78d7d3e349f20a2d11099c3d7109081393cb09ffc0a28133324ae009 \
	ad102-bootloader-570.144.bin|nvidia/ad102/gsp/bootloader-570.144.bin|36972|65ab2e6b6e0fca95365c4deac79a34582abcfeb15b6ae234138f22e7183118a8 \
	ad102-booter_load-570.144.bin|nvidia/ad102/gsp/booter_load-570.144.bin|57720|8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a \
	ad102-booter_unload-570.144.bin|nvidia/ad102/gsp/booter_unload-570.144.bin|41592|975b85a14ded8e430d30f000c3c1afdd55c15dee04f35ff9dfd876acd7e67186 \
	LICENCE.nvidia|LICENCE.nvidia|6212|bc5225a57f49c5249dcf238e4ae6437811677a2a8a7f579c3d839e058653ee44 \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
override NVIDIA_GSP_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/nvidia-gsp)
override NVIDIA_GSP_FIRMWARE_LICENSE_PROMPT := y
override NVIDIA_GSP_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override NVIDIA_GSP_FIRMWARE_TAG := 20260410
override NVIDIA_GSP_FIRMWARE_TAG_OBJECT := 4585dd5a5f0cee08990d754701d8866d9e9266e6
override NVIDIA_GSP_FIRMWARE_VERSION := 570.144
override NVIDIA_GSP_FIRMWARE_SHA256_COMMAND := sha256sum
override NVIDIA_GSP_FIRMWARE_FILE_COUNT := 16

NVIDIA_GSP_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/nvidia-gsp
# The cache holds one file set of one revision.  The set grew from Turing's
# seven files to sixteen (ws242-p001); a cache of the old set keeps its own
# directory, so it is never mistaken for a broken cache of the new one.
override NVIDIA_GSP_FIRMWARE_SET := tu-ga-ad
override NVIDIA_GSP_FIRMWARE_CACHE_DIR := $(NVIDIA_GSP_FIRMWARE_CACHE_ROOT)/$(NVIDIA_GSP_FIRMWARE_REVISION)-$(NVIDIA_GSP_FIRMWARE_SET)
override NVIDIA_GSP_FIRMWARE_CACHE_NAMES := $(foreach entry,$(NVIDIA_GSP_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override NVIDIA_GSP_FIRMWARE_CACHE_FILES := $(addprefix $(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/,$(NVIDIA_GSP_FIRMWARE_CACHE_NAMES))
override NVIDIA_GSP_FIRMWARE_MANIFEST := userland/firmware/nvidia-gsp/nvidia-gsp-firmware.manifest
override NVIDIA_GSP_FIRMWARE_DIR := /lib/firmware/nvidia
override NVIDIA_GSP_FIRMWARE_LICENSE_DIR := /usr/share/licenses/nvidia-gsp-firmware

# The installed names, below the chip directories the driver reads.  The
# image builder installs files only, so each file is installed once, under the
# directory WHENCE gives it, and the driver maps a chip to the directories
# (src/drivers/gpu/nvrtx/identify.c): TU116 and TU117 take the tu102 image,
# bootloader and falcon bootloader with their own booters, every GA10x chip
# ga102, and every AD10x chip ad102 with the ga102 image.
override NVIDIA_GSP_FIRMWARE_DATA := \
	$(NVIDIA_GSP_FIRMWARE_DIR)/tu102/gsp/gsp-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/gsp-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/tu102/gsp/bootloader-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/bootloader-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/tu102/gsp/booter_load-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/booter_load-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/tu102/gsp/booter_unload-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/booter_unload-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/tu102/acr/bl.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/bl.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/tu116/gsp/booter_load-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/tu116-booter_load-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/tu116/gsp/booter_unload-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/tu116-booter_unload-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/ga102/gsp/gsp-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/ga102-gsp-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/ga102/gsp/bootloader-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/ga102-bootloader-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/ga102/gsp/booter_load-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/ga102-booter_load-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/ga102/gsp/booter_unload-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/ga102-booter_unload-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/ad102/gsp/bootloader-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/ad102-bootloader-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/ad102/gsp/booter_load-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/ad102-booter_load-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_DIR)/ad102/gsp/booter_unload-570.144.bin=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/ad102-booter_unload-570.144.bin \
	$(NVIDIA_GSP_FIRMWARE_LICENSE_DIR)/LICENCE.nvidia=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/LICENCE.nvidia \
	$(NVIDIA_GSP_FIRMWARE_LICENSE_DIR)/WHENCE=$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)/WHENCE \
	/usr/share/zedbsd/packages/nvidia-gsp-firmware.manifest=$(NVIDIA_GSP_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override NVIDIA_GSP_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,nvidia-gsp-firmware,$(NVIDIA_GSP_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/nvidia-gsp/nvidia-gsp-firmware.notice,LICENCE.nvidia,$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.nvidia,3,$(NVIDIA_GSP_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.nvidia,4,$(NVIDIA_GSP_FIRMWARE_FILES)),$(NVIDIA_GSP_FIRMWARE_ZEDBSD_URL),$(NVIDIA_GSP_FIRMWARE_BASE_URL)/$(call ZEDBSD_FIRMWARE_FIELD,LICENCE.nvidia,2,$(NVIDIA_GSP_FIRMWARE_FILES))$(NVIDIA_GSP_FIRMWARE_URL_SUFFIX),$(NVIDIA_GSP_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,nvidia-gsp-firmware,$(filter nvidia-gsp-firmware,$(ZEDBSD_USER_PROGRAMS)),$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(NVIDIA_GSP_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error nvidia-gsp-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter nvidia-gsp-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: nvidia-gsp-firmware-license
firmware-licenses-pending: nvidia-gsp-firmware-license-pending
endif
.PHONY: nvidia-gsp-firmware-license nvidia-gsp-firmware-license-pending
nvidia-gsp-firmware-license:
	@if test ! -d '$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)'; then $(NVIDIA_GSP_FIRMWARE_LICENSE_COMMAND); fi
nvidia-gsp-firmware-license-pending:
	@if test ! -d '$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(NVIDIA_GSP_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,nvidia-gsp-firmware,NVIDIA GSP firmware for Turing Ampere and Ada,amd64,n,package,,y,firmware,firmware/nvidia-gsp,,,data,$(NVIDIA_GSP_FIRMWARE_DATA),,))
override USERLAND_nvidia-gsp-firmware_DATA := $(NVIDIA_GSP_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += nvidia-gsp-firmware

.PHONY: download nvidia-gsp-firmware nvidia-gsp-firmware-cache \
	nvidia-gsp-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: nvidia-gsp-firmware
endif

nvidia-gsp-firmware: $(NVIDIA_GSP_FIRMWARE_CACHE_FILES)

nvidia-gsp-firmware-fixture-cache: nvidia-gsp-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
nvidia-gsp-firmware-cache:
	@set -eu; \
	cache='$(NVIDIA_GSP_FIRMWARE_CACHE_DIR)'; \
	files='$(strip $(NVIDIA_GSP_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(NVIDIA_GSP_FIRMWARE_FILE_COUNT)'; then \
			echo "nvidia-gsp-firmware: cache must contain exactly $(NVIDIA_GSP_FIRMWARE_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "nvidia-gsp-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "nvidia-gsp-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(NVIDIA_GSP_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "nvidia-gsp-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "nvidia-gsp-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(NVIDIA_GSP_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(NVIDIA_GSP_FIRMWARE_LICENSE_COMMAND); \
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
		echo "nvidia-gsp-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.nvidia-gsp.$(NVIDIA_GSP_FIRMWARE_REVISION).XXXXXX"); \
	repository='$(NVIDIA_GSP_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(NVIDIA_GSP_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "nvidia-gsp-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(NVIDIA_GSP_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(NVIDIA_GSP_FIRMWARE_BASE_URL)'; \
	suffix='$(NVIDIA_GSP_FIRMWARE_URL_SUFFIX)'; \
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

$(NVIDIA_GSP_FIRMWARE_CACHE_FILES): nvidia-gsp-firmware-cache
	@test -f "$@"
