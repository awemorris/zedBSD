/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the Amlogic SD/eMMC driver and its S905X5M glue on a fake
 * controller with a fake SD card behind it.
 *
 * The fake controller keeps its registers in memory and runs a command
 * when the driver writes the argument register; the fake card answers the
 * SD commands and moves blocks through the bounce buffer as the
 * controller's DMA would.  The checks look at the commands the driver
 * sent (index, argument, the command configuration's bits), the clock
 * divider and source, the bus width, the data that moved, and the
 * recovery from an injected CRC error.  The glue check runs the glue on
 * the ODROID-C5's device tree and looks at the pins, the gate, the clock
 * selector and the supplies it set.
 *
 *   meson-mmc-host-test driver
 *   meson-mmc-host-test version1
 *   meson-mmc-host-test glue odroid-c5.dtb
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/mmc/meson-mmc.h"
#include "drivers/platform/s905x5m/s905x5m-mmc.h"
#include "drivers/platform/s905x5m/s905x5m-soc.h"
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/disk.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The controller's registers the model gives meaning to. */
#define R_CLOCK		0x000U
#define R_START		0x040U
#define R_CFG		0x044U
#define R_STATUS	0x048U
#define R_IRQ_EN	0x04cU
#define R_CMD_CFG	0x050U
#define R_CMD_ARG	0x054U
#define R_CMD_DAT	0x058U
#define R_CMD_RSP	0x05cU
#define R_CMD_RSP1	0x060U
#define R_CMD_RSP2	0x064U
#define R_CMD_RSP3	0x068U

/* The command configuration's bits. */
#define C_LENGTH	0x000001ffU
#define C_BLOCK_MODE	0x00000200U
#define C_R1B		0x00000400U
#define C_END		0x00000800U
#define C_NO_RESP	0x00010000U
#define C_DATA		0x00040000U
#define C_WRITE		0x00080000U
#define C_NO_CRC	0x00100000U
#define C_LONG		0x00200000U
#define C_RESP		0x00400000U
#define C_OWNER		0x80000000U

/* The status bits. */
#define S_RX_CRC	0x00000001U
#define S_RESP_TIMEOUT	0x00000800U
#define S_END		0x00002000U
#define S_DAT0		0x00010000U

/* The fake card's capacity in blocks, and where the bounce buffer seems to be on the bus. */
#define CARD_BLOCKS	4096U
#define BOUNCE_BUS	0x10000000ULL
#define BOUNCE_BYTES	(64U * 1024U)

/* One command the driver sent. */
struct sent {
	uint32_t index;
	uint32_t argument;
	uint32_t value;
	int app;
};

/* The fake controller and the card in its slot. */
struct model {
	uint8_t registers[0x1000];

	/* The card. */
	int version2;
	int high_capacity;
	int app;
	int powered_polls;
	uint32_t rca;
	uint32_t cid[4];
	uint32_t csd[4];
	uint8_t scr[8];
	uint8_t memory[CARD_BLOCKS * 512U];

	/* Errors to inject: data CRC errors on the next multiple-block reads. */
	int crc_errors;

	/* What happened. */
	struct sent log[512];
	unsigned sent_count;
	uint32_t dividers[16];
	unsigned divider_count;
	int sources[16];
	unsigned source_count;
};

static struct model card;

/* The bounce buffer the controller's DMA reaches. */
static uint8_t bounce[BOUNCE_BYTES];

/* The fake register blocks of the SoC for the glue. */
static uint8_t clock_block[0x1000];
static uint8_t reset_block[0x1000];
static uint8_t pins_block[0x1000];

/* The device tree the glue reads, loaded from the file the test names. */
static uint8_t *tree;

/* The fake monotonic counter: 1 MHz, ten microseconds per read. */
static uint64_t fake_now;

/* The completed request's outcome. */
static int completed_error;
static size_t completed_bytes;

/* How many checks failed. */
static int failures;

/* Prints one check's outcome and counts a failure. */
static void
check(
	int condition,
	const char *what)
{
	printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		failures++;
}

/* Reads a 32-bit word of a register block. */
static uint32_t
get32(
	const uint8_t *memory,
	uint32_t offset)
{
	uint32_t value;

	memcpy(&value, memory + offset, 4);
	return value;
}

/* Writes a 32-bit word of a register block. */
static void
put32(
	uint8_t *memory,
	uint32_t offset,
	uint32_t value)
{
	memcpy(memory + offset, &value, 4);
}

/* Sets width bits of a 128-bit register at start. */
static void
set_bits(
	uint32_t *words,
	uint32_t start,
	uint32_t width,
	uint32_t value)
{
	uint32_t bit;
	uint32_t position;

	for (bit = 0; bit < width; bit++) {
		position = start + bit;
		if (((value >> bit) & 1U) != 0)
			words[position / 32U] |= 1U << (position % 32U);
		else
			words[position / 32U] &= ~(1U << (position % 32U));
	}
}

/* Ends the command with the status bits and a short response. */
static void
finish(
	uint32_t status,
	uint32_t response)
{
	put32(card.registers, R_STATUS, get32(card.registers, R_STATUS) | status);
	put32(card.registers, R_CMD_RSP, response);
}

/* The card status of a card in the transfer state, ready for data. */
static uint32_t
card_status(
	void)
{
	return (4U << 9) | 0x100U;
}

/* How many bytes the command's data phase moves. */
static uint32_t
data_bytes(
	uint32_t value)
{
	uint32_t length;
	uint32_t block_log2;

	length = value & C_LENGTH;
	if ((value & C_BLOCK_MODE) != 0) {
		block_log2 = (get32(card.registers, R_CFG) >> 4) & 0xfU;
		return length << block_log2;
	}
	if (length == 0)
		return 512U;
	return length;
}

/* Runs the command the configuration and the argument describe. */
static void
run_command(
	uint32_t argument)
{
	uint32_t value;
	uint32_t index;
	uint32_t bytes;
	uint32_t address;
	uint32_t block;
	uint8_t *dma;
	int app;

	value = get32(card.registers, R_CMD_CFG);
	index = (value >> 24) & 0x3fU;
	app = card.app;
	card.app = 0;
	if (card.sent_count < sizeof(card.log) / sizeof(card.log[0])) {
		card.log[card.sent_count].index = index;
		card.log[card.sent_count].argument = argument;
		card.log[card.sent_count].value = value;
		card.log[card.sent_count].app = app;
		card.sent_count++;
	}

	/* The DMA reaches the bounce buffer only. */
	address = get32(card.registers, R_CMD_DAT);
	dma = NULL;
	bytes = 0;
	if ((value & C_DATA) != 0) {
		bytes = data_bytes(value);
		if (address < BOUNCE_BUS || address + bytes > BOUNCE_BUS + BOUNCE_BYTES) {
			printf("DMA outside the bounce buffer: %x + %u\n", address, bytes);
			finish(S_RX_CRC, 0);
			return;
		}
		dma = bounce + (address - BOUNCE_BUS);
	}

	/* Application commands. */
	if (app) {
		switch (index) {
		case 41:
			if (card.powered_polls > 0) {
				card.powered_polls--;
				finish(S_END, 0x00ff8000U);
				return;
			}
			finish(S_END, 0x80ff8000U | (card.high_capacity ? 0x40000000U : 0));
			return;
		case 51:
			memcpy(dma, card.scr, 8);
			finish(S_END, card_status());
			return;
		case 6:
			finish(S_END, card_status());
			return;
		default:
			finish(S_RESP_TIMEOUT, 0);
			return;
		}
	}

	switch (index) {
	case 0:
		finish(S_END, 0);
		return;
	case 8:
		if (!card.version2) {
			finish(S_RESP_TIMEOUT, 0);
			return;
		}
		finish(S_END, argument & 0xfffU);
		return;
	case 55:
		card.app = 1;
		finish(S_END, card_status() | 0x20U);
		return;
	case 2:
		put32(card.registers, R_CMD_RSP1, card.cid[1]);
		put32(card.registers, R_CMD_RSP2, card.cid[2]);
		put32(card.registers, R_CMD_RSP3, card.cid[3]);
		finish(S_END, card.cid[0]);
		return;
	case 3:
		finish(S_END, card.rca << 16);
		return;
	case 9:
		put32(card.registers, R_CMD_RSP1, card.csd[1]);
		put32(card.registers, R_CMD_RSP2, card.csd[2]);
		put32(card.registers, R_CMD_RSP3, card.csd[3]);
		finish(S_END, card.csd[0]);
		return;
	case 7:
	case 16:
	case 12:
	case 13:
		finish(S_END, card_status());
		return;
	case 6:
		memset(dma, 0, 64);
		dma[12] = 0x80U;
		dma[13] = 0x03U;
		finish(S_END, card_status());
		return;
	case 17:
	case 18:
	case 24:
	case 25:
		block = argument;
		if (!card.high_capacity)
			block = argument / 512U;
		if (block + bytes / 512U > CARD_BLOCKS) {
			finish(S_RESP_TIMEOUT, 0);
			return;
		}
		if (index == 18 && card.crc_errors > 0) {
			card.crc_errors--;
			finish(S_RX_CRC, card_status());
			return;
		}
		if (index == 17 || index == 18)
			memcpy(dma, card.memory + (size_t)block * 512U, bytes);
		else
			memcpy(card.memory + (size_t)block * 512U, dma, bytes);
		finish(S_END, card_status());
		return;
	default:
		finish(S_RESP_TIMEOUT, 0);
		return;
	}
}

uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	const uint8_t *target;
	uint32_t value;

	target = (const uint8_t *)address;
	memcpy(&value, target, 4);

	/* The first data line of the fake card is always high: it is never busy. */
	if (target == card.registers + R_STATUS)
		value |= S_DAT0;
	return value;
}

