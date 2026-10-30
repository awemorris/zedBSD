/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 SDIO card layer.
 *
 * The commands, the card common control registers (CCCR), the function
 * basic registers (FBR) and the CIS tuples are those of the SD
 * Association's SDIO Simplified Specification (version 3.00).  An I/O card
 * is enumerated with CMD5 (operating voltage, until the card reports its
 * power-up done), CMD3 (relative address) and CMD7 (select), after which
 * every register is reached with CMD52, one byte at a time, or CMD53, a run
 * of bytes or blocks.
 *
 * The slot is found through the chip's device tree node, whose parent is
 * the controller.  On QEMU's raspi4b the same controller is the SD card,
 * and the SD card driver may have bound to it on a board too; in both
 * cases the slot is refused before any register is touched.
 */

#include "drivers/platform/rpi4/rpi4-sdio.h"
#include "drivers/platform/rpi4/rpi4-board.h"
#include <kern/klog.h>
#include <uapi/errno.h>

/* The commands of an I/O card. */
#define SDIO_CMD_SEND_RELATIVE_ADDR	3U
#define SDIO_CMD_IO_SEND_OP_COND	5U
#define SDIO_CMD_SELECT_CARD		7U
#define SDIO_CMD_IO_RW_DIRECT		52U
#define SDIO_CMD_IO_RW_EXTENDED		53U

/* The R4 answer to CMD5: power-up done, the function count, a memory part, the I/O OCR. */
#define R4_READY			0x80000000U
#define R4_FUNCTIONS_SHIFT		28U
#define R4_FUNCTIONS_MASK		0x7U
#define R4_MEMORY_PRESENT		0x08000000U
#define R4_VOLTAGES_MASK		0x00ffffffU

/* The OCR windows a 3.3 V bus offers: 3.2-3.3 V and 3.3-3.4 V. */
#define OCR_3V2_3V3			0x00100000U
#define OCR_3V3_3V4			0x00200000U

/* The R6 answer to CMD3: the address and its error bits (CRC, illegal, general). */
#define R6_RCA_SHIFT			16U
#define R6_ERRORS			0x0000e000U

/* The R1 answer to CMD7: its CRC, illegal command and general error bits. */
#define R1_ERRORS			0x00c80000U

/*
 * The R5 answer to CMD52 and CMD53: the flag byte above the data byte.
 * Errors are a CRC failure, an illegal command, a general error, a wrong
 * function number and an address out of range.
 */
#define R5_FLAGS_SHIFT			8U
#define R5_ERRORS			0xcbU
#define R5_DATA_MASK			0xffU

/* The CMD52 and CMD53 argument: write, function, RAW or block mode, increment, address, count. */
#define IO_WRITE			0x80000000U
#define IO_FUNCTION_SHIFT		28U
#define IO_BLOCK_MODE			0x08000000U
#define IO_INCREMENT			0x04000000U
#define IO_ADDRESS_SHIFT		9U
#define IO_ADDRESS_MAX			0x1ffffU
#define IO_BYTE_COUNT_MAX		512U
#define IO_BLOCK_COUNT_MAX		511U
#define IO_COUNT_MASK			0x1ffU

/* The CCCR, in function 0's space. */
#define CCCR_REVISION			0x00U
#define CCCR_IO_ENABLE			0x02U
#define CCCR_IO_READY			0x03U
#define CCCR_BUS_INTERFACE		0x07U
#define CCCR_CAPABILITY			0x08U
#define CCCR_CIS_POINTER		0x09U
#define CCCR_BLOCK_SIZE			0x10U

/* The revision byte: the CCCR's format below, the SDIO specification above. */
#define CCCR_REVISION_MASK		0x0fU
#define CCCR_SDIO_SHIFT			4U

/* The bus interface control's width field, and four lines in it. */
#define BUS_WIDTH_MASK			0x03U
#define BUS_WIDTH_4BIT			0x02U

/* The card capability: a low-speed card, and one that has four lines anyway. */
#define CAPABILITY_LOW_SPEED		0x40U
#define CAPABILITY_LOW_SPEED_4BIT	0x80U

/* Each function's FBR: 0x100 bytes per function, with its block size in it. */
#define FBR_STRIDE			0x100U
#define FBR_BLOCK_SIZE			0x10U

/* The CIS tuples this layer reads, and the link that ends a chain. */
#define CISTPL_NULL			0x00U
#define CISTPL_MANFID			0x20U
#define CISTPL_FUNCE			0x22U
#define CISTPL_END			0xffU
#define CIS_LINK_END			0xffU

/* The parts of the tuples: MANFID's four bytes, function 0's FUNCE. */
#define MANFID_LENGTH			4U
#define FUNCE_FUNCTION0			0x00U
#define FUNCE_FUNCTION0_LENGTH		4U

/* How much of the CIS is read: well beyond the tuples a chip's common CIS holds. */
#define CIS_WINDOW			256U

/* How long the card may take to answer CMD5 and to finish its power-up, and how often it is asked. */
#define CMD5_TIMEOUT_MS			1000U
#define CMD5_INTERVAL_MS		10U

