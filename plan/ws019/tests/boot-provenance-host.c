/* Current loader path decoder and kernel retained provenance, no firmware. */
#include <kern/boot.h>
#include "bootloader/uefi/volume-discovery.h"
#include "src/hal/amd64/bsp-pcat/handoff-validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"provenance line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static unsigned checks;
int main(void)
{
	/* PCI node, GPT hard-drive node, end node. Signature offset is 4+24. */
	uint8_t path[50] = {1, 1, 4, 0, 4, 1, 42, 0};
	struct zbl_uefi_partition_path view;
	struct boot_provenance record, saved;
	struct zbl6_handoff_v7_uefi handoff;
	unsigned i;
	uint32_t flags = ZBL6_HANDOFF_FLAG_UEFI | ZBL6_HANDOFF_FLAG_MEMORY_MAP |
	    ZBL6_HANDOFF_FLAG_ACPI_RSDP | ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
	    ZBL6_HANDOFF_FLAG_BOOT_UUID | ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS |
	    ZBL6_HANDOFF_FLAG_BOOT_ALLOCATIONS;
	path[8] = 1; path[13] = 8; path[21] = 16; path[44] = 2; path[45] = 2;
	path[46] = 127; path[47] = 255; path[48] = 4;
	for (i = 0; i < 16; i++) path[28 + i] = (uint8_t)(i + 1);
	CHECK(zbl_uefi_partition_path_parse(path, sizeof(path), &view) == ZBL_UEFI_DEVICE_PATH_OK);
	memset(&record, 0, sizeof(record)); record.version = 1; record.config_matches = 2;
	CHECK(zbl_uefi_partition_identity_copy(&view, &record.firmware));
	record.configuration = record.firmware; record.configuration.signature[15] = 255;
	CHECK(kern_boot_provenance_set(&record) == 0);
	CHECK(!strcmp(kern_boot_source_selector(0), "PARTUUID=04030201-0605-0807-090a-0b0c0d0e0f10"));
	CHECK(!strcmp(kern_boot_source_selector(1), "PARTUUID=04030201-0605-0807-090a-0b0c0d0e0fff"));
	CHECK(kern_boot_config_matches() == 2);
	saved = record; memset(&record, 0, sizeof(record));
	CHECK(kern_boot_config_matches() == 2 && strstr(kern_boot_source_selector(1), "0fff") != NULL);
	for (i = 0; i < 7; i++) {
		record = saved;
		if (i == 0) record.version = 2;
		if (i == 1) record.config_matches = 0;
		if (i == 2) record.config_matches = 129;
		if (i == 3) record.firmware.index = 0;
		if (i == 4) record.configuration.block_count = 0;
		if (i == 5) record.configuration.first_lba = UINT64_MAX;
		if (i == 6) memset(record.configuration.signature, 0, 16);
		CHECK(kern_boot_provenance_set(&record) == EINVAL);
		CHECK(!strcmp(kern_boot_source_selector(0), "unavailable") && kern_boot_config_matches() == 0);
	}
	record = saved; record.firmware.scheme = record.configuration.scheme = ZEDBSD_PARTITION_SCHEME_MBR;
	CHECK(kern_boot_provenance_set(&record) == 0);
	CHECK(!strcmp(kern_boot_source_selector(0), "unavailable") && kern_boot_config_matches() == 2);
	CHECK(kern_boot_provenance_set(NULL) == 0 && kern_boot_config_matches() == 0);
	view.path_size = view.partition_offset + 41;
	CHECK(!zbl_uefi_partition_identity_copy(&view, &record.firmware));
	memset(&handoff, 0, sizeof(handoff));
	handoff.prefix.common.common.magic = ZBL6_HANDOFF_MAGIC;
	handoff.prefix.common.common.version = 7;
	handoff.prefix.common.common.size = sizeof(handoff);
	handoff.prefix.common.common.flags = flags;
	CHECK(zbl6_handoff_classify_raw(&handoff) == ZBL6_HANDOFF_FORM_V7_UEFI);
	CHECK(zbl6_handoff_classify(7, sizeof(handoff) - 1, flags) == ZBL6_HANDOFF_FORM_INVALID);
	CHECK(zbl6_handoff_classify(7, sizeof(handoff), flags & ~ZBL6_HANDOFF_FLAG_BOOT_ALLOCATIONS) == ZBL6_HANDOFF_FORM_INVALID);
	CHECK(zbl6_handoff_classify(6, ZBL6_HANDOFF_V6_UEFI_SIZE, flags) == ZBL6_HANDOFF_FORM_V6_UEFI);
	CHECK(zbl6_uefi_partition_handoff_valid(7, ZBL6_PARTITION_SCHEME_GPT, 0, 0, flags));
	printf("boot provenance: %u checks PASS\n", checks);
	return 0;
}