void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uint8_t *target;
	uint32_t offset;
	uint32_t old;

	target = (uint8_t *)address;
	if (target < card.registers || target >= card.registers + sizeof(card.registers)) {
		memcpy(target, &value, 4);
		return;
	}

	offset = (uint32_t)(target - card.registers);
	if (offset == R_STATUS) {
		old = get32(card.registers, R_STATUS);
		put32(card.registers, R_STATUS, old & ~value);
		return;
	}
	if (offset == R_CLOCK && card.divider_count < 16U)
		card.dividers[card.divider_count++] = value & 0x3fU;
	put32(card.registers, offset, value);
	if (offset == R_CMD_ARG)
		run_command(value);
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	(void)attributes;
	if (size > 0x1000U)
		return ENOMEM;
	if (address == 0xfe000000ULL) {
		*mapped = clock_block;
		return 0;
	}
	if (address == 0xfe002000ULL) {
		*mapped = reset_block;
		return 0;
	}
	if (address == 0xfe004000ULL) {
		*mapped = pins_block;
		return 0;
	}
	if (address == 0xfe08a000ULL) {
		*mapped = card.registers;
		return 0;
	}
	printf("unexpected map of %llx\n", (unsigned long long)address);
	return ENOMEM;
}

void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	if (address == 0x1000ULL)
		return tree;
	if (address == BOUNCE_BUS)
		return bounce;
	return NULL;
}

