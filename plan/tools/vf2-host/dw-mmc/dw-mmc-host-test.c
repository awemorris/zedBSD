/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the DesignWare SD/MMC driver and its JH7110 glue on a
 * fake controller with a fake SD card behind it.
 *
 * The fake controller keeps its registers in memory and runs a command
 * when the driver writes the command register's start bit; the fake card
 * answers the SD commands and moves blocks through the FIFO.  The checks
 * look at the commands the driver sent (index, argument, the command
 * register's bits), the clock divider settings, the bus width, the data
 * that moved, and the recovery from injected errors.
 *
 *   dw-mmc-host-test driver
 *   dw-mmc-host-test phase
 *   dw-mmc-host-test no-hook
 *   dw-mmc-host-test glue vf2-mmc.dtb
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drivers/generic/fdt.h>
#include <drivers/mmc/dw-mmc.h>
#include <drivers/platform/visionfive2/vf2-crg.h>
#include <drivers/platform/visionfive2/vf2-mmc.h>
#include <drivers/platform/visionfive2/vf2-pinctrl.h>
#include <drivers/platform/visionfive2/vf2-syscon.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/disk.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The controller's registers the model gives meaning to. */
#define R_CTRL		0x000U
#define R_CLKDIV	0x008U
#define R_CLKENA	0x010U
#define R_CTYPE		0x018U
#define R_BLKSIZ	0x01cU
#define R_BYTCNT	0x020U
#define R_CMDARG	0x028U
#define R_CMD		0x02cU
#define R_RESP0		0x030U
#define R_RINTSTS	0x044U
#define R_STATUS	0x048U
#define R_FIFOTH	0x04cU
#define R_VERID		0x06cU
#define R_HCON		0x070U
#define R_UHS_EXT	0x108U

/* The command register's bits. */
#define C_RESPONSE	0x00000040U
#define C_LONG		0x00000080U
#define C_CRC		0x00000100U
#define C_DATA		0x00000200U
#define C_WRITE		0x00000400U
#define C_AUTO_STOP	0x00001000U
#define C_WAIT_DATA	0x00002000U
#define C_STOP_ABORT	0x00004000U
#define C_INIT		0x00008000U
#define C_CLOCK_ONLY	0x00200000U
#define C_HOLD		0x20000000U
#define C_START		0x80000000U

/* The raw interrupt status bits. */
#define S_CD		0x00000004U
#define S_DTO		0x00000008U
#define S_TXDR		0x00000010U
#define S_RXDR		0x00000020U
#define S_DCRC		0x00000080U
#define S_RTO		0x00000100U
#define S_ACD		0x00004000U

/* The fake card's capacity in blocks. */
#define CARD_BLOCKS	4096U

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
	uint32_t fifo_offset;
	uint32_t fifo_width;
	uint32_t fifo_depth;

	/* The card. */
	int version2;
	int high_capacity;
	int high_speed;
	int busy_polls;
	int app;
	uint32_t rca;
	uint32_t cid[4];
	uint32_t csd[4];
	uint8_t scr[8];
	uint8_t memory[CARD_BLOCKS * 512U];
	int switched;
	uint32_t bus_width_argument;

	/* The data phase in progress. */
	int reading;
	int writing;
	int auto_stop;
	uint32_t block;
	uint8_t buffer[0x20000];
	uint32_t position;
	uint32_t length;
	int busy_reads;

	/* Errors to inject: data CRC errors on the next data commands. */
	int crc_errors;

	/*
	 * A slot whose multiple-block reads at the bypassed clock (high
	 * speed) fail with a data CRC error outside the sample phases
	 * good_low..good_high; good_low above good_high makes every phase fail.
	 */
	int phase_window;
	uint32_t good_low;
	uint32_t good_high;

	/* What happened. */
	struct sent log[256];
	unsigned sent_count;
	uint32_t dividers[16];
	unsigned divider_count;
	unsigned fifo_resets;
};

static struct model card;

/* The fake register blocks of the SoC for the glue. */
struct fake_block {
	uint64_t address;
	size_t size;
	uint8_t *memory;
};

static uint8_t sys_crg[0x10000];
static uint8_t stg_crg[0x10000];
static uint8_t aon_crg[0x10000];
static uint8_t sys_syscon[0x1000];
static uint8_t stg_syscon[0x1000];
static uint8_t aon_syscon[0x1000];
static uint8_t sys_pinctrl[0x10000];
static uint8_t aon_pinctrl[0x10000];

static struct fake_block blocks[] = {
	{ 0x13020000ULL, sizeof(sys_crg), sys_crg },
	{ 0x10230000ULL, sizeof(stg_crg), stg_crg },
	{ 0x17000000ULL, sizeof(aon_crg), aon_crg },
	{ 0x13030000ULL, sizeof(sys_syscon), sys_syscon },
	{ 0x10240000ULL, sizeof(stg_syscon), stg_syscon },
	{ 0x17010000ULL, sizeof(aon_syscon), aon_syscon },
	{ 0x13040000ULL, sizeof(sys_pinctrl), sys_pinctrl },
	{ 0x17020000ULL, sizeof(aon_pinctrl), aon_pinctrl },
	{ 0x16020000ULL, 0x10000U, card.registers }
};

/* Each generator's reset assert and status offsets, for the fake status. */
struct fake_reset {
	uint8_t *memory;
	uint32_t assert_offset;
	uint32_t status_offset;
	uint32_t registers;
};

