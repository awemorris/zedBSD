/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the backplane, the enumeration ROM, the firmware download and
 * the start of the frame function.
 *
 * The chip's cores sit on an AXI backplane.  The host reaches it through
 * function 1: three window registers hold an address's high bits and the
 * low fifteen go into the function 1 address.  Each core has a wrapper
 * whose reset and I/O control registers start, stop and halt it.
 *
 */

#include "drivers/wifi/bcm43455/bcm43455-chip.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include "drivers/wifi/bcm43455/bcm43455-hw.h"
#include "drivers/wifi/bcm43455/bcm43455-firmware.h"
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

/* How long the slow (ALP) clock may take to come, in milliseconds. */
#define ALP_WAIT_MS			50U

/*
 * How long a requested clock (ALP for the download, HT afterwards) may take to
 * come.
 */
#define HT_WAIT_MS			5000U

/* How long function 2 may take to report ready once the firmware runs. */
#define FRAMES_READY_MS			3000U

/* How many times a core's reset is let go before it is given up. */
#define RESET_RELEASE_TRIES		50U

/* The shortest firmware image the download takes. */
#define FIRMWARE_MIN_BYTES		64U

/*
 * The most RAM a 43455 can have; a bank table that adds up to more is garbled.
 */
#define RAM_SIZE_MAX			(2U * 1024U * 1024U)

static int set_window(struct bcm43455_chip *chip, uint32_t address);
static int transfer_memory(struct bcm43455_chip *chip, uint32_t address, uint8_t *data, const uint8_t *source, size_t length, int write);
static int start_backplane_clock(struct bcm43455_chip *chip);
static int scan_erom(struct bcm43455_chip *chip, uint32_t erom);
static void keep_core(struct bcm43455_chip *chip, const struct bcm43455_core *core);
static int read_ram_size(struct bcm43455_chip *chip);
static int hold_core_in_reset(struct bcm43455_chip *chip, const struct bcm43455_core *core, uint32_t control_running, uint32_t control_held);
static int restart_core(struct bcm43455_chip *chip, const struct bcm43455_core *core, uint32_t control_running, uint32_t control_held, uint32_t control_released);
static int halt_cores(struct bcm43455_chip *chip);
static int read_power_unit(struct bcm43455_chip *chip);
static int prepare_interface(struct bcm43455_chip *chip);
static int request_clock(struct bcm43455_chip *chip, uint8_t request, uint8_t available, const char *name);
static int verify_memory(struct bcm43455_chip *chip, uint32_t address, const uint8_t *expected, size_t length);
static int wait_clock(struct bcm43455_chip *chip, uint8_t mask, unsigned timeout_ms);

/*
 * Starts the backplane clock, identifies the chip and finds its cores.
 */
int
bcm43455_chip_identify(
	struct bcm43455_chip *chip,
	const struct bcm43455_bus_ops *ops,
	void *context)
{
	uint32_t identity;
	uint32_t erom;
	unsigned bus;
	int error;

	/* Refuses a missing chip or bus. */
	if (chip == NULL || ops == NULL)
		return EINVAL;

	/* Starts from a chip that knows nothing. */
	kern_memset(chip, 0, sizeof(*chip));
	chip->ops = ops;
	chip->context = context;
	chip->ram_base = BCM43455_RAM_BASE;

	/* Runs the backplane on its slow clock, which needs no firmware. */
	error = start_backplane_clock(chip);
	if (error != 0)
		return error;

	/* Reads the chip id register of the chip common core. */
	error = bcm43455_chip_read32(chip, BCM43455_CHIPCOMMON_BASE + BCM43455_CHIPCOMMON_ID, &identity);
	if (error != 0)
		return error;
	chip->id = (uint16_t)(identity & BCM43455_CHIP_ID_MASK);
	chip->revision = (identity >> BCM43455_CHIP_REVISION_SHIFT) & BCM43455_CHIP_REVISION_MASK;
	bus = (identity >> BCM43455_CHIP_BUS_SHIFT) & BCM43455_CHIP_BUS_MASK;
	kern_logf("bcm43455: chip %04x revision %u bus type %u\n", (unsigned)chip->id, chip->revision, bus);

	/* Refuses any chip but a 43455 on an AXI backplane. */
	if (chip->id != BCM43455_CHIP_ID ||
	    chip->revision < BCM43455_CHIP_REVISION_MIN ||
	    bus != BCM43455_CHIP_BUS_AXI)
		return ENODEV;

	/* Reads the capabilities, logged for the save-restore question. */
	error = bcm43455_chip_read32(chip, BCM43455_CHIPCOMMON_BASE + BCM43455_CHIPCOMMON_CAPABILITY, &chip->capability);
	if (error != 0)
		return error;
	error = bcm43455_chip_read32(chip, BCM43455_CHIPCOMMON_BASE + BCM43455_CHIPCOMMON_CAPABILITY_EXT, &chip->capability_ext);
	if (error != 0)
		return error;
	kern_logf("bcm43455: capabilities %08x, extended %08x\n", chip->capability, chip->capability_ext);

	/* Finds the cores in the enumeration ROM. */
	error = bcm43455_chip_read32(chip, BCM43455_CHIPCOMMON_BASE + BCM43455_CHIPCOMMON_EROM, &erom);
	if (error != 0)
		return error;
	error = scan_erom(chip, erom);
	if (error != 0)
		return error;

	/* Measures the RAM the ARM core runs from. */
	error = read_ram_size(chip);
	if (error != 0)
		return error;
	kern_logf(
		"bcm43455: SDIO core %08x, ARM CR4 %08x (wrapper %08x), D11 wrapper %08x, RAM %06x + %06x\n",
		chip->sdio.base,
		chip->cr4.base,
		chip->cr4.wrapper,
		chip->d11.wrapper,
		chip->ram_base,
		chip->ram_size);

	/*
	 * Halts the ARM core and the radio, so nothing runs from RAM until the
	 * download.
	 */
	error = halt_cores(chip);
	if (error != 0)
		return error;

	/*
	 * Finds the PMU and learns whether the firmware will run save-restore.
	 */
	error = read_power_unit(chip);
	if (error != 0)
		return error;

	/*
	 * Prepares the SDIO interface and the PMU for the firmware, and leaves
	 * the chip on its slow clock.
	 */
	error = prepare_interface(chip);
	if (error != 0)
		return error;

	/* Succeeded: the chip waits for its firmware. */
	return 0;
}