int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	(void)alignment;
	(void)boundary;
	if (size > BOUNCE_BYTES || max_address < BOUNCE_BUS + size - 1U)
		return ENOMEM;
	run->paddr = BOUNCE_BUS;
	run->size = size;
	return 0;
}

bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	fake_now += 10U;
	*counter = fake_now;
	*frequency_hz = 1000000U;
	return true;
}

void kern_dcache_clean_range(const void *address, size_t size) { (void)address; (void)size; }
void kern_dcache_invalidate_range(const void *address, size_t size) { (void)address; (void)size; }
void kern_io_write_barrier(void) { }
void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name) { (void)lock; (void)rank; (void)name; }
unsigned long spin_lock_irqsave(struct spinlock *lock) { (void)lock; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled) { (void)lock; (void)enabled; }
int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name) { (void)mutex; (void)rank; (void)name; return 0; }
void mutex_lock(struct mutex *mutex) { (void)mutex; }
void mutex_unlock(struct mutex *mutex) { (void)mutex; }
struct disk *disk_alloc(void) { return calloc(1, sizeof(struct disk)); }
int disk_create(struct disk *disk) { (void)disk; return 0; }
void disk_set_stats_kind(struct disk *disk, uint32_t kind) { (void)disk; (void)kind; }
void bio_complete(struct bio *bio, int error, size_t transferred) { (void)bio; completed_error = error; completed_bytes = transferred; }
void kern_logf(const char *format, ...) { va_list ap; va_start(ap, format); printf("  log: "); vprintf(format, ap); va_end(ap); }