static const struct fake_reset resets[] = {
	{ sys_crg, 0x2f8U, 0x308U, 4U },
	{ stg_crg, 0x74U, 0x78U, 1U },
	{ aon_crg, 0x38U, 0x3cU, 1U }
};

/* The fake monotonic counter: 1 MHz, a millisecond per read. */
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
	printf("%s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		failures++;
}

static uint32_t
get32(
	const uint8_t *memory,
	uint32_t offset)
{
	uint32_t value;

	memcpy(&value, memory + offset, 4);
	return value;
}

static void
put32(
	uint8_t *memory,
	uint32_t offset,
	uint32_t value)
{
	memcpy(memory + offset, &value, 4);
}

/* Sets raw interrupt status bits. */
static void
raise(
	uint32_t bits)
{
	put32(card.registers, R_RINTSTS, get32(card.registers, R_RINTSTS) | bits);
}

/* Puts a short response. */
static void
respond(
	uint32_t value)
{
	put32(card.registers, R_RESP0, value);
}

/* Puts a long response, word 0 holding bits 31:0. */
static void
respond_long(
	const uint32_t *words)
{
	unsigned index;

	for (index = 0; index < 4U; index++)
		put32(card.registers, R_RESP0 + index * 4U, words[index]);
}

/* The card status the card answers an R1 command with: transfer state, ready. */
static uint32_t
card_status(
	void)
{
	return (4U << 9) | 0x100U;
}

/* Starts a read data phase from a buffer. */
static void
start_read(
	const uint8_t *data,
	uint32_t length,
	int auto_stop)
{
	memcpy(card.buffer, data, length);
	card.position = 0;
	card.length = length;
	card.reading = 1;
	card.auto_stop = auto_stop;
}

/* Ends the data phase in progress. */
static void
end_data(
	void)
{
	uint32_t status;

	status = S_DTO;
	if (card.auto_stop)
		status |= S_ACD;
	raise(status);
	card.reading = 0;
	card.writing = 0;
}

/* Runs one command the driver started. */
static void
run_command(
	uint32_t value)
{
	uint32_t index;
	uint32_t argument;
	uint32_t length;
	uint32_t block;
	uint8_t status[64];
	int app;

	index = value & 0x3fU;
	argument = get32(card.registers, R_CMDARG);
	app = card.app;
	card.app = 0;
	if (card.sent_count < 256U) {
		card.log[card.sent_count].index = index;
		card.log[card.sent_count].argument = argument;
		card.log[card.sent_count].value = value;
		card.log[card.sent_count].app = app;
		card.sent_count++;
	}
	length = get32(card.registers, R_BYTCNT);

	switch (index) {
	case 0:
		raise(S_CD);
		return;
	case 8:
		if (!card.version2) {
			raise(S_CD | S_RTO);
			return;
		}
		respond(argument & 0xfffU);
		raise(S_CD);
		return;
	case 55:
		card.app = 1;
		respond(card_status() | 0x20U);
		raise(S_CD);
		return;
	case 41:
		if (!app)
			break;
		if (card.busy_polls > 0) {
			card.busy_polls--;
			respond(0x00ff8000U);
		} else if (card.high_capacity && (argument & 0x40000000U) != 0) {
			respond(0xc0ff8000U);
		} else {
			respond(0x80ff8000U);
		}
		raise(S_CD);
		return;
	case 2:
		respond_long(card.cid);
		raise(S_CD);
		return;
	case 3:
		respond(card.rca << 16);
		raise(S_CD);
		return;
	case 9:
		respond_long(card.csd);
		raise(S_CD);
		return;
	case 7:
		respond(card_status());
		card.busy_reads = 2;
		raise(S_CD);
		return;
	case 16:
	case 13:
	case 12:
		respond(card_status());
		raise(S_CD);
		return;
	case 6:
		respond(card_status());
		raise(S_CD);
		if (app) {
			card.bus_width_argument = argument;
			return;
		}
		memset(status, 0, sizeof(status));
		status[12] = 0x80U;
		status[13] = card.high_speed ? 0x03U : 0x01U;
		if ((argument & 0x80000000U) != 0)
			card.switched = 1;
		start_read(status, length, 0);
		return;
	case 51:
		if (!app)
			break;
		respond(card_status());
		raise(S_CD);
		start_read(card.scr, length, 0);
		return;
	case 17:
	case 18:
	case 24:
	case 25:
		respond(card_status());
		raise(S_CD);
		block = argument;
		if (!card.high_capacity)
			block = argument / 512U;
		card.block = block;
		if (card.phase_window && index == 18 && get32(card.registers, R_CLKDIV) == 0) {
			uint32_t phase = (get32(card.registers, R_UHS_EXT) >> 16) & 0x1fU;

			if (phase < card.good_low || phase > card.good_high) {
				raise(S_DCRC);
				return;
			}
		}
		if (card.crc_errors > 0) {
			card.crc_errors--;
			raise(S_DCRC);
			return;
		}
		if (index == 17 || index == 18) {
			start_read(card.memory + (size_t)block * 512U, length, (value & C_AUTO_STOP) != 0);
		} else {
			card.position = 0;
			card.length = length;
			card.writing = 1;
			card.auto_stop = (value & C_AUTO_STOP) != 0;
			card.busy_reads = 3;
		}
		return;
	}
	raise(S_CD | S_RTO);
}

