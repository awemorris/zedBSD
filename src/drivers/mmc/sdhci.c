/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An SD Host Controller with an SD memory card, driven by programmed I/O.
 *
 * The driver moves data through the controller's buffer port and polls the
 * controller's interrupt status; it takes no interrupt and uses no DMA.  It
 * brings one SD memory card up from the idle state (identification at
 * 400 kHz, then the transfer clock and the four-line bus when the card takes
 * it) and publishes it as one disk.  A request is moved with one
 * multiple-block command (CMD18 or CMD25) per run of up to SD_RUN_BLOCKS
 * blocks, the controller ending each run with an automatic CMD12, as the SD
 * Host Controller Simplified Specification describes for a transfer without
 * DMA.  A run that fails is moved again one block at a time, each block
 * after the controller and the card were brought back to the transfer
 * state.
 *
 * Every register is reached with 32-bit accesses only, since some
 * controllers (the BCM2711's eMMC2) take no narrower ones.  The board's
 * glue maps the registers, switches the controller's clocks and pins on,
 * and hands the controller over (struct drv_sdhci_config).
 */

#include <drivers/mmc/sdhci.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/disk.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

/* Marks a parameter that the disk operations table passes but this driver does not need. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The controller's registers, as offsets from its base. */
#define REG_BLOCK_SIZE 0x04
#define REG_BLOCK_COUNT 0x06
#define REG_ARGUMENT 0x08
#define REG_TRANSFER_MODE 0x0c
#define REG_COMMAND 0x0e
#define REG_RESPONSE0 0x10
#define REG_BUFFER 0x20
#define REG_PRESENT 0x24
#define REG_HOST_CONTROL 0x28
#define REG_POWER_CONTROL 0x29
#define REG_CLOCK_CONTROL 0x2c
#define REG_TIMEOUT_CONTROL 0x2e
#define REG_SOFTWARE_RESET 0x2f
#define REG_INT_STATUS 0x30
#define REG_INT_ENABLE 0x34
#define REG_SIGNAL_ENABLE 0x38
#define REG_AUTO_CMD_ERROR 0x3c
#define REG_CAPABILITIES 0x40
#define REG_HOST_VERSION 0xfe

/* The bits of the present state, the interrupt status, and the command register. */
#define PRESENT_CMD_INHIBIT 0x00000001U
#define PRESENT_DATA_INHIBIT 0x00000002U
#define INT_CMD_COMPLETE 0x00000001U
#define INT_TRANSFER_COMPLETE 0x00000002U
#define INT_BUFFER_WRITE_READY 0x00000010U
#define INT_BUFFER_READ_READY 0x00000020U
#define INT_ERROR 0x00008000U
#define INT_ERROR_MASK 0xffff0000U
#define CMD_RESP_LONG 0x0001U
#define CMD_RESP_SHORT 0x0002U
#define CMD_RESP_BUSY 0x0003U
#define CMD_CRC 0x0008U
#define CMD_INDEX 0x0010U
#define CMD_DATA 0x0020U

/*
 * The bits of the transfer mode register: the block count register counts
 * the blocks of the run, the controller sends CMD12 itself after the last
 * one, the data comes from the card, and the command moves more than one
 * block.
 */
#define XFER_BLOCK_COUNT_ENABLE 0x0002U
#define XFER_AUTO_CMD12 0x0004U
#define XFER_READ 0x0010U
#define XFER_MULTI_BLOCK 0x0020U

/* The bits of the software reset register: the whole controller, or one line. */
#define SOFTWARE_RESET_ALL 0x01U
#define SOFTWARE_RESET_CMD 0x02U
#define SOFTWARE_RESET_DATA 0x04U

/* The bits of the clock control register: the internal clock, its stability, the card clock. */
#define CLOCK_INTERNAL_ENABLE 0x0001U
#define CLOCK_INTERNAL_STABLE 0x0002U
#define CLOCK_CARD_ENABLE 0x0004U

/* Capabilities register voltage support bits. */
#define CAPABILITY_VOLTAGE_3V3 (1U << 24)
#define CAPABILITY_VOLTAGE_3V0 (1U << 25)
#define CAPABILITY_VOLTAGE_1V8 (1U << 26)

/* The bus power voltages, and the longest data time-out the controller counts. */
#define POWER_ON_3V3 0x0fU
#define POWER_ON_3V0 0x0dU
#define POWER_ON_1V8 0x0bU
#define TIMEOUT_LONGEST 0x0eU

/* The bit of the host control register that drives four data lines. */
#define HOST_CONTROL_FOUR_BIT 0x02U

/*
 * The specification version the host version register's low byte reports
 * for version 3.00.  Earlier controllers hold the clock divisor in eight
 * bits as a power of two up to 128; version 3.00 holds it in ten.
 */
#define SPEC_VERSION_3 2U
#define SD_DIVISOR_MAX_V2 0x80U
#define SD_DIVISOR_MAX_V3 0x200U

/* The clock while the card is identified, and the default clock once it is. */
#define SD_IDENTIFY_CLOCK_HZ 400000U
#define SD_DEFAULT_CLOCK_HZ 25000000U

/* The base clock taken when neither the glue nor the capabilities name one. */
#define SD_FALLBACK_BASE_CLOCK_HZ 100000000U

/* The card's state in the R1 status that CMD13 answers with. */
#define CARD_STATE_SHIFT 9
#define CARD_STATE_MASK 0x0fU
#define CARD_STATE_TRANSFER 4U
#define CARD_STATE_DATA 5U
#define CARD_STATE_RECEIVE 6U
#define CARD_STATE_PROGRAM 7U
#define CARD_READY_FOR_DATA 0x00000100U

/* The bits of the operating conditions ACMD41 answers with: powered up, and high capacity. */
#define OCR_POWERED_UP 0x80000000U
#define OCR_HIGH_CAPACITY 0x40000000U

/* What ACMD41 asks for: high capacity and the 3.2 to 3.4 V window. */
#define OCR_REQUEST 0x40300000U

/* What CMD8 sends: the 2.7 to 3.6 V range and the check pattern. */
#define CMD8_ARGUMENT 0x1aaU

/* What ACMD6 sends for the four-line bus. */
#define ACMD6_FOUR_BIT 2U

/* The microseconds of one second, which the waits below are counted in. */
#define SD_MICROSECONDS_PER_SECOND 1000000ULL

/* How long a reset of the controller or of its lines may take. */
#define SD_RESET_SECONDS 1U

/* How long the internal clock may take to become stable. */
#define SD_CLOCK_SECONDS 1U

/* How long the command and data lines may stay inhibited. */
#define SD_INHIBIT_SECONDS 2U

/*
 * How long the bus power and then the identification clock are left to
 * settle before the card is spoken to.  The reset of the controller
 * switches the bus power off, and a slot whose card supply follows the bus
 * power powers the card up again only at the write that switches it on.
 *
 * Value referenced: Linux v6.12 drivers/mmc/core/core.c (mmc_power_up,
 * power_delay_ms, 10 ms after the power and again after the clock).
 */
#define SD_POWER_SETTLE_MICROSECONDS 10000U

/* How long the card may take to power up after the first ACMD41. */
#define SD_POWER_UP_SECONDS 2U

/*
 * The pause between two ACMD41s while the card powers up, which takes the
 * card tens of milliseconds at least; a card is not asked again at once.
 *
 * Value referenced: OpenBSD sys/dev/sdmmc/sdmmc_mem.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_mem_send_op_cond, 10 ms
 * between the tries).
 */
#define SD_POWER_UP_POLL_MICROSECONDS 10000U

/* How long a command, a data buffer and a read may take before it is abandoned. */
#define SD_COMMAND_SECONDS 2U

/*
 * How long a written block may keep the card busy.  A card that tidies its
 * flash in the background can hold the data line for well over the 250 to
 * 500 ms the specification names, and a block given up while the card is
 * still programming it is a block the next command collides with.
 */
#define SD_WRITE_BUSY_SECONDS 10U

/* How long the card has to come back to the transfer state after an error. */
#define SD_RECOVERY_SECONDS 10U

/* How many times one block is tried, with a recovery between the tries. */
#define SD_TRANSFER_ATTEMPTS 3U

/* How many stop commands one recovery sends to a card stuck in a transfer. */
#define SD_STOP_ATTEMPTS 3U

/* The pause between two status questions while the card is busy. */
#define SD_STATUS_POLL_MICROSECONDS 1000U

/* The bytes of one block, the only block length the driver uses. */
#define SD_BLOCK_BYTES 512U

/* The 32-bit words of one block, as the buffer port moves them. */
#define SD_BLOCK_WORDS (SD_BLOCK_BYTES / 4U)

/*
 * The most blocks one multiple-block command moves: 64 KiB, the largest
 * request the disk layer hands down (KERN_IO_BATCH_MAX), and well inside
 * the 16-bit block count register.
 */
#define SD_RUN_BLOCKS 128U

/*
 * The size a card is published with when its CSD gives none: a structure
 * version the driver does not know, or a size that decodes to nothing.
 */
#define SD_UNKNOWN_BLOCK_COUNT 0xffffffffULL

/*
 * How a long response (CMD2's, CMD9's) sits in the four response
 * registers: bits 127:8 of the card's 136-bit answer, the CRC and the end
 * bit dropped, as bits 119:0 of the four words.  A field at bit N of the
 * CSD is at bit N - 8 of the words.
 *
 * Values referenced: SD Host Controller Simplified Specification 4.20,
 * the Response register's mapping of R2; QEMU v10.0.11 hw/sd/sdhci.c
 * (sdhci_send_command, the 16-byte response).
 */
#define RESPONSE_WORDS 4U
#define RESPONSE_DROPPED_BITS 8U

/*
 * The fields of the CSD that give the card's size, as the CSD's own bit
 * positions and widths: the structure version (0 for a standard-capacity
 * card, 1 for a high-capacity one); for version 1 the read block length,
 * the device size and its multiplier; for version 2 the device size in
 * 512 KiB units.
 *
 * Values referenced: SD Physical Layer Simplified Specification 9.10,
 * the CSD register (CSD Version 1.0 and CSD Version 2.0 tables).
 */
#define CSD_STRUCTURE_BIT 126U
#define CSD_STRUCTURE_WIDTH 2U
#define CSD_STRUCTURE_VERSION_1 0U
#define CSD_STRUCTURE_VERSION_2 1U
#define CSD_V1_READ_BL_LEN_BIT 80U
#define CSD_V1_READ_BL_LEN_WIDTH 4U
#define CSD_V1_C_SIZE_BIT 62U
#define CSD_V1_C_SIZE_WIDTH 12U
#define CSD_V1_C_SIZE_MULT_BIT 47U
#define CSD_V1_C_SIZE_MULT_WIDTH 3U
#define CSD_V2_C_SIZE_BIT 48U
#define CSD_V2_C_SIZE_WIDTH 22U

/* The 512-byte blocks of one unit of a version 2 device size, 512 KiB. */
#define CSD_V2_BLOCKS_PER_UNIT 1024ULL

/*
 * One controller the driver serves, and the card in its slot.
 *
 * A unit is claimed by a successful drv_sdhci_attach() and never released;
 * the disk it publishes points back at it through d_data.  A failed attach
 * leaves the unit free, so the glue may try the same unit again.
 */
struct sdhci_unit {
	/* The controller's registers, and its bus address for the boot log. */
	volatile uint8_t *base;
	uint64_t physical_base;

	/* The glue's base clock (zero: from the capabilities) and the transfer clock. */
	uint64_t base_clock_hz;
	uint64_t max_clock_hz;

	/* The glue's quirks, and the specification version the controller reports. */
	unsigned quirks;
	unsigned spec_version;

	/* The unit's number, which names its disk. */
	unsigned number;

	/* Nonzero once the attach published the disk; the unit is then taken. */
	int attached;

	/* What the attach found; card.rca is the card's address. */
	struct drv_sdhci_card card;

	/* The published disk, or NULL before the attach finished. */
	struct disk *disk;

	/*
	 * The halves of the register pairs written as one word (see
	 * write_register16): the transfer mode until the command, the block
	 * size until the count.
	 */
	uint16_t transfer_mode;
	uint16_t block_size;

	/* One command at a time: the controller has one set of registers. */
	struct mutex lock;

	/*
	 * Set once a failed transfer could neither be recovered nor the card
	 * initialized again.  Every later transfer fails at once instead of
	 * waiting out its deadlines, so a sync or a shutdown that meets a dead
	 * card ends with an error rather than hanging.  Under the lock.
	 */
	int failed;

	/*
	 * The interrupt status the last failed wait saw, an error or a
	 * time-out.  It is what a failure report shows, since the wait clears
	 * the register.  Under the lock once the disk is published.
	 */
	uint32_t last_error_status;
};

/*
 * The controllers the driver serves, indexed by the glue's unit number.
 *
 * A slot is filled by drv_sdhci_attach() and kept for the kernel's life.
 */
static struct sdhci_unit sdhci_units[DRV_SDHCI_UNIT_MAX];

static uint32_t read_register32(const struct sdhci_unit *unit, unsigned offset);
static uint16_t read_register16(const struct sdhci_unit *unit, unsigned offset);
static uint8_t read_register8(const struct sdhci_unit *unit, unsigned offset);
static void write_register32(struct sdhci_unit *unit, unsigned offset, uint32_t value);
static void write_register16(struct sdhci_unit *unit, unsigned offset, uint16_t value);
static void write_register8(struct sdhci_unit *unit, unsigned offset, uint8_t value);
static int start_deadline(uint64_t microseconds, uint64_t *deadline);
static int deadline_passed(uint64_t deadline);
static void pause_microseconds(unsigned microseconds);
static void report_card_detect(const struct drv_sdhci_config *config);
static int controller_init(struct sdhci_unit *unit);
static int set_clock(struct sdhci_unit *unit, uint64_t target);
static void read_long_response(const struct sdhci_unit *unit, uint32_t response[RESPONSE_WORDS]);
static uint32_t csd_field(const uint32_t response[RESPONSE_WORDS], unsigned bit, unsigned width);
static uint64_t csd_block_count(const uint32_t response[RESPONSE_WORDS]);
static int command(struct sdhci_unit *unit, uint32_t index, uint32_t argument, uint16_t flags, uint32_t *response);
static int app_command(struct sdhci_unit *unit, uint32_t command_index, uint32_t argument, uint16_t flags, uint32_t *response);
static int wait_bits(struct sdhci_unit *unit, unsigned reg, uint32_t mask, uint32_t expected);
static int wait_interrupt(struct sdhci_unit *unit, uint32_t wanted, unsigned seconds);
static int transfer_blocks(struct sdhci_unit *unit, int write, uint64_t block, uint32_t count, void *buffer);
static int transfer_block_recovering(struct sdhci_unit *unit, int write, uint64_t block, void *buffer);
static int transfer_run_recovering(struct sdhci_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int restore_card(struct sdhci_unit *unit);
static int recover_card(struct sdhci_unit *unit);
static int reset_lines(struct sdhci_unit *unit, uint8_t lines);
static int stop_transmission(struct sdhci_unit *unit);
static int publish_disk(struct sdhci_unit *unit);
static int sd_submit(struct disk *disk, struct bio *bio);
static int sd_ioctl(struct disk *disk, unsigned long request, void *argument);

/* The operations of an SD card's disk. */
static const struct disk_ops sd_ops = {
	.submit = sd_submit,
	.ioctl = sd_ioctl
};

/*
 * Brings up the card in a controller's slot and publishes it as a disk.
 *
 * Returns 0 with the disk, EBUSY when the unit number is outside the table
 * or already in use, or the errno of the step that failed; a failed attach
 * leaves no disk and the unit free for another try.
 */
int
drv_sdhci_attach(
	const struct drv_sdhci_config *config,
	struct disk **disk)
{
	struct sdhci_unit *unit;
	uint32_t capabilities;
	uint32_t present;
	uint32_t status;
	uint16_t version;
	int error;

	/* Refuses a description without registers, or nowhere to put the disk. */
	if (config == NULL || disk == NULL)
		return EINVAL;
	if (config->registers == NULL)
		return EINVAL;

	/* Refuses a unit number outside the table. */
	if (config->unit >= DRV_SDHCI_UNIT_MAX)
		return EBUSY;

	/* Claims the unit the glue numbered; one that published a disk is taken. */
	unit = &sdhci_units[config->unit];
	if (unit->attached)
		return EBUSY;

	/* Starts without a disk, which every failure below leaves. */
	*disk = NULL;

	/* Takes over the glue's description and clears what an earlier failed attach left. */
	unit->base = config->registers;
	unit->physical_base = config->physical_base;
	unit->base_clock_hz = config->base_clock_hz;
	unit->max_clock_hz = config->max_clock_hz;
	unit->quirks = config->quirks;
	unit->number = config->unit;
	unit->disk = NULL;
	unit->transfer_mode = 0;
	unit->block_size = 0;
	unit->failed = 0;
	unit->last_error_status = 0;

	/* A glue that names no transfer clock gets the default speed's. */
	if (unit->max_clock_hz == 0)
		unit->max_clock_hz = SD_DEFAULT_CLOCK_HZ;

	/* Writes what the slot's card detect says to the boot log. */
	report_card_detect(config);

	/* Reads what the controller advertises. */
	capabilities = read_register32(unit, REG_CAPABILITIES);
	present = read_register32(unit, REG_PRESENT);
	version = read_register16(unit, REG_HOST_VERSION);

	/* Keeps the capabilities for the card's description and the version for the clock divisor. */
	unit->card.capabilities = capabilities;
	unit->card.rca = 0;
	unit->card.high_capacity = 0;
	unit->card.block_count = SD_UNKNOWN_BLOCK_COUNT;
	unit->card.bus_width = 1U;
	unit->card.card_clock = 0;
	unit->spec_version = (unsigned)(version & 0xffU);

	/* Writes the controller's address and what it advertises to the boot log. */
	kern_logf("sdhci: base=%p caps=%x present=%x version=%x\n",
		  (void *)(uintptr_t)unit->physical_base,
		  capabilities,
		  present,
		  (unsigned)version);

	/* Resets the controller and brings the card to the transfer state. */
	error = controller_init(unit);
	if (error != 0) {
		status = read_register32(unit, REG_INT_STATUS);
		present = read_register32(unit, REG_PRESENT);
		kern_logf("sdhci: init error=%u status=%x present=%x\n",
			  (unsigned)error,
			  status,
			  present);

		/* Failed: the card did not come up. */
		return error;
	}

	/* Publishes the card as a disk. */
	error = publish_disk(unit);
	if (error != 0)
		return error;

	/* Writes how the card is addressed to the boot log. */
	if (unit->card.high_capacity) {
		kern_logf("sdhci: %s ready (block addressing)\n", unit->disk->d_name);
	} else {
		kern_logf("sdhci: %s ready (byte addressing)\n", unit->disk->d_name);
	}

	/* Succeeded: the unit is taken and the card is a disk. */
	unit->attached = 1;
	*disk = unit->disk;
	return 0;
}

/*
 * Copies what the attach of a unit found in its slot.
 *
 * Returns 0, or ENODEV for a unit without a published card.
 */
int
drv_sdhci_card_info(
	unsigned unit_number,
	struct drv_sdhci_card *card)
{
	const struct sdhci_unit *unit;

	/* Refuses nowhere to put the description. */
	if (card == NULL)
		return EINVAL;

	/* Refuses a unit outside the table. */
	if (unit_number >= DRV_SDHCI_UNIT_MAX)
		return ENODEV;

	/* Refuses a unit without a published card. */
	unit = &sdhci_units[unit_number];
	if (!unit->attached)
		return ENODEV;

	/* Succeeded: the card's description. */
	*card = unit->card;
	return 0;
}

/* Reads one 32-bit register of the controller. */
static uint32_t
read_register32(
	const struct sdhci_unit *unit,
	unsigned offset)
{
	uint32_t value;

	/* Reads the word at the offset. */
	value = kern_mmio_read32(unit->base + offset);

	/* Reports the register's contents. */
	return value;
}

/* Reads a 16-bit register out of the word that holds it. */
static uint16_t
read_register16(
	const struct sdhci_unit *unit,
	unsigned offset)
{
	uint32_t word;
	unsigned shift;

	/* Reads the word, and finds the half of it at the offset. */
	word = read_register32(unit, offset & ~3U);
	shift = (offset & 2U) * 8U;

	/* Reports the half. */
	return (uint16_t)(word >> shift);
}

/* Reads an 8-bit register out of the word that holds it. */
static uint8_t
read_register8(
	const struct sdhci_unit *unit,
	unsigned offset)
{
	uint32_t word;
	unsigned shift;

	/* Reads the word, and finds the byte of it at the offset. */
	word = read_register32(unit, offset & ~3U);
	shift = (offset & 3U) * 8U;

	/* Reports the byte. */
	return (uint8_t)(word >> shift);
}

/* Writes one 32-bit register of the controller. */
static void
write_register32(
	struct sdhci_unit *unit,
	unsigned offset,
	uint32_t value)
{
	/* Writes the word at the offset. */
	kern_mmio_write32(unit->base + offset, value);
}

/*
 * Writes a 16-bit register with 32-bit accesses only.
 *
 * Two pairs cannot be written half at a time: writing the command starts
 * it, so the transfer mode must go in the same write, and the block size is
 * written with the block count.  The first half of each pair is kept until
 * the second is written.  Any other register is a read-modify-write of its
 * word.
 */
static void
write_register16(
	struct sdhci_unit *unit,
	unsigned offset,
	uint16_t value)
{
	uint32_t word;
	unsigned shift;

	/* The transfer mode waits for the command. */
	if (offset == REG_TRANSFER_MODE) {
		unit->transfer_mode = value;
		return;
	}

	/* The block size waits for the block count. */
	if (offset == REG_BLOCK_SIZE) {
		unit->block_size = value;
		return;
	}

	/* The command goes out with the transfer mode, which is then used up. */
	if (offset == REG_COMMAND) {
		word = ((uint32_t)value << 16) | unit->transfer_mode;
		write_register32(unit, REG_TRANSFER_MODE, word);
		unit->transfer_mode = 0;
		return;
	}

	/* The block count goes out with the block size. */
	if (offset == REG_BLOCK_COUNT) {
		word = ((uint32_t)value << 16) | unit->block_size;
		write_register32(unit, REG_BLOCK_SIZE, word);
		return;
	}

	/* Replaces the half at the offset; the other half of the word keeps what it holds. */
	word = read_register32(unit, offset & ~3U);
	shift = (offset & 2U) * 8U;
	word &= ~(0xffffU << shift);
	word |= (uint32_t)value << shift;
	write_register32(unit, offset & ~3U, word);
}

/* Writes an 8-bit register as a read-modify-write of the word that holds it. */
static void
write_register8(
	struct sdhci_unit *unit,
	unsigned offset,
	uint8_t value)
{
	uint32_t word;
	unsigned shift;

	/* Replaces the byte at the offset; the other bytes of the word keep what they hold. */
	word = read_register32(unit, offset & ~3U);
	shift = (offset & 3U) * 8U;
	word &= ~(0xffU << shift);
	word |= (uint32_t)value << shift;
	write_register32(unit, offset & ~3U, word);
}

/* Computes the counter value a wait of the given length ends at. */
static int
start_deadline(
	uint64_t microseconds,
	uint64_t *deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* Reads the monotonic counter; without one no wait can end. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted)
		return EIO;

	/* Succeeded: the counter value the wait ends at. */
	*deadline = now + frequency * microseconds / SD_MICROSECONDS_PER_SECOND;
	return 0;
}

/* Reports whether the monotonic counter has passed a deadline. */
static int
deadline_passed(
	uint64_t deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* A counter that cannot be read ends the wait rather than spinning forever. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted)
		return 1;

	/* The deadline has passed once the counter reaches it. */
	if (now >= deadline)
		return 1;

	/* The wait goes on. */
	return 0;
}