/*
 * Reads one 32-bit backplane register.
 */
int
bcm43455_chip_read32(
	struct bcm43455_chip *chip,
	uint32_t address,
	uint32_t *value)
{
	uint8_t bytes[4];
	uint32_t offset;
	int error;

	/* Points the window at the register. */
	error = set_window(chip, address);
	if (error != 0)
		return error;

	/* Reads the four bytes through function 1. */
	offset = (address & BCM43455_WINDOW_OFFSET_MASK) | BCM43455_WINDOW_ACCESS_32;
	error = chip->ops->read(chip->context, BCM43455_FUNCTION_BACKPLANE, offset, 1, bytes, sizeof(bytes));
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	*value = bcm43455_get_le32(bytes);
	return 0;
}

/*
 * Writes one 32-bit backplane register.
 */
int
bcm43455_chip_write32(
	struct bcm43455_chip *chip,
	uint32_t address,
	uint32_t value)
{
	uint8_t bytes[4];
	uint32_t offset;
	int error;

	/* Points the window at the register. */
	error = set_window(chip, address);
	if (error != 0)
		return error;

	/* Writes the four bytes through function 1. */
	bcm43455_put_le32(bytes, value);
	offset = (address & BCM43455_WINDOW_OFFSET_MASK) | BCM43455_WINDOW_ACCESS_32;
	error = chip->ops->write(chip->context, BCM43455_FUNCTION_BACKPLANE, offset, 1, bytes, sizeof(bytes));
	if (error != 0)
		return error;

	/* Succeeded: the register holds the value. */
	return 0;
}

/*
 * Writes backplane memory.
 */
int
bcm43455_chip_write_memory(
	struct bcm43455_chip *chip,
	uint32_t address,
	const uint8_t *data,
	size_t length)
{
	int error;

	/* Moves the bytes through the bounce buffer. */
	error = transfer_memory(chip, address, NULL, data, length, 1);
	if (error != 0)
		return error;

	/* Succeeded: the memory holds the bytes. */
	return 0;
}

/*
 * Reads backplane memory.
 */
int
bcm43455_chip_read_memory(
	struct bcm43455_chip *chip,
	uint32_t address,
	uint8_t *data,
	size_t length)
{
	int error;

	/* Moves the bytes straight into the caller's buffer. */
	error = transfer_memory(chip, address, data, NULL, length, 0);
	if (error != 0)
		return error;

	/* Succeeded: data holds the memory's bytes. */
	return 0;
}

/*
 * Writes the firmware and the NVRAM and starts the ARM core.
 *
 * The order is: slow clock, firmware, read back, NVRAM below the top of
 * RAM, read back, reset vector at address 0, ARM core released, clock given
 * back.  The download runs on the slow (ALP) clock, not the fast (HT) one.
 * The fast clock needs the PMU resources the firmware sets up from the
 * NVRAM, so before the firmware runs it never comes (on the Pi 4 the wait
 * for it ends in "fast clock not available").
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_load_microcode(), bwfm_sdio_buscore_activate(),
 * bwfm_sdio_preinit() and bwfm_sdio_htclk(), lines 500-548 and 596-640) and
 * OpenBSD sys/dev/ic/bwfm.c revision 1.113 (bwfm_chip_cr4_set_active(), lines
 * 1275-1285).
 */
int
bcm43455_chip_download(
	struct bcm43455_chip *chip,
	const uint8_t *firmware,
	size_t firmware_length,
	const uint8_t *nvram,
	size_t nvram_length)
{
	uint32_t nvram_address;
	uint32_t vector;
	uint32_t halted;
	int error;

	/* Refuses images that are not whole words or that RAM cannot hold. */
	if (firmware == NULL || nvram == NULL)
		return EINVAL;
	if (firmware_length < FIRMWARE_MIN_BYTES || (firmware_length % 4U) != 0)
		return EINVAL;
	error = bcm43455_nvram_place(chip->ram_base, chip->ram_size, firmware_length, nvram_length, &nvram_address);
	if (error != 0)
		return error;

	/*
	 * Moves the download on the slow clock; the fast one comes only once
	 * the firmware runs.
	 */
	error = request_clock(chip, BCM43455_CLOCK_ALP_REQUEST, BCM43455_CLOCK_ALP_AVAILABLE, "slow");
	if (error != 0)
		return error;

	/* Writes the firmware at the bottom of RAM and reads it back whole. */
	error = bcm43455_chip_write_memory(chip, chip->ram_base, firmware, firmware_length);
	if (error != 0)
		return error;
	error = verify_memory(chip, chip->ram_base, firmware, firmware_length);
	if (error != 0)
		return error;

	/*
	 * Writes the NVRAM at the top of RAM, where the firmware looks for it,
	 * and reads it back.
	 */
	error = bcm43455_chip_write_memory(chip, nvram_address, nvram, nvram_length);
	if (error != 0)
		return error;
	error = verify_memory(chip, nvram_address, nvram, nvram_length);
	if (error != 0)
		return error;
	kern_logf(
		"bcm43455: firmware %u bytes at %06x, NVRAM %u bytes at %06x\n",
		(unsigned)firmware_length,
		chip->ram_base,
		(unsigned)nvram_length,
		nvram_address);

	/* Clears the SDIO core's interrupt state left from before. */
	error = bcm43455_chip_write32(chip, chip->sdio.base + BCM43455_SDIO_CORE_INTERRUPTS, 0xffffffffU);
	if (error != 0)
		return error;

	/*
	 * Puts the firmware's reset vector, its first word, where the ARM core
	 * starts.
	 */
	vector = bcm43455_get_le32(firmware);
	error = bcm43455_chip_write32(chip, 0, vector);
	if (error != 0)
		return error;

	/*
	 * Lets the ARM core out of reset without the halt: the firmware starts.
	 */
	halted = BCM43455_CR4_IO_CPU_HALT;
	error = restart_core(chip, &chip->cr4, halted, 0, 0);
	if (error != 0)
		return error;

	/*
	 * Gives the clock request back to the chip until the frame function
	 * starts.
	 */
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_CHIP_CLOCK, 0);
	if (error != 0)
		return error;

	/* Succeeded: the firmware runs. */
	return 0;
}

