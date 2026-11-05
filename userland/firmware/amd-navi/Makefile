# zedBSD optional AMD Navi10 firmware package (RDNA1; linux-firmware 20260410)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The amdrdna driver (src/drivers/gpu/amdrdna, plan/ws039/design.md section
# 4.1) has the GPU's security processor load these files on a Navi10 GPU
# (Radeon RX 5600 and RX 5700): the security processor's boot pieces and
# secure OS (sos), the display security application (asd), the
# system-management unit's firmware (smc), the graphics pipe's PFP, ME and
# CE, the compute processors (mec, mec2), the run-list controller (rlc) and
# the two copy engines (sdma, sdma1).  The video decoder's firmware (vcn),
# the trusted applications (ta) and gpu_info are not used and not installed.
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

# Each entry is CACHE-NAME|UPSTREAM-PATH|SIZE|SHA-256.
override AMD_NAVI_FIRMWARE_FIXTURE_GOAL := amd-navi-firmware-fixture-cache
ifneq ($(filter $(AMD_NAVI_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(AMD_NAVI_FIRMWARE_FIXTURE_GOAL))
$(error $(AMD_NAVI_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic focused fixture may replace acquisition identities.  This
# goal cannot assemble an image, and every later production goal restores the
# immutable values in the other branch.
AMD_NAVI_FIRMWARE_BASE_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
AMD_NAVI_FIRMWARE_REVISION ?= dc85ccedc9c973682fbcf4d628ca61174bcc3120
AMD_NAVI_FIRMWARE_URL_SUFFIX ?=
AMD_NAVI_FIRMWARE_FILES ?= \
	navi10_sos.bin|amdgpu/navi10_sos.bin|184512|3112ea8519545ac89c1623e2e08e841a6a9bc31c2aab2f9b787979e51277e395 \
	navi10_asd.bin|amdgpu/navi10_asd.bin|205312|2d084493e763e60cd90ac25b054f73e4857a24864334cd3c5b092f3273cd12de \
	navi10_smc.bin|amdgpu/navi10_smc.bin|267970|49ed339d0ff71fe64347380ee12b00c3c69a546f454bcd4323c411c72fe6fb2e \
	navi10_pfp.bin|amdgpu/navi10_pfp.bin|263424|310032fe0698871b80be6eb5877ae7ed3395cb712629698c5c631e0af7c4e04a \
	navi10_me.bin|amdgpu/navi10_me.bin|263424|faf2f9ff33580b53f7b90f410447efa1d2b21366f344ff5a383d074bf1ea8c35 \
	navi10_ce.bin|amdgpu/navi10_ce.bin|263296|074ec5270eb7cad73906a1a591c274a40895b170091bc8068180a4c4d7771165 \
	navi10_mec.bin|amdgpu/navi10_mec.bin|268160|936de4938bcfd5e1691b1e544247000f7b4b7430eef317e28f8560bb9e88e758 \
	navi10_mec2.bin|amdgpu/navi10_mec2.bin|268160|936de4938bcfd5e1691b1e544247000f7b4b7430eef317e28f8560bb9e88e758 \
	navi10_rlc.bin|amdgpu/navi10_rlc.bin|43952|1f52478fb78dee904361b93ad67955fcff151bb7a7772bbfbd2c448c814be1f5 \
	navi10_sdma.bin|amdgpu/navi10_sdma.bin|33792|3fc635b9a17819eaf13913882676f0cd07a6c1a31c87b3ef0dcedebf594c8dd0 \
	navi10_sdma1.bin|amdgpu/navi10_sdma1.bin|33792|317b398d0563c8c89b10c31712296843143385f5a1ddcee2031536fe67576c3b \
	LICENSE.amdgpu|LICENSE.amdgpu|2938|572872598565dc3513470de971a32bf9db301f47afeef3636345eadae33b2eee \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
# The package directory of the zedBSD firmware repository; empty fetches
# from the upstream alone (userland/firmware/zedbsd-firmware.mk).
AMD_NAVI_FIRMWARE_ZEDBSD_URL ?=
# The fixture does not ask about the license unless it sets this to y.
AMD_NAVI_FIRMWARE_LICENSE_PROMPT ?= n
AMD_NAVI_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
else
# Production callers cannot replace the source, accepted bytes, verifier, or
# installed mapping from the command line while the static manifest continues
# to describe this reviewed package.
override AMD_NAVI_FIRMWARE_BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
override AMD_NAVI_FIRMWARE_REVISION := dc85ccedc9c973682fbcf4d628ca61174bcc3120
override AMD_NAVI_FIRMWARE_URL_SUFFIX := ?id=dc85ccedc9c973682fbcf4d628ca61174bcc3120
override AMD_NAVI_FIRMWARE_FILES := \
	navi10_sos.bin|amdgpu/navi10_sos.bin|184512|3112ea8519545ac89c1623e2e08e841a6a9bc31c2aab2f9b787979e51277e395 \
	navi10_asd.bin|amdgpu/navi10_asd.bin|205312|2d084493e763e60cd90ac25b054f73e4857a24864334cd3c5b092f3273cd12de \
	navi10_smc.bin|amdgpu/navi10_smc.bin|267970|49ed339d0ff71fe64347380ee12b00c3c69a546f454bcd4323c411c72fe6fb2e \
	navi10_pfp.bin|amdgpu/navi10_pfp.bin|263424|310032fe0698871b80be6eb5877ae7ed3395cb712629698c5c631e0af7c4e04a \
	navi10_me.bin|amdgpu/navi10_me.bin|263424|faf2f9ff33580b53f7b90f410447efa1d2b21366f344ff5a383d074bf1ea8c35 \
	navi10_ce.bin|amdgpu/navi10_ce.bin|263296|074ec5270eb7cad73906a1a591c274a40895b170091bc8068180a4c4d7771165 \
	navi10_mec.bin|amdgpu/navi10_mec.bin|268160|936de4938bcfd5e1691b1e544247000f7b4b7430eef317e28f8560bb9e88e758 \
	navi10_mec2.bin|amdgpu/navi10_mec2.bin|268160|936de4938bcfd5e1691b1e544247000f7b4b7430eef317e28f8560bb9e88e758 \
	navi10_rlc.bin|amdgpu/navi10_rlc.bin|43952|1f52478fb78dee904361b93ad67955fcff151bb7a7772bbfbd2c448c814be1f5 \
	navi10_sdma.bin|amdgpu/navi10_sdma.bin|33792|3fc635b9a17819eaf13913882676f0cd07a6c1a31c87b3ef0dcedebf594c8dd0 \
	navi10_sdma1.bin|amdgpu/navi10_sdma1.bin|33792|317b398d0563c8c89b10c31712296843143385f5a1ddcee2031536fe67576c3b \
	LICENSE.amdgpu|LICENSE.amdgpu|2938|572872598565dc3513470de971a32bf9db301f47afeef3636345eadae33b2eee \
	WHENCE|WHENCE|425450|c282239a5a2d849677e9304e6f361e475e1b6e71e7c771c03f8986f71b309527
override AMD_NAVI_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/amd-navi)
override AMD_NAVI_FIRMWARE_LICENSE_PROMPT := y
override AMD_NAVI_FIRMWARE_FETCH := curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
endif

override AMD_NAVI_FIRMWARE_TAG := 20260410
override AMD_NAVI_FIRMWARE_TAG_OBJECT := 4585dd5a5f0cee08990d754701d8866d9e9266e6
override AMD_NAVI_FIRMWARE_VERSION := 20260410
override AMD_NAVI_FIRMWARE_SHA256_COMMAND := sha256sum
override AMD_NAVI_FIRMWARE_FILE_COUNT := 13

AMD_NAVI_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/amd-navi
# The cache holds the file set of one revision.
override AMD_NAVI_FIRMWARE_CACHE_DIR := $(AMD_NAVI_FIRMWARE_CACHE_ROOT)/$(AMD_NAVI_FIRMWARE_REVISION)
override AMD_NAVI_FIRMWARE_CACHE_NAMES := $(foreach entry,$(AMD_NAVI_FIRMWARE_FILES),$(word 1,$(subst |, ,$(entry))))
override AMD_NAVI_FIRMWARE_CACHE_FILES := $(addprefix $(AMD_NAVI_FIRMWARE_CACHE_DIR)/,$(AMD_NAVI_FIRMWARE_CACHE_NAMES))
override AMD_NAVI_FIRMWARE_MANIFEST := userland/firmware/amd-navi/amd-navi-firmware.manifest
override AMD_NAVI_FIRMWARE_DIR := /lib/firmware/amdgpu
override AMD_NAVI_FIRMWARE_LICENSE_DIR := /usr/share/licenses/amd-navi-firmware

# The installed names, below the directory the driver reads them from.
override AMD_NAVI_FIRMWARE_DATA := \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_sos.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_sos.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_asd.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_asd.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_smc.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_smc.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_pfp.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_pfp.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_me.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_me.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_ce.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_ce.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_mec.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_mec.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_mec2.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_mec2.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_rlc.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_rlc.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_sdma.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_sdma.bin \
	$(AMD_NAVI_FIRMWARE_DIR)/navi10_sdma1.bin=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/navi10_sdma1.bin \
	$(AMD_NAVI_FIRMWARE_LICENSE_DIR)/LICENSE.amdgpu=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/LICENSE.amdgpu \
	$(AMD_NAVI_FIRMWARE_LICENSE_DIR)/WHENCE=$(AMD_NAVI_FIRMWARE_CACHE_DIR)/WHENCE \
	/usr/share/zedbsd/packages/amd-navi-firmware.manifest=$(AMD_NAVI_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override AMD_NAVI_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,amd-navi-firmware,$(AMD_NAVI_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/amd-navi/amd-navi-firmware.notice,LICENSE.amdgpu,$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.amdgpu,3,$(AMD_NAVI_FIRMWARE_FILES)),$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.amdgpu,4,$(AMD_NAVI_FIRMWARE_FILES)),$(AMD_NAVI_FIRMWARE_ZEDBSD_URL),$(AMD_NAVI_FIRMWARE_BASE_URL)/$(call ZEDBSD_FIRMWARE_FIELD,LICENSE.amdgpu,2,$(AMD_NAVI_FIRMWARE_FILES))$(AMD_NAVI_FIRMWARE_URL_SUFFIX),$(AMD_NAVI_FIRMWARE_FETCH))
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,amd-navi-firmware,$(filter amd-navi-firmware,$(ZEDBSD_USER_PROGRAMS)),$(AMD_NAVI_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(AMD_NAVI_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error amd-navi-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter amd-navi-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: amd-navi-firmware-license
firmware-licenses-pending: amd-navi-firmware-license-pending
endif
.PHONY: amd-navi-firmware-license amd-navi-firmware-license-pending
amd-navi-firmware-license:
	@if test ! -d '$(AMD_NAVI_FIRMWARE_CACHE_DIR)'; then $(AMD_NAVI_FIRMWARE_LICENSE_COMMAND); fi
amd-navi-firmware-license-pending:
	@if test ! -d '$(AMD_NAVI_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(AMD_NAVI_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,amd-navi-firmware,AMD Navi10 GPU firmware for RX 5600 and RX 5700,amd64,n,package,,y,firmware,firmware/amd-navi,,,data,$(AMD_NAVI_FIRMWARE_DATA),,))
override USERLAND_amd-navi-firmware_DATA := $(AMD_NAVI_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += amd-navi-firmware

.PHONY: download amd-navi-firmware amd-navi-firmware-cache \
	amd-navi-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: amd-navi-firmware
endif

amd-navi-firmware: $(AMD_NAVI_FIRMWARE_CACHE_FILES)

amd-navi-firmware-fixture-cache: amd-navi-firmware-cache

# A selected build revalidates the complete cache.  Existing corruption is an
# error rather than an implicit update.  A missing cache is acquired in a
# sibling directory and published with one rename only after every file
# passes exact type, size, and SHA-256 checks.
amd-navi-firmware-cache:
	@set -eu; \
	cache='$(AMD_NAVI_FIRMWARE_CACHE_DIR)'; \
	files='$(strip $(AMD_NAVI_FIRMWARE_FILES))'; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != '$(AMD_NAVI_FIRMWARE_FILE_COUNT)'; then \
			echo "amd-navi-firmware: cache must contain exactly $(AMD_NAVI_FIRMWARE_FILE_COUNT) files: $$directory" >&2; \
			return 1; \
		fi; \
		for entry in $$files; do \
			old_ifs=$$IFS; IFS='|'; set -- $$entry; IFS=$$old_ifs; \
			file="$$directory/$$1"; expected_size="$$3"; expected_hash="$$4"; \
			if test ! -f "$$file" || test -L "$$file"; then \
				echo "amd-navi-firmware: missing or unsafe cache file: $$file" >&2; \
				return 1; \
			fi; \
			actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
			if test "$$actual_size" != "$$expected_size"; then \
				echo "amd-navi-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
				return 1; \
			fi; \
			actual_hash=$$('$(AMD_NAVI_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
			if test "$$actual_hash" != "$$expected_hash"; then \
				echo "amd-navi-firmware: SHA-256 mismatch for $$file" >&2; \
				return 1; \
			fi; \
		done; \
	}; \
	if test -e "$$cache" || test -L "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "amd-navi-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(AMD_NAVI_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(AMD_NAVI_FIRMWARE_LICENSE_COMMAND); \
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
		echo "amd-navi-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.amd-navi.$(AMD_NAVI_FIRMWARE_REVISION).XXXXXX"); \
	repository='$(AMD_NAVI_FIRMWARE_ZEDBSD_URL)'; \
	fetch() { \
		if test -n "$$repository" && \
			$(AMD_NAVI_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$repository/$$1"; then \
			return 0; \
		fi; \
		if test -n "$$repository"; then \
			echo "amd-navi-firmware: could not fetch $$1 from the zedBSD firmware repository; fetching it from upstream" >&2; \
		fi; \
		$(AMD_NAVI_FIRMWARE_FETCH) --output "$$temporary/$$1" "$$2"; \
	}; \
	base='$(AMD_NAVI_FIRMWARE_BASE_URL)'; \
	suffix='$(AMD_NAVI_FIRMWARE_URL_SUFFIX)'; \
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

$(AMD_NAVI_FIRMWARE_CACHE_FILES): amd-navi-firmware-cache
	@test -f "$@"
