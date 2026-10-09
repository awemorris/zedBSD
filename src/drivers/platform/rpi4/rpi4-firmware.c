/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 firmware property interface.
 *
 * A request is a buffer of 32-bit words in RAM: its size, a request code,
 * one tag with its value buffer, and an end tag.  The ARM side posts the
 * buffer's VideoCore bus address, with the property channel in its low four
 * bits, to mailbox 1 and waits until the same word comes back through
 * mailbox 0; the firmware has then written its answer into the buffer.  The
 * VideoCore reads RAM through an alias that bypasses the ARM caches, so the
 * buffer is cleaned before it is posted and invalidated before it is read.
 *
 * The HAL used the same mailbox to set up the framebuffer before the kernel
 * started and does not use it afterwards, so this driver owns it now.
 */

#include "drivers/platform/rpi4/rpi4-firmware.h"
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* Mailbox 0 carries answers to the ARM side; mailbox 1 carries requests. */
#define MAILBOX_READ			0x00U
#define MAILBOX_READ_STATUS		0x18U
#define MAILBOX_WRITE			0x20U
#define MAILBOX_WRITE_STATUS		0x38U
#define MAILBOX_FULL			0x80000000U
#define MAILBOX_EMPTY			0x40000000U
#define MAILBOX_CHANNEL_PROPERTY	8U

/* The mailbox registers the driver needs, up to mailbox 1's status. */
#define MAILBOX_REGISTER_SPAN		0x3cU

/*
 * The VideoCore sees ARM RAM below 1 GiB at this bus address, uncached.
 * The request buffer must therefore lie below 1 GiB.
 */
#define FIRMWARE_BUS_ALIAS		0xc0000000U
#define FIRMWARE_BUFFER_LIMIT		0x3fffffffULL
#define FIRMWARE_BUFFER_SIZE		4096U

/* Request and answer codes of the buffer and of its tag. */
#define PROPERTY_REQUEST		0x00000000U
#define PROPERTY_SUCCESS		0x80000000U
#define PROPERTY_TAG_ANSWERED		0x80000000U
#define PROPERTY_TAG_LENGTH		0x7fffffffU
#define PROPERTY_END_TAG		0x00000000U

/* The words of the buffer before the tag's values. */
#define PROPERTY_WORD_SIZE		0U
#define PROPERTY_WORD_CODE		1U
#define PROPERTY_WORD_TAG		2U
#define PROPERTY_WORD_CAPACITY		3U
#define PROPERTY_WORD_TAG_CODE		4U
#define PROPERTY_WORD_VALUES		5U

/* How often and how long the mailbox is polled: 10 us for up to one second. */
#define MAILBOX_POLL_US			10U
#define MAILBOX_POLL_MAX_US		20U
#define MAILBOX_POLL_LIMIT		100000U

/* Where the VL805's PCI address sits in the notification's value. */
#define XHCI_RESET_BUS_SHIFT		20U
#define XHCI_RESET_DEVICE_SHIFT		15U
#define XHCI_RESET_FUNCTION_SHIFT	12U

/*
 * The mailbox registers.
 *
 * Mapped once by drv_rpi4_firmware_init() and kept for as long as the kernel
 * runs; NULL until then, which makes every request fail with ENODEV.
 */
static volatile uint8_t *mailbox;

/*
 * The request buffer and its physical address.
 *
 * One page below 1 GiB, allocated with the mailbox and never freed.  Only
 * the request in flight uses it; busy says whether one is.
 */
static uint32_t *message;
static uint64_t message_physical;

/*
 * Whether a request is in flight.
 *
 * It is set and cleared with interrupts disabled, so a second caller gets
 * EBUSY instead of overwriting the buffer the firmware is reading.
 */
static bool busy;

static int claim_buffer(void);
static void release_buffer(void);
static void build_request(uint32_t tag, const uint32_t *values, unsigned request_count, unsigned capacity);
static int post_request(void);
static int wait_mailbox(unsigned status_offset, uint32_t busy_bit);
static int read_answer(uint32_t *values, unsigned capacity, uint32_t *answered);

/*
 * Finds the mailbox in the device tree and prepares the request buffer.
 */