/*
 * Brings the frame function up behind the running firmware, the
 * save-restore branch included.  The interrupt enable sets master, F1 and
 * F2.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_preinit(), lines 432-462) and Infineon wifi-host-driver bd99da7b
 * whd_bus_sdio_protocol.c (lines 410-411).
 */
int
bcm43455_chip_start(
	struct bcm43455_chip *chip)
{
	uint8_t clock;
	uint8_t wakeup;
	uint8_t enable;
	uint8_t capability;
	int error;

	/*
	 * Asks for the fast clock, which the running firmware provides, and
	 * holds it while the frame function starts.
	 */
	error = request_clock(chip, BCM43455_CLOCK_HT_REQUEST, BCM43455_CLOCK_HT_AVAILABLE, "fast");
	if (error != 0)
		return error;
	error = chip->ops->read_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_CHIP_CLOCK, &clock);
	if (error != 0)
		return error;
	error = chip->ops->write_byte(
		chip->context,
		BCM43455_FUNCTION_BACKPLANE,
		BCM43455_F1_CHIP_CLOCK,
		(uint8_t)(clock | BCM43455_CLOCK_FORCE_HT));
	if (error != 0)
		return error;

	/* Tells the firmware which protocol version the host speaks. */
	error = bcm43455_chip_write32(
		chip,
		chip->sdio.base + BCM43455_SDIO_TO_CHIP_DATA,
		(uint32_t)BCM43455_SDPCM_PROTOCOL << BCM43455_TO_CHIP_VERSION_SHIFT);
	if (error != 0)
		return error;

	/*
	 * Enables the frame function, which reports ready once the firmware
	 * serves it.
	 */
	error = chip->ops->enable_function(chip->context, BCM43455_FUNCTION_FRAMES, FRAMES_READY_MS);
	if (error != 0) {
		kern_logf("bcm43455: frame function not ready (%d)\n", error);
		return error;
	}

	/*
	 * Lets the to-host mailbox and the chip's activity raise the SDIO
	 * core's interrupt.
	 */
	error = bcm43455_chip_write32(
		chip,
		chip->sdio.base + BCM43455_SDIO_CORE_INTERRUPT_MASK,
		BCM43455_INT_MAILBOX_MASK | BCM43455_INT_CHIP_ACTIVE);
	if (error != 0)
		return error;

	/* Sets the read FIFO's watermark. */
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_WATERMARK, BCM43455_F1_WATERMARK_VALUE);
	if (error != 0)
		return error;

	/*
	 * With save-restore the firmware decides when the chip sleeps: the
	 * chip waits for the fast clock when it wakes, the card takes CMD14,
	 * and only the force bit stays.  Without it the host's request is put
	 * back as it was before the force.
	 */
	if (chip->save_restore) {
		error = chip->ops->read_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_WAKEUP_CONTROL, &wakeup);
		if (error != 0)
			return error;
		error = chip->ops->write_byte(
			chip->context,
			BCM43455_FUNCTION_BACKPLANE,
			BCM43455_F1_WAKEUP_CONTROL,
			(uint8_t)(wakeup | BCM43455_WAKEUP_HT_WAIT));
		if (error != 0)
			return error;
		capability = (uint8_t)(BCM43455_CARD_CAPABILITY_CMD14 | BCM43455_CARD_CAPABILITY_CMD14_EXT);
		error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_COMMON, BCM43455_CCCR_CARD_CAPABILITY, capability);
		if (error != 0)
			return error;
		error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_CHIP_CLOCK, BCM43455_CLOCK_FORCE_HT);
		if (error != 0)
			return error;
	} else {
		error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_CHIP_CLOCK, clock);
		if (error != 0)
			return error;
	}

	/* Lets both functions interrupt the host. */
	enable = (uint8_t)(BCM43455_CCCR_INTERRUPT_MASTER | BCM43455_CCCR_INTERRUPT_F1 | BCM43455_CCCR_INTERRUPT_F2);
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_COMMON, BCM43455_CCCR_INTERRUPT_ENABLE, enable);
	if (error != 0)
		return error;

	/* Leaves the window where the frames are moved. */
	error = bcm43455_chip_frame_window(chip);
	if (error != 0)
		return error;

	/* Succeeded: the frame function carries frames. */
	return 0;
}

