/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5 firmware's property mailbox, as the V3D 7.1 driver
 * asks it about the engine's clock.
 *
 * A request is a buffer of 32-bit words in RAM: its size, a request code,
 * one tag with its value buffer, and an end tag.  The ARM side posts the
 * buffer's address, with the property channel in its low four bits, to
 * mailbox 1 and waits until the same word comes back through mailbox 0; the
 * firmware has then written its answer into the buffer.  Unlike the Pi 4's,
 * the Pi 5's VideoCore names ARM RAM by its physical address, without an
 * alias.  The buffer is cleaned before it is posted and invalidated before
 * it is read.
 *
 * The HAL used the same mailbox to set up the framebuffer before the kernel
 * started; it uses it again only to turn the board off or reset it.  The
 * engine's bring-up is the only kernel user on this board, and it runs on a
 * single thread; a busy word keeps a second caller from overwriting a
 * request in flight all the same.
 *
 * The tag values and the clock number are those of the Raspberry Pi
 * firmware wiki's mailbox property interface.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/v3d71-private.h"

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

/* The request buffer: one page below 1 GiB, which the firmware reaches. */
#define FIRMWARE_BUFFER_LIMIT		0x3fffffffULL
#define FIRMWARE_BUFFER_SIZE		4096U

/* The most value words one request carries. */
#define FIRMWARE_MAX_VALUES		8U

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

/*
 * The board's state while frames run (Raspberry Pi firmware wiki, "Mailbox
 * property interface"; Linux drivers/hwmon/raspberrypi-hwmon.c for the
 * throttling flags): a clock's rate as set and as measured, the SoC's
 * temperature in thousandths of a degree (sensor 0), and the throttling
 * flags, whose request word names the sticky bits to clear (zero keeps
 * them).  The clock numbers are those of the Pi 4's firmware.
 */
#define FIRMWARE_TAG_CLOCK_HZ		0x00030002U
#define FIRMWARE_TAG_CLOCK_MEASURED_HZ	0x00030047U
#define FIRMWARE_TAG_TEMPERATURE	0x00030006U
#define FIRMWARE_TAG_THROTTLED		0x00030046U
#define FIRMWARE_CLOCK_ARM		3U
#define FIRMWARE_CLOCK_CORE		4U
#define FIRMWARE_CLOCK_V3D		5U
#define FIRMWARE_TEMPERATURE_SOC	0U

/*
 * The mailbox registers.
 *
 * Mapped once by bcm2712_firmware_init() and kept for as long as the kernel
 * runs; NULL until then, and every request fails with ENODEV while it is.
 * Written only by the engine's bring-up thread before its first request.
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
 * Whether a request is in flight: zero when the buffer is free.
 *
 * Taken with an atomic test-and-set, so a second caller waits instead of
 * overwriting the buffer the firmware is reading.
 */
static volatile unsigned busy;

static int claim_buffer(void);
static void release_buffer(void);
static void build_request(uint32_t tag, const uint32_t *values, unsigned request_count, unsigned capacity);
static int post_request(void);
static int wait_mailbox(unsigned status_offset, uint32_t busy_bit);
static int read_answer(uint32_t *values, unsigned capacity, uint32_t *answered);

/*
 * Finds the mailbox in the device tree and prepares the request buffer.
 *
 * A call after a successful one does nothing.  Returns ENODEV when the tree
 * names no mailbox.
 */
int
bcm2712_firmware_init(
	const struct drv_fdt *fdt)
{
	struct kern_pmem run;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	void *registers;
	int error;

	/* Leaves a prepared mailbox alone. */
	if (mailbox != NULL)
		return 0;

	/* Finds the mailbox the firmware's tree describes. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2835-mbox", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;

	/* Reads where its registers are and refuses a block too small. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < MAILBOX_REGISTER_SPAN)
		return EINVAL;

	/* Allocates the request buffer where the firmware can read it. */
	error = kern_pmem_alloc_limited(FIRMWARE_BUFFER_SIZE, FIRMWARE_BUFFER_SIZE, FIRMWARE_BUFFER_LIMIT, 0, &run);
	if (error != 0)
		return error;

	/* Maps the registers; the buffer goes back if that fails. */
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &registers);
	if (error != 0) {
		(void)kern_pmem_free(&run);
		return error;
	}

	/* Publishes the buffer and then the mailbox, which makes requests possible. */
	message = kern_pmem_to_kernel(run.paddr);
	message_physical = run.paddr;
	mailbox = registers;

	/* Succeeded: the mailbox and the buffer are ready. */
	return 0;
}

/*
 * Sends one property tag and waits for the answer.
 *
 * values holds request_count words on entry and the answer on return;
 * capacity is how many words the tag's value buffer has, and answered
 * reports how many bytes the firmware wrote.  Returns EIO when the firmware
 * refused the request, ENOTSUP when it did not understand the tag and
 * ETIMEDOUT when it did not answer.
 */