/* How often a function's ready bit is looked at. */
#define READY_INTERVAL_MS		1U

/* The device tree's interrupt: a GIC SPI, level high, at 32 past the SGIs and PPIs. */
#define FDT_GIC_SPI			0U
#define FDT_IRQ_LEVEL_HIGH		4U
#define FDT_SPI_BASE			32U
#define FDT_SPI_MAX			987U

/* The highest GPIO function code. */
#define GPIO_FUNCTION_CODE_MAX		7U

static int node_compatible(const struct drv_fdt *fdt, uint32_t node, const char *name);
static int describe_interrupt(const struct drv_fdt *fdt, uint32_t node, struct drv_rpi4_sdio_slot *slot);
static int describe_pins(const struct drv_fdt *fdt, uint32_t node, struct drv_rpi4_sdio_slot *slot);
static int ask_operating_condition(struct drv_rpi4_sdio_card *card, const struct drv_rpi4_sdio_waiter *waiter);
static int address_card(struct drv_rpi4_sdio_card *card);
static int read_common_registers(struct drv_rpi4_sdio_card *card);
static int widen_bus(struct drv_rpi4_sdio_card *card);
static void read_cis(struct drv_rpi4_sdio_card *card);
static int send_command(struct drv_rpi4_sdio_card *card, unsigned index, uint32_t argument, enum drv_rpi4_sdio_response response, uint32_t *reply);
static int io_direct(struct drv_rpi4_sdio_card *card, int write, unsigned function, uint32_t address, uint8_t in, uint8_t *out);
static int block_size_address(struct drv_rpi4_sdio_card *card, unsigned function, uint32_t *address);
static int wait_ms(const struct drv_rpi4_sdio_waiter *waiter, unsigned milliseconds);

/*
 * Finds the Wi-Fi chip's SDIO slot in the device tree.
 */
int
drv_rpi4_sdio_describe_slot(
	const struct drv_fdt *fdt,
	uintptr_t sd_base,
	struct drv_rpi4_sdio_slot *slot)
{
	uint64_t size;
	uint32_t chip;
	uint32_t controller;
	bool emulated;
	bool enabled;
	int mmc;
	int sdhci;
	int error;

	/* Refuses a missing tree or nowhere to put the slot. */
	if (fdt == NULL || slot == NULL)
		return EINVAL;

	/* QEMU's raspi4b has no Wi-Fi chip, and its SD card sits on this controller. */
	emulated = drv_rpi4_board_emulated(fdt);
	if (emulated)
		return ENODEV;

	/* Finds the chip, which the tree describes as the controller's child. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm4329-fmac", DRV_FDT_NO_NODE, &chip);
	if (error != 0)
		return ENODEV;

	/* Takes the controller the chip hangs off. */
	error = drv_fdt_parent(fdt, chip, &controller);
	if (error != 0)
		return ENODEV;

	/* Refuses a slot an overlay switched off, such as disable-wifi. */
	enabled = drv_fdt_node_enabled(fdt, controller);
	if (!enabled)
		return ENODEV;
	enabled = drv_fdt_node_enabled(fdt, chip);
	if (!enabled)
		return ENODEV;

	/* Requires the legacy EMMC controller this host drives. */
	mmc = node_compatible(fdt, controller, "brcm,bcm2835-mmc");
	sdhci = node_compatible(fdt, controller, "brcm,bcm2835-sdhci");
	if (!mmc && !sdhci)
		return EINVAL;

	/* Reads where its registers are. */
	error = drv_fdt_reg(fdt, controller, 0, &slot->registers, &size);
	if (error != 0)
		return EINVAL;

	/* Refuses a register block smaller than the controller's. */
	if (size < DRV_RPI4_SDIO_HOST_REGISTER_SPAN)
		return EINVAL;

	/* Leaves the controller to the SD card driver that bound to it. */
	if (sd_base != 0 && (uint64_t)sd_base == slot->registers)
		return EBUSY;

	/* Reads the interrupt and the pins. */
	error = describe_interrupt(fdt, controller, slot);
	if (error != 0)
		return error;
	error = describe_pins(fdt, controller, slot);
	if (error != 0)
		return error;

	/* Succeeded: the slot is the chip's and nobody else uses it. */
	return 0;
}

/*
 * Decodes the answer to CMD5.
 */
int
drv_rpi4_sdio_decode_ocr(
	uint32_t reply,
	struct drv_rpi4_sdio_ocr *ocr)
{
	/* Refuses nowhere to put the answer. */
	if (ocr == NULL)
		return EINVAL;

	/* The card has finished its power-up when C is set. */
	ocr->ready = 0;
	if ((reply & R4_READY) != 0)
		ocr->ready = 1;

	/* The number of I/O functions besides function 0. */
	ocr->function_count = (reply >> R4_FUNCTIONS_SHIFT) & R4_FUNCTIONS_MASK;

