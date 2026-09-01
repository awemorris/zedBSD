# zedBSD optional RTL8822B firmware package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

override RTL8822B_FIRMWARE_FIXTURE_GOAL := rtl8822b-firmware-fixture-cache
ifneq ($(filter $(RTL8822B_FIRMWARE_FIXTURE_GOAL),$(MAKECMDGOALS)),)
ifneq ($(strip $(MAKECMDGOALS)),$(RTL8822B_FIRMWARE_FIXTURE_GOAL))
$(error $(RTL8822B_FIRMWARE_FIXTURE_GOAL) must be the only requested goal)
endif
# Only the hermetic package fixture may replace acquisition metadata.  This
# target never assembles a root filesystem, and a later production target
# revalidates its own immutable cache identity.
RTL8822B_FIRMWARE_MIRROR_BASE_URL ?= https://raw.githubusercontent.com/endlessm/linux-firmware
RTL8822B_FIRMWARE_MIRROR_REVISION ?= 2f56219d20e4becccd718963fc3bcc671c543ce5
RTL8822B_FIRMWARE_SIZE ?= 161240
RTL8822B_FIRMWARE_SHA256 ?= a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418
RTL8822B_FIRMWARE_LICENSE_SIZE ?= 2115
RTL8822B_FIRMWARE_LICENSE_SHA256 ?= a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e
RTL8822B_FIRMWARE_MIRROR_WHENCE_SIZE ?= 364259
RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256 ?= 54474263d8418a29e7b11485e79936d15a81dd75381308b29cadf2d53b78aa51
else
# These values define the selected production package.  `override` is
# intentional: accepting command-line substitutions would allow different
# bytes to be installed while the committed manifest still named this pinned
# revision and digest.
override RTL8822B_FIRMWARE_MIRROR_BASE_URL := https://raw.githubusercontent.com/endlessm/linux-firmware
override RTL8822B_FIRMWARE_MIRROR_REVISION := 2f56219d20e4becccd718963fc3bcc671c543ce5
override RTL8822B_FIRMWARE_SIZE := 161240
override RTL8822B_FIRMWARE_SHA256 := a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418
override RTL8822B_FIRMWARE_LICENSE_SIZE := 2115
override RTL8822B_FIRMWARE_LICENSE_SHA256 := a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e
override RTL8822B_FIRMWARE_MIRROR_WHENCE_SIZE := 364259
override RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256 := 54474263d8418a29e7b11485e79936d15a81dd75381308b29cadf2d53b78aa51
endif
override RTL8822B_FIRMWARE_OFFICIAL_REVISION := 458e40fdbb4dad5134ec230a42df21aea1b5baf8

override RTL8822B_FIRMWARE_UPSTREAM_PATH := rtw88/rtw8822b_fw.bin
override RTL8822B_FIRMWARE_LICENSE_UPSTREAM_PATH := LICENCE.rtlwifi_firmware.txt
override RTL8822B_FIRMWARE_WHENCE_UPSTREAM_PATH := WHENCE
override RTL8822B_FIRMWARE_VERSION := 30.20.0

# The acquisition mirror and official provenance commits have different
# WHENCE files.  Keep both identities explicit instead of treating the mirror
# as byte-identical metadata.
override RTL8822B_FIRMWARE_OFFICIAL_WHENCE_SHA256 := 34f954c7d068ec4fd5fcc216471912dd3cf40ff60a7ffa8d06ff6f9b5999551f

RTL8822B_FIRMWARE_FETCH ?= curl --fail --location --silent --show-error
override RTL8822B_FIRMWARE_SHA256_COMMAND := sha256sum
RTL8822B_FIRMWARE_CACHE_ROOT ?= build/sources/firmware/rtl8822b
override RTL8822B_FIRMWARE_CACHE_DIR := $(RTL8822B_FIRMWARE_CACHE_ROOT)/$(RTL8822B_FIRMWARE_MIRROR_REVISION)
override RTL8822B_FIRMWARE_CACHE_BLOB := $(RTL8822B_FIRMWARE_CACHE_DIR)/rtw8822b_fw.bin
override RTL8822B_FIRMWARE_CACHE_LICENSE := $(RTL8822B_FIRMWARE_CACHE_DIR)/LICENCE.rtlwifi_firmware.txt
override RTL8822B_FIRMWARE_CACHE_WHENCE := $(RTL8822B_FIRMWARE_CACHE_DIR)/WHENCE
override RTL8822B_FIRMWARE_MANIFEST := userland/firmware/rtl8822b/rtl8822b-firmware.manifest

override RTL8822B_FIRMWARE_DATA := \
	/lib/firmware/rtw88/rtw8822b_fw.bin=$(RTL8822B_FIRMWARE_CACHE_BLOB) \
	/usr/share/licenses/rtl8822b-firmware/LICENCE.rtlwifi_firmware.txt=$(RTL8822B_FIRMWARE_CACHE_LICENSE) \
	/usr/share/zedbsd/packages/rtl8822b-firmware.manifest=$(RTL8822B_FIRMWARE_MANIFEST)