/*
 * Points the window at the chip common core.
 */
int
bcm43455_chip_frame_window(
	struct bcm43455_chip *chip)
{
	int error;

	/*
	 * The frame function's address lies in the chip common core's window.
	 */
	error = set_window(chip, BCM43455_CHIPCOMMON_BASE);
	if (error != 0)
		return error;

	/* Succeeded: frames may move. */
	return 0;
}

/*
 * Points the backplane window at the 32 KB that hold address, writing
 * only when it moves.
 */
static int
set_window(
	struct bcm43455_chip *chip,
	uint32_t address)
{
	uint32_t window;
	int error;

	/* Nothing to do while the window already covers the address. */
	window = address & ~(uint32_t)BCM43455_WINDOW_OFFSET_MASK;
	if (chip->window_valid != 0 && chip->window == window)
		return 0;

	/*
	 * A window half written points nowhere known, so it stays invalid
	 * until all three registers are written.
	 */
	chip->window_valid = 0;

	/* Writes bit 15 of the address, then bits 16 to 23, then 24 to 31. */
	error = chip->ops->write_byte(
		chip->context,
		BCM43455_FUNCTION_BACKPLANE,
		BCM43455_F1_WINDOW_LOW,
		(uint8_t)((window >> 8) & BCM43455_WINDOW_LOW_MASK));
	if (error != 0)
		return error;
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_WINDOW_MID, (uint8_t)((window >> 16) & 0xffU));
	if (error != 0)
		return error;
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_WINDOW_HIGH, (uint8_t)(window >> 24));
	if (error != 0)
		return error;

	/* Succeeded: the window covers the address. */
	chip->window = window;
	chip->window_valid = 1;
	return 0;
}

/*
 * Moves backplane memory in pieces that stay inside one window and fit
 * the bus: byte mode up to a block, whole blocks beyond.  A write takes
 * its bytes from source through the bounce buffer; a read puts them in
 * data.
 */
static int
transfer_memory(
	struct bcm43455_chip *chip,
	uint32_t address,
	uint8_t *data,
	const uint8_t *source,
	size_t length,
	int write)
{
	size_t done;
	size_t piece;
	size_t room;
	uint32_t offset;
	unsigned block;
	int error;

	/* Refuses a length that is not whole words. */
	if ((length % 4U) != 0)
		return EINVAL;

	/* Moves the memory one piece at a time. */
	block = chip->ops->block_size(chip->context, BCM43455_FUNCTION_BACKPLANE);
	done = 0;
	while (done < length) {
		/* Points the window at the piece's start. */
		error = set_window(chip, address + (uint32_t)done);
		if (error != 0)
			return error;

		/*
		 * Takes no more than the window holds past the start, nor than
		 * the bounce buffer.
		 */
		offset = (address + (uint32_t)done) & BCM43455_WINDOW_OFFSET_MASK;
		room = (size_t)(BCM43455_WINDOW_SIZE - offset);
		piece = length - done;
		if (piece > room)
			piece = room;
		if (piece > BCM43455_CHIP_TRANSFER_MAX)
			piece = BCM43455_CHIP_TRANSFER_MAX;

		/* A piece longer than a block is cut to whole blocks. */
		if (block != 0 && piece > block)
			piece -= piece % block;

		/* Moves the piece. */
		offset |= BCM43455_WINDOW_ACCESS_32;
		if (write) {
			kern_memcpy(chip->bounce, source + done, piece);
			error = chip->ops->write(chip->context, BCM43455_FUNCTION_BACKPLANE, offset, 1, chip->bounce, piece);
		} else {
			error = chip->ops->read(chip->context, BCM43455_FUNCTION_BACKPLANE, offset, 1, data + done, piece);
		}

		/* Reports a piece the bus did not move. */
		if (error != 0)
			return error;

		/* Moves past the piece. */
		done += piece;
	}

	/*
	 * A write's bounce buffer may have held part of the NVRAM; it keeps
	 * nothing.
	 */
	if (write)
		kern_memset(chip->bounce, 0, sizeof(chip->bounce));

	/* Succeeded: the memory moved. */
	return 0;
}

/*
 * Asks for the backplane's slow clock, waits for it, and then keeps it on
 * by force; also drops the SDIO lines' internal pull-ups, which the board
 * provides.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_buscore_prepare(), lines 1002-1041).
 */
static int
start_backplane_clock(
	struct bcm43455_chip *chip)
{
	int error;

	/* Asks for the slow clock without the chip's own clock requests. */
	error = chip->ops->write_byte(
		chip->context,
		BCM43455_FUNCTION_BACKPLANE,
		BCM43455_F1_CHIP_CLOCK,
		(uint8_t)(BCM43455_CLOCK_HW_REQUEST_OFF | BCM43455_CLOCK_ALP_REQUEST));
	if (error != 0)
		return error;

	/* Waits until the chip says the backplane clock runs. */
	error = wait_clock(chip, BCM43455_CLOCK_ALP_AVAILABLE, ALP_WAIT_MS);
	if (error != 0) {
		kern_logf("bcm43455: backplane clock not available (%d)\n", error);
		return error;
	}

	/* Keeps the slow clock on. */
	error = chip->ops->write_byte(
		chip->context,
		BCM43455_FUNCTION_BACKPLANE,
		BCM43455_F1_CHIP_CLOCK,
		(uint8_t)(BCM43455_CLOCK_HW_REQUEST_OFF | BCM43455_CLOCK_FORCE_ALP));
	if (error != 0)
		return error;