/* The driver test's clock source hook: records which source was asked for. */
static int
record_source(
	void *context,
	enum drv_meson_mmc_source source)
{
	(void)context;
	if (card.source_count < 16U)
		card.sources[card.source_count++] = (int)source;
	return 0;
}

/* Builds the card: version 2 or 1, its CID, CSD and SCR, and its contents. */
static void
reset_model(
	int version2)
{
	uint32_t index;

	memset(&card, 0, sizeof(card));
	memset(bounce, 0, sizeof(bounce));
	card.version2 = version2;
	card.high_capacity = version2;
	card.rca = 0xb368U;
	card.powered_polls = 2;

	/* The CID: manufacturer 03, OEM "SD", product "SU08G", revision 8.0, serial 12345678. */
	set_bits(card.cid, 120U, 8U, 0x03U);
	set_bits(card.cid, 104U, 16U, ('S' << 8) | 'D');
	set_bits(card.cid, 96U, 8U, 'S');
	set_bits(card.cid, 88U, 8U, 'U');
	set_bits(card.cid, 80U, 8U, '0');
	set_bits(card.cid, 72U, 8U, '8');
	set_bits(card.cid, 64U, 8U, 'G');
	set_bits(card.cid, 56U, 8U, 0x80U);
	set_bits(card.cid, 24U, 32U, 0x12345678U);

	/* The CSD: the switch class, 512-byte reads, and the capacity. */
	set_bits(card.csd, 84U, 12U, 0x5b5U);
	set_bits(card.csd, 80U, 4U, 9U);
	if (version2) {
		set_bits(card.csd, 126U, 2U, 1U);
		set_bits(card.csd, 48U, 22U, CARD_BLOCKS / 1024U - 1U);
	} else {
		/* (size + 1) << (multiplier + 2 + 9) bytes: 1024 << (0 + 2 + 9) = 2 MiB. */
		set_bits(card.csd, 126U, 2U, 0U);
		set_bits(card.csd, 62U, 12U, 1023U);
		set_bits(card.csd, 47U, 3U, 0U);
	}

	/* The SCR: specification 2.00, one and four data lines. */
	card.scr[0] = 0x02U;
	card.scr[1] = 0x05U;

	/* Fills the card with a pattern that names each block. */
	for (index = 0; index < CARD_BLOCKS * 512U; index++)
		card.memory[index] = (uint8_t)(index * 7U + index / 512U);
}

/* Finds the n-th command of an index in the log, or NULL. */
static const struct sent *
find_sent(
	uint32_t index,
	int app,
	unsigned nth)
{
	unsigned position;

	for (position = 0; position < card.sent_count; position++) {
		if (card.log[position].index != index || card.log[position].app != app)
			continue;
		if (nth == 0)
			return &card.log[position];
		nth--;
	}
	return NULL;
}

/* Reports whether the log starts with the commands, an app command shown as 100 + index. */
static int
sequence_is(
	const uint32_t *expected,
	unsigned count)
{
	unsigned position;
	uint32_t got;

	if (card.sent_count < count)
		return 0;
	for (position = 0; position < count; position++) {
		got = card.log[position].index;
		if (card.log[position].app)
			got += 100U;
		if (got != expected[position]) {
			printf("  command %u is %u, expected %u\n", position, got, expected[position]);
			return 0;
		}
	}
	return 1;
}

/* Submits one request to the disk and reports its error. */
static int
submit(
	struct disk *disk,
	enum bio_op op,
	uint64_t block,
	uint32_t count,
	void *data)
{
	struct bio bio;

	memset(&bio, 0, sizeof(bio));
	bio.b_op = op;
	bio.b_disk = disk;
	bio.b_block = block;
	bio.b_mapped_block = block;
	bio.b_block_count = count;
	bio.b_data = data;
	completed_error = -1;
	completed_bytes = 0;
	(void)disk->d_ops->submit(disk, &bio);
	return completed_error;
}

/* Reports whether every command was given to the controller and ended the chain. */
static int
all_commands_well_formed(
	void)
{
	unsigned position;

	for (position = 0; position < card.sent_count; position++) {
		if ((card.log[position].value & C_OWNER) == 0)
			return 0;
		if ((card.log[position].value & C_END) == 0)
			return 0;
	}
	return 1;
}