/* The driver's preemption nesting, and the FIFO words it moved with preemption on (it must move none). */
static int preempt_depth;
static int fifo_preemptible;

/* Takes one word written to the FIFO. */
static void
fifo_write(
	uint32_t value)
{
	if (preempt_depth <= 0)
		fifo_preemptible++;
	if (!card.writing || card.position >= card.length)
		return;
	memcpy(card.buffer + card.position, &value, 4);
	card.position += 4U;
	if (card.position == card.length) {
		memcpy(card.memory + (size_t)card.block * 512U, card.buffer, card.length);
		end_data();
	}
}

/* Gives one word read from the FIFO. */
static uint32_t
fifo_read(
	void)
{
	uint32_t value;

	value = 0;
	if (preempt_depth <= 0)
		fifo_preemptible++;
	if (!card.reading || card.position >= card.length)
		return value;
	memcpy(&value, card.buffer + card.position, 4);
	card.position += 4U;
	if (card.position == card.length)
		end_data();
	return value;
}

/* Reads a register of the fake controller. */
static uint32_t
controller_read(
	uint32_t offset)
{
	uint32_t value;
	uint32_t entries;

	if (offset == card.fifo_offset || (card.fifo_width == 8U && offset == card.fifo_offset + 4U))
		return fifo_read();
	value = get32(card.registers, offset);
	if (offset == R_RINTSTS) {
		if (card.reading && card.position < card.length)
			value |= S_RXDR;
		if (card.writing && card.position < card.length)
			value |= S_TXDR;
		return value;
	}
	if (offset == R_STATUS) {
		value = 0;
		if (card.reading) {
			entries = (card.length - card.position) / card.fifo_width;
			if (entries > card.fifo_depth)
				entries = card.fifo_depth;
			value |= entries << 17;
		}
		if (card.busy_reads > 0) {
			card.busy_reads--;
			value |= 0x200U;
		}
		return value;
	}
	return value;
}

/* Writes a register of the fake controller. */
static void
controller_write(
	uint32_t offset,
	uint32_t value)
{
	if (offset == card.fifo_offset || (card.fifo_width == 8U && offset == card.fifo_offset + 4U)) {
		fifo_write(value);
		return;
	}
	if (offset == R_RINTSTS) {
		put32(card.registers, R_RINTSTS, get32(card.registers, R_RINTSTS) & ~value);
		return;
	}
	if (offset == R_CTRL) {
		if ((value & 0x2U) != 0) {
			card.fifo_resets++;
			card.reading = 0;
			card.writing = 0;
		}
		put32(card.registers, R_CTRL, value & ~0x7U);
		return;
	}
	if (offset == R_CMD) {
		put32(card.registers, R_CMD, value & ~C_START);
		if ((value & C_START) == 0)
			return;
		if ((value & C_CLOCK_ONLY) != 0) {
			if (card.divider_count < 16U && get32(card.registers, R_CLKENA) != 0)
				card.dividers[card.divider_count++] = get32(card.registers, R_CLKDIV);
			return;
		}
		run_command(value);
		return;
	}
	put32(card.registers, offset, value);
}

uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	const uint8_t *target;
	uint32_t value;

	target = (const uint8_t *)address;
	if (target >= card.registers && target < card.registers + sizeof(card.registers))
		return controller_read((uint32_t)(target - card.registers));
	memcpy(&value, target, 4);
	return value;
}

void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uint8_t *target;
	unsigned index;
	uint32_t offset;

	target = (uint8_t *)address;
	if (target >= card.registers && target < card.registers + sizeof(card.registers)) {
		controller_write((uint32_t)(target - card.registers), value);
		return;
	}
	memcpy(target, &value, 4);

	/* A reset assert register's status follows it: released is set. */
	for (index = 0; index < sizeof(resets) / sizeof(resets[0]); index++) {
		if (target < resets[index].memory + resets[index].assert_offset)
			continue;
		offset = (uint32_t)(target - resets[index].memory) - resets[index].assert_offset;
		if (offset >= resets[index].registers * 4U)
			continue;
		put32(resets[index].memory, resets[index].status_offset + offset, ~value);
	}
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	unsigned index;

	(void)attributes;
	for (index = 0; index < sizeof(blocks) / sizeof(blocks[0]); index++) {
		if (blocks[index].address == address && size <= blocks[index].size) {
			*mapped = blocks[index].memory;
			return 0;
		}
	}
	printf("unexpected map of %llx\n", (unsigned long long)address);
	return ENOMEM;
}

bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	fake_now += 1000U;
	*counter = fake_now;
	*frequency_hz = 1000000U;
	return true;
}

int kern_device_unmap(void *mapped, size_t size) { (void)mapped; (void)size; return 0; }
unsigned long spin_lock_irqsave(struct spinlock *lock) { (void)lock; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled) { (void)lock; (void)enabled; }
int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name) { (void)mutex; (void)rank; (void)name; return 0; }
void mutex_lock(struct mutex *mutex) { (void)mutex; }
void mutex_unlock(struct mutex *mutex) { (void)mutex; }
struct disk *disk_alloc(void) { return calloc(1, sizeof(struct disk)); }
int disk_create(struct disk *disk) { (void)disk; return 0; }
void disk_set_stats_kind(struct disk *disk, uint32_t kind) { (void)disk; (void)kind; }
void bio_complete(struct bio *bio, int error, size_t transferred) { (void)bio; completed_error = error; completed_bytes = transferred; }
void kern_preempt_disable(void) { preempt_depth++; }
void kern_preempt_enable(void) { preempt_depth--; }
void kern_logf(const char *format, ...) { va_list ap; va_start(ap, format); printf("  log: "); vprintf(format, ap); va_end(ap); }