	/* Drops the internal pull-ups. */
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_PULL_UP, 0);
	if (error != 0)
		return error;

	/* Succeeded: the backplane runs. */
	return 0;
}

/*
 * Walks the enumeration ROM and keeps the cores the driver needs.
 *
 * A component takes two words; master port descriptors are skipped; each
 * address descriptor may be followed by a high address word and a size
 * word.  A core's base is its first 4 KB or 8 KB slave port 0 address, its
 * wrapper the first master wrapper (for a core with master ports) or
 * slave wrapper.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_chip_dmp_erom_scan() and bwfm_chip_dmp_get_regaddr(), lines 1182-1240).
 */
static int
scan_erom(
	struct bcm43455_chip *chip,
	uint32_t erom)
{
	struct bcm43455_core core;
	uint32_t word;
	uint32_t extra;
	unsigned words;
	unsigned type;
	unsigned kind;
	unsigned port;
	unsigned size;
	unsigned master_wraps;
	unsigned slave_wraps;
	unsigned have_core;
	int error;

	/* Reads descriptor after descriptor until the end mark. */
	kern_memset(&core, 0, sizeof(core));
	have_core = 0;
	master_wraps = 0;
	slave_wraps = 0;
	for (words = 0; words < BCM43455_EROM_WORDS_MAX; words++) {
		/* Reads the next descriptor. */
		error = bcm43455_chip_read32(chip, erom + words * 4U, &word);
		if (error != 0)
			return error;

		/* The end mark ends the table, and the last core with it. */
		type = word & BCM43455_EROM_TYPE_MASK;
		if (type == BCM43455_EROM_END) {
			if (have_core)
				keep_core(chip, &core);
			break;
		}

		/*
		 * A component starts a core: its id here, its revision and port
		 * counts in the next word.
		 */
		if (type == BCM43455_EROM_COMPONENT) {
			if (have_core)
				keep_core(chip, &core);
			kern_memset(&core, 0, sizeof(core));
			core.id = (uint16_t)((word >> BCM43455_EROM_PART_SHIFT) & BCM43455_EROM_PART_MASK);
			words++;
			error = bcm43455_chip_read32(chip, erom + words * 4U, &word);
			if (error != 0)
				return error;
			core.revision = (uint8_t)((word >> BCM43455_EROM_REVISION_SHIFT) & BCM43455_EROM_REVISION_MASK);
			master_wraps = (word >> BCM43455_EROM_MASTER_WRAPS_SHIFT) & BCM43455_EROM_COUNT_MASK;
			slave_wraps = (word >> BCM43455_EROM_SLAVE_WRAPS_SHIFT) & BCM43455_EROM_COUNT_MASK;
			have_core = 1;
			continue;
		}

		/*
		 * Anything but an address descriptor (a master port) says
		 * nothing the driver needs.
		 */
		if ((word & BCM43455_EROM_TYPE_SHORT_MASK) != BCM43455_EROM_ADDRESS)
			continue;

		/* Skips the high address word of a wide address. */
		if ((word & BCM43455_EROM_WIDE) != 0)
			words++;

		/* Skips the size words of a described size, one or two. */
		size = (word >> BCM43455_EROM_SIZE_SHIFT) & BCM43455_EROM_SIZE_MASK;
		if (size == BCM43455_EROM_SIZE_DESCRIBED) {
			words++;
			error = bcm43455_chip_read32(chip, erom + words * 4U, &extra);
			if (error != 0)
				return error;
			if ((extra & BCM43455_EROM_WIDE) != 0)
				words++;
		}

		/* An address outside a core belongs to nothing. */
		if (!have_core)
			continue;

		/*
		 * A core's registers and its wrapper are 4 KB or 8 KB; larger
		 * regions are memory.
		 */
		if (size != BCM43455_EROM_SIZE_4K && size != BCM43455_EROM_SIZE_8K)
			continue;

		/*
		 * Keeps the core's first slave port 0 address as its registers.
		 */
		kind = (word >> BCM43455_EROM_KIND_SHIFT) & BCM43455_EROM_KIND_MASK;
		port = (word >> BCM43455_EROM_PORT_SHIFT) & BCM43455_EROM_PORT_MASK;
		if (kind == BCM43455_EROM_KIND_SLAVE &&
		    port == 0 &&
		    core.base == 0)
			core.base = word & BCM43455_EROM_BASE_MASK;

		/*
		 * Keeps the core's first wrapper of the kind its ports call
		 * for.
		 */
		if (core.wrapper == 0) {
			if (kind == BCM43455_EROM_KIND_MASTER_WRAP && master_wraps != 0)
				core.wrapper = word & BCM43455_EROM_BASE_MASK;
			else if (kind == BCM43455_EROM_KIND_SLAVE_WRAP &&
			         master_wraps == 0 &&
			         slave_wraps != 0)
				core.wrapper = word & BCM43455_EROM_BASE_MASK;
		}
	}

	/*
	 * Refuses a table that does not describe every core the driver drives.
	 */
	if (chip->sdio.base == 0) {
		kern_logf("bcm43455: no SDIO device core in the enumeration ROM\n");
		return EIO;
	}

	/* Refuses a table without the processor core and its wrapper. */
	if (chip->cr4.base == 0 || chip->cr4.wrapper == 0) {
		kern_logf("bcm43455: no ARM CR4 core in the enumeration ROM\n");
		return EIO;
	}

	/* Refuses a table without the radio core's wrapper. */
	if (chip->d11.wrapper == 0) {
		kern_logf("bcm43455: no D11 core in the enumeration ROM\n");
		return EIO;
	}

	/* Succeeded: the cores are known. */
	return 0;
}