/* Attaches a card to a fresh driver unit with the test's description. */
static int
attach(
	unsigned unit,
	struct disk **disk)
{
	struct drv_meson_mmc_config config;

	memset(&config, 0, sizeof(config));
	config.registers = card.registers;
	config.bounce = bounce;
	config.bounce_bus_address = BOUNCE_BUS;
	config.bounce_bytes = BOUNCE_BYTES;
	config.slow_rate = 24000000ULL;
	config.fast_rate = 1000000000ULL;
	config.select_source = record_source;
	config.bus_width = 4U;
	config.max_frequency = 200000000ULL;
	config.high_speed = 1;
	config.unit = unit;
	return drv_meson_mmc_attach(&config, disk);
}

/* Checks the bring-up, the transfers and the recovery of a version 2 card. */
static void
check_driver(
	void)
{
	static const uint32_t bring_up[] = {
		0, 8, 55, 141, 55, 141, 55, 141, 2, 3, 9, 7, 16, 55, 151, 55, 106, 6, 6
	};
	struct drv_meson_mmc_card info;
	const struct sent *sent;
	struct disk *disk;
	uint8_t data[8U * 512U];
	int same;
	int error;

	reset_model(1);
	error = attach(0, &disk);
	check(error == 0, "a version 2 card attaches");
	if (error != 0)
		return;
	check(sequence_is(bring_up, sizeof(bring_up) / sizeof(bring_up[0])), "the bring-up sends CMD0 CMD8 ACMD41 CMD2 CMD3 CMD9 CMD7 CMD16 ACMD51 ACMD6 CMD6 CMD6");
	check(all_commands_well_formed(), "every command has the owner and end-of-chain bits");

	sent = find_sent(0, 0, 0);
	check(sent != NULL && (sent->value & C_NO_RESP) != 0, "CMD0 asks for no response");
	sent = find_sent(8, 0, 0);
	check(sent != NULL && sent->argument == 0x123U && (sent->value & C_RESP) != 0, "CMD8 asks 3.3 V with pattern 23");
	sent = find_sent(41, 1, 0);
	check(sent != NULL && (sent->value & C_NO_CRC) != 0 && sent->argument == 0x40300000U, "ACMD41 asks high capacity at 3.3 V without a CRC check");
	sent = find_sent(2, 0, 0);
	check(sent != NULL && (sent->value & C_LONG) != 0, "CMD2 asks for a long response");
	sent = find_sent(7, 0, 0);
	check(sent != NULL && (sent->value & C_R1B) != 0 && sent->argument == 0xb3680000U, "CMD7 names the card and waits for busy");
	sent = find_sent(51, 1, 0);
	check(sent != NULL && (sent->value & C_DATA) != 0 && (sent->value & C_BLOCK_MODE) == 0 && (sent->value & C_LENGTH) == 8U, "ACMD51 reads 8 bytes");
	sent = find_sent(6, 1, 0);
	check(sent != NULL && sent->argument == 2U, "ACMD6 sets four data lines");
	sent = find_sent(6, 0, 1);
	check(sent != NULL && sent->argument == 0x80fffff1U && (sent->value & C_LENGTH) == 64U, "CMD6 switches to high speed and reads 64 bytes");

	check((get32(card.registers, R_CFG) & 3U) == 1U, "the controller runs four data lines");
	check((get32(card.registers, R_CLOCK) & 0x10000000U) != 0, "the clock register keeps the clock always on");
	check(card.divider_count >= 3U && card.dividers[0] == 0x3fU, "the clock starts at the largest divider");
	check(card.divider_count >= 3U && card.dividers[1] == 60U, "identification divides 24 MHz by 60");
	check(card.divider_count >= 3U && card.dividers[2] == 1U, "the default speed runs 24 MHz undivided");
	check(card.divider_count >= 4U && card.dividers[3] == 20U, "high speed divides 1 GHz by 20");
	check(card.source_count >= 3U && card.sources[0] == 0 && card.sources[1] == 0 && card.sources[2] == 1, "the sources are slow, slow, fast");
	check(get32(card.registers, R_IRQ_EN) == 0, "no interrupt is enabled");

	error = drv_meson_mmc_card_info(0, &info);
	check(error == 0, "the card's description is published");
	check(info.block_count == CARD_BLOCKS, "the capacity is read from the CSD");
	check(info.high_capacity == 1, "the card is high capacity");
	check(strcmp(info.product, "SU08G") == 0, "the product name is read from the CID");
	check(info.serial == 0x12345678U, "the serial number is read from the CID");
	check(info.interface_echo == 0x123U, "the CMD8 echo is kept");
	check(info.card_clock == 50000000ULL && info.high_speed == 1, "the card runs at 50 MHz high speed");
	check(strcmp(disk->d_name, "mmcblk0") == 0, "the disk is mmcblk0");
	check(disk->d_block_count == CARD_BLOCKS && disk->d_max_transfer_blocks == 128U, "the disk has the card's blocks and runs of 128");

	/* One block: CMD17 counting 512 bytes as zero. */
	card.sent_count = 0;
	error = submit(disk, BIO_READ, 5, 1, data);
	same = memcmp(data, card.memory + 5U * 512U, 512U) == 0;
	check(error == 0 && same, "a one-block read returns the block");
	sent = find_sent(17, 0, 0);
	check(sent != NULL && sent->argument == 5U && (sent->value & C_BLOCK_MODE) == 0 && (sent->value & C_LENGTH) == 0, "CMD17 reads block 5 counting 512 bytes as zero");

	/* Several blocks: CMD18 in block mode, then CMD12. */
	card.sent_count = 0;
	error = submit(disk, BIO_READ, 9, 8, data);
	same = memcmp(data, card.memory + 9U * 512U, 8U * 512U) == 0;
	check(error == 0 && same, "an eight-block read returns the blocks");
	sent = find_sent(18, 0, 0);
	check(sent != NULL && (sent->value & C_BLOCK_MODE) != 0 && (sent->value & C_LENGTH) == 8U, "CMD18 counts eight blocks");
	check(card.sent_count == 2U && card.log[1].index == 12U && (card.log[1].value & C_R1B) != 0, "CMD12 ends the read and waits for busy");

	/* A write: CMD25, CMD12, then CMD13. */
	memset(data, 0x5a, 3U * 512U);
	card.sent_count = 0;
	error = submit(disk, BIO_WRITE, 100, 3, data);
	same = memcmp(card.memory + 100U * 512U, data, 3U * 512U) == 0;
	check(error == 0 && same, "a three-block write reaches the card");
	check(card.sent_count == 3U && card.log[0].index == 25U && (card.log[0].value & C_WRITE) != 0 && card.log[1].index == 12U && card.log[2].index == 13U, "the write sends CMD25 CMD12 CMD13");

	/* A CRC error at high speed: the run is tried again at the default speed. */
	card.crc_errors = 1;
	card.sent_count = 0;
	error = submit(disk, BIO_READ, 20, 4, data);
	same = memcmp(data, card.memory + 20U * 512U, 4U * 512U) == 0;
	check(error == 0 && same, "a read after a CRC error returns the blocks");
	error = drv_meson_mmc_card_info(0, &info);
	check(error == 0 && info.slowed == 1 && info.card_clock == 24000000ULL, "the CRC error moved the clock down to 24 MHz");
	check(find_sent(18, 0, 1) != NULL, "the read was sent again");

	/* The bounce buffer bounds a run. */
	card.sent_count = 0;
	check(submit(disk, BIO_FLUSH, 0, 0, NULL) == 0 && card.sent_count == 0, "a flush sends nothing");
}