	/* A combined card has an SD memory part as well. */
	ocr->memory_present = 0;
	if ((reply & R4_MEMORY_PRESENT) != 0)
		ocr->memory_present = 1;

	/* The voltage windows the card runs at. */
	ocr->voltages = reply & R4_VOLTAGES_MASK;

	/* Succeeded: ocr describes the card. */
	return 0;
}

/*
 * Picks the windows of a card's I/O OCR that the 3.3 V bus offers.
 */
int
drv_rpi4_sdio_voltage_window(
	uint32_t voltages,
	uint32_t *window)
{
	uint32_t usable;

	/* Refuses nowhere to put the answer. */
	if (window == NULL)
		return EINVAL;

	/* Keeps the windows around 3.3 V, the only voltage the slot has. */
	usable = voltages & (OCR_3V2_3V3 | OCR_3V3_3V4);
	if (usable == 0)
		return ENOTSUP;

	/* Succeeded: window is what CMD5 offers the card. */
	*window = usable;
	return 0;
}

/*
 * Decodes the answer to CMD52.
 */
int
drv_rpi4_sdio_decode_r5(
	uint32_t reply,
	uint8_t *data)
{
	uint32_t flags;

	/* Refuses an answer that reports an error, whatever its data byte holds. */
	flags = (reply >> R5_FLAGS_SHIFT) & 0xffU;
	if ((flags & R5_ERRORS) != 0)
		return EIO;

	/* Passes the data byte on when the caller wants it. */
	if (data != NULL)
		*data = (uint8_t)(reply & R5_DATA_MASK);

	/* Succeeded: the card carried the access out. */
	return 0;
}

/*
 * Walks a card information structure's tuple chain.
 */
int
drv_rpi4_sdio_parse_cis(
	const uint8_t *bytes,
	size_t length,
	struct drv_rpi4_sdio_cis *cis)
{
	size_t offset;
	unsigned code;
	unsigned link;
	const uint8_t *body;

	/* Refuses missing bytes or nowhere to put what they say. */
	if (bytes == NULL || cis == NULL)
		return EINVAL;

	/* Starts with nothing found. */
	cis->has_manufacturer = 0;
	cis->manufacturer = 0;
	cis->card = 0;
	cis->has_function0 = 0;
	cis->function0_block_max = 0;
	cis->speed_code = 0;

	/* Follows the chain tuple by tuple until its end. */
	offset = 0;
	while (offset < length) {
		/* The end tuple closes the chain. */
		code = bytes[offset];
		if (code == CISTPL_END)
			return 0;

		/* A null tuple is a single byte of padding. */
		if (code == CISTPL_NULL) {
			offset++;
			continue;
		}

		/* Refuses a tuple whose link byte lies beyond the bytes. */
		if (offset + 1U >= length)
			return EINVAL;

		/* A link of 0xff ends the chain at this tuple. */
		link = bytes[offset + 1U];
		if (link == CIS_LINK_END)
			return 0;

		/* Refuses a tuple whose body runs past the bytes. */
		if (offset + 2U + link > length)
			return EINVAL;
		body = bytes + offset + 2U;

		/* Reads the manufacturer and card codes, little-endian. */
		if (code == CISTPL_MANFID && link >= MANFID_LENGTH) {
			cis->manufacturer = (uint16_t)(body[0] | (body[1] << 8));
			cis->card = (uint16_t)(body[2] | (body[3] << 8));
			cis->has_manufacturer = 1;
		}

		/* Reads function 0's extension: its largest block and its fastest rate. */
		if (code == CISTPL_FUNCE && link >= FUNCE_FUNCTION0_LENGTH && body[0] == FUNCE_FUNCTION0) {
			cis->function0_block_max = (uint16_t)(body[1] | (body[2] << 8));
			cis->speed_code = body[3];
			cis->has_function0 = 1;
		}

		/* Moves to the next tuple. */
		offset += 2U + link;
	}

	/* Reports a chain that ran out of bytes before its end. */
	return EINVAL;
}

/*
 * Brings a freshly powered card to the selected state and reads it.
 */
int
drv_rpi4_sdio_enumerate(
	struct drv_rpi4_sdio_card *card,
	struct drv_rpi4_sdio_host *host,
	const struct drv_rpi4_sdio_waiter *waiter)
{
	unsigned i;
	int error;

	/* Refuses a missing card, host or waiter. */
	if (card == NULL || host == NULL || waiter == NULL)
		return EINVAL;

	/* Starts from a card that knows nothing. */
	card->host = host;
	card->rca = 0;
	card->cccr_revision = 0;
	card->sdio_revision = 0;
	card->capability = 0;
	card->cis_pointer = 0;
	card->cis.has_manufacturer = 0;
	card->cis.manufacturer = 0;
	card->cis.card = 0;
	card->cis.has_function0 = 0;
	card->cis.function0_block_max = 0;
	card->cis.speed_code = 0;
	for (i = 0; i <= DRV_RPI4_SDIO_FUNCTION_MAX; i++)
		card->block_size[i] = 0;

	/* Agrees on the voltage and waits for the card's power-up. */
	error = ask_operating_condition(card, waiter);
	if (error != 0)
		return error;

	/* Gives the card its address and selects it. */
	error = address_card(card);
	if (error != 0)
		return error;

	/* Reads the revision, the capability and where the CIS is. */
	error = read_common_registers(card);
	if (error != 0)
		return error;

	/* Moves to four lines and 25 MHz, unless the card is a low-speed one. */
	error = widen_bus(card);
	if (error != 0)
		return error;

	/* Reads the manufacturer and the card code; a CIS that does not parse is only logged. */
	read_cis(card);

	/* Succeeded: the card is selected and its common registers are known. */
	return 0;
}

