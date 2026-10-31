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

/*
 * Sends a whole property message the caller laid out (header, tags, end
 * tag) and copies the firmware's answer back over it.
 *
 * words is the message's length in 32-bit words, its first word the
 * length in bytes.  Returns 0 when the firmware handed the message back;
 * the caller reads the response code and each tag's own code itself.
 */
int
bcm2712_firmware_message(
	uint32_t *words,
	unsigned count)
{
	unsigned index;
	int error;

	/* Refuses a message the buffer cannot hold or whose length word disagrees. */
	if (words == NULL || count < 3U || count * 4U > FIRMWARE_BUFFER_SIZE)
		return EINVAL;
	if (words[PROPERTY_WORD_SIZE] != count * 4U)
		return EINVAL;

	/* Refuses a request before the mailbox is prepared. */
	if (mailbox == NULL)
		return ENODEV;

	/* Takes the buffer, sends the message and copies the answer back. */
	error = claim_buffer();
	if (error != 0)
		return error;
	for (index = 0; index < count; index++)
		message[index] = words[index];
	error = post_request();
	if (error == 0) {
		kern_dcache_invalidate_range(message, count * 4U);
		for (index = 0; index < count; index++)
			words[index] = message[index];
	}
	release_buffer();

	/* Succeeded when the firmware handed the message back. */
	return error;
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

/* The display questions of the firmware's property interface. */
#define SCREEN_TAG_DISPLAYS		0x00040013U
#define SCREEN_TAG_EDID			0x00030020U

/* The words of the display-count request, and of the EDID one (a block number, a status and 128 bytes). */
#define SCREEN_COUNT_WORDS		8U
#define SCREEN_EDID_WORDS		40U

/* The device tree's structure tokens. */
#define SCREEN_FDT_BEGIN_NODE		1U
#define SCREEN_FDT_END_NODE		2U
#define SCREEN_FDT_PROPERTY		3U
#define SCREEN_FDT_NOP			4U

/* The largest firmware log read, and how many of its display lines are printed. */
#define SCREEN_LOG_LIMIT		0x100000U
#define SCREEN_LOG_LINES		24U
#define SCREEN_LOG_LINE_BYTES		120U
#define SCREEN_LOG_SHORTEST		8U

/* Reads a big-endian word of the device tree blob. */
static uint32_t
screen_be32(
	const uint8_t *bytes)
{
	/* Succeeded: the word in the CPU's order. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

/* Whether a firmware log line is about the display: its HDMI ports, their EDID and the framebuffer. */
static bool
screen_line_wanted(
	const char *line)
{
	static const char *const words[] = { "HDMI", "EDID", "BSC_", "FB ", "isplay" };
	unsigned index;

	/* A line is wanted when any of the words is in it. */
	for (index = 0; index < sizeof(words) / sizeof(words[0]); index++)
		if (kern_strstr(line, words[index]) != NULL)
			return true;

	/* Not about the display. */
	return false;
}

/*
 * Finds where the firmware's own log lies: the two words of the tree's
 * /chosen/log property, its physical base and size.  Returns 0 when found.
 */
static int
screen_log_place(
	const struct drv_fdt *fdt,
	uint32_t *base,
	uint32_t *size)
{
	const uint8_t *structure;
	const char *name;
	uint32_t offset;
	uint32_t token;
	uint32_t length;
	unsigned depth;
	bool chosen;

	/* Walks the structure block, tracking whether the walk is directly inside /chosen. */
	structure = fdt->blob + fdt->structure_offset;
	offset = 0;
	depth = 0;
	chosen = false;
	while (offset + 4U <= fdt->structure_size) {
		token = screen_be32(structure + offset);
		offset += 4U;
		if (token == SCREEN_FDT_BEGIN_NODE) {
			/* A node: its name, padded to a word; /chosen is a child of the root. */
			name = (const char *)(structure + offset);
			length = (uint32_t)kern_strlen(name) + 1U;
			offset += (length + 3U) & ~3U;
			depth++;
			chosen = depth == 2U && kern_strcmp(name, "chosen") == 0;
		} else if (token == SCREEN_FDT_END_NODE) {
			/* The end of a node leaves any /chosen. */
			if (depth == 0)
				return ENOENT;
			depth--;
			chosen = false;
		} else if (token == SCREEN_FDT_PROPERTY) {
			/* A property: its length, its name's offset and its value. */
			if (offset + 8U > fdt->structure_size)
				return ENOENT;
			length = screen_be32(structure + offset);
			name = (const char *)(fdt->blob + fdt->strings_offset + screen_be32(structure + offset + 4U));
			if (chosen && length >= 8U && kern_strcmp(name, "log") == 0) {
				*base = screen_be32(structure + offset + 8U);
				*size = screen_be32(structure + offset + 12U);
				return 0;
			}
			offset += 8U + ((length + 3U) & ~3U);
		} else if (token != SCREEN_FDT_NOP) {
			/* The end of the structure, or a token the walk does not know. */
			return ENOENT;
		}
	}

	/* No log in the tree. */
	return ENOENT;
}

/* Prints the display lines of the firmware's own log: its printable runs that name the display. */
static void
screen_report_log(
	const struct drv_fdt *fdt)
{
	const volatile uint8_t *log;
	void *mapped;
	char line[SCREEN_LOG_LINE_BYTES];
	uint32_t base;
	uint32_t size;
	uint32_t index;
	unsigned length;
	unsigned printed;
	uint8_t byte;

	/* Maps the log the tree names; a missing or oversized one is skipped. */
	if (screen_log_place(fdt, &base, &size) != 0)
		return;
	if (base == 0 || size == 0 || size > SCREEN_LOG_LIMIT)
		return;
	if (kern_device_map(base, size, KERN_DEVICE_UNCACHED, &mapped) != 0)
		return;
	log = mapped;

	/* Splits the log into printable runs and prints the ones about the display. */
	length = 0;
	printed = 0;
	for (index = 0; index <= size && printed < SCREEN_LOG_LINES; index++) {
		byte = index < size ? log[index] : 0U;
		if (byte >= 0x20U && byte <= 0x7eU && length < sizeof(line) - 1U) {
			line[length++] = (char)byte;
			continue;
		}
		line[length] = 0;
		if (length >= SCREEN_LOG_SHORTEST && screen_line_wanted(line)) {
			kern_logf("rpi5-screen: firmware: %s\n", line);
			printed++;
		}
		length = 0;
	}
}

/*
 * Reports why the boot found no screen: how many displays the firmware
 * set up, whether it reads HDMI 0's EDID, and what its own log says about
 * the display (a monitor whose EDID cannot be read gets no display, and
 * so no framebuffer, from the Pi 5's firmware).  Changes nothing.
 */
void
bcm2712_screen_report(
	const struct drv_fdt *fdt)
{
	uint32_t words[SCREEN_EDID_WORDS];
	int count_error;
	int edid_error;
	uint32_t count;
	uint32_t edid_code;

	/* The number of displays the firmware set up. */
	kern_memset(words, 0, sizeof(words));
	words[PROPERTY_WORD_SIZE] = SCREEN_COUNT_WORDS * 4U;
	words[PROPERTY_WORD_TAG] = SCREEN_TAG_DISPLAYS;
	words[PROPERTY_WORD_CAPACITY] = 4U;
	count_error = bcm2712_firmware_message(words, SCREEN_COUNT_WORDS);
	count = words[PROPERTY_WORD_VALUES];
	if (words[PROPERTY_WORD_CODE] != PROPERTY_SUCCESS)
		count_error = EIO;

	/* HDMI 0's first EDID block, as the firmware reads it. */
	kern_memset(words, 0, sizeof(words));
	words[PROPERTY_WORD_SIZE] = SCREEN_EDID_WORDS * 4U;
	words[PROPERTY_WORD_TAG] = SCREEN_TAG_EDID;
	words[PROPERTY_WORD_CAPACITY] = (SCREEN_EDID_WORDS - PROPERTY_WORD_VALUES - 1U) * 4U;
	words[PROPERTY_WORD_TAG_CODE] = 4U;
	edid_error = bcm2712_firmware_message(words, SCREEN_EDID_WORDS);
	edid_code = words[PROPERTY_WORD_CODE];
	kern_logf("rpi5-screen: no firmware framebuffer; displays %u (%d), HDMI 0 EDID %s (%d, %x)\n",
		  count, count_error,
		  edid_error == 0 && edid_code == PROPERTY_SUCCESS && words[PROPERTY_WORD_VALUES + 1] == 0 ? "read" : "unreadable",
		  edid_error, edid_code);

	/* The firmware's own account of the display. */
	screen_report_log(fdt);
}