/* Keeps a core the driver needs; the first of each kind wins. */
static void
keep_core(
	struct bcm43455_chip *chip,
	const struct bcm43455_core *core)
{
	struct bcm43455_core *slot;

	/* Finds where the driver keeps a core of this id. */
	slot = NULL;
	if (core->id == BCM43455_CORE_CHIPCOMMON)
		slot = &chip->chipcommon;
	else if (core->id == BCM43455_CORE_SDIO_DEVICE)
		slot = &chip->sdio;
	else if (core->id == BCM43455_CORE_ARM_CR4)
		slot = &chip->cr4;
	else if (core->id == BCM43455_CORE_D11)
		slot = &chip->d11;
	else if (core->id == BCM43455_CORE_PMU)
		slot = &chip->pmu;

	/* Keeps only the first core of a needed kind. */
	if (slot == NULL)
		return;
	if (slot->id != 0)
		return;
	*slot = *core;
}

/*
 * Adds up the ARM CR4's RAM banks.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_chip_tcm_ramsize(), lines 1533-1556).
 */
static int
read_ram_size(
	struct bcm43455_chip *chip)
{
	uint32_t capability;
	uint32_t info;
	uint32_t unit;
	uint32_t total;
	unsigned banks;
	unsigned bank;
	int error;

	/* The capability register counts the banks of both kinds. */
	error = bcm43455_chip_read32(chip, chip->cr4.base + BCM43455_CR4_CAPABILITY, &capability);
	if (error != 0)
		return error;
	banks = capability & BCM43455_CR4_BANKS_A_MASK;
	banks += (capability >> BCM43455_CR4_BANKS_B_SHIFT) & BCM43455_CR4_BANKS_B_MASK;

	/* Selects each bank and adds its size. */
	total = 0;
	for (bank = 0; bank < banks; bank++) {
		/* Selects the bank and reads what it holds. */
		error = bcm43455_chip_write32(chip, chip->cr4.base + BCM43455_CR4_BANK_INDEX, bank);
		if (error != 0)
			return error;
		error = bcm43455_chip_read32(chip, chip->cr4.base + BCM43455_CR4_BANK_INFO, &info);
		if (error != 0)
			return error;

		/* A bank counts blocks of 8 KB, or of 1 KB when it says so. */
		unit = BCM43455_CR4_BANK_UNIT;
		if ((info & BCM43455_CR4_BANK_1K) != 0)
			unit = BCM43455_CR4_BANK_UNIT / 8U;
		total += ((info & BCM43455_CR4_BANK_SIZE_MASK) + 1U) * unit;
	}

	/* Refuses a table without RAM or with more than the chip can have. */
	if (total == 0 || total > RAM_SIZE_MAX) {
		kern_logf("bcm43455: RAM bank table unusable (%u banks, %u bytes)\n", banks, total);
		return EIO;
	}

	/* Succeeded: the RAM's size. */
	chip->ram_size = total;
	return 0;
}

/*
 * Puts a core into reset with the given I/O control before and during it.
 * A core already in reset only gets its in-reset control.  The register
 * sequence is the AXI wrapper's.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_chip_ai_disable(), lines 1062-1098).
 */
static int
hold_core_in_reset(
	struct bcm43455_chip *chip,
	const struct bcm43455_core *core,
	uint32_t control_running,
	uint32_t control_held)
{
	uint32_t control;
	uint32_t readback;
	int error;

	/* Reads whether the core is in reset already. */
	error = bcm43455_chip_read32(chip, core->wrapper + BCM43455_WRAP_RESET_CONTROL, &control);
	if (error != 0)
		return error;

	/*
	 * A running core is given its pre-reset control, then put into reset.
	 */
	if ((control & BCM43455_RESET_CONTROL_RESET) == 0) {
		/*
		 * Sets the pre-reset control with the clock forced on, and
		 * reads it back to post the write.
		 */
		error = bcm43455_chip_write32(
			chip,
			core->wrapper + BCM43455_WRAP_IO_CONTROL,
			control_running | BCM43455_IO_CONTROL_FORCE_GATE | BCM43455_IO_CONTROL_CLOCK);
		if (error != 0)
			return error;
		error = bcm43455_chip_read32(chip, core->wrapper + BCM43455_WRAP_IO_CONTROL, &readback);
		if (error != 0)
			return error;

		/* Asserts reset and lets it take hold. */
		error = bcm43455_chip_write32(chip, core->wrapper + BCM43455_WRAP_RESET_CONTROL, BCM43455_RESET_CONTROL_RESET);
		if (error != 0)
			return error;
		error = chip->ops->sleep(chip->context, 1);
		if (error != 0)
			return error;
	}

	/*
	 * Sets the in-reset control with the clock forced on, and reads it
	 * back.
	 */
	error = bcm43455_chip_write32(
		chip,
		core->wrapper + BCM43455_WRAP_IO_CONTROL,
		control_held | BCM43455_IO_CONTROL_FORCE_GATE | BCM43455_IO_CONTROL_CLOCK);
	if (error != 0)
		return error;
	error = bcm43455_chip_read32(chip, core->wrapper + BCM43455_WRAP_IO_CONTROL, &readback);
	if (error != 0)
		return error;

	/* Succeeded: the core is in reset. */
	return 0;
}

/*
 * Resets a core from whatever state it is in and lets it run with the
 * post-reset I/O control.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_chip_ai_reset(), lines 1101-1125).
 */