/* Builds a 128-bit register from fields: sets width bits at start. */
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
		if ((value >> bit) & 1U)
			words[position / 32U] |= 1U << (position % 32U);
		else
			words[position / 32U] &= ~(1U << (position % 32U));
	}
}

/* Prepares the fake controller and a card. */
static void
reset_model(
	int version2,
	int fifo_width,
	uint32_t version)
{
	unsigned index;

	memset(&card, 0, sizeof(card));
	card.fifo_width = (uint32_t)fifo_width;
	card.fifo_depth = 16U;
	card.fifo_offset = 0x200U;
	if (version < 0x240aU)
		card.fifo_offset = 0x100U;
	put32(card.registers, R_VERID, 0x53000000U | version);
	put32(card.registers, R_HCON, (fifo_width == 8 ? 2U : 1U) << 7);
	put32(card.registers, R_FIFOTH, 15U << 16);
	card.version2 = version2;
	card.high_capacity = version2;
	card.high_speed = 1;
	card.busy_polls = 1;
	card.rca = 0xb368U;

	/* CID: manufacturer 0x03, OEM "SD", product "SC32G", revision 8.0, serial 0x12345678. */
	set_bits(card.cid, 120, 8, 0x03U);
	set_bits(card.cid, 104, 16, ('S' << 8) | 'D');
	set_bits(card.cid, 96, 8, 'S');
	set_bits(card.cid, 88, 8, 'C');
	set_bits(card.cid, 80, 8, '3');
	set_bits(card.cid, 72, 8, '2');
	set_bits(card.cid, 64, 8, 'G');
	set_bits(card.cid, 56, 8, 0x80U);
	set_bits(card.cid, 24, 32, 0x12345678U);

	/* CSD: version 2 with C_SIZE 59639 (29.1 GiB), or version 1 of 2 MiB. */
	if (version2) {
		set_bits(card.csd, 126, 2, 1U);
		set_bits(card.csd, 48, 22, 59639U);
	} else {
		set_bits(card.csd, 126, 2, 0U);
		set_bits(card.csd, 80, 4, 9U);
		set_bits(card.csd, 62, 12, 1023U);
		set_bits(card.csd, 47, 3, 0U);
	}
	set_bits(card.csd, 84, 12, 0x5b5U);

	/* SCR: structure 0, specification 2, bus widths 1 and 4. */
	card.scr[0] = 0x02U;
	card.scr[1] = 0x05U;

	for (index = 0; index < sizeof(card.memory); index++)
		card.memory[index] = (uint8_t)(index * 7U + index / 512U);
}

/* Finds the nth command the driver sent with an index, or NULL. */
static const struct sent *
find_sent(
	uint32_t index,
	unsigned nth)
{
	unsigned position;

	for (position = 0; position < card.sent_count; position++) {
		if (card.log[position].index != index)
			continue;
		if (nth == 0)
			return &card.log[position];
		nth--;
	}
	return NULL;
}