$(eval $(call ZEDBSD_USERLAND_PACKAGE,rtl8822b-firmware,Realtek RTL8822B firmware,i386 pc98 amd64,n,package,,y,firmware,firmware/rtl8822b,,,data,$(RTL8822B_FIRMWARE_DATA),,))
override USERLAND_rtl8822b-firmware_DATA := $(RTL8822B_FIRMWARE_DATA)

.PHONY: rtl8822b-firmware rtl8822b-firmware-cache \
	rtl8822b-firmware-fixture-cache

rtl8822b-firmware: $(RTL8822B_FIRMWARE_CACHE_BLOB) \
	$(RTL8822B_FIRMWARE_CACHE_LICENSE) $(RTL8822B_FIRMWARE_CACHE_WHENCE)

rtl8822b-firmware-fixture-cache: rtl8822b-firmware-cache

# Every selected build revalidates the cache.  A corrupt existing cache is an
# error: silently replacing it would conceal local damage or an upstream
# substitution.  A missing cache is acquired into a sibling temporary
# directory and becomes visible only after all three files pass validation.
rtl8822b-firmware-cache:
	@set -eu; \
	cache='$(RTL8822B_FIRMWARE_CACHE_DIR)'; \
	verify_file() { \
		file="$$1"; expected_size="$$2"; expected_hash="$$3"; \
		if test ! -f "$$file" || test -L "$$file"; then \
			echo "rtl8822b-firmware: missing or unsafe cache file: $$file" >&2; \
			return 1; \
		fi; \
		actual_size=$$(wc -c < "$$file" | tr -d '[:space:]'); \
		if test "$$actual_size" != "$$expected_size"; then \
			echo "rtl8822b-firmware: size mismatch for $$file: expected $$expected_size, got $$actual_size" >&2; \
			return 1; \
		fi; \
		actual_hash=$$('$(RTL8822B_FIRMWARE_SHA256_COMMAND)' "$$file" | awk '{print $$1}'); \
		if test "$$actual_hash" != "$$expected_hash"; then \
			echo "rtl8822b-firmware: SHA-256 mismatch for $$file" >&2; \
			return 1; \
		fi; \
	}; \
	verify_cache() { \
		directory="$$1"; \
		entry_count=$$(find "$$directory" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]'); \
		if test "$$entry_count" != 3; then \
			echo "rtl8822b-firmware: cache must contain exactly three files: $$directory" >&2; \
			return 1; \
		fi; \
		verify_file "$$directory/rtw8822b_fw.bin" '$(RTL8822B_FIRMWARE_SIZE)' '$(RTL8822B_FIRMWARE_SHA256)'; \
		verify_file "$$directory/LICENCE.rtlwifi_firmware.txt" '$(RTL8822B_FIRMWARE_LICENSE_SIZE)' '$(RTL8822B_FIRMWARE_LICENSE_SHA256)'; \
		verify_file "$$directory/WHENCE" '$(RTL8822B_FIRMWARE_MIRROR_WHENCE_SIZE)' '$(RTL8822B_FIRMWARE_MIRROR_WHENCE_SHA256)'; \
	}; \
	if test -e "$$cache"; then \
		if test ! -d "$$cache" || test -L "$$cache"; then \
			echo "rtl8822b-firmware: unsafe cache path: $$cache" >&2; \
			exit 1; \
		fi; \
		verify_cache "$$cache"; \
		exit 0; \
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
		echo "rtl8822b-firmware: cache acquisition already in progress: $$cache" >&2; \
		exit 1; \
	fi; \
	locked=1; \
	trap cleanup EXIT HUP INT TERM; \
	temporary=$$(mktemp -d "$$parent/.rtl8822b.$(RTL8822B_FIRMWARE_MIRROR_REVISION).XXXXXX"); \
	base='$(RTL8822B_FIRMWARE_MIRROR_BASE_URL)/$(RTL8822B_FIRMWARE_MIRROR_REVISION)'; \
	$(RTL8822B_FIRMWARE_FETCH) --output "$$temporary/rtw8822b_fw.bin" "$$base/$(RTL8822B_FIRMWARE_UPSTREAM_PATH)"; \
	$(RTL8822B_FIRMWARE_FETCH) --output "$$temporary/LICENCE.rtlwifi_firmware.txt" "$$base/$(RTL8822B_FIRMWARE_LICENSE_UPSTREAM_PATH)"; \
	$(RTL8822B_FIRMWARE_FETCH) --output "$$temporary/WHENCE" "$$base/$(RTL8822B_FIRMWARE_WHENCE_UPSTREAM_PATH)"; \
	verify_cache "$$temporary"; \
	mv -- "$$temporary" "$$cache"; \
	temporary=; \
	rmdir -- "$$lock"; \
	locked=0; \
	trap - EXIT HUP INT TERM

$(RTL8822B_FIRMWARE_CACHE_BLOB) \
$(RTL8822B_FIRMWARE_CACHE_LICENSE) \
$(RTL8822B_FIRMWARE_CACHE_WHENCE): rtl8822b-firmware-cache
	@test -f "$@"