static int
restart_core(
	struct bcm43455_chip *chip,
	const struct bcm43455_core *core,
	uint32_t control_running,
	uint32_t control_held,
	uint32_t control_released)
{
	uint32_t control;
	uint32_t readback;
	unsigned tries;
	int error;

	/* Puts the core into reset first, which works from any state. */
	error = hold_core_in_reset(chip, core, control_running, control_held);
	if (error != 0)
		return error;

	/* Lets go of reset until the core reports it released. */
	for (tries = 0;
	     ;
	     tries++) {
		/* Reads whether the reset still holds. */
		error = bcm43455_chip_read32(chip, core->wrapper + BCM43455_WRAP_RESET_CONTROL, &control);
		if (error != 0)
			return error;
		if ((control & BCM43455_RESET_CONTROL_RESET) == 0)
			break;

		/* A core that never comes out of reset is given up on. */
		if (tries == RESET_RELEASE_TRIES) {
			kern_logf("bcm43455: core %03x stays in reset\n", (unsigned)core->id);
			return EIO;
		}

		/* Releases the reset and gives it a moment. */
		error = bcm43455_chip_write32(chip, core->wrapper + BCM43455_WRAP_RESET_CONTROL, 0);
		if (error != 0)
			return error;
		error = chip->ops->sleep(chip->context, 1);
		if (error != 0)
			return error;
	}

	/*
	 * Sets the running control with the clock on and the forced gate off,
	 * and reads it back.
	 */
	error = bcm43455_chip_write32(chip, core->wrapper + BCM43455_WRAP_IO_CONTROL, control_released | BCM43455_IO_CONTROL_CLOCK);
	if (error != 0)
		return error;
	error = bcm43455_chip_read32(chip, core->wrapper + BCM43455_WRAP_IO_CONTROL, &readback);
	if (error != 0)
		return error;

	/* Succeeded: the core runs. */
	return 0;
}

/*
 * Leaves the ARM core out of reset but halted, and the radio core in
 * reset with its PHY clocked, so the RAM can be written.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_chip_cr4_set_passive(), lines 1288-1306).
 */
static int
halt_cores(
	struct bcm43455_chip *chip)
{
	uint32_t control;
	uint32_t halt_kept;
	int error;

	/*
	 * Resets the ARM core, keeping only its halt bit across, and leaves it
	 * halted.
	 */
	error = bcm43455_chip_read32(chip, chip->cr4.wrapper + BCM43455_WRAP_IO_CONTROL, &control);
	if (error != 0)
		return error;
	halt_kept = control & BCM43455_CR4_IO_CPU_HALT;
	error = restart_core(chip, &chip->cr4, halt_kept, BCM43455_CR4_IO_CPU_HALT, BCM43455_CR4_IO_CPU_HALT);
	if (error != 0)
		return error;

	/*
	 * Puts the radio core into reset, its PHY reset first and its clock
	 * kept.
	 */
	error = hold_core_in_reset(
		chip,
		&chip->d11,
		BCM43455_D11_IO_PHY_RESET | BCM43455_D11_IO_PHY_CLOCK,
		BCM43455_D11_IO_PHY_CLOCK);
	if (error != 0)
		return error;

	/* Succeeded: nothing runs from RAM. */
	return 0;
}

/*
 * Finds the PMU (a core of its own on a chip that says so, else part of
 * the chip common core), reads its revision, and reads whether the chip
 * runs save-restore.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_chip_get_pmu(), lines 1027-1040, and bwfm_chip_sr_capable(), lines
 * 1384-1401).
 */
static int
read_power_unit(
	struct bcm43455_chip *chip)
{
	uint32_t capability;
	uint32_t control;
	int separate;
	int error;

	/*
	 * A chip common core of revision 35 or later with the AOB bit has a PMU
	 * core of its own.
	 */
	separate = 0;
	if (chip->chipcommon.revision >= BCM43455_CHIPCOMMON_REVISION_AOB) {
		if ((chip->capability_ext & BCM43455_CAPABILITY_EXT_AOB) != 0 && chip->pmu.base != 0)
			separate = 1;
	}

	/*
	 * The PMU's registers are in its own core, else in the chip common
	 * core.
	 */
	chip->pmu_base = BCM43455_CHIPCOMMON_BASE;
	if (separate)
		chip->pmu_base = chip->pmu.base;

	/* A chip without a PMU has no revision and no save-restore. */
	chip->pmu_revision = 0;
	chip->save_restore = 0;
	if ((chip->capability & BCM43455_CAPABILITY_PMU) == 0) {
		kern_logf("bcm43455: no PMU\n");
		return 0;
	}

	/* Reads the PMU's revision from its capabilities. */
	error = bcm43455_chip_read32(chip, chip->pmu_base + BCM43455_PMU_CAPABILITY, &capability);
	if (error != 0)
		return error;
	chip->pmu_revision = capability & BCM43455_PMU_REVISION_MASK;

	/*
	 * On the 4345 save-restore is bit 2 of chip control register 3, from
	 * PMU revision 17.
	 */
	if (chip->pmu_revision >= BCM43455_PMU_REVISION_SR) {
		error = bcm43455_chip_write32(chip, chip->pmu_base + BCM43455_PMU_CHIP_CONTROL_ADDRESS, BCM43455_PMU_CHIP_CONTROL_SR);
		if (error != 0)
			return error;
		error = bcm43455_chip_read32(chip, chip->pmu_base + BCM43455_PMU_CHIP_CONTROL_DATA, &control);
		if (error != 0)
			return error;
		if ((control & BCM43455_SAVE_RESTORE_BIT) != 0)
			chip->save_restore = 1;
	}

	/* Succeeded: logs what the board's first bring-up must confirm. */
	kern_logf(
		"bcm43455: PMU at %08x revision %u, save-restore %u\n",
		chip->pmu_base,
		chip->pmu_revision,
		chip->save_restore);
	return 0;
}