/* Reports whether the sent indexes from a position match a list. */
static int
sequence_is(
	unsigned from,
	const uint32_t *indexes,
	unsigned count)
{
	unsigned position;

	if (from + count != card.sent_count) {
		printf("  sent %u commands from %u, expected %u:", card.sent_count - from, from, count);
		for (position = from; position < card.sent_count; position++)
			printf(" %u", card.log[position].index);
		printf("\n");
		return 0;
	}
	for (position = 0; position < count; position++) {
		if (card.log[from + position].index != indexes[position]) {
			printf("  command %u is %u, expected %u\n", position, card.log[from + position].index, indexes[position]);
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

/* Reports whether every command carried the start and hold bits, and the wait where it should. */
static int
all_commands_well_formed(
	void)
{
	unsigned position;
	uint32_t value;
	uint32_t index;

	for (position = 0; position < card.sent_count; position++) {
		value = card.log[position].value;
		index = card.log[position].index;
		if ((value & (C_START | C_HOLD)) != (C_START | C_HOLD))
			return 0;
		if (index == 12 || index == 13) {
			if ((value & C_WAIT_DATA) != 0)
				return 0;
		} else if ((value & C_WAIT_DATA) == 0) {
			return 0;
		}
		if (index == 12 && (value & C_STOP_ABORT) == 0)
			return 0;
	}
	return 1;
}

/* Fills a buffer with a pattern of a seed. */
static void
fill(
	uint8_t *data,
	size_t length,
	unsigned seed)
{
	size_t index;

	for (index = 0; index < length; index++)
		data[index] = (uint8_t)(index * 13U + seed);
}

static int
card_detect_present(
	void *context,
	int *present)
{
	*present = *(int *)context;
	return 0;
}

/* The driver on its own: a high-capacity card with a 32-bit FIFO, then a standard-capacity one with a 64-bit FIFO. */
static void
check_driver(
	void)
{
	static const uint32_t bring_up[] = { 0, 8, 55, 41, 55, 41, 2, 3, 9, 7, 16, 55, 51, 55, 6, 6, 6 };
	static const uint32_t bring_up_v1[] = { 0, 8, 55, 41, 55, 41, 2, 3, 9, 7, 16, 55, 51 };
	static const uint32_t recovered_read[] = { 18, 12, 13, 18 };
	static uint8_t data[200U * 512U];
	static uint8_t other[200U * 512U];
	struct drv_dw_mmc_config config;
	struct drv_dw_mmc_card info;
	const struct sent *sent;
	struct disk *disk;
	unsigned before;
	int present;
	int error;

	/* An empty slot: no disk and no command. */
	reset_model(1, 4, 0x290aU);
	memset(&config, 0, sizeof(config));
	config.registers = card.registers;
	config.clock_rate = 49500000U;
	config.fifo_depth = 32U;
	config.bus_width = 4U;
	config.max_frequency = 100000000U;
	config.high_speed = 1;
	config.power_on_delay_ms = 200U;
	config.card_present = card_detect_present;
	present = 0;
	config.context = &present;
	config.unit = 0;
	error = drv_dw_mmc_attach(&config, &disk);
	check(error == ENODEV && card.sent_count == 0, "empty slot: ENODEV, no command sent");

	/* A high-capacity card. */
	present = 1;
	error = drv_dw_mmc_attach(&config, &disk);
	check(error == 0 && disk != NULL, "SDHC card attaches");
	if (error != 0)
		return;
	check(sequence_is(0, bring_up, sizeof(bring_up) / sizeof(bring_up[0])),
	      "bring-up: CMD0 8 ACMD41 x2 2 3 9 7 16 ACMD51 ACMD6 CMD6 x2");
	check(all_commands_well_formed(), "every command: start, hold, waits for data except 12/13");
	check(card.log[0].value == (C_START | C_HOLD | C_WAIT_DATA | C_INIT), "CMD0: initialization clocks, no response");
	check(card.log[1].argument == 0x123U && (card.log[1].value & (C_RESPONSE | C_CRC)) == (C_RESPONSE | C_CRC),
	      "CMD8: 2.7-3.6 V and pattern 0x23, short response with CRC");
	check(card.log[3].app && card.log[3].argument == 0x40300000U &&
	      (card.log[3].value & (C_RESPONSE | C_CRC)) == C_RESPONSE,
	      "ACMD41: 3.3 V window and high capacity, no CRC check");
	check(card.log[2].argument == 0, "CMD55 before the address is known carries RCA 0");
	check((card.log[6].value & (C_RESPONSE | C_LONG | C_CRC)) == (C_RESPONSE | C_LONG | C_CRC), "CMD2: long response");
	check(card.log[8].argument == (0xb368U << 16) && (card.log[8].value & C_LONG) != 0, "CMD9: card address, long response");
	check(card.log[9].argument == (0xb368U << 16), "CMD7 selects the card's address");
	check(card.log[10].argument == 512U, "CMD16: 512-byte blocks");
	check(card.log[11].argument == (0xb368U << 16) && card.log[12].app &&
	      (card.log[12].value & (C_DATA | C_WRITE)) == C_DATA,
	      "ACMD51: data read after CMD55 with the card's address");
	check(card.log[14].app && card.log[14].argument == 2U && card.bus_width_argument == 2U, "ACMD6: four lines");
	check(card.log[15].argument == 0x00ffffffU && (card.log[15].value & C_DATA) != 0, "CMD6 asks without switching");
	check(card.log[16].argument == 0x80fffff1U && card.switched, "CMD6 switches group 1 to high speed");
	check(card.divider_count == 3 && card.dividers[0] == 62U && card.dividers[1] == 1U && card.dividers[2] == 0U,
	      "card clock: /124 = 399 kHz, /2 = 24.75 MHz, bypass = 49.5 MHz");
	check(get32(card.registers, R_CTYPE) == 1U, "controller drives four lines");
	check(get32(card.registers, R_FIFOTH) == ((2U << 28) | (15U << 16) | 16U), "FIFO thresholds at its halves");
	check((get32(card.registers, R_CTRL) & 0x02000030U) == 0x10U, "PIO: DMA off, interrupt enable on");
	check(get32(card.registers, 0x004) == 1U, "card power on");
	error = drv_dw_mmc_card_info(0, &info);
	check(error == 0 && info.high_capacity && info.csd_version == 2U && info.block_count == 59640ULL * 1024ULL,
	      "card info: SDHC, 61071360 blocks");
	check(strcmp(info.product, "SC32G") == 0 && strcmp(info.oem, "SD") == 0 &&
	      info.manufacturer == 3U && info.serial == 0x12345678U && info.revision == 0x80U,
	      "card info: CID decoded");
	check(info.bus_width == 4U && info.card_clock == 49500000ULL && info.high_speed, "card info: 4-bit 49.5 MHz high speed");
	check(info.fifo_width == 4U && info.fifo_depth == 32U && info.controller_version == 0x5300290aU, "card info: controller");
	check(strcmp(disk->d_name, "mmcblk0") == 0 && disk->d_block_count == 59640ULL * 1024ULL &&
	      disk->d_block_size == 512U && disk->d_max_transfer_blocks == 128U,
	      "disk mmcblk0: capacity, 512-byte blocks, runs of 128");
	error = drv_dw_mmc_attach(&config, &disk);
	check(error == EBUSY, "the same unit twice: EBUSY");

	/* Reads and writes. */
	before = card.sent_count;
	error = submit(disk, BIO_READ, 100, 1, data);
	sent = find_sent(17, 0);
	check(error == 0 && completed_bytes == 512U && sent != NULL && sent->argument == 100U &&
	      get32(card.registers, R_BYTCNT) == 512U && (sent->value & C_AUTO_STOP) == 0 &&
	      memcmp(data, card.memory + 100U * 512U, 512) == 0,
	      "read 1 block: CMD17 at block 100, data matches");
	error = submit(disk, BIO_READ, 7, 8, data);
	sent = find_sent(18, 0);
	check(error == 0 && sent != NULL && sent->argument == 7U && (sent->value & C_AUTO_STOP) != 0 &&
	      get32(card.registers, R_BYTCNT) == 4096U && memcmp(data, card.memory + 7U * 512U, 4096) == 0,
	      "read 8 blocks: CMD18 with automatic stop, 4096 bytes, data matches");
	fill(data, 512U, 1U);
	error = submit(disk, BIO_WRITE, 300, 1, data);
	sent = find_sent(24, 0);
	check(error == 0 && sent != NULL && sent->argument == 300U && (sent->value & C_WRITE) != 0 &&
	      card.log[card.sent_count - 1U].index == 13U && memcmp(card.memory + 300U * 512U, data, 512) == 0,
	      "write 1 block: CMD24 then CMD13, card holds the data");
	fill(data, 3U * 512U, 2U);
	error = submit(disk, BIO_WRITE, 400, 3, data);
	sent = find_sent(25, 0);
	check(error == 0 && sent != NULL && sent->argument == 400U && (sent->value & (C_WRITE | C_AUTO_STOP)) == (C_WRITE | C_AUTO_STOP) &&
	      memcmp(card.memory + 400U * 512U, data, 3U * 512U) == 0,
	      "write 3 blocks: CMD25 with automatic stop, card holds the data");
	error = submit(disk, BIO_READ, 400, 3, other);
	check(error == 0 && memcmp(other, data, 3U * 512U) == 0, "the written blocks read back");
	before = card.sent_count;
	error = submit(disk, BIO_READ, 1000, 200, data);
	check(error == 0 && card.sent_count == before + 2U &&
	      card.log[before].argument == 1000U && card.log[before + 1U].argument == 1128U &&
	      memcmp(data, card.memory + 1000U * 512U, 200U * 512U) == 0,
	      "200 blocks: two runs, 128 and 72 blocks");
	error = submit(disk, BIO_FLUSH, 0, 0, NULL);
	check(error == 0, "flush completes at once");

	/* A data CRC error is recovered. */
	card.crc_errors = 1;
	before = card.sent_count;
	error = submit(disk, BIO_READ, 50, 4, data);
	check(error == 0 && sequence_is(before, recovered_read, 4U) && memcmp(data, card.memory + 50U * 512U, 2048) == 0,
	      "one CRC error: stop, status, the run again, data matches");
	card.crc_errors = 3;
	error = submit(disk, BIO_READ, 50, 4, data);
	check(error == 0 && memcmp(data, card.memory + 50U * 512U, 2048) == 0, "three CRC errors: recovered within the five tries (BUG-343)");
	card.crc_errors = 7;
	error = submit(disk, BIO_READ, 50, 4, data);
	check(error == 0 && card.crc_errors == 0 && memcmp(data, card.memory + 50U * 512U, 2048) == 0,
	      "seven CRC errors: the run a block at a time after its five tries, data matches");
	card.crc_errors = 1000;
	error = submit(disk, BIO_READ, 50, 4, data);
	check(error == EIO && completed_bytes == 0, "CRC errors on every try: EIO");
	card.crc_errors = 0;
	error = submit(disk, BIO_READ, 60, 2, data);
	check(error == 0 && memcmp(data, card.memory + 60U * 512U, 1024) == 0, "the card serves the next request");

	/* A standard-capacity card, addressed by byte, with a 64-bit FIFO at the old port. */
	reset_model(0, 8, 0x210aU);
	config.registers = card.registers;
	config.fifo_depth = 0;
	config.bus_width = 1U;
	config.high_speed = 0;
	config.card_present = NULL;
	config.unit = 1;
	error = drv_dw_mmc_attach(&config, &disk);
	check(error == 0, "SDSC card attaches");
	if (error != 0)
		return;
	check(sequence_is(0, bring_up_v1, sizeof(bring_up_v1) / sizeof(bring_up_v1[0])),
	      "SDSC bring-up: CMD8 unanswered, no bus width or speed switch");
	check(card.log[3].argument == 0x00300000U, "ACMD41 without high capacity");
	error = drv_dw_mmc_card_info(1, &info);
	check(error == 0 && !info.high_capacity && info.csd_version == 1U && info.block_count == 4096ULL,
	      "SDSC capacity: 1024 x 4 x 512 bytes = 4096 blocks");
	check(info.fifo_width == 8U && info.fifo_depth == 16U && info.bus_width == 1U && info.card_clock == 24750000ULL,
	      "64-bit FIFO, depth from the threshold register, 1-bit 24.75 MHz");
	check(strcmp(disk->d_name, "mmcblk1") == 0, "second unit is mmcblk1");
	error = submit(disk, BIO_READ, 3, 2, data);
	sent = find_sent(18, 0);
	check(error == 0 && sent != NULL && sent->argument == 3U * 512U && memcmp(data, card.memory + 3U * 512U, 1024) == 0,
	      "SDSC read: byte address, 64-bit FIFO data matches");
	fill(data, 2U * 512U, 3U);
	error = submit(disk, BIO_WRITE, 9, 2, data);
	check(error == 0 && memcmp(card.memory + 9U * 512U, data, 1024) == 0, "SDSC write through the 64-bit FIFO");
}

/* The glue on a VisionFive 2-shaped tree. */
static void
check_glue(
	const char *path)
{
	static const uint32_t bring_up[] = { 0, 8, 55, 41, 55, 41, 2, 3, 9, 7, 16, 55, 51, 55, 6, 6, 6 };
	static const uint32_t no_switch[] = { 0, 8, 55, 41, 55, 41, 2, 3, 9, 7, 16, 55, 51, 55, 6 };
	struct drv_dw_mmc_config config;
	struct drv_dw_mmc_card info;
	struct drv_fdt fdt;
	struct disk *disk;
	unsigned char *blob;
	FILE *file;
	long length;
	int error;

	file = fopen(path, "rb");
	if (file == NULL) {
		perror(path);
		exit(2);
	}
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	blob = malloc((size_t)length);
	if (blob == NULL || fread(blob, 1, (size_t)length, file) != (size_t)length) {
		perror(path);
		exit(2);
	}
	fclose(file);

	/* PLL2 1188 MHz, the AXI configuration bus / 3 = 396 MHz, the card clock / 2 with its gate closed. */
	put32(sys_syscon, 0x2c, 1U | (1U << 15) | (1U << 16) | (99U << 17));
	put32(sys_syscon, 0x30, 0U << 28);
	put32(sys_syscon, 0x34, 2U);
	put32(sys_syscon, 0x9c, 0x0aU);
	put32(sys_crg, 5 * 4, 1U << 24);
	put32(sys_crg, 7 * 4, 3U);
	put32(sys_crg, 8 * 4, 2U);
	put32(sys_crg, 9 * 4, 0x80000000U);
	put32(sys_crg, 94 * 4, 2U);
	put32(sys_crg, 0x2f8 + 8, 1U << 1);

	error = drv_fdt_open(&fdt, blob, (size_t)length);
	check(error == 0, "tree opens");
	error = drv_vf2_syscon_init(&fdt);
	check(error == 0, "system control blocks");
	error = drv_vf2_crg_init(&fdt);
	check(error == 0, "clock generators");
	error = drv_vf2_pinctrl_init(&fdt);
	check(error == 0, "pin controllers");

	/* No card: the detect pin reads high. */
	reset_model(1, 4, 0x290aU);
	put32(card.registers, R_UHS_EXT, 5U << 16);
	put32(sys_pinctrl, 0x11c, 1U << 9);
	error = drv_vf2_mmc_attach(&fdt, &disk);
	check(error == ENODEV && card.sent_count == 0, "glue: empty slot, ENODEV, no command");

	/* A card: the detect pin reads low. */
	put32(sys_pinctrl, 0x11c, 0U);
	error = drv_vf2_mmc_attach(&fdt, &disk);
	check(error == 0 && disk != NULL, "glue: card attaches");
	if (error != 0)
		return;
	check(strcmp(disk->d_name, "mmcblk0") == 0, "glue: the disk is mmcblk0");
	check((get32(sys_crg, 92 * 4) & 0x80000000U) != 0 && (get32(sys_crg, 94 * 4) & 0x80000000U) != 0,
	      "glue: SDIO1 AHB and card clock gates open");
	check((get32(sys_crg, 94 * 4) & 0xfU) == 8U, "glue: card clock divider 8 (396 MHz to 49.5 MHz)");
	check((get32(sys_crg, 0x2f8 + 8) & (1U << 1)) == 0, "glue: SDIO1 reset released");
	check(sequence_is(0, bring_up, sizeof(bring_up) / sizeof(bring_up[0])), "glue: bring-up sequence");
	error = drv_dw_mmc_card_info(0, &info);
	check(error == 0 && info.fifo_depth == 32U && info.bus_width == 4U && info.high_speed &&
	      info.card_clock == 24750000ULL,
	      "glue: fifo-depth 32, bus-width 4, cap-sd-highspeed from the tree, held to 24.75 MHz");
	check(card.divider_count == 3 && card.dividers[0] == 62U, "glue: identification at 399 kHz");

	/* A card whose CSD lacks the switch class stays at the default speed (unit 1, without the glue). */
	reset_model(1, 4, 0x290aU);
	set_bits(card.csd, 84, 12, 0x1b5U);
	memset(&config, 0, sizeof(config));
	config.registers = card.registers;
	config.clock_rate = 49500000U;
	config.fifo_depth = 32U;
	config.bus_width = 4U;
	config.high_speed = 1;
	config.unit = 1;
	error = drv_dw_mmc_attach(&config, &disk);
	check(error == 0 && sequence_is(0, no_switch, sizeof(no_switch) / sizeof(no_switch[0])),
	      "no switch class: four lines, no CMD6");
	error = drv_dw_mmc_card_info(1, &info);
	check(error == 0 && !info.high_speed && info.card_clock == 24750000ULL, "no switch class: 24.75 MHz default speed");
}

/* The glue's hook for the sample phase, as the JH7110 glue sets it. */
static void
set_phase(
	void *context,
	uint32_t phase)
{
	uint8_t *registers;
	uint32_t value;

	registers = context;
	value = kern_mmio_read32(registers + R_UHS_EXT);
	value &= ~(0x1fU << 16);
	value |= (phase & 0x1fU) << 16;
	kern_mmio_write32(registers + R_UHS_EXT, value);
}

/* Attaches a high-speed SDXC-shaped card with the phase hook, on a unit. */
static struct disk *
attach_phase_card(
	unsigned unit,
	int hook)
{
	struct drv_dw_mmc_config config;
	struct disk *disk;
	int error;

	reset_model(1, 4, 0x290aU);
	memset(&config, 0, sizeof(config));
	config.registers = card.registers;
	config.clock_rate = 49500000U;
	config.fifo_depth = 32U;
	config.bus_width = 4U;
	config.max_frequency = 100000000U;
	config.high_speed = 1;
	config.unit = unit;
	if (hook) {
		config.sample_phase = 0;
		config.sample_phase_count = 32U;
		config.set_sample_phase = set_phase;
		config.phase_context = card.registers;
	}
	error = drv_dw_mmc_attach(&config, &disk);
	if (error != 0)
		return NULL;
	return disk;
}

/*
 * BUG-340: data CRC errors at high speed tune the sample phase, and when
 * no phase reads, move the clock to the default speed.
 */
static void
check_phase(
	void)
{
	static uint8_t data[32U * 512U];
	struct drv_dw_mmc_card info;
	struct disk *disk;
	unsigned before;
	int error;

	/* Phases 10 to 20 read; the firmware's phase 0 does not. */
	disk = attach_phase_card(0, 1);
	check(disk != NULL, "phase: card attaches at high speed");
	if (disk == NULL)
		return;
	card.phase_window = 1;
	card.good_low = 10U;
	card.good_high = 20U;
	error = submit(disk, BIO_READ, 100, 1, data);
	check(error == 0 && memcmp(data, card.memory + 100U * 512U, 512) == 0,
	      "phase: a single block reads at phase 0");
	error = submit(disk, BIO_READ, 137, 32, data);
	check(error == 0 && memcmp(data, card.memory + 137U * 512U, sizeof(data)) == 0,
	      "phase: 32 blocks read after the tuning, data matches");
	check(((get32(card.registers, R_UHS_EXT) >> 16) & 0x1fU) == 15U, "phase: the middle of 10-20, 15, is set");
	error = drv_dw_mmc_card_info(0, &info);
	check(error == 0 && info.sample_phase == 15U && !info.slowed && info.card_clock == 49500000ULL,
	      "phase: card info phase 15, still 49.5 MHz");
	before = card.sent_count;
	error = submit(disk, BIO_READ, 500, 32, data);
	check(error == 0 && card.sent_count == before + 1U && memcmp(data, card.memory + 500U * 512U, sizeof(data)) == 0,
	      "phase: the next 32 blocks read at once");

	/* No phase reads at high speed: the clock goes down to 24.75 MHz. */
	disk = attach_phase_card(1, 1);
	check(disk != NULL, "slow: card attaches at high speed");
	if (disk == NULL)
		return;
	card.phase_window = 1;
	card.good_low = 1U;
	card.good_high = 0U;
	error = submit(disk, BIO_READ, 137, 32, data);
	check(error == 0 && memcmp(data, card.memory + 137U * 512U, sizeof(data)) == 0,
	      "slow: 32 blocks read at the default speed, data matches");
	check(((get32(card.registers, R_UHS_EXT) >> 16) & 0x1fU) == 0U, "slow: the phase goes back to 0");
	check(get32(card.registers, R_CLKDIV) == 1U, "slow: divider 1, 24.75 MHz");
	error = drv_dw_mmc_card_info(1, &info);
	check(error == 0 && info.slowed && info.card_clock == 24750000ULL && info.sample_phase == 0U,
	      "slow: card info slowed, 24.75 MHz");
}

/* Without a phase hook, data CRC errors at high speed move the clock down at once. */
static void
check_no_hook(
	void)
{
	static uint8_t data[32U * 512U];
	struct drv_dw_mmc_card info;
	struct disk *disk;
	unsigned before;
	int error;

	disk = attach_phase_card(0, 0);
	check(disk != NULL, "no hook: card attaches at high speed");
	if (disk == NULL)
		return;
	card.phase_window = 1;
	card.good_low = 10U;
	card.good_high = 20U;
	before = card.sent_count;
	error = submit(disk, BIO_READ, 137, 32, data);
	check(error == 0 && memcmp(data, card.memory + 137U * 512U, sizeof(data)) == 0,
	      "no hook: 32 blocks read at the default speed, data matches");
	check(sequence_is(before, (const uint32_t[]){ 18, 12, 13, 18 }, 4U), "no hook: stop, status, one more try");
	error = drv_dw_mmc_card_info(0, &info);
	check(error == 0 && info.slowed && info.card_clock == 24750000ULL, "no hook: 24.75 MHz");
}

int
main(
	int argc,
	char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "driver") == 0) {
		check_driver();
	} else if (argc >= 2 && strcmp(argv[1], "phase") == 0) {
		check_phase();
	} else if (argc >= 2 && strcmp(argv[1], "no-hook") == 0) {
		check_no_hook();
	} else if (argc >= 3 && strcmp(argv[1], "glue") == 0) {
		check_glue(argv[2]);
	} else {
		fprintf(stderr, "usage: %s driver | phase | no-hook | glue vf2-mmc.dtb\n", argv[0]);
		return 2;
	}

	check(fifo_preemptible == 0 && preempt_depth == 0, "the FIFO is served with preemption off, and it is put back on");
	printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
	return failures == 0 ? 0 : 1;
}
