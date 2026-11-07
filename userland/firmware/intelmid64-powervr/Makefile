# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The PowerVR Rogue G6430 GPU of Intel Moorefield (Intel Atom Z3560 / Z3580)
# boots this signed firmware, /lib/firmware/powervr/rgx.fw.signed.1.72.4.12.
# It is distributed by Google in the official AOSP device binary driver package
# for Nexus Player ("fugu"): intel-fugu-opr2.170623.027-82108990.tgz.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk
include $(if $(TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)/zedbsd-firmware.mk

override INTELMID64_POWERVR_FIRMWARE_FIXTURE_GOAL := intelmid64-powervr-firmware-fixture-cache
ifneq ($(filter $(INTELMID64_POWERVR_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(INTELMID64_POWERVR_FIRMWARE_FIXTURE_GOAL))
$(error $(INTELMID64_POWERVR_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
INTELMID64_POWERVR_FIRMWARE_ARCHIVE_URL ?= https://dl.google.com/dl/android/aosp/intel-fugu-opr2.170623.027-82108990.tgz
INTELMID64_POWERVR_FIRMWARE_ARCHIVE_FILE ?= intel-fugu-opr2.170623.027-82108990.tgz
INTELMID64_POWERVR_FIRMWARE_ARCHIVE_SIZE ?= 60116819
INTELMID64_POWERVR_FIRMWARE_ARCHIVE_SHA256 ?= 114c20b8335f3c166c76d590c6d238153c463c2fa2dea3da56d1ce3ffd1ec6ed
INTELMID64_POWERVR_FIRMWARE_BLOB_SIZE ?= 147456
INTELMID64_POWERVR_FIRMWARE_BLOB_SHA256 ?= a781d3ae3c03ac2001469acf5364b4a9d5e4afc73767151c52a6eb900233225f
INTELMID64_POWERVR_FIRMWARE_LICENSE_SIZE ?= 14770
INTELMID64_POWERVR_FIRMWARE_LICENSE_SHA256 ?= 6966993d5b46d5b720b7765a7a3138c40c9bd3e4325dc78ce8802e0def2a2911
INTELMID64_POWERVR_FIRMWARE_LICENSE_UPSTREAM_URL ?= https://android.googlesource.com/device/asus/fugu/+/refs/heads/master/self-extractors/intel/LICENSE?format=TEXT
INTELMID64_POWERVR_FIRMWARE_ZEDBSD_URL ?=
INTELMID64_POWERVR_FIRMWARE_LICENSE_PROMPT ?= n
else
override INTELMID64_POWERVR_FIRMWARE_ARCHIVE_URL := https://dl.google.com/dl/android/aosp/intel-fugu-opr2.170623.027-82108990.tgz
override INTELMID64_POWERVR_FIRMWARE_ARCHIVE_FILE := intel-fugu-opr2.170623.027-82108990.tgz
override INTELMID64_POWERVR_FIRMWARE_ARCHIVE_SIZE := 60116819
override INTELMID64_POWERVR_FIRMWARE_ARCHIVE_SHA256 := 114c20b8335f3c166c76d590c6d238153c463c2fa2dea3da56d1ce3ffd1ec6ed
override INTELMID64_POWERVR_FIRMWARE_BLOB_SIZE := 147456
override INTELMID64_POWERVR_FIRMWARE_BLOB_SHA256 := a781d3ae3c03ac2001469acf5364b4a9d5e4afc73767151c52a6eb900233225f
override INTELMID64_POWERVR_FIRMWARE_LICENSE_SIZE := 14770
override INTELMID64_POWERVR_FIRMWARE_LICENSE_SHA256 := 6966993d5b46d5b720b7765a7a3138c40c9bd3e4325dc78ce8802e0def2a2911
override INTELMID64_POWERVR_FIRMWARE_LICENSE_UPSTREAM_URL := https://android.googlesource.com/device/asus/fugu/+/refs/heads/master/self-extractors/intel/LICENSE?format=TEXT
override INTELMID64_POWERVR_FIRMWARE_ZEDBSD_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_URL),$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/intelmid64-powervr)
override INTELMID64_POWERVR_FIRMWARE_LICENSE_PROMPT := y
endif

override INTELMID64_POWERVR_FIRMWARE_REVISION := 1.72.4.12
override INTELMID64_POWERVR_FIRMWARE_VERSION := 1.72.4.12

INTELMID64_POWERVR_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5
override INTELMID64_POWERVR_FIRMWARE_SHA256_COMMAND := sha256sum
INTELMID64_POWERVR_FIRMWARE_CACHE_ROOT ?= $(if $(TOPLEVEL_BUILD),build,$(ZEDBSD_REPO_ROOT)/build)/sources/firmware/intelmid64-powervr
override INTELMID64_POWERVR_FIRMWARE_CACHE_DIR := $(INTELMID64_POWERVR_FIRMWARE_CACHE_ROOT)/$(INTELMID64_POWERVR_FIRMWARE_REVISION)
override INTELMID64_POWERVR_FIRMWARE_CACHE_BLOB := $(INTELMID64_POWERVR_FIRMWARE_CACHE_DIR)/rgx.fw.signed.1.72.4.12
override INTELMID64_POWERVR_FIRMWARE_CACHE_LICENSE := $(INTELMID64_POWERVR_FIRMWARE_CACHE_DIR)/LICENSE.intel
override INTELMID64_POWERVR_FIRMWARE_MANIFEST := userland/firmware/intelmid64-powervr/intelmid64-powervr-firmware.manifest

override INTELMID64_POWERVR_FIRMWARE_DATA := \
	/lib/firmware/powervr/rgx.fw.signed.1.72.4.12=$(INTELMID64_POWERVR_FIRMWARE_CACHE_BLOB) \
	/lib/firmware/powervr/rgx.fw.signed=$(INTELMID64_POWERVR_FIRMWARE_CACHE_BLOB) \
	/lib/firmware/powervr/rgx.fw=$(INTELMID64_POWERVR_FIRMWARE_CACHE_BLOB) \
	/usr/share/licenses/intelmid64-powervr-firmware/LICENSE.intel=$(INTELMID64_POWERVR_FIRMWARE_CACHE_LICENSE) \
	/usr/share/licenses/intelmid64-powervr-firmware/intelmid64-powervr-firmware.notice=$(ZEDBSD_FIRMWARE_TREE)/intelmid64-powervr/intelmid64-powervr-firmware.notice \
	/usr/share/zedbsd/packages/intelmid64-powervr-firmware.manifest=$(INTELMID64_POWERVR_FIRMWARE_MANIFEST)

# The license the first fetch asks about (userland/firmware/zedbsd-firmware.mk).
override INTELMID64_POWERVR_FIRMWARE_LICENSE_COMMAND = $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,intelmid64-powervr-firmware,$(INTELMID64_POWERVR_FIRMWARE_CACHE_ROOT),$(ZEDBSD_FIRMWARE_TREE)/intelmid64-powervr/intelmid64-powervr-firmware.notice,LICENSE.intel,$(INTELMID64_POWERVR_FIRMWARE_LICENSE_SIZE),$(INTELMID64_POWERVR_FIRMWARE_LICENSE_SHA256),$(INTELMID64_POWERVR_FIRMWARE_ZEDBSD_URL),$(INTELMID64_POWERVR_FIRMWARE_LICENSE_UPSTREAM_URL),sh '$(ZEDBSD_FIRMWARE_TREE)/intelmid64-powervr/fetch-license.sh')
ifneq ($(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,intelmid64-powervr-firmware,$(filter intelmid64-powervr-firmware,$(ZEDBSD_USER_PROGRAMS)),$(INTELMID64_POWERVR_FIRMWARE_CACHE_DIR)),)
ifneq ($(shell $(INTELMID64_POWERVR_FIRMWARE_LICENSE_COMMAND) && echo accepted),accepted)
$(error intelmid64-powervr-firmware: the firmware license was not accepted; see above)
endif
endif
ifneq ($(filter intelmid64-powervr-firmware,$(ZEDBSD_USER_PROGRAMS)),)
firmware-licenses: intelmid64-powervr-firmware-license
firmware-licenses-pending: intelmid64-powervr-firmware-license-pending
endif
.PHONY: intelmid64-powervr-firmware-license intelmid64-powervr-firmware-license-pending
intelmid64-powervr-firmware-license:
	@if test ! -d '$(INTELMID64_POWERVR_FIRMWARE_CACHE_DIR)'; then $(INTELMID64_POWERVR_FIRMWARE_LICENSE_COMMAND); fi
intelmid64-powervr-firmware-license-pending:
	@if test ! -d '$(INTELMID64_POWERVR_FIRMWARE_CACHE_DIR)'; then ZEDBSD_FIRMWARE_LICENSE_MODE=pending $(INTELMID64_POWERVR_FIRMWARE_LICENSE_COMMAND); fi

$(eval $(call USERLAND_PACKAGE,intelmid64-powervr-firmware,Intel Moorefield PowerVR Rogue G6430 GPU firmware,*,n,package,,y,firmware,firmware/intelmid64-powervr,,,data,$(INTELMID64_POWERVR_FIRMWARE_DATA),,))
override USERLAND_intelmid64-powervr-firmware_DATA := $(INTELMID64_POWERVR_FIRMWARE_DATA)
ZEDBSD_USERLAND_DOWNLOAD_TARGETS += intelmid64-powervr-firmware

.PHONY: download intelmid64-powervr-firmware intelmid64-powervr-cache \
	intelmid64-powervr-firmware-fixture-cache

ifndef TOPLEVEL_BUILD
download: intelmid64-powervr-firmware
endif

intelmid64-powervr-firmware: $(INTELMID64_POWERVR_FIRMWARE_CACHE_BLOB) \
	$(INTELMID64_POWERVR_FIRMWARE_CACHE_LICENSE)

intelmid64-powervr-firmware-fixture-cache: intelmid64-powervr-cache

# Every selected build revalidates the cache.
intelmid64-powervr-cache:
	@set -eu; \
	cache='$(INTELMID64_POWERVR_FIRMWARE_CACHE_DIR)'; \
	verify_file() { \
		file="$$1"; expected_size="$$2"; expected_hash="$$3"; \
		if test ! -f "$$file" || test -L "$$file"; then \
			echo "intelmid64-powervr-firmware: missing or unsafe cache file: $$file" >&2; \
			return 1; \
		fi; \
		actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
		if test "$$actual_size" != "$$expected_size"; then \
			echo "intelmid64-powervr-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
			return 1; \
		fi; \
		actual_hash=$$('$(INTELMID64_POWERVR_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
		if test "$$actual_hash" != "$$expected_hash"; then \
			echo "intelmid64-powervr-firmware: SHA-256 mismatch for $$file" >&2; \
			return 1; \
		fi; \
	}; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != 2; then \
			echo "intelmid64-powervr-firmware: cache must contain exactly two files: $$directory" >&2; \
			return 1; \
		fi; \
		verify_file "$$directory/rgx.fw.signed.1.72.4.12" '$(INTELMID64_POWERVR_FIRMWARE_BLOB_SIZE)' '$(INTELMID64_POWERVR_FIRMWARE_BLOB_SHA256)'; \
		verify_file "$$directory/LICENSE.intel" '$(INTELMID64_POWERVR_FIRMWARE_LICENSE_SIZE)' '$(INTELMID64_POWERVR_FIRMWARE_LICENSE_SHA256)'; \
	}; \
	if test -e "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "intelmid64-powervr-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
	fi; \
	if test '$(INTELMID64_POWERVR_FIRMWARE_LICENSE_PROMPT)' = y; then \
		$(INTELMID64_POWERVR_FIRMWARE_LICENSE_COMMAND); \
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
		echo "intelmid64-powervr-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.intelmid64-powervr.$(INTELMID64_POWERVR_FIRMWARE_REVISION).XXXXXX"); \
	repository='$(INTELMID64_POWERVR_FIRMWARE_ZEDBSD_URL)'; \
	fetched_mirror=0; \
	if test -n "$$repository"; then \
		if $(INTELMID64_POWERVR_FIRMWARE_FETCH) --output "$$temporary/rgx.fw.signed.1.72.4.12" "$$repository/rgx.fw.signed.1.72.4.12" && \
		   $(INTELMID64_POWERVR_FIRMWARE_FETCH) --output "$$temporary/LICENSE.intel" "$$repository/LICENSE.intel"; then \
			fetched_mirror=1; \
		else \
			echo "intelmid64-powervr-firmware: could not fetch files from the zedBSD firmware repository; fetching from Google AOSP" >&2; \
			rm -f -- "$$temporary/rgx.fw.signed.1.72.4.12" "$$temporary/LICENSE.intel"; \
		fi; \
	fi; \
	if test "$$fetched_mirror" -eq 0; then \
		echo "intelmid64-powervr-firmware: downloading driver package from Google AOSP..." >&2; \
		archive_url='$(INTELMID64_POWERVR_FIRMWARE_ARCHIVE_URL)'; \
		archive_tmp="$$temporary/$(INTELMID64_POWERVR_FIRMWARE_ARCHIVE_FILE)"; \
		$(INTELMID64_POWERVR_FIRMWARE_FETCH) --output "$$archive_tmp" "$$archive_url"; \
		verify_file "$$archive_tmp" '$(INTELMID64_POWERVR_FIRMWARE_ARCHIVE_SIZE)' '$(INTELMID64_POWERVR_FIRMWARE_ARCHIVE_SHA256)'; \
		tar -zxvf "$$archive_tmp" -C "$$temporary" extract-intel-fugu.sh >/dev/null; \
		sed -n '16,255p' "$$temporary/extract-intel-fugu.sh" > "$$temporary/LICENSE.intel"; \
		tail -n +291 "$$temporary/extract-intel-fugu.sh" | tar -zx -C "$$temporary" vendor/intel/fugu/proprietary/rgx.fw.signed.1.72.4.12; \
		mv -- "$$temporary/vendor/intel/fugu/proprietary/rgx.fw.signed.1.72.4.12" "$$temporary/rgx.fw.signed.1.72.4.12"; \
		rm -rf -- "$$temporary/extract-intel-fugu.sh" "$$temporary/vendor" "$$archive_tmp"; \
	fi; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(INTELMID64_POWERVR_FIRMWARE_CACHE_BLOB) \
$(INTELMID64_POWERVR_FIRMWARE_CACHE_LICENSE): intelmid64-powervr-cache
	@test -f "$@"