int
drv_rpi4_firmware_init(
	const struct drv_fdt *fdt)
{
	struct kern_pmem run;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	bool enabled;
	void *registers;
	int error;

	/* Does nothing the second time. */
	if (mailbox != NULL)
		return 0;

	/* Finds the mailbox the firmware's tree describes. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2835-mbox", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;

	/* Refuses a disabled mailbox. */
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* Reads where its registers are and refuses a block too small. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < MAILBOX_REGISTER_SPAN)
		return EINVAL;

	/* Allocates the request buffer below the VideoCore's 1 GiB alias. */
	error = kern_pmem_alloc_limited(FIRMWARE_BUFFER_SIZE, FIRMWARE_BUFFER_SIZE, FIRMWARE_BUFFER_LIMIT, 0, &run);
	if (error != 0)
		return error;

	/* Maps the registers; the buffer goes back if that fails. */
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &registers);
	if (error != 0) {
		(void)kern_pmem_free(&run);
		return error;
	}

	/* Succeeded: publishes the buffer, then the mailbox that makes requests possible. */
	message = kern_pmem_to_kernel(run.paddr);
	message_physical = run.paddr;
	mailbox = registers;
	return 0;
}

/*
 * Sends one property tag and waits for the answer.
 */
int
drv_rpi4_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered)
{
	int error;

	/* Requires answer metadata even for a notification without values. */
	if (answered == NULL)
		return EINVAL;

	/* Nonempty value buffers require storage; zero-capacity tags carry none. */
	if (capacity != 0 && values == NULL)
		return EINVAL;
	if (capacity > DRV_RPI4_FIRMWARE_MAX_VALUES)
		return EINVAL;
	if (request_count > capacity)
		return EINVAL;

	/* Refuses a request before the mailbox is known. */
	if (mailbox == NULL)
		return ENODEV;

	/* Takes the buffer for this request. */
	error = claim_buffer();
	if (error != 0)
		return error;

	/* Builds the request and hands it to the firmware. */
	build_request(tag, values, request_count, capacity);
	error = post_request();
	if (error != 0) {
		release_buffer();
		return error;
	}

	/* Reads the firmware's answer back and gives the buffer up. */
	error = read_answer(values, capacity, answered);
	release_buffer();

	/* Reports a refused or unknown request. */
	if (error != 0)
		return error;

	/* Succeeded: values holds the answer. */
	return 0;
}

/*
 * Asks the firmware to load the USB controller's firmware at a PCI address.
 */
int
drv_rpi4_firmware_notify_xhci_reset(
	const struct drv_pci_address *address)
{
	uint32_t value;
	uint32_t answered;
	int error;

	/* Refuses a missing address. */
	if (address == NULL)
		return EINVAL;

	/* Names the controller as the firmware expects: bus, device and function. */
	value = (uint32_t)address->bus << XHCI_RESET_BUS_SHIFT;
	value |= (uint32_t)address->device << XHCI_RESET_DEVICE_SHIFT;
	value |= (uint32_t)address->function << XHCI_RESET_FUNCTION_SHIFT;

	/* Sends the notification; the firmware returns once the load is done. */
	error = drv_rpi4_firmware_property(DRV_RPI4_FIRMWARE_TAG_NOTIFY_XHCI_RESET, &value, 1U, 1U, &answered);
	if (error != 0)
		return error;

	/* Succeeded: the controller has its firmware. */
	return 0;
}

/* Marks the buffer as used by one request, or reports another in flight. */
static int
claim_buffer(void)
{
	bool enabled;
	bool taken;

	/* Tests and sets the flag with interrupts held off. */
	enabled = kern_irq_disable();
	taken = busy;
	busy = true;
	if (enabled)
		kern_irq_enable();

	/* Refuses a second request while one is in flight. */
	if (taken)
		return EBUSY;

	/* Succeeded: the buffer belongs to this request. */
	return 0;
}

/* Gives the buffer back after a request. */
static void
release_buffer(void)
{
	bool enabled;

	/* Clears the flag with interrupts held off. */
	enabled = kern_irq_disable();
	busy = false;
	if (enabled)
		kern_irq_enable();
}

/*
 * Lays out one tag in the request buffer.
 *
 * The buffer holds its size, the request code, the tag, the tag's value
 * buffer size, the tag's request code, the values and the end tag.
 */
static void
build_request(
	uint32_t tag,
	const uint32_t *values,
	unsigned request_count,
	unsigned capacity)
{
	unsigned words;
	unsigned index;

	/* The buffer's size is a whole number of 16-byte units. */
	words = PROPERTY_WORD_VALUES + capacity + 1U;
	words = (words + 3U) & ~3U;
	kern_memset(message, 0, words * 4U);

	/* Writes the header of the buffer and of the tag. */
	message[PROPERTY_WORD_SIZE] = words * 4U;
	message[PROPERTY_WORD_CODE] = PROPERTY_REQUEST;
	message[PROPERTY_WORD_TAG] = tag;
	message[PROPERTY_WORD_CAPACITY] = capacity * 4U;
	message[PROPERTY_WORD_TAG_CODE] = PROPERTY_REQUEST;

	/* Copies the request's values; the rest of the value buffer stays zero. */
	for (index = 0; index < request_count; index++)
		message[PROPERTY_WORD_VALUES + index] = values[index];

	/* Ends the tag list. */
	message[PROPERTY_WORD_VALUES + capacity] = PROPERTY_END_TAG;
}