/*
 * Reads one byte with CMD52.
 */
int
drv_rpi4_sdio_read_byte(
	struct drv_rpi4_sdio_card *card,
	unsigned function,
	uint32_t address,
	uint8_t *value)
{
	int error;

	/* Refuses nowhere to put the byte. */
	if (value == NULL)
		return EINVAL;

	/* Reads the byte. */
	error = io_direct(card, 0, function, address, 0, value);
	if (error != 0)
		return error;

	/* Succeeded: value holds the byte. */
	return 0;
}

/*
 * Writes one byte with CMD52.
 */
int
drv_rpi4_sdio_write_byte(
	struct drv_rpi4_sdio_card *card,
	unsigned function,
	uint32_t address,
	uint8_t value)
{
	int error;

	/* Writes the byte. */
	error = io_direct(card, 1, function, address, value, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the card holds the byte. */
	return 0;
}

/*
 * Moves bytes of a function's space with CMD53.
 */
int
drv_rpi4_sdio_transfer(
	struct drv_rpi4_sdio_card *card,
	enum drv_rpi4_sdio_data direction,
	unsigned function,
	uint32_t address,
	int increment,
	uint8_t *buffer,
	size_t length)
{
	struct drv_rpi4_sdio_command command;
	uint32_t argument;
	uint32_t count;
	unsigned block_size;
	uint8_t flags;
	int error;

	/* Refuses a missing card or buffer, nothing to move, or an unknown direction. */
	if (card == NULL || buffer == NULL || length == 0)
		return EINVAL;
	if (direction != DRV_RPI4_SDIO_DATA_READ && direction != DRV_RPI4_SDIO_DATA_WRITE)
		return EINVAL;

	/* Refuses a function the card does not have or an address beyond 17 bits. */
	if (function > card->ocr.function_count)
		return EINVAL;
	if (address > IO_ADDRESS_MAX)
		return EINVAL;

	/* Names the function, the address and whether it moves with the bytes. */
	argument = (uint32_t)function << IO_FUNCTION_SHIFT;
	argument |= address << IO_ADDRESS_SHIFT;
	if (increment)
		argument |= IO_INCREMENT;
	if (direction == DRV_RPI4_SDIO_DATA_WRITE)
		argument |= IO_WRITE;

	/*
	 * Moves up to one block in byte mode, a count of 512 written as zero;
	 * a longer run goes in block mode in the function's block size.
	 */
	block_size = card->block_size[function];
	if (length <= IO_BYTE_COUNT_MAX && (block_size == 0 || length <= block_size)) {
		count = (uint32_t)length & IO_COUNT_MASK;
		command.block_size = (unsigned)length;
		command.block_count = 1;
	} else {
		/* Refuses a run that is not whole blocks or is too long for one command. */
		if (block_size == 0)
			return EINVAL;
		if (length % block_size != 0)
			return EINVAL;
		if (length / block_size > IO_BLOCK_COUNT_MAX)
			return EINVAL;

		count = (uint32_t)(length / block_size);
		argument |= IO_BLOCK_MODE;
		command.block_size = block_size;
		command.block_count = (unsigned)count;
	}
	argument |= count;

	/* Describes the command and its data. */
	command.index = SDIO_CMD_IO_RW_EXTENDED;
	command.argument = argument;
	command.response = DRV_RPI4_SDIO_RESPONSE_SHORT;
	command.data = direction;
	command.buffer = buffer;
	command.reply = 0;

	/* Moves the bytes. */
	error = drv_rpi4_sdio_host_command(card->host, &command);
	if (error != 0)
		return error;

	/* Refuses a transfer the card reports as failed. */
	error = drv_rpi4_sdio_decode_r5(command.reply, &flags);
	if (error != 0)
		return error;

	/* Succeeded: the bytes have moved. */
	return 0;
}

/*
 * Enables a function and waits for it to report ready.
 */
int
drv_rpi4_sdio_enable_function(
	struct drv_rpi4_sdio_card *card,
	unsigned function,
	const struct drv_rpi4_sdio_waiter *waiter,
	unsigned timeout_ms)
{
	uint8_t enable;
	uint8_t ready;
	uint8_t bit;
	unsigned waited;
	int error;

	/* Refuses function 0, which is always on, or one the card does not have. */
	if (card == NULL || waiter == NULL)
		return EINVAL;
	if (function == 0 || function > card->ocr.function_count)
		return EINVAL;
	bit = (uint8_t)(1U << function);

	/* Sets the function's bit in the I/O enable register, keeping the others. */
	error = drv_rpi4_sdio_read_byte(card, 0, CCCR_IO_ENABLE, &enable);
	if (error != 0)
		return error;
	error = drv_rpi4_sdio_write_byte(card, 0, CCCR_IO_ENABLE, (uint8_t)(enable | bit));
	if (error != 0)
		return error;

	/* Waits for the function to report itself ready. */
	for (waited = 0; waited <= timeout_ms; waited += READY_INTERVAL_MS) {
		/* Finishes once the ready bit is up. */
		error = drv_rpi4_sdio_read_byte(card, 0, CCCR_IO_READY, &ready);
		if (error != 0)
			return error;
		if ((ready & bit) != 0)
			return 0;

		/* Gives the function a little longer. */
		error = wait_ms(waiter, READY_INTERVAL_MS);
		if (error != 0)
			return error;
	}

	/* Reports a function that never became ready. */
	return ETIMEDOUT;
}

/*
 * Disables a function.
 */
int
drv_rpi4_sdio_disable_function(
	struct drv_rpi4_sdio_card *card,
	unsigned function)
{
	uint8_t enable;
	uint8_t bit;
	int error;

	/* Refuses function 0 or one the card does not have. */
	if (card == NULL)
		return EINVAL;
	if (function == 0 || function > card->ocr.function_count)
		return EINVAL;
	bit = (uint8_t)(1U << function);

	/* Clears the function's bit in the I/O enable register, keeping the others. */
	error = drv_rpi4_sdio_read_byte(card, 0, CCCR_IO_ENABLE, &enable);
	if (error != 0)
		return error;
	error = drv_rpi4_sdio_write_byte(card, 0, CCCR_IO_ENABLE, (uint8_t)(enable & ~bit));
	if (error != 0)
		return error;

	/* Succeeded: the function is off. */
	return 0;
}

/*
 * Sets a function's block size and reads it back.
 */
int
drv_rpi4_sdio_set_block_size(
	struct drv_rpi4_sdio_card *card,
	unsigned function,
	unsigned size)
{
	uint32_t address;
	uint8_t low;
	uint8_t high;
	int error;

	/* Refuses a size outside 1 to 2048 bytes. */
	if (card == NULL)
		return EINVAL;
	if (size == 0 || size > DRV_RPI4_SDIO_BLOCK_SIZE_MAX)
		return EINVAL;

	/* Finds the function's block size register. */
	error = block_size_address(card, function, &address);
	if (error != 0)
		return error;

	/* Writes the size, low byte first. */
	error = drv_rpi4_sdio_write_byte(card, 0, address, (uint8_t)(size & 0xffU));
	if (error != 0)
		return error;
	error = drv_rpi4_sdio_write_byte(card, 0, address + 1U, (uint8_t)(size >> 8));
	if (error != 0)
		return error;

	/* Reads the size back. */
	error = drv_rpi4_sdio_read_byte(card, 0, address, &low);
	if (error != 0)
		return error;
	error = drv_rpi4_sdio_read_byte(card, 0, address + 1U, &high);
	if (error != 0)
		return error;

	/* Refuses a size the card did not take. */
	if (((unsigned)high << 8 | low) != size)
		return EIO;

	/* Succeeded: CMD53 in block mode moves blocks of this size. */
	card->block_size[function] = (uint16_t)size;
	return 0;
}

/*
 * Reports whether a node lists a name among its compatible strings.
 */
static int
node_compatible(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t offset;
	uint32_t i;
	int error;

	/* A node without the property lists nothing. */
	error = drv_fdt_property(fdt, node, "compatible", &value, &length);
	if (error != 0)
		return 0;

	/* Compares each NUL-terminated string of the list with the name. */
	offset = 0;
	while (offset < length) {
		/* Compares this string byte by byte, up to its NUL. */
		for (i = 0; offset + i < length; i++) {
			/* A difference ends this string's comparison. */
			if (value[offset + i] != (uint8_t)name[i])
				break;

			/* Both ended together: the name is listed. */
			if (name[i] == '\0')
				return 1;
		}

		/* Skips to the string after this one's NUL. */
		while (offset < length && value[offset] != 0)
			offset++;
		offset++;
	}

	/* Reports a name the node does not list. */
	return 0;
}

/*
 * Reads the controller's interrupt: a level-high GIC SPI, numbered for
 * the kernel 32 past the SGIs and PPIs.
 */
static int
describe_interrupt(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_rpi4_sdio_slot *slot)
{
	const uint8_t *value;
	uint32_t length;
	uint64_t cell;
	int error;

	/* Requires the three cells of the GIC binding. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return EINVAL;
	if (length < 12U)
		return EINVAL;

	/* Accepts a shared peripheral interrupt only. */
	cell = drv_fdt_cells_value(value, 0, 1);
	if (cell != FDT_GIC_SPI)
		return EINVAL;

	/* Restricts the SPI to the GIC's range. */
	cell = drv_fdt_cells_value(value, 1, 1);
	if (cell > FDT_SPI_MAX)
		return EINVAL;
	slot->irq = (int)cell + (int)FDT_SPI_BASE;

	/* Accepts the level-high line the dispatcher programs. */
	cell = drv_fdt_cells_value(value, 2, 1);
	if (cell != FDT_IRQ_LEVEL_HIGH)
		return EINVAL;

	/* Succeeded: irq names the controller's line. */
	return 0;
}

/*
 * Reads the pin group the controller's pinctrl-0 names: the pins, their
 * function and their pulls.
 */
static int
describe_pins(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_rpi4_sdio_slot *slot)
{
	const uint8_t *value;
	const uint8_t *pull_value;
	uint32_t length;
	uint32_t pull_length;
	uint32_t group;
	uint64_t cell;
	unsigned count;
	unsigned pull_count;
	unsigned i;
	int error;

	/* Finds the pin group through its phandle. */
	error = drv_fdt_property(fdt, node, "pinctrl-0", &value, &length);
	if (error != 0 || length < 4U)
		return EINVAL;
	cell = drv_fdt_cells_value(value, 0, 1);
	error = drv_fdt_find_phandle(fdt, (uint32_t)cell, &group);
	if (error != 0)
		return EINVAL;

	/* Reads the pins, one cell each, up to the slot's six. */
	error = drv_fdt_property(fdt, group, "brcm,pins", &value, &length);
	if (error != 0 || length == 0 || length % 4U != 0)
		return EINVAL;
	count = length / 4U;
	if (count > DRV_RPI4_SDIO_SLOT_PIN_MAX)
		return EINVAL;
	for (i = 0; i < count; i++) {
		/* Refuses a pin the GPIO block does not have. */
		cell = drv_fdt_cells_value(value, i, 1);
		if (cell >= DRV_RPI4_GPIO_PIN_COUNT)
			return EINVAL;
		slot->pins[i] = (unsigned)cell;
	}
	slot->pin_count = count;

	/* Reads the one function the group gives every pin. */
	error = drv_fdt_property(fdt, group, "brcm,function", &value, &length);
	if (error != 0 || length != 4U)
		return EINVAL;
	cell = drv_fdt_cells_value(value, 0, 1);
	if (cell > GPIO_FUNCTION_CODE_MAX)
		return EINVAL;
	slot->function = (enum drv_rpi4_gpio_function)cell;

	/* Reads the pulls: one for every pin, or one for all. */
	error = drv_fdt_property(fdt, group, "brcm,pull", &pull_value, &pull_length);
	if (error != 0 || pull_length == 0 || pull_length % 4U != 0)
		return EINVAL;
	pull_count = pull_length / 4U;
	if (pull_count != 1U && pull_count != count)
		return EINVAL;
	for (i = 0; i < count; i++) {
		/* Takes this pin's pull, or the group's one pull. */
		cell = drv_fdt_cells_value(pull_value, 0, 1);
		if (pull_count == count)
			cell = drv_fdt_cells_value(pull_value, i, 1);

		/* Converts the tree's number, refusing one it does not define. */
		error = drv_rpi4_gpio_pull_from_dt((uint32_t)cell, &slot->pulls[i]);
		if (error != 0)
			return EINVAL;
	}

	/* Succeeded: the slot knows how its bus reaches the chip. */
	return 0;
}

/*
 * Asks the card for its voltage range, offers it the 3.3 V windows and
 * waits for it to finish its power-up.
 */
static int
ask_operating_condition(
	struct drv_rpi4_sdio_card *card,
	const struct drv_rpi4_sdio_waiter *waiter)
{
	uint32_t reply;
	uint32_t window;
	unsigned waited;
	int error;

	/*
	 * Asks with no voltage, which only inquires.  A chip just out of
	 * reset may not answer yet, so a timeout is asked again.
	 */
	error = ETIMEDOUT;
	reply = 0;
	for (waited = 0; waited <= CMD5_TIMEOUT_MS; waited += CMD5_INTERVAL_MS) {
		/* Stops at an answer or at an error other than silence. */
		error = send_command(card, SDIO_CMD_IO_SEND_OP_COND, 0, DRV_RPI4_SDIO_RESPONSE_SHORT_UNCHECKED, &reply);
		if (error != ETIMEDOUT)
			break;

		/* Gives the chip a little longer to come up. */
		error = wait_ms(waiter, CMD5_INTERVAL_MS);
		if (error != 0)
			return error;
		error = ETIMEDOUT;
	}

	/* Reports a card that never answered, or answered with an error. */
	if (error != 0) {
		kern_logf("rpi4-sdio: no answer to CMD5 (%d)\n", error);
		return error;
	}

	/* Refuses a card without I/O functions: it is not an SDIO card. */
	(void)drv_rpi4_sdio_decode_ocr(reply, &card->ocr);
	if (card->ocr.function_count == 0) {
		kern_logf("rpi4-sdio: card without I/O functions (R4 %08x)\n", reply);
		return ENODEV;
	}

	/* Picks the windows around 3.3 V. */
	error = drv_rpi4_sdio_voltage_window(card->ocr.voltages, &window);
	if (error != 0) {
		kern_logf("rpi4-sdio: card does not run at 3.3 V (OCR %06x)\n", card->ocr.voltages);
		return error;
	}

	/* Offers the windows until the card reports its power-up done. */
	for (waited = 0; waited <= CMD5_TIMEOUT_MS; waited += CMD5_INTERVAL_MS) {
		/* Finishes once the card is ready. */
		error = send_command(card, SDIO_CMD_IO_SEND_OP_COND, window, DRV_RPI4_SDIO_RESPONSE_SHORT_UNCHECKED, &reply);
		if (error != 0)
			return error;
		(void)drv_rpi4_sdio_decode_ocr(reply, &card->ocr);
		if (card->ocr.ready)
			return 0;

		/* Gives the power-up a little longer. */
		error = wait_ms(waiter, CMD5_INTERVAL_MS);
		if (error != 0)
			return error;
	}

	/* Reports a card that never finished its power-up. */
	kern_logf("rpi4-sdio: card power-up not done (R4 %08x)\n", reply);
	return ETIMEDOUT;
}

/*
 * Gives the card its relative address (CMD3) and selects it (CMD7).
 */
static int
address_card(
	struct drv_rpi4_sdio_card *card)
{
	uint32_t reply;
	int error;

	/* Asks the card to publish an address. */
	error = send_command(card, SDIO_CMD_SEND_RELATIVE_ADDR, 0, DRV_RPI4_SDIO_RESPONSE_SHORT, &reply);
	if (error != 0)
		return error;

	/* Refuses an answer with an error bit or the address zero, which names no card. */
	if ((reply & R6_ERRORS) != 0)
		return EIO;
	card->rca = (uint16_t)(reply >> R6_RCA_SHIFT);
	if (card->rca == 0)
		return EIO;

	/* Selects the card, which then answers CMD52 and CMD53. */
	error = send_command(card, SDIO_CMD_SELECT_CARD, (uint32_t)card->rca << R6_RCA_SHIFT, DRV_RPI4_SDIO_RESPONSE_SHORT_BUSY, &reply);
	if (error != 0)
		return error;

	/* Refuses a selection the card reports as failed. */
	if ((reply & R1_ERRORS) != 0)
		return EIO;

	/* Succeeded: the card is in its command state. */
	return 0;
}

/*
 * Reads the CCCR's revision, the card capability and the common CIS pointer.
 */
static int
read_common_registers(
	struct drv_rpi4_sdio_card *card)
{
	uint8_t revision;
	uint8_t pointer;
	unsigned i;
	int error;

	/* The CCCR format and SDIO specification revisions. */
	error = drv_rpi4_sdio_read_byte(card, 0, CCCR_REVISION, &revision);
	if (error != 0)
		return error;
	card->cccr_revision = revision & CCCR_REVISION_MASK;
	card->sdio_revision = revision >> CCCR_SDIO_SHIFT;

	/* What the card can do: whether it is a low-speed card, among others. */
	error = drv_rpi4_sdio_read_byte(card, 0, CCCR_CAPABILITY, &card->capability);
	if (error != 0)
		return error;

	/* Assembles the common CIS pointer, three bytes, least significant first. */
	card->cis_pointer = 0;
	for (i = 0; i < 3U; i++) {
		error = drv_rpi4_sdio_read_byte(card, 0, CCCR_CIS_POINTER + i, &pointer);
		if (error != 0)
			return error;
		card->cis_pointer |= (uint32_t)pointer << (8U * i);
	}

	/* Succeeded: the common registers are known. */
	return 0;
}

/*
 * Moves the card and the host to four lines and the bus to 25 MHz.
 *
 * A low-speed card stays at 400 kHz, on four lines only when it says it
 * has them.
 */
static int
widen_bus(
	struct drv_rpi4_sdio_card *card)
{
	uint8_t control;
	int low_speed;
	int four_lines;
	int error;

	/* Decides on the width and the rate from the card's capability. */
	low_speed = 0;
	if ((card->capability & CAPABILITY_LOW_SPEED) != 0)
		low_speed = 1;
	four_lines = 1;
	if (low_speed && (card->capability & CAPABILITY_LOW_SPEED_4BIT) == 0)
		four_lines = 0;

	/* Tells the card to use four lines, keeping the rest of the bus interface control. */
	if (four_lines) {
		error = drv_rpi4_sdio_read_byte(card, 0, CCCR_BUS_INTERFACE, &control);
		if (error != 0)
			return error;
		control = (uint8_t)((control & ~BUS_WIDTH_MASK) | BUS_WIDTH_4BIT);
		error = drv_rpi4_sdio_write_byte(card, 0, CCCR_BUS_INTERFACE, control);
		if (error != 0)
			return error;

		/* Follows the card on the host's side. */
		error = drv_rpi4_sdio_host_set_bus_width(card->host, 4);
		if (error != 0)
			return error;
	}

	/* A low-speed card keeps the enumeration clock. */
	if (low_speed)
		return 0;

	/* Runs a full-speed card at 25 MHz. */
	error = drv_rpi4_sdio_host_set_clock(card->host, DRV_RPI4_SDIO_HOST_DEFAULT_HZ);
	if (error != 0)
		return error;

	/* Succeeded: the bus is as wide and as fast as the card allows. */
	return 0;
}

/*
 * Reads the common CIS window with CMD52 and walks its tuples.  A CIS that
 * cannot be read or parsed leaves the codes unknown and is only logged:
 * the bring-up after it checks the chip by its own registers.
 */
static void
read_cis(
	struct drv_rpi4_sdio_card *card)
{
	uint8_t bytes[CIS_WINDOW];
	unsigned i;
	int error;

	/* Refuses a pointer outside function 0's space. */
	if (card->cis_pointer == 0 || card->cis_pointer + CIS_WINDOW > IO_ADDRESS_MAX) {
		kern_logf("rpi4-sdio: CIS pointer %06x out of range\n", card->cis_pointer);
		return;
	}

	/* Reads the window one byte at a time. */
	for (i = 0; i < CIS_WINDOW; i++) {
		error = drv_rpi4_sdio_read_byte(card, 0, card->cis_pointer + i, &bytes[i]);
		if (error != 0) {
			kern_logf("rpi4-sdio: CIS byte %u not read (%d)\n", i, error);
			return;
		}
	}

	/* Walks the tuples for the codes. */
	error = drv_rpi4_sdio_parse_cis(bytes, sizeof(bytes), &card->cis);
	if (error != 0)
		kern_logf("rpi4-sdio: CIS at %06x does not parse (%d)\n", card->cis_pointer, error);
}

/*
 * Sends a command without data and takes its reply.
 */
static int
send_command(
	struct drv_rpi4_sdio_card *card,
	unsigned index,
	uint32_t argument,
	enum drv_rpi4_sdio_response response,
	uint32_t *reply)
{
	struct drv_rpi4_sdio_command command;
	int error;

	/* Describes a command without data. */
	command.index = index;
	command.argument = argument;
	command.response = response;
	command.data = DRV_RPI4_SDIO_DATA_NONE;
	command.buffer = NULL;
	command.block_size = 0;
	command.block_count = 0;
	command.reply = 0;

	/* Sends it. */
	error = drv_rpi4_sdio_host_command(card->host, &command);
	if (error != 0)
		return error;

	/* Succeeded: reply holds the card's answer. */
	*reply = command.reply;
	return 0;
}

/*
 * Reads or writes one byte with CMD52.
 */
static int
io_direct(
	struct drv_rpi4_sdio_card *card,
	int write,
	unsigned function,
	uint32_t address,
	uint8_t in,
	uint8_t *out)
{
	uint32_t argument;
	uint32_t reply;
	int error;

	/* Refuses a function the card does not have or an address beyond 17 bits. */
	if (card == NULL)
		return EINVAL;
	if (function > card->ocr.function_count)
		return EINVAL;
	if (address > IO_ADDRESS_MAX)
		return EINVAL;

	/* Names the function, the address and, for a write, the byte. */
	argument = (uint32_t)function << IO_FUNCTION_SHIFT;
	argument |= address << IO_ADDRESS_SHIFT;
	if (write) {
		argument |= IO_WRITE;
		argument |= in;
	}

	/* Sends CMD52. */
	error = send_command(card, SDIO_CMD_IO_RW_DIRECT, argument, DRV_RPI4_SDIO_RESPONSE_SHORT, &reply);
	if (error != 0)
		return error;

	/* Refuses an access the card reports as failed, and takes the byte read. */
	error = drv_rpi4_sdio_decode_r5(reply, out);
	if (error != 0)
		return error;

	/* Succeeded: the byte has moved. */
	return 0;
}

/*
 * Finds a function's block size register: in the CCCR for function 0,
 * in its FBR for the others.
 */
static int
block_size_address(
	struct drv_rpi4_sdio_card *card,
	unsigned function,
	uint32_t *address)
{
	/* Refuses a function the card does not have. */
	if (function > card->ocr.function_count)
		return EINVAL;

	/* Function 0 keeps its size among the common registers. */
	if (function == 0) {
		*address = CCCR_BLOCK_SIZE;
		return 0;
	}

	/* Succeeded: the size is in the function's FBR. */
	*address = FBR_STRIDE * function + FBR_BLOCK_SIZE;
	return 0;
}

/*
 * Waits through the caller's waiter.
 */
static int
wait_ms(
	const struct drv_rpi4_sdio_waiter *waiter,
	unsigned milliseconds)
{
	int error;

	/* Sleeps, or learns that the caller is stopping. */
	error = waiter->sleep(waiter->context, milliseconds);
	if (error != 0)
		return error;

	/* Succeeded: the time has passed. */
	return 0;
}