int
bcm2712_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered)
{
	int error;

	/* Refuses a request the buffer cannot describe. */
	if (answered == NULL || values == NULL)
		return EINVAL;
	if (capacity == 0 || capacity > FIRMWARE_MAX_VALUES)
		return EINVAL;
	if (request_count > capacity)
		return EINVAL;

	/* Refuses a request before the mailbox is prepared. */
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
 * Asks one value about a firmware clock: its state, rate or limit.
 *
 * Refuses an answer about another clock or one too short to hold the value.
 */
int
bcm2712_clock_get(
	uint32_t tag,
	uint32_t clock_id,
	uint32_t *value)
{
	uint32_t values[2];
	uint32_t answered;
	int error;

	/* Asks the question: the clock, and room for its answer. */
	values[0] = clock_id;
	values[1] = 0;
	error = bcm2712_firmware_property(tag, values, 1, 2, &answered);
	if (error != 0)
		return error;

	/* Refuses an answer that does not describe the clock asked about. */
	if (answered < 8U || values[0] != clock_id)
		return EIO;

	/* Succeeded: value holds the answer. */
	*value = values[1];
	return 0;
}

/*
 * Changes one setting of a firmware clock: its state or rate.
 *
 * The third word of the request leaves the firmware's turbo policy alone.
 */
int
bcm2712_clock_set(
	uint32_t tag,
	uint32_t clock_id,
	uint32_t value)
{
	uint32_t values[3];
	uint32_t answered;
	int error;

	/* Sends the change: the clock, the new setting and the policy word. */
	values[0] = clock_id;
	values[1] = value;
	values[2] = 0;
	error = bcm2712_firmware_property(tag, values, 3, 3, &answered);
	if (error != 0)
		return error;

	/* Refuses an answer about another clock. */
	if (answered < 8U || values[0] != clock_id)
		return EIO;

	/* Succeeded: the firmware accepted the change. */
	return 0;
}

/* Takes the buffer for one request, waiting while another request is in flight. */
static int
claim_buffer(void)
{
	unsigned polls;
	int taken;

	/* Tries to take the buffer, sleeping a polling interval between tries. */
	for (polls = 0; polls < 2U * MAILBOX_POLL_LIMIT; polls++) {
		/* Takes the buffer when no other request holds it. */
		taken = atomic_raw_try_acquire_zero(&busy);
		if (taken)
			return 0;

		/* Waits one interval before trying again. */
		kern_usleep_range(MAILBOX_POLL_US, MAILBOX_POLL_MAX_US);
	}

	/* Failed: another request held the buffer for too long. */
	kern_logf("v3d: firmware request buffer stayed taken\n");
	return EBUSY;
}

/* Gives the buffer back after a request. */
static void
release_buffer(void)
{
	/* Publishes the answer's reads before the next request may write. */
	atomic_raw_store_release(&busy, 0U);
}

/* Lays out one tag in the request buffer. */
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

/* Posts the request buffer and waits until the firmware hands it back. */
static int
post_request(void)
{
	uint32_t request;
	uint32_t answer;
	unsigned attempts;
	int error;

	/* Makes the request visible to the VideoCore, which reads RAM without the ARM caches. */
	kern_dcache_clean_range(message, message[PROPERTY_WORD_SIZE]);

	/* Waits for room in mailbox 1; the word is the buffer's physical address and the channel. */
	request = (uint32_t)message_physical | MAILBOX_CHANNEL_PROPERTY;
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

/* Waits up to one second until a status bit that blocks the next step clears. */
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
		kern_logf("v3d: firmware mailbox status %x stayed busy\n", status);
		return ETIMEDOUT;
	}

	/* Succeeded: the mailbox is ready for the next step. */
	return 0;
}

/* Reads the firmware's answer out of the request buffer. */
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
		kern_logf("v3d: firmware tag %x refused (%x)\n",
			  message[PROPERTY_WORD_TAG],
			  message[PROPERTY_WORD_CODE]);
		return EIO;
	}

	/* Refuses a tag the firmware did not recognize. */
	tag_code = message[PROPERTY_WORD_TAG_CODE];
	if ((tag_code & PROPERTY_TAG_ANSWERED) == 0) {
		kern_logf("v3d: firmware tag %x not supported\n", message[PROPERTY_WORD_TAG]);
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

/*
 * Asks the firmware for the board's state; each question it does not
 * answer leaves its field zero.
 */
void
bcm2711_board_state_read(
	struct bcm2711_board_state *state)
{
	uint32_t values[1];
	uint32_t answered;

	/* Every field starts unanswered. */
	kern_memset(state, 0, sizeof(*state));

	/* The clocks: the CPU's as set and as measured, the core's and V3D's. */
	if (bcm2712_clock_get(FIRMWARE_TAG_CLOCK_HZ, FIRMWARE_CLOCK_ARM, &state->arm_hz) != 0)
		state->arm_hz = 0U;
	if (bcm2712_clock_get(FIRMWARE_TAG_CLOCK_MEASURED_HZ, FIRMWARE_CLOCK_ARM, &state->arm_measured_hz) != 0)
		state->arm_measured_hz = 0U;
	if (bcm2712_clock_get(FIRMWARE_TAG_CLOCK_HZ, FIRMWARE_CLOCK_CORE, &state->core_hz) != 0)
		state->core_hz = 0U;
	if (bcm2712_clock_get(FIRMWARE_TAG_CLOCK_HZ, FIRMWARE_CLOCK_V3D, &state->v3d_hz) != 0)
		state->v3d_hz = 0U;

	/* The SoC's temperature. */
	if (bcm2712_clock_get(FIRMWARE_TAG_TEMPERATURE, FIRMWARE_TEMPERATURE_SOC, &state->milli_celsius) != 0)
		state->milli_celsius = 0U;

	/* The throttling flags, the sticky ones kept. */
	values[0] = 0U;
	answered = 0U;
	if (bcm2712_firmware_property(FIRMWARE_TAG_THROTTLED, values, 1U, 1U, &answered) == 0 && answered >= 4U)
		state->throttled = values[0];
}