/* Checks a version 1 card, which does not answer CMD8 and is addressed by byte. */
static void
check_version1(
	void)
{
	struct drv_meson_mmc_card info;
	const struct sent *sent;
	struct disk *disk;
	uint8_t data[512];
	int error;

	reset_model(0);
	error = attach(1, &disk);
	check(error == 0, "a version 1 card attaches after CMD8 times out");
	if (error != 0)
		return;
	sent = find_sent(41, 1, 0);
	check(sent != NULL && sent->argument == 0x00300000U, "ACMD41 does not ask for high capacity");
	error = drv_meson_mmc_card_info(1, &info);
	check(error == 0 && info.csd_version == 1U && info.block_count == 4096U, "the version 1 CSD gives 4096 blocks");
	check(strcmp(disk->d_name, "mmcblk1") == 0, "the disk is mmcblk1");
	card.sent_count = 0;
	error = submit(disk, BIO_READ, 3, 1, data);
	sent = find_sent(17, 0, 0);
	check(error == 0 && sent != NULL && sent->argument == 3U * 512U, "a standard capacity card is addressed by byte");
}

/* Loads the device tree the glue reads. */
static int
load_tree(
	const char *path)
{
	FILE *file;
	long size;

	file = fopen(path, "rb");
	if (file == NULL)
		return 0;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	tree = malloc((size_t)size);
	if (tree == NULL || fread(tree, 1, (size_t)size, file) != (size_t)size) {
		fclose(file);
		return 0;
	}
	fclose(file);
	return 1;
}

