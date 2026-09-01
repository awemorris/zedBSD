/*
 * zedBSD Intel AX211 private firmware-file contract
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_FIRMWARE_H
#define ZEDBSD_DRIVERS_INTEL_AX211_FIRMWARE_H

#include "intel-ax211-internal.h"

#include <stddef.h>
#include <stdint.h>

#define INTEL_AX211_FIRMWARE_VFS_PATH \
	"/lib/firmware/" INTEL_AX211_FIRMWARE_PATH
#define INTEL_AX211_PNVM_VFS_PATH \
	"/lib/firmware/" INTEL_AX211_PNVM_PATH

struct intel_ax211_firmware_files {
	uint8_t *ucode_bytes;
	size_t ucode_size;
	struct intel_ax211_firmware_manifest ucode_manifest;
	uint8_t *pnvm_bytes;
	size_t pnvm_size;
	struct intel_ax211_pnvm_inventory pnvm_inventory;
};

/* files must be zero initialized or own one previous successful load. */
int intel_ax211_firmware_files_load(
	struct intel_ax211_firmware_files *files);
void intel_ax211_firmware_files_release(
	struct intel_ax211_firmware_files *files);

#ifdef INTEL_AX211_FIRMWARE_LOADER_HOST_TEST
int intel_ax211_firmware_loader_host_parse(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_firmware_manifest *manifest);
int intel_ax211_firmware_loader_host_inspect_pnvm(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_pnvm_inventory *inventory);
int intel_ax211_firmware_loader_test_sha256(
	const void *data,
	size_t length,
	uint8_t digest[32]);
#endif

#endif