/*
 * Posts the request buffer and waits until the firmware hands it back.
 *
 * Words that come back for other channels are dropped; nothing else in the
 * kernel uses the mailbox.
 */
static int
post_request(void)
{
	uint32_t request;
	uint32_t answer;
	unsigned attempts;
	int error;

	/* Makes the request visible to the VideoCore, which reads RAM uncached. */
	kern_dcache_clean_range(message, message[PROPERTY_WORD_SIZE]);

	/* Waits for room in mailbox 1 and posts the buffer's bus address. */
	request = ((uint32_t)message_physical | FIRMWARE_BUS_ALIAS) | MAILBOX_CHANNEL_PROPERTY;
	error = wait_mailbox(MAILBOX_WRITE_STATUS, MAILBOX_FULL);
	if (error != 0)
		return error;

	/* Posts the request. */
	kern_mmio_write32(mailbox + MAILBOX_WRITE, request);

	/* Reads mailbox 0 until the request comes back. */
	for (attempts = 0; attempts < MAILBOX_POLL_LIMIT; attempts++) {
		/* Waits for a word in mailbox 0. */
		error = wait_mailbox(MAILBOX_READ_STATUS, MAILBOX_EMPTY);
		if (error != 0)
			return error;

		/* Stops when the word is this request's. */
		answer = kern_mmio_read32(mailbox + MAILBOX_READ);
		if (answer == request)
			break;
	}

	/* Reports a firmware that answered only other channels. */
	if (attempts == MAILBOX_POLL_LIMIT)
		return ETIMEDOUT;

	/* Succeeded: the firmware has written its answer. */
	return 0;
}

/*
 * Waits until a status bit that blocks the next step clears.
 *
 * Gives up after one second of polling.
 */
static int
wait_mailbox(
	unsigned status_offset,
	uint32_t busy_bit)
{
	uint32_t status;
	unsigned polls;

	/* Polls the status until the bit clears or the limit passes. */
	status = 0;
	for (polls = 0; polls < MAILBOX_POLL_LIMIT; polls++) {
		/* Stops as soon as the mailbox is ready. */
		status = kern_mmio_read32(mailbox + status_offset);
		if ((status & busy_bit) == 0)
			break;

		/* Waits one polling interval before looking again. */
		kern_usleep_range(MAILBOX_POLL_US, MAILBOX_POLL_MAX_US);
	}

	/* Reports a mailbox that never became ready. */
	if (polls == MAILBOX_POLL_LIMIT) {
		kern_logf("rpi4-firmware: mailbox status %x stayed busy\n", status);
		return ETIMEDOUT;
	}

	/* Succeeded: the mailbox is ready for the next step. */
	return 0;
}

/*
 * Reads the firmware's answer out of the request buffer.
 *
 * The buffer's code says whether the firmware handled the request; the tag's
 * code says whether it knew the tag.
 */
static int
read_answer(
	uint32_t *values,
	unsigned capacity,
	uint32_t *answered)
{
	uint32_t tag_code;
	unsigned words;
	unsigned index;

	/* Drops any cached copy, so the reads see what the firmware wrote. */
	kern_dcache_invalidate_range(message, message[PROPERTY_WORD_SIZE]);

	/* Refuses a request the firmware did not handle. */
	if (message[PROPERTY_WORD_CODE] != PROPERTY_SUCCESS) {
		kern_logf("rpi4-firmware: tag %x refused (%x)\n",
			  message[PROPERTY_WORD_TAG],
			  message[PROPERTY_WORD_CODE]);
		return EIO;
	}

	/* Refuses a tag the firmware did not recognize. */
	tag_code = message[PROPERTY_WORD_TAG_CODE];
	if ((tag_code & PROPERTY_TAG_ANSWERED) == 0) {
		kern_logf("rpi4-firmware: tag %x not supported\n", message[PROPERTY_WORD_TAG]);
		return ENOTSUP;
	}

	/* Copies back as much of the answer as the value buffer holds. */
	*answered = tag_code & PROPERTY_TAG_LENGTH;
	words = (*answered + 3U) / 4U;
	if (words > capacity)
		words = capacity;
	for (index = 0; index < words; index++)
		values[index] = message[PROPERTY_WORD_VALUES + index];

	/* Succeeded: values holds the answer. */
	return 0;
}
