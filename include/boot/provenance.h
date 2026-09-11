/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_BOOT_PROVENANCE_H
#define KERN_BOOT_PROVENANCE_H

#define KERN_BOOT_PROVENANCE_VERSION 1U
#define KERN_BOOT_PROVENANCE_SIZE 88U
#define KERN_BOOT_SOURCE_SELECTOR_SIZE 46U
#ifndef __ASSEMBLER__
#include <stdint.h>
struct boot_partition_identity {
	uint32_t scheme;
	uint32_t index;
	uint64_t first_lba;
	uint64_t block_count;
	uint8_t signature[16];
};
struct boot_provenance {
	uint32_t version;
	uint32_t config_matches;
	struct boot_partition_identity firmware;
	struct boot_partition_identity configuration;
};
typedef char boot_identity_size_check[sizeof(struct boot_partition_identity) == 40 ? 1 : -1];
typedef char boot_provenance_size_check[sizeof(struct boot_provenance) == KERN_BOOT_PROVENANCE_SIZE ? 1 : -1];
#endif
#endif