/* Spins for at least the given number of microseconds. */
static void
pause_microseconds(
	unsigned microseconds)
{
	uint64_t deadline;
	int error;
	int passed;

	/* Sets the end of the pause; without a counter the pause is skipped. */
	error = start_deadline(microseconds, &deadline);
	if (error != 0)
		return;

	/* Spins until the counter passes the end of the pause. */
	for (;;) {
		passed = deadline_passed(deadline);
		if (passed)
			break;
	}
}

/*
 * Asks the glue's card detect whether a card is in the slot, for the log.
 *
 * The card is initialized whatever the answer: a detect pin that is wired
 * differently from what the glue expects must not hide a card.
 */
static void
report_card_detect(
	const struct drv_sdhci_config *config)
{
	int present;
	int error;

	/* A slot without a card detect has nothing to report. */
	if (config->card_present == NULL)
		return;

	/* Asks the glue. */
	present = 1;
	error = config->card_present(config->context, &present);
	if (error != 0) {
		kern_logf("sdhci: card detect failed (%d)\n", error);
		return;
	}

	/* Reports an empty slot; the initialization below shows whether it is. */
	if (present == 0)
		kern_logf("sdhci: card detect says absent\n");
}

/*
 * Resets the controller and brings the card from the idle state to the
 * transfer state.
 *
 * The card is identified at 400 kHz (CMD0, CMD8, ACMD41, CMD2, CMD3), its
 * CSD is read for its size (CMD9), it is selected (CMD7), an older card is
 * told the block length (CMD16), and the clock goes up to the transfer
 * clock.  The four-line bus is asked for last (ACMD6); a card that refuses
 * it stays on one line.
 */