/* Checks the glue on the ODROID-C5's device tree. */
static void
check_glue(
	const char *path)
{
	struct disk *disk;
	uint32_t selector;
	uint32_t mux;
	uint32_t pull_enable;
	uint32_t pull_up;
	uint32_t direction;
	uint32_t level;
	int error;

	check(load_tree(path), "the device tree loads");
	if (tree == NULL)
		return;
	reset_model(1);

	/* The card detect reads low: a card is in. */
	put32(pins_block, 0xc0U + 0x50U * 4U, 0x00U);
	error = drv_s905x5m_mmc_init(0x1000ULL);
	check(error == 0, "the glue brings the card up");
	disk = drv_s905x5m_mmc_disk();
	check(disk != NULL && strcmp(disk->d_name, "mmcblk0") == 0, "the glue publishes mmcblk0");

	/* The pins: C0..C5 function 1, C6 a GPIO; pulled up; C6 an input. */
	mux = get32(pins_block, 0x09U * 4U);
	check(mux == 0x00111111U, "GPIOC_0..5 carry the SD function and GPIOC_6 is a GPIO");
	pull_enable = get32(pins_block, 0xc0U + (0x50U + 3U) * 4U);
	pull_up = get32(pins_block, 0xc0U + (0x50U + 4U) * 4U);
	check((pull_enable & 0x7fU) == 0x7fU && (pull_up & 0x7fU) == 0x7fU, "GPIOC_0..6 are pulled up");
	check(get32(pins_block, 0xc0U + (0x50U + 7U) * 4U) == 0x0aaaU, "GPIOC_0..5 drive 3 mA");
	direction = get32(pins_block, 0xc0U + (0x50U + 2U) * 4U);
	check((direction & 0x40U) != 0, "GPIOC_6 is an input");

	/* The supplies: GPIOH_6 high, GPIODV_4 low, both outputs. */
	level = get32(pins_block, 0xc0U + (0x20U + 1U) * 4U);
	direction = get32(pins_block, 0xc0U + (0x20U + 2U) * 4U);
	check((level & 0x40U) != 0 && (direction & 0x40U) == 0, "GPIOH_6 drives the card's supply on");
	level = get32(pins_block, 0xc0U + (0x70U + 1U) * 4U);
	direction = get32(pins_block, 0xc0U + (0x70U + 2U) * 4U);
	check((level & 0x10U) == 0 && (direction & 0x10U) == 0, "GPIODV_4 drives 3.3 V signalling");

	/* The gate and the selector: high speed from the 1 GHz clock, undivided, gated on. */
	check((get32(clock_block, 0x44U) & (1U << 25)) != 0, "the controller's peripheral gate is on");
	selector = get32(clock_block, 0x16cU);
	check(((selector >> 25) & 7U) == 1U && ((selector >> 16) & 0x7fU) == 0 && (selector & (1U << 23)) != 0, "the selector feeds the 1 GHz clock undivided");
	check((get32(card.registers, R_CMD_DAT) & ~3U) == (uint32_t)BOUNCE_BUS, "the DMA goes to the bounce buffer");
}

int
main(
	int argc,
	char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "driver") == 0) {
		check_driver();
	} else if (argc >= 2 && strcmp(argv[1], "version1") == 0) {
		check_version1();
	} else if (argc >= 3 && strcmp(argv[1], "glue") == 0) {
		check_glue(argv[2]);
	} else {
		fprintf(stderr, "usage: %s driver | version1 | glue TREE\n", argv[0]);
		return 2;
	}
	printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
	if (failures != 0)
		return 1;
	return 0;
}