/*
 * Keeps the SDIO interface awake, lets an SDIO reset reset the WLAN
 * backplane, reloads the PMU's resources, and leaves the frame function
 * off and the chip on its slow clock.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44 (the
 * attach after bwfm_chip_attach(), lines 311-334).
 */
static int
prepare_interface(
	struct bcm43455_chip *chip)
{
	uint32_t control;
	uint8_t sleep;
	uint8_t card;
	int error;

	/*
	 * From SDIO core revision 12 the interface sleeps unless told to keep
	 * SDIO on.
	 */
	if (chip->sdio.revision >= BCM43455_SDIO_REVISION_KSO) {
		error = chip->ops->read_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_SLEEP_CONTROL, &sleep);
		if (error != 0)
			return error;
		if ((sleep & BCM43455_SLEEP_KEEP_SDIO_ON) == 0) {
			error = chip->ops->write_byte(
				chip->context,
				BCM43455_FUNCTION_BACKPLANE,
				BCM43455_F1_SLEEP_CONTROL,
				(uint8_t)(sleep | BCM43455_SLEEP_KEEP_SDIO_ON));
			if (error != 0)
				return error;
		}
	}

	/* Makes an SDIO card reset also reset the WLAN backplane. */
	error = chip->ops->read_byte(chip->context, BCM43455_FUNCTION_COMMON, BCM43455_CCCR_CARD_CONTROL, &card);
	if (error != 0)
		return error;
	error = chip->ops->write_byte(
		chip->context,
		BCM43455_FUNCTION_COMMON,
		BCM43455_CCCR_CARD_CONTROL,
		(uint8_t)(card | BCM43455_CARD_CONTROL_WLAN_RESET));
	if (error != 0)
		return error;

	/* Reloads the PMU's resource tables. */
	if (chip->pmu_revision != 0) {
		error = bcm43455_chip_read32(chip, chip->pmu_base + BCM43455_PMU_CONTROL, &control);
		if (error != 0)
			return error;
		error = bcm43455_chip_write32(chip, chip->pmu_base + BCM43455_PMU_CONTROL, control | BCM43455_PMU_CONTROL_RESOURCE_RELOAD);
		if (error != 0)
			return error;
	}

	/* The frame function stays off until the firmware runs. */
	error = chip->ops->disable_function(chip->context, BCM43455_FUNCTION_FRAMES);
	if (error != 0)
		return error;

	/* Gives the clock requests back to the chip. */
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_CHIP_CLOCK, 0);
	if (error != 0)
		return error;

	/* Succeeded: the interface is ready for the download. */
	return 0;
}

/*
 * Asks for a clock (the slow ALP or the fast HT one) and waits for it: the
 * request alone, then up to HT_WAIT_MS.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_htclk()).
 */
static int
request_clock(
	struct bcm43455_chip *chip,
	uint8_t request,
	uint8_t available,
	const char *name)
{
	int error;

	/* Asks the chip for the clock. */
	error = chip->ops->write_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_CHIP_CLOCK, request);
	if (error != 0)
		return error;

	/* Waits until the chip says the clock runs. */
	error = wait_clock(chip, available, HT_WAIT_MS);
	if (error != 0) {
		kern_logf("bcm43455: %s clock not available (%d)\n", name, error);
		return error;
	}

	/* Succeeded: the chip runs on the clock. */
	return 0;
}

/*
 * Reads memory back, a piece at a time through the bounce buffer, and
 * compares it with what was written.
 */
static int
verify_memory(
	struct bcm43455_chip *chip,
	uint32_t address,
	const uint8_t *expected,
	size_t length)
{
	size_t done;
	size_t piece;
	int same;
	int error;

	/* Compares the range one bounce buffer at a time. */
	done = 0;
	while (done < length) {
		/* Reads the next piece back. */
		piece = length - done;
		if (piece > sizeof(chip->bounce))
			piece = sizeof(chip->bounce);
		error = bcm43455_chip_read_memory(chip, address + (uint32_t)done, chip->bounce, piece);
		if (error != 0) {
			kern_memset(chip->bounce, 0, sizeof(chip->bounce));
			return error;
		}

		/*
		 * Compares it, then clears the copy, which may hold NVRAM
		 * settings.
		 */
		same = kern_memcmp(chip->bounce, expected + done, piece);
		kern_memset(chip->bounce, 0, sizeof(chip->bounce));
		if (same != 0) {
			kern_logf("bcm43455: RAM at %06x does not read back what was written\n", address + (uint32_t)done);
			return EIO;
		}

		/* Moves past the piece. */
		done += piece;
	}

	/* Succeeded: the memory holds what was written. */
	return 0;
}

/*
 * Waits for a bit of the chip clock register in one-millisecond steps.
 */
static int
wait_clock(
	struct bcm43455_chip *chip,
	uint8_t mask,
	unsigned timeout_ms)
{
	uint8_t clock;
	unsigned waited;
	int error;

	/* Reads the register until the bit is set or the time is spent. */
	for (waited = 0;
	     ;
	     waited++) {
		/* Reads the clock state. */
		error = chip->ops->read_byte(chip->context, BCM43455_FUNCTION_BACKPLANE, BCM43455_F1_CHIP_CLOCK, &clock);
		if (error != 0)
			return error;
		if ((clock & mask) != 0)
			break;

		/* The clock did not come in time. */
		if (waited == timeout_ms)
			return ETIMEDOUT;

		/* Waits a millisecond. */
		error = chip->ops->sleep(chip->context, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the clock is there. */
	return 0;
}
