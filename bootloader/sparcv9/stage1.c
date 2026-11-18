/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * OpenBoot first stage for zedBSD SPARC V9.
 *
 * It reads the second stage's header and payload from the boot disk into
 * memory claimed from the firmware, checks them, and enters the second
 * stage.
 */

#include "ofw.h"
#include "stage2-format.h"

/* The disk's sector size. */
#define SECTOR_SIZE 512UL

/* The memory claimed for the second stage: its whole load window. */
#define STAGE2_CLAIM_SIZE (SPARCV9_STAGE2_LOAD_LIMIT - \
			   SPARCV9_STAGE2_LOAD_ADDRESS)

static unsigned int get16(const unsigned char *bytes);
static unsigned int get32(const unsigned char *bytes);
static unsigned long get64(const unsigned char *bytes);
static unsigned int checksum(const unsigned char *bytes, unsigned long size);
static void fail(const char *message);

/* Loads the second stage from the boot disk and enters it. */
void
sparcv9_stage1_main(
	ofw_client_t client)
{
	unsigned char header[SECTOR_SIZE];
	char bootpath[256];
	ofw_scell_t disk;
	unsigned int payload_size;
	unsigned int payload_checksum;
	unsigned int entry_offset;
	unsigned int magic;
	unsigned int version;
	unsigned int header_size;
	unsigned int payload_lba;
	unsigned int sum;
	unsigned long load_address;
	void (*entry)(ofw_client_t, ofw_cell_t);
	void *claimed;
	long amount;
	int status;

	/* Starts the client interface and says the first stage runs. */
	ofw_init(client);
	ofw_puts("SPARCV9 STAGE1\n");

	/* Finds the disk the firmware booted from. */
	status = ofw_bootpath(bootpath, sizeof(bootpath));
	if (status < 0)
		fail("missing bootpath");

	/* Opens the boot disk. */
	disk = ofw_open(bootpath);
	if (disk <= 0)
		fail("cannot open boot disk");

	/* Moves to the second stage's header. */
	status = ofw_seek((ofw_cell_t)disk, (unsigned long long)SPARCV9_STAGE2_HEADER_LBA * SECTOR_SIZE);
	if (status != 0)
		fail("cannot seek to stage2 header");

	/* Reads the header's sector. */
	amount = ofw_read((ofw_cell_t)disk, header, sizeof(header));
	if (amount != (long)sizeof(header))
		fail("short stage2 header");

	/* Refuses a header of another identity. */
	magic = get32(header);
	if (magic != SPARCV9_STAGE2_MAGIC)
		fail("invalid stage2 header");

	/* Refuses a header of another version. */
	version = get16(header + 4);
	if (version != SPARCV9_STAGE2_VERSION)
		fail("invalid stage2 header");

	/* Refuses a header of another size. */
	header_size = get16(header + 6);
	if (header_size != SPARCV9_STAGE2_HEADER_SIZE)
		fail("invalid stage2 header");

	/* Refuses a payload that is not where the disk layout puts it. */
	payload_lba = get32(header + 8);
	if (payload_lba != SPARCV9_STAGE2_PAYLOAD_LBA)
		fail("invalid stage2 header");

	/* Reads the payload's size, load address, entry point and checksum. */
	payload_size = get32(header + 12);
	load_address = get64(header + 16);
	entry_offset = get32(header + 24);
	payload_checksum = get32(header + 28);

	/* Refuses a payload at another address, empty, larger than the window, or entered outside itself. */
	if (load_address != SPARCV9_STAGE2_LOAD_ADDRESS ||
	    payload_size == 0 ||
	    payload_size > STAGE2_CLAIM_SIZE ||
	    entry_offset >= payload_size)
		fail("invalid stage2 range");

	/* Claims the load window with the same physical addresses behind it. */
	claimed = ofw_claim_fixed((void *)load_address, load_address, STAGE2_CLAIM_SIZE);
	if (claimed != (void *)load_address)
		fail("cannot claim stage2 memory");

	/* Moves to the payload. */
	status = ofw_seek((ofw_cell_t)disk, (unsigned long long)SPARCV9_STAGE2_PAYLOAD_LBA * SECTOR_SIZE);
	if (status != 0)
		fail("cannot seek to stage2 payload");

	/* Reads the payload into the window. */
	amount = ofw_read((ofw_cell_t)disk, claimed, payload_size);
	if (amount != (long)payload_size)
		fail("short stage2 payload");

	/* Refuses a payload whose checksum is not the header's. */
	sum = checksum((const unsigned char *)claimed, payload_size);
	if (sum != payload_checksum)
		fail("stage2 checksum mismatch");

	/* Enters the second stage with the client interface and the open disk. */
	entry = (void (*)(ofw_client_t, ofw_cell_t))(load_address + entry_offset);
	entry(client, (ofw_cell_t)disk);

	/* The second stage does not come back. */
	fail("stage2 returned");
}

/* Reads a big-endian 16-bit field. */
static unsigned int
get16(
	const unsigned char *bytes)
{
	/* Combines the high byte and the low byte. */
	return ((unsigned int)bytes[0] << 8) | bytes[1];
}

/* Reads a big-endian 32-bit field. */
static unsigned int
get32(
	const unsigned char *bytes)
{
	unsigned int high;
	unsigned int low;

	/* Reads the two halves. */
	high = get16(bytes);
	low = get16(bytes + 2);

	/* Reports the combined value. */
	return (high << 16) | low;
}

/* Reads a big-endian 64-bit field. */
static unsigned long
get64(
	const unsigned char *bytes)
{
	unsigned long high;
	unsigned long low;

	/* Reads the two halves. */
	high = get32(bytes);
	low = get32(bytes + 4);

	/* Reports the combined value. */
	return (high << 32) | low;
}

/* Sums the bytes of the payload. */
static unsigned int
checksum(
	const unsigned char *bytes,
	unsigned long size)
{
	unsigned int value;

	/* Adds each byte. */
	value = 0;
	while (size != 0) {
		value += *bytes;
		bytes++;
		size--;
	}

	/* Reports the sum. */
	return value;
}

/* Reports why the first stage stopped and returns to the firmware. */
static void
fail(
	const char *message)
{
	/* Writes the message and exits to the firmware. */
	ofw_puts("SPARCV9 STAGE1 ERROR: ");
	ofw_puts(message);
	ofw_puts("\n");
	ofw_exit();
}