static int
controller_init(
	struct sdhci_unit *unit)
{
	uint32_t csd[RESPONSE_WORDS];
	uint64_t deadline;
	uint64_t blocks;
	uint32_t structure;
	uint32_t response;
	uint8_t host_control;
	uint8_t pending;
	int passed;
	int error;

	/* Starts the reset of the whole controller. */
	write_register8(unit, REG_SOFTWARE_RESET, SOFTWARE_RESET_ALL);

	/* Sets how long the controller has to finish the reset. */
	error = start_deadline(SD_RESET_SECONDS * SD_MICROSECONDS_PER_SECOND, &deadline);
	if (error != 0)
		return error;

	/* Waits until the controller clears the reset bit. */
	for (;;) {
		/* The reset is done once its bit reads back clear. */
		pending = (uint8_t)(read_register8(unit, REG_SOFTWARE_RESET) & SOFTWARE_RESET_ALL);
		if (pending == 0)
			break;

		/* A reset that never ends is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Forces card detection if requested (e.g. ACPI platforms with GPIO CD). */
	if ((unit->quirks & DRV_SDHCI_QUIRK_FORCE_CD) != 0) {
		write_register8(unit, REG_HOST_CONTROL,
				(uint8_t)(read_register8(unit, REG_HOST_CONTROL) | 0xc0U));
	}

	/* Powers the bus, sets the longest data time-out, and polls every interrupt without signalling it. */
	uint8_t power = POWER_ON_3V3;
	if ((unit->card.capabilities & CAPABILITY_VOLTAGE_3V3) != 0) {
		power = POWER_ON_3V3;
	} else if ((unit->card.capabilities & CAPABILITY_VOLTAGE_3V0) != 0) {
		power = POWER_ON_3V0;
	} else if ((unit->card.capabilities & CAPABILITY_VOLTAGE_1V8) != 0) {
		power = POWER_ON_1V8;
	}
	write_register8(unit, REG_POWER_CONTROL, power);
	write_register8(unit, REG_TIMEOUT_CONTROL, TIMEOUT_LONGEST);
	write_register32(unit, REG_INT_ENABLE, 0xffffffffU);
	write_register32(unit, REG_SIGNAL_ENABLE, 0);

	/* Lets the card's supply settle. */
	pause_microseconds(SD_POWER_SETTLE_MICROSECONDS);

	/* Starts the card clock at the identification rate. */
	error = set_clock(unit, SD_IDENTIFY_CLOCK_HZ);
	if (error != 0) {
		kern_logf("sdhci: clock identification %u\n", (unsigned)error);

		/* Failed: the clock did not become stable. */
		return error;
	}

	/* Gives the card the clocks it counts before its first command. */
	pause_microseconds(SD_POWER_SETTLE_MICROSECONDS);

	/* Puts the card in the idle state. */
	error = command(unit, 0, 0, 0, NULL);
	if (error != 0) {
		kern_logf("sdhci: CMD0 %u\n", (unsigned)error);

		/* Failed: the controller did not send the command. */
		return error;
	}

	/* Tells the card the voltage; an older card that does not answer is still tried. */
	response = 0;
	error = command(unit, 8, CMD8_ARGUMENT, CMD_RESP_SHORT | CMD_CRC | CMD_INDEX, &response);
	if (error != 0) {
		kern_logf("sdhci: CMD8 error=%u irq=%x\n",
			  (unsigned)error,
			  unit->last_error_status);
	} else {
		kern_logf("sdhci: CMD8 response=%x\n", response);
	}

	/* Sets how long the card has to power up; CMD55 goes to address 0 until the card has one. */
	unit->card.rca = 0;
	error = start_deadline(SD_POWER_UP_SECONDS * SD_MICROSECONDS_PER_SECOND, &deadline);
	if (error != 0)
		return error;

	/* Asks the card to power up until it says it has, or the deadline passes. */
	for (;;) {
		/* A card that answers with its power-up bit set is ready. */
		error = app_command(unit, 41, OCR_REQUEST, CMD_RESP_SHORT, &response);
		if (error == 0 && (response & OCR_POWERED_UP) != 0)
			break;

		/* A card that stays busy past the deadline is given up on below. */
		passed = deadline_passed(deadline);
		if (passed)
			break;

		/* Gives the card time to power up before it is asked again. */
		pause_microseconds(SD_POWER_UP_POLL_MICROSECONDS);
	}

	/* Refuses a card that never answered or never powered up. */
	if (error != 0 || (response & OCR_POWERED_UP) == 0) {
		kern_logf("sdhci: ACMD41 error=%u irq=%x ocr=%x\n",
			  (unsigned)error,
			  unit->last_error_status,
			  response);

		/* Failed: the card did not power up in time. */
		return ETIMEDOUT;
	}

	/* A high-capacity card is addressed in blocks, an older one in bytes. */
	unit->card.high_capacity = 0;
	if ((response & OCR_HIGH_CAPACITY) != 0)
		unit->card.high_capacity = 1;

	/* Asks the card for its identity, which moves it to the identification state. */
	error = command(unit, 2, 0, CMD_RESP_LONG | CMD_CRC, NULL);
	if (error != 0) {
		kern_logf("sdhci: CMD2 %u\n", (unsigned)error);

		/* Failed: the card did not identify itself. */
		return error;
	}

	/* Asks the card for its address on the bus. */
	error = command(unit, 3, 0, CMD_RESP_SHORT | CMD_CRC | CMD_INDEX, &response);
	if (error != 0) {
		kern_logf("sdhci: CMD3 %u\n", (unsigned)error);

		/* Failed: the card published no address. */
		return error;
	}

	/* Refuses the zero address, which no card may take. */
	unit->card.rca = response >> 16;
	if (unit->card.rca == 0)
		return EIO;

	/* Reads the card's CSD. */
	error = command(unit, 9, unit->card.rca << 16, CMD_RESP_LONG | CMD_CRC, NULL);
	if (error != 0) {
		kern_logf("sdhci: CMD9 %u\n", (unsigned)error);

		/* Failed: the card did not send its CSD. */
		return error;
	}

	/*
	 * Takes the card's size from the CSD, which the response registers
	 * hold until the next command.  A CSD that gives no size leaves the
	 * size unknown, as a wrong size would cut the partitions short.
	 */
	read_long_response(unit, csd);
	blocks = csd_block_count(csd);
	unit->card.block_count = SD_UNKNOWN_BLOCK_COUNT;
	if (blocks != 0)
		unit->card.block_count = blocks;

	/* Writes the CSD's version and the size taken from it to the boot log. */
	structure = csd_field(csd, CSD_STRUCTURE_BIT, CSD_STRUCTURE_WIDTH);
	kern_logf("sdhci: csd structure=%u blocks=%llu\n",
		  (unsigned)structure,
		  (unsigned long long)unit->card.block_count);

	/* Selects the card, which moves it to the transfer state. */
	error = command(unit, 7, unit->card.rca << 16, CMD_RESP_BUSY | CMD_CRC | CMD_INDEX, NULL);
	if (error != 0) {
		kern_logf("sdhci: CMD7 %u\n", (unsigned)error);

		/* Failed: the card was not selected. */
		return error;
	}

	/* Tells an older card the block length; a high-capacity card's is fixed. */
	if (!unit->card.high_capacity) {
		error = command(unit, 16, SD_BLOCK_BYTES, CMD_RESP_SHORT | CMD_CRC | CMD_INDEX, NULL);
		if (error != 0)
			return error;
	}

	/* Raises the card clock to the transfer rate. */
	error = set_clock(unit, unit->max_clock_hz);
	if (error != 0) {
		kern_logf("sdhci: clock transfer %u\n", (unsigned)error);

		/* Failed: the clock did not become stable. */
		return error;
	}

	/* Asks for the four-line bus; a card that refuses it stays on one line. */
	unit->card.bus_width = 1U;
	error = app_command(unit, 6, ACMD6_FOUR_BIT, CMD_RESP_SHORT | CMD_CRC | CMD_INDEX, NULL);
	if (error == 0) {
		host_control = read_register8(unit, REG_HOST_CONTROL);
		host_control |= HOST_CONTROL_FOUR_BIT;
		write_register8(unit, REG_HOST_CONTROL, host_control);
		unit->card.bus_width = 4U;
	}

	/* Succeeded: the card is in the transfer state. */
	return 0;
}

/*
 * Sets the card clock to the fastest rate at or below the target.
 *
 * The divisor is a power of two that halves the base clock once more each
 * step; a controller older than version 3.00 holds it in eight bits, up to
 * 128.  The internal clock is started, waited for until stable, and only
 * then given to the card.
 */
static int
set_clock(
	struct sdhci_unit *unit,
	uint64_t target)
{
	uint64_t deadline;
	uint64_t base;
	uint32_t capabilities;
	uint32_t divisor;
	uint32_t divisor_max;
	uint32_t encoded;
	uint16_t clock;
	int passed;
	int error;

	/* Takes the base clock the capabilities advertise, in MHz, or the fallback when they say none. */
	capabilities = read_register32(unit, REG_CAPABILITIES);
	base = (uint64_t)((capabilities >> 8) & 0xffU) * 1000000U;
	if (base == 0)
		base = SD_FALLBACK_BASE_CLOCK_HZ;

	/* A base clock the glue knows wins over what the capabilities say. */
	if (unit->base_clock_hz != 0)
		base = unit->base_clock_hz;

	/* Picks the largest divisor the controller's version holds. */
	if (unit->spec_version >= SPEC_VERSION_3) {
		divisor_max = SD_DIVISOR_MAX_V3;
	} else {
		divisor_max = SD_DIVISOR_MAX_V2;
	}

	/* Doubles the divisor until the card clock is at or below the target. */
	divisor = 1;
	while (divisor < divisor_max && base / (2U * divisor) > target)
		divisor <<= 1;

	/* Encodes the divisor: its low eight bits at 15:8, its upper two at 7:6. */
	encoded = (divisor & 0xffU) << 8;
	encoded |= (divisor & 0x300U) >> 2;

	/* Stops the clock, then starts the internal clock at the new rate. */
	write_register16(unit, REG_CLOCK_CONTROL, 0);
	write_register16(unit, REG_CLOCK_CONTROL, (uint16_t)(encoded | CLOCK_INTERNAL_ENABLE));

	/* Sets how long the internal clock has to settle. */
	error = start_deadline(SD_CLOCK_SECONDS * SD_MICROSECONDS_PER_SECOND, &deadline);
	if (error != 0)
		return error;

	/* Waits until the internal clock is stable. */
	for (;;) {
		/* The clock is usable once the controller says it is stable. */
		clock = read_register16(unit, REG_CLOCK_CONTROL);
		if ((clock & CLOCK_INTERNAL_STABLE) != 0)
			break;

		/* A clock that never settles is given up on below. */
		passed = deadline_passed(deadline);
		if (passed)
			break;
	}

	/* Refuses a clock that did not become stable. */
	if ((clock & CLOCK_INTERNAL_STABLE) == 0)
		return ETIMEDOUT;

	/* Gives the clock to the card. */
	write_register16(unit, REG_CLOCK_CONTROL, (uint16_t)(clock | CLOCK_CARD_ENABLE));

	/* Succeeded: the card runs at the divided clock. */
	unit->card.card_clock = base / (2U * divisor);
	return 0;
}

/* Reads the four response registers that hold a long response. */
static void
read_long_response(
	const struct sdhci_unit *unit,
	uint32_t response[RESPONSE_WORDS])
{
	unsigned index;

	/* Reads the words from the lowest bits of the answer up. */
	for (index = 0; index < RESPONSE_WORDS; index++)
		response[index] = read_register32(unit, REG_RESPONSE0 + index * 4U);
}

/*
 * Extracts one field of the CSD from a long response.
 *
 * The bit is the field's lowest bit as the CSD numbers it; the field may
 * span two of the response's words.
 */
static uint32_t
csd_field(
	const uint32_t response[RESPONSE_WORDS],
	unsigned bit,
	unsigned width)
{
	unsigned position;
	unsigned word;
	unsigned shift;
	uint32_t value;
	uint32_t mask;

	/* Finds the field in the words, which lack the CSD's lowest eight bits. */
	position = bit - RESPONSE_DROPPED_BITS;
	word = position / 32U;
	shift = position % 32U;

	/* Takes the field's low part from its word, and the rest from the next one up. */
	value = response[word] >> shift;
	if (shift + width > 32U && word + 1U < RESPONSE_WORDS)
		value |= response[word + 1U] << (32U - shift);

	/* Succeeded: the field, without the bits above it. */
	mask = (1U << width) - 1U;
	return value & mask;
}

/*
 * Computes the card's size in 512-byte blocks from its CSD.
 *
 * A version 1 CSD gives the size as (C_SIZE + 1) * 2^(C_SIZE_MULT + 2)
 * blocks of 2^READ_BL_LEN bytes, a version 2 CSD as (C_SIZE + 1) units of
 * 512 KiB.  Returns 0 for a CSD of any other version, or one whose size
 * is less than a block.
 *
 * Procedure referenced: SD Physical Layer Simplified Specification 9.10,
 * 5.3.2 (C_SIZE, C_SIZE_MULT, READ_BL_LEN) and 5.3.3 (C_SIZE).
 */
static uint64_t
csd_block_count(
	const uint32_t response[RESPONSE_WORDS])
{
	uint64_t bytes;
	uint64_t blocks;
	uint32_t structure;
	uint32_t size;
	uint32_t multiplier;
	uint32_t block_length;

	/* Reads which layout the CSD has. */
	structure = csd_field(response, CSD_STRUCTURE_BIT, CSD_STRUCTURE_WIDTH);

	/* A high-capacity card counts its size in 512 KiB units. */
	if (structure == CSD_STRUCTURE_VERSION_2) {
		size = csd_field(response, CSD_V2_C_SIZE_BIT, CSD_V2_C_SIZE_WIDTH);
		blocks = ((uint64_t)size + 1U) * CSD_V2_BLOCKS_PER_UNIT;

		/* Succeeded: the high-capacity card's size. */
		return blocks;
	}

	/* Refuses a layout the driver does not know, such as an ultra-capacity card's. */
	if (structure != CSD_STRUCTURE_VERSION_1)
		return 0;

	/* A standard-capacity card multiplies its device size by two powers of two. */
	size = csd_field(response, CSD_V1_C_SIZE_BIT, CSD_V1_C_SIZE_WIDTH);
	multiplier = csd_field(response, CSD_V1_C_SIZE_MULT_BIT, CSD_V1_C_SIZE_MULT_WIDTH);
	block_length = csd_field(response, CSD_V1_READ_BL_LEN_BIT, CSD_V1_READ_BL_LEN_WIDTH);
	bytes = ((uint64_t)size + 1U) << (multiplier + 2U);
	bytes <<= block_length;
	blocks = bytes / SD_BLOCK_BYTES;

	/* Succeeded: the standard-capacity card's size, zero when less than a block. */
	return blocks;
}

/*
 * Sends one command and waits for its completion.
 *
 * The response's first word goes to response when the caller asks for it.
 */
static int
command(
	struct sdhci_unit *unit,
	uint32_t index,
	uint32_t argument,
	uint16_t flags,
	uint32_t *response)
{
	uint32_t inhibit;
	int error;

	/* A command with data waits for the data line as well as the command line. */
	inhibit = PRESENT_CMD_INHIBIT;
	if ((flags & CMD_DATA) != 0)
		inhibit |= PRESENT_DATA_INHIBIT;

	/* Waits until the controller takes a command. */
	error = wait_bits(unit, REG_PRESENT, inhibit, 0);
	if (error != 0)
		return error;

	/* Clears the old status, and sends the command with its argument. */
	write_register32(unit, REG_INT_STATUS, 0xffffffffU);
	write_register32(unit, REG_ARGUMENT, argument);
	write_register16(unit, REG_COMMAND, (uint16_t)((index << 8) | flags));

	/*
	 * Waits for the command to complete.  A command that failed leaves the
	 * command line inhibited until the line is reset, so it is reset here
	 * for the next command; the failure is what the caller is told.
	 */
	error = wait_interrupt(unit, INT_CMD_COMPLETE, SD_COMMAND_SECONDS);
	if (error != 0) {
		(void)reset_lines(unit, SOFTWARE_RESET_CMD);
		return error;
	}

	/* Hands the response to a caller that wants it. */
	if (response != NULL)
		*response = read_register32(unit, REG_RESPONSE0);

	/* Succeeded: the card answered. */
	return 0;
}

/* Sends an application command: CMD55 with the card's address, then the command. */
static int
app_command(
	struct sdhci_unit *unit,
	uint32_t command_index,
	uint32_t argument,
	uint16_t flags,
	uint32_t *response)
{
	int error;

	/* Tells the card that an application command follows. */
	error = command(unit, 55, unit->card.rca << 16, CMD_RESP_SHORT | CMD_CRC | CMD_INDEX, NULL);
	if (error != 0)
		return error;

	/* Sends the application command itself. */
	error = command(unit, command_index, argument, flags, response);
	if (error != 0)
		return error;

	/* Succeeded: the card answered the application command. */
	return 0;
}

/* Waits until the masked bits of a register read as expected. */
static int
wait_bits(
	struct sdhci_unit *unit,
	unsigned reg,
	uint32_t mask,
	uint32_t expected)
{
	uint64_t deadline;
	uint32_t bits;
	int passed;
	int error;

	/* Sets how long the bits have to change. */
	error = start_deadline(SD_INHIBIT_SECONDS * SD_MICROSECONDS_PER_SECOND, &deadline);
	if (error != 0)
		return error;

	/* Polls the register until the bits read as expected, or the deadline. */
	for (;;) {
		/* The wait ends once the masked bits read as expected. */
		bits = read_register32(unit, reg) & mask;
		if (bits == expected)
			break;

		/* Bits that never change are given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the bits read as expected. */
	return 0;
}

/*
 * Waits for the interrupt status to show the wanted event.
 *
 * An error bit ends the wait at once.  Whatever ends the wait other than
 * the wanted event is kept in last_error_status for the failure report,
 * since the status register is cleared here or by the recovery.
 */
static int
wait_interrupt(
	struct sdhci_unit *unit,
	uint32_t wanted,
	unsigned seconds)
{
	uint64_t deadline;
	uint32_t status;
	int passed;
	int error;

	/* Sets the deadline the controller has to answer by. */
	error = start_deadline((uint64_t)seconds * SD_MICROSECONDS_PER_SECOND, &deadline);
	if (error != 0)
		return error;

	/* Polls the status until the event, an error, or the deadline. */
	for (;;) {
		/* An error bit ends the wait, and is acknowledged. */
		status = read_register32(unit, REG_INT_STATUS);
		if ((status & (INT_ERROR | INT_ERROR_MASK)) != 0) {
			unit->last_error_status = status;
			write_register32(unit, REG_INT_STATUS, status);

			/* Failed: the controller reported an error. */
			return EIO;
		}

		/* The wanted event is acknowledged and ends the wait. */
		if ((status & wanted) != 0) {
			write_register32(unit, REG_INT_STATUS, wanted);

			/* Succeeded. */
			return 0;
		}

		/* A controller that never answers is given up on at the deadline. */
		passed = deadline_passed(deadline);
		if (passed) {
			unit->last_error_status = status;
			return ETIMEDOUT;
		}
	}
}

/*
 * Moves a run of 512-byte blocks between the card and the buffer by PIO.
 *
 * One block is moved with CMD17 or CMD24.  More are moved with CMD18 or
 * CMD25: the block count register holds the length of the run, and the
 * controller sends CMD12 by itself once the last block has crossed the
 * bus.  Each block goes through the buffer port once the controller raises
 * buffer read ready or buffer write ready for it.  The run ends with one
 * wait for the transfer-complete interrupt, which the controller raises
 * only when the card has released the data line after programming the
 * last block (and, for a run, after the automatic CMD12).  Between the
 * blocks of a run the controller holds the next block back while the card
 * is busy, so the driver sends one command and waits for completion once
 * per run rather than once per block.
 */
static int
transfer_blocks(
	struct sdhci_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	uint32_t *words;
	uint32_t argument;
	uint32_t ready;
	uint32_t command_index;
	uint32_t moved;
	uint16_t mode;
	unsigned ready_seconds;
	unsigned completion_seconds;
	unsigned index;
	int error;

	/* A run of no blocks, or one longer than a command is given, is refused. */
	if (count == 0 || count > SD_RUN_BLOCKS)
		return EINVAL;

	/* A run past what a 32-bit argument can name is refused. */
	if (block > 0xffffffffULL - (count - 1U))
		return EINVAL;

	/* A high-capacity card is addressed in blocks, an older one in bytes. */
	if (unit->card.high_capacity) {
		argument = (uint32_t)block;
	} else {
		argument = (uint32_t)(block * SD_BLOCK_BYTES);
	}

	/* Picks the buffer event and the completion wait of the direction. */
	if (write) {
		ready = INT_BUFFER_WRITE_READY;
		mode = 0;
		completion_seconds = SD_WRITE_BUSY_SECONDS;
	} else {
		ready = INT_BUFFER_READ_READY;
		mode = XFER_READ;
		completion_seconds = SD_COMMAND_SECONDS;
	}

	/* Picks the single-block or the multiple-block command of the direction. */
	if (count == 1U && write) {
		command_index = 24U;
	} else if (count == 1U) {
		command_index = 17U;
	} else if (write) {
		command_index = 25U;
	} else {
		command_index = 18U;
	}

	/*
	 * A run's length is held by the block count register, which counts
	 * down as the blocks cross the bus, and the controller stops the
	 * card with CMD12 when it reaches zero.
	 */
	if (count != 1U) {
		mode |= XFER_MULTI_BLOCK;
		mode |= XFER_BLOCK_COUNT_ENABLE;
		mode |= XFER_AUTO_CMD12;
	}

	/* Describes the run of 512-byte blocks in the direction of the transfer. */
	write_register16(unit, REG_BLOCK_SIZE, SD_BLOCK_BYTES);
	write_register16(unit, REG_BLOCK_COUNT, (uint16_t)count);
	write_register16(unit, REG_TRANSFER_MODE, mode);

	/* Sends the read or write command, which starts the data phase. */
	error = command(unit,
			command_index,
			argument,
			CMD_RESP_SHORT | CMD_CRC | CMD_INDEX | CMD_DATA,
			NULL);
	if (error != 0)
		return error;

	/* Moves each block through the buffer port once the controller is ready for it. */
	words = buffer;
	for (moved = 0; moved < count; moved++) {
		/*
		 * The first block is ready at once.  Before a later block of
		 * a write the card may still be programming the one before,
		 * and holds the data line busy while it does.
		 */
		ready_seconds = SD_COMMAND_SECONDS;
		if (write && moved != 0)
			ready_seconds = SD_WRITE_BUSY_SECONDS;

		/* Waits until the controller's buffer can take or give the block. */
		error = wait_interrupt(unit, ready, ready_seconds);
		if (error != 0)
			return error;

		/* Moves the block through the buffer port, one word at a time. */
		for (index = 0; index < SD_BLOCK_WORDS; index++) {
			if (write) {
				write_register32(unit, REG_BUFFER, words[index]);
			} else {
				words[index] = read_register32(unit, REG_BUFFER);
			}
		}

		/* The next block follows in the caller's buffer. */
		words += SD_BLOCK_WORDS;
	}

	/* Waits until the run has crossed the bus and a write left the card idle. */
	error = wait_interrupt(unit, INT_TRANSFER_COMPLETE, completion_seconds);
	if (error != 0)
		return error;

	/* Succeeded: the run is on the card or in the buffer. */
	return 0;
}

/*
 * Moves one block, recovering the controller and the card after a failure.
 *
 * Without the recovery one error leaves the inhibit bits set and every
 * later command times out, so the volume above can neither write its
 * buffers nor be synced at shutdown.  A block is tried again after each
 * recovery; reading or writing the same block again is harmless.  A card
 * that cannot be recovered is initialized again, and one that cannot be
 * initialized either is marked failed.
 */
static int
transfer_block_recovering(
	struct sdhci_unit *unit,
	int write,
	uint64_t block,
	void *buffer)
{
	const char *direction;
	uint32_t present;
	unsigned attempt;
	int error;
	int recovery;

	/* Names the direction in the reports. */
	if (write) {
		direction = "write";
	} else {
		direction = "read";
	}

	/* Tries the block, recovering between the tries. */
	error = EIO;
	for (attempt = 1; attempt <= SD_TRANSFER_ATTEMPTS; attempt++) {
		/* Moves the block; a success after a failure is reported as such. */
		unit->last_error_status = 0;
		error = transfer_blocks(unit, write, block, 1U, buffer);
		if (error == 0) {
			if (attempt > 1) {
				kern_logf("sdhci: %s of block %llu succeeded on attempt %u\n",
					  direction,
					  (unsigned long long)block,
					  attempt);
			}

			/* Succeeded: the block moved. */
			return 0;
		}

		/* Reports the failure with what the controller showed. */
		present = read_register32(unit, REG_PRESENT);
		kern_logf("sdhci: %s of block %llu failed on attempt %u: error %d, interrupt status %x, present state %x\n",
			  direction,
			  (unsigned long long)block,
			  attempt,
			  error,
			  unit->last_error_status,
			  present);

		/* Brings the controller and the card back to the transfer state. */
		recovery = restore_card(unit);
		if (recovery != 0)
			return recovery;
	}

	/* Failed: every attempt failed, though the card took commands again. */
	return error;
}

/*
 * Moves a run of blocks with one command, recovering after a failure.
 *
 * A run that fails may have moved any prefix of its blocks, and which
 * prefix is not known: a write's blocks may be on the card or not, a
 * read's may be in the buffer or not.  After the controller and the card
 * are recovered, the whole run is moved again one block at a time, each
 * block with its own retries; writing or reading a block again is
 * harmless.  A run of one block is the single-block path straight away.
 */
static int
transfer_run_recovering(
	struct sdhci_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	const char *direction;
	uint32_t present;
	uint16_t auto_error;
	uint32_t index;
	int error;
	int recovery;

	/* A run of one block has nothing to gain from the multiple-block command. */
	if (count == 1U) {
		error = transfer_block_recovering(unit, write, block, data);
		if (error != 0)
			return error;

		/* Succeeded: the block moved. */
		return 0;
	}

	/* Names the direction in the reports. */
	if (write) {
		direction = "write";
	} else {
		direction = "read";
	}

	/* Moves the run with one command. */
	unit->last_error_status = 0;
	error = transfer_blocks(unit, write, block, count, data);
	if (error == 0) {
		/* Succeeded: the run moved. */
		return 0;
	}

	/* A run the driver refused never reached the controller; there is nothing to recover. */
	if (error == EINVAL)
		return EINVAL;

	/* Reports the failure with what the controller showed, the automatic CMD12 included. */
	auto_error = read_register16(unit, REG_AUTO_CMD_ERROR);
	present = read_register32(unit, REG_PRESENT);
	kern_logf("sdhci: %s of %u blocks at block %llu failed: error %d, interrupt status %x, auto command error %x, present state %x; moving them one block at a time\n",
		  direction,
		  (unsigned)count,
		  (unsigned long long)block,
		  error,
		  unit->last_error_status,
		  (unsigned)auto_error,
		  present);

	/* Brings the controller and the card back to the transfer state. */
	recovery = restore_card(unit);
	if (recovery != 0)
		return recovery;

	/* Moves the run again one block at a time, stopping at the first block that fails. */
	for (index = 0; index < count; index++) {
		error = transfer_block_recovering(unit,
						  write,
						  block + index,
						  data + (size_t)index * SD_BLOCK_BYTES);
		if (error != 0)
			return error;
	}

	/* Succeeded: every block of the run moved on its own. */
	return 0;
}

/*
 * Brings the card back to the transfer state after a failed transfer.
 *
 * The recovery is tried first; a card it cannot bring back is initialized
 * again.  A card that cannot be initialized either is marked failed, and
 * every later transfer fails at once.
 */
static int
restore_card(
	struct sdhci_unit *unit)
{
	int recovery;

	/* Brings the controller and the card back to the transfer state, or initializes the card again. */
	recovery = recover_card(unit);
	if (recovery != 0) {
		kern_logf("sdhci: the card did not return to the transfer state (error %d); initializing it again\n",
			  recovery);
		recovery = controller_init(unit);
	}

	/* A card that cannot be initialized either fails its I/O from now on. */
	if (recovery != 0) {
		kern_logf("sdhci: the card could not be initialized again (error %d); failing every later transfer\n",
			  recovery);
		unit->failed = 1;
		return EIO;
	}

	/* Succeeded: the card takes the next transfer. */
	return 0;
}

/*
 * Brings the controller and the card back to a state that takes commands.
 *
 * An error interrupt or a time-out leaves the controller's command and
 * data state machines where they stopped, with their inhibit bits set.
 * Both lines are reset, and the card is asked for its state until it is
 * back in the transfer state and ready for data.  A card still sending or
 * receiving the block is told to stop; one still programming is waited
 * for.
 */
static int
recover_card(
	struct sdhci_unit *unit)
{
	uint64_t deadline;
	uint32_t card_status;
	uint32_t card_state;
	unsigned stops;
	int passed;
	int error;

	/* Resets the command and the data state machines of the controller. */
	error = reset_lines(unit, SOFTWARE_RESET_CMD | SOFTWARE_RESET_DATA);
	if (error != 0)
		return error;

	/* Sets how long the card has to settle. */
	error = start_deadline(SD_RECOVERY_SECONDS * SD_MICROSECONDS_PER_SECOND, &deadline);
	if (error != 0)
		return error;

	/* Asks the card for its state until it can take the next transfer. */
	stops = 0;
	for (;;) {
		/* A card that does not settle by the deadline is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;

		/* Reads the card's status; a failed question resets the command line. */
		card_status = 0;
		error = command(unit,
				13,
				unit->card.rca << 16,
				CMD_RESP_SHORT | CMD_CRC | CMD_INDEX,
				&card_status);
		if (error != 0) {
			(void)reset_lines(unit, SOFTWARE_RESET_CMD);
			pause_microseconds(SD_STATUS_POLL_MICROSECONDS);
			continue;
		}

		/* The state field says where the card's own state machine stands. */
		card_state = (card_status >> CARD_STATE_SHIFT) & CARD_STATE_MASK;

		/* A card in the transfer state and ready for data takes the next block. */
		if (card_state == CARD_STATE_TRANSFER) {
			if ((card_status & CARD_READY_FOR_DATA) != 0)
				break;
			pause_microseconds(SD_STATUS_POLL_MICROSECONDS);
			continue;
		}

		/* A card still programming the block is waited for. */
		if (card_state == CARD_STATE_PROGRAM) {
			pause_microseconds(SD_STATUS_POLL_MICROSECONDS);
			continue;
		}

		/* A card in any other state than a transfer is not ours to fix here. */
		if (card_state != CARD_STATE_DATA &&
		    card_state != CARD_STATE_RECEIVE) {
			kern_logf("sdhci: the card is in state %u after an error\n",
				  (unsigned)card_state);
			return EIO;
		}

		/* A card still in a transfer is told to stop, a bounded number of times. */
		if (stops >= SD_STOP_ATTEMPTS)
			return ETIMEDOUT;
		stops++;
		error = stop_transmission(unit);
		if (error != 0)
			(void)reset_lines(unit, SOFTWARE_RESET_CMD | SOFTWARE_RESET_DATA);
	}

	/* Clears what the recovery itself left in the interrupt status. */
	write_register32(unit, REG_INT_STATUS, 0xffffffffU);

	/* Succeeded: the card is in the transfer state and ready for data. */
	return 0;
}

/*
 * Resets the named lines of the controller and waits for the reset to end.
 *
 * Resetting the command line clears the command inhibit; resetting the data
 * line clears the data inhibit and the buffer.  Neither touches the clock,
 * the bus width or the card.
 */
static int
reset_lines(
	struct sdhci_unit *unit,
	uint8_t lines)
{
	uint64_t deadline;
	uint8_t pending;
	int passed;
	int error;

	/* Starts the reset of the lines. */
	write_register8(unit, REG_SOFTWARE_RESET, lines);

	/* Sets how long the controller has to finish the reset. */
	error = start_deadline(SD_RESET_SECONDS * SD_MICROSECONDS_PER_SECOND, &deadline);
	if (error != 0)
		return error;

	/* Waits until the controller clears the reset bits it was given. */
	for (;;) {
		/* The reset is done once its bits read back clear. */
		pending = (uint8_t)(read_register8(unit, REG_SOFTWARE_RESET) & lines);
		if (pending == 0)
			break;

		/* A reset that never ends is reported. */
		passed = deadline_passed(deadline);
		if (passed) {
			kern_logf("sdhci: the reset of lines %x did not end\n",
				  (unsigned)lines);
			return ETIMEDOUT;
		}
	}

	/* Acknowledges whatever the interrupted operation had raised. */
	write_register32(unit, REG_INT_STATUS, 0xffffffffU);

	/* Succeeded: the reset lines take commands again. */
	return 0;
}

/*
 * Tells the card to stop sending or receiving a block.
 *
 * CMD12 answers with R1b: the card may hold the data line busy while it
 * finishes, and the controller keeps the data inhibit set until then.
 */
static int
stop_transmission(
	struct sdhci_unit *unit)
{
	int error;

	/* Sends the stop command. */
	error = command(unit, 12, 0, CMD_RESP_BUSY | CMD_CRC | CMD_INDEX, NULL);
	if (error != 0)
		return error;

	/* Waits for the card to release the data line. */
	error = wait_bits(unit, REG_PRESENT, PRESENT_DATA_INHIBIT, 0);
	if (error != 0)
		return error;

	/* Succeeded: the card has left the transfer. */
	return 0;
}

/* Allocates the unit's disk, names it mmcblk<unit>, and publishes it. */
static int
publish_disk(
	struct sdhci_unit *unit)
{
	struct disk *disk;
	int error;

	/* Allocates the disk. */
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;

	/* Prepares the lock that serializes the unit's commands. */
	(void)mutex_init(&unit->lock, LOCK_RANK_DISK, "sdhci");

	/* Names the disk after the unit; the table holds fewer than ten units. */
	disk->d_name[0] = 'm';
	disk->d_name[1] = 'm';
	disk->d_name[2] = 'c';
	disk->d_name[3] = 'b';
	disk->d_name[4] = 'l';
	disk->d_name[5] = 'k';
	disk->d_name[6] = (char)('0' + unit->number);
	disk->d_name[7] = '\0';

	/* Describes a removable card of 512-byte blocks, of the size its CSD gave, moved in runs. */
	disk->d_flags = DISK_REMOVABLE;
	disk->d_block_size = SD_BLOCK_BYTES;
	disk->d_block_count = unit->card.block_count;
	disk->d_max_transfer_blocks = SD_RUN_BLOCKS;
	disk->d_ops = &sd_ops;
	disk->d_data = unit;

	/* Lists the card in the disk statistics as an SD/MMC disk. */
	disk_set_stats_kind(disk, DISK_STATS_KIND_SDMMC);

	/* Publishes the disk. */
	error = disk_create(disk);
	if (error != 0)
		return error;

	/* Succeeded: the unit's disk is published. */
	unit->disk = disk;
	return 0;
}

/*
 * Serves one block request from the disk layer.
 *
 * The card is driven by PIO under the unit lock, a run of up to
 * SD_RUN_BLOCKS blocks per command.  A flush has nothing to do: the card
 * has no write cache enabled, and a written block is reported only once
 * the card has programmed it.
 */
static int
sd_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct sdhci_unit *unit;
	uint8_t *data;
	uint32_t done;
	uint32_t run;
	size_t transferred;
	int write;
	int error;

	/* Finds the unit behind the disk and the request's data. */
	unit = disk->d_data;
	data = bio->b_data;

	/* A write moves the data to the card; anything else that moves data reads it. */
	write = 0;
	if (bio->b_op == BIO_WRITE)
		write = 1;

	/* Picks the requests the card serves. */
	error = 0;
	if (bio->b_op == BIO_FLUSH) {
		/* A flush has nothing left to write. */
		error = 0;
	} else if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE) {
		/* Discards and the rest are not served. */
		error = EOPNOTSUPP;
	} else {
		/* Moves every block of the request, stopping at the first failure. */
		mutex_lock(&unit->lock);

		/* A card marked failed is not tried again. */
		if (unit->failed)
			error = EIO;

		/*
		 * Moves the blocks in order, a run per command.  The disk
		 * layer splits at d_max_transfer_blocks, so a request is
		 * normally one run; a longer one is still served in runs.
		 */
		run = 0;
		for (done = 0;
		     done < bio->b_block_count && error == 0;
		     done += run) {
			/* The run, or what is left of the request when that is less. */
			run = bio->b_block_count - done;
			if (run > SD_RUN_BLOCKS)
				run = SD_RUN_BLOCKS;

			/* Moves the run, falling back to single blocks if it fails. */
			error = transfer_run_recovering(unit,
							write,
							bio->b_mapped_block + done,
							run,
							data + (size_t)done * SD_BLOCK_BYTES);
		}

		mutex_unlock(&unit->lock);
	}

	/* Reports a failed request with what it asked for. */
	if (error != 0) {
		kern_logf("sdhci: op=%u lba=%llu count=%u error=%d\n",
			  (unsigned)bio->b_op,
			  (unsigned long long)bio->b_mapped_block,
			  bio->b_block_count,
			  error);
	}

	/* Completes the request with the bytes it moved. */
	transferred = 0;
	if (error == 0)
		transferred = (size_t)bio->b_block_count * SD_BLOCK_BYTES;
	bio_complete(bio, error, transferred);

	/* Succeeded: the request was completed, with its own error if any. */
	return 0;
}

/* Refuses every control request: the card has none to offer. */
static int
sd_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	UNUSED_PARAMETER(disk);
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(argument);

	/* Failed: no request is supported. */
	return EOPNOTSUPP;
}
