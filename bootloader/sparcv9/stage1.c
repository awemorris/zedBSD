/*
 * OpenBoot first stage for zedBSD SPARC V9.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "ofw.h"
#include "stage2-format.h"

#define SECTOR_SIZE 512UL
#define STAGE2_CLAIM_SIZE (SPARCV9_STAGE2_LOAD_LIMIT - \
			   SPARCV9_STAGE2_LOAD_ADDRESS)

static unsigned int
get16(const unsigned char *bytes)
{
	return ((unsigned int)bytes[0] << 8) | bytes[1];
}

static unsigned int
get32(const unsigned char *bytes)
{
	return (get16(bytes) << 16) | get16(bytes + 2);
}

static unsigned long
get64(const unsigned char *bytes)
{
	return ((unsigned long)get32(bytes) << 32) | get32(bytes + 4);
}

static unsigned int
checksum(const unsigned char *bytes, unsigned long size)
{
	unsigned int value;

	value = 0;
	while (size-- != 0)
		value += *bytes++;
	return value;
}

static void
fail(const char *message)
{
	ofw_puts("SPARCV9 STAGE1 ERROR: ");
	ofw_puts(message);
	ofw_puts("\n");
	ofw_exit();
}

void
sparcv9_stage1_main(ofw_client_t client)
{
	unsigned char header[SECTOR_SIZE];
	char bootpath[256];
	ofw_scell_t disk;
	unsigned int payload_size;
	unsigned int payload_checksum;
	unsigned int entry_offset;
	unsigned long load_address;
	void (*entry)(ofw_client_t, ofw_cell_t);
	void *claimed;

	ofw_init(client);
	ofw_puts("SPARCV9 STAGE1\n");
	if (ofw_bootpath(bootpath, sizeof(bootpath)) < 0)
		fail("missing bootpath");
	disk = ofw_open(bootpath);
	if (disk <= 0)
		fail("cannot open boot disk");
	if (ofw_seek((ofw_cell_t)disk,
	    (unsigned long long)SPARCV9_STAGE2_HEADER_LBA * SECTOR_SIZE) != 0)
		fail("cannot seek to stage2 header");
	if (ofw_read((ofw_cell_t)disk, header, sizeof(header)) !=
	    (long)sizeof(header))
		fail("short stage2 header");
	if (get32(header) != SPARCV9_STAGE2_MAGIC ||
	    get16(header + 4) != SPARCV9_STAGE2_VERSION ||
	    get16(header + 6) != SPARCV9_STAGE2_HEADER_SIZE ||
	    get32(header + 8) != SPARCV9_STAGE2_PAYLOAD_LBA)
		fail("invalid stage2 header");
	payload_size = get32(header + 12);
	load_address = get64(header + 16);
	entry_offset = get32(header + 24);
	payload_checksum = get32(header + 28);
	if (load_address != SPARCV9_STAGE2_LOAD_ADDRESS ||
	    payload_size == 0 || payload_size > STAGE2_CLAIM_SIZE ||
	    entry_offset >= payload_size)
		fail("invalid stage2 range");
	claimed = ofw_claim_fixed((void *)load_address, load_address,
	    STAGE2_CLAIM_SIZE);
	if (claimed != (void *)load_address)
		fail("cannot claim stage2 memory");
	if (ofw_seek((ofw_cell_t)disk,
	    (unsigned long long)SPARCV9_STAGE2_PAYLOAD_LBA * SECTOR_SIZE) != 0)
		fail("cannot seek to stage2 payload");
	if (ofw_read((ofw_cell_t)disk, claimed, payload_size) !=
	    (long)payload_size)
		fail("short stage2 payload");
	if (checksum((const unsigned char *)claimed, payload_size) !=
	    payload_checksum)
		fail("stage2 checksum mismatch");
	entry = (void (*)(ofw_client_t, ofw_cell_t))
	    (load_address + entry_offset);
	entry(client, (ofw_cell_t)disk);
	fail("stage2 returned");
}
