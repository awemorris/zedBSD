/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's Intel firmware load without system calls
 * (ws143-p003, plan/ws143/design.md section 3): Intel Read Version's TLV
 * answer taken apart, the firmware file's name, whether the controller can
 * be loaded, the Secure Send fragments a .sfi file is sent in, and the
 * records of a .ddc file.  The host tests build them.
 *
 * The constants were checked against FreeBSD's iwmbtfw (BSD-2-Clause, the
 * design's reference); no code is taken from it.
 */

#ifndef BLUETOOTHD_INTEL_H
#define BLUETOOTHD_INTEL_H

#include <stddef.h>
#include <stdint.h>

/* Intel's vendor ID on USB. */
#define BTD_INTEL_VENDOR		0x8087U

/* The vendor commands of the load. */
#define BTD_INTEL_RESET			0xfc01U
#define BTD_INTEL_READ_VERSION		0xfc05U
#define BTD_INTEL_SECURE_SEND		0xfc09U
#define BTD_INTEL_BOOT_PARAMETER	0xfc0eU
#define BTD_INTEL_SET_EVENT_MASK	0xfc52U
#define BTD_INTEL_WRITE_DDC		0xfc8bU

/* The vendor events' first byte: the download is complete, the controller booted. */
#define BTD_INTEL_EVENT_DOWNLOADED	0x06U
#define BTD_INTEL_EVENT_BOOTED		0x02U

/* The image types Read Version reports. */
#define BTD_INTEL_IMAGE_BOOTLOADER	0x01U
#define BTD_INTEL_IMAGE_OPERATIONAL	0x03U

/* The Secure Send fragments' types. */
#define BTD_INTEL_FRAGMENT_CSS		0x00U
#define BTD_INTEL_FRAGMENT_COMMANDS	0x01U
#define BTD_INTEL_FRAGMENT_SIGNATURE	0x02U
#define BTD_INTEL_FRAGMENT_KEY		0x03U

/* The longest fragment of the command buffer. */
#define BTD_INTEL_FRAGMENT_MAX		252U

/*
 * What Intel Read Version told (the TLV form): the CNVi and CNVR ids the
 * firmware file is named by, the hardware variant (bits 16 to 21 of the
 * CNVi BT id), the image type, the build, the two fields that decide
 * whether this driver can load it, and the address burnt in.  have_ says
 * which fields were there.
 */
struct btd_intel_version {
	int have_cnvi_top;
	int have_cnvr_top;
	int have_cnvi_bt;
	int have_image_type;
	int have_limited_cce;
	int have_sbe_type;
	int have_address;
	uint32_t cnvi_top;
	uint32_t cnvr_top;
	uint32_t cnvi_bt;
	uint8_t image_type;
	uint16_t timestamp;
	uint8_t build_type;
	uint32_t build_number;
	uint8_t limited_cce;
	uint8_t sbe_type;
	uint8_t address[6];
};

/* One Secure Send fragment: its type, and where in the file its data is. */
struct btd_intel_fragment {
	uint8_t type;
	uint32_t offset;
	uint32_t length;
};

/*
 * A firmware file's plan: its fragments in order (the caller's array) and
 * the boot parameter Intel Reset takes, found in the file's 0xFC0E
 * command.
 */
struct btd_intel_plan {
	struct btd_intel_fragment *fragments;
	size_t capacity;
	size_t count;
	int have_boot_parameter;
	uint32_t boot_parameter;
};

int btd_intel_version_parse(const uint8_t *returned, size_t length, struct btd_intel_version *version);
uint8_t btd_intel_variant(const struct btd_intel_version *version);
uint16_t btd_intel_pack_id(uint32_t top);
int btd_intel_file_name(const struct btd_intel_version *version, const char *folder, const char *suffix, char *path, size_t size);
int btd_intel_loadable(const struct btd_intel_version *version, const char **reason);
size_t btd_intel_plan_capacity(size_t file_length);
int btd_intel_plan_make(const uint8_t *file, size_t length, const struct btd_intel_version *version, struct btd_intel_plan *plan, const char **reason);
int btd_intel_ddc_next(const uint8_t *file, size_t length, size_t *offset, const uint8_t **record, size_t *record_length);

#endif
