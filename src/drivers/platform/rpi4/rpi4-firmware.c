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
#include <kern/atomic.h>
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
 * The tags of the firmware's GPIO expander (Raspberry Pi firmware wiki,
 * "Mailbox property interface"; the configuration tags and the layouts of
 * FreeBSD's bcm2835_firmware.h, BSD-2-Clause).  Each request starts with
 * the GPIO's number; the firmware overwrites that word with zero when it
 * handled the request, so a nonzero first word in the answer is a failure.
 *
 * The firmware handles the two setting tags but leaves their tag code as
 * the request wrote it, zero, without the answered bit and the length
 * (BUG-311; FreeBSD's bcm2835_mbox.c works around the same: "When setting
 * the GPIO config or state the firmware doesn't set tag->val_len
 * correctly").  read_answer() takes such an answer as a whole value
 * buffer; the status word still tells success from failure.
 */
#define FIRMWARE_TAG_GET_GPIO_STATE	0x00030041U
#define FIRMWARE_TAG_SET_GPIO_STATE	0x00038041U
#define FIRMWARE_TAG_GET_GPIO_CONFIG	0x00030043U
#define FIRMWARE_TAG_SET_GPIO_CONFIG	0x00038043U

/* The expander's lines are GPIO 128 and up in the firmware's numbering. */
#define FIRMWARE_GPIO_EXPANDER_BASE	128U

/* The first word of a GPIO answer when the firmware handled the request. */
#define FIRMWARE_GPIO_STATUS_OK		0U

/*
 * The value words of the GPIO tags: a state request and answer is the GPIO
 * and its state; a configuration answer is the GPIO, the direction, the
 * polarity, whether termination is on and whether it pulls up.
 */
#define FIRMWARE_GPIO_STATE_WORDS	2U
#define FIRMWARE_GPIO_CONFIG_WORDS	5U

/*
 * The value words of a configuration change: the GPIO, the direction, the
 * polarity, whether termination is on, whether it pulls up, and the level
 * an output starts at.
 */
#define FIRMWARE_GPIO_SET_CONFIG_WORDS	6U

/*
 * The bytes an answer must hold: the status word alone for a change, and
 * the status word with the word read after it for a question.
 */
#define FIRMWARE_GPIO_STATUS_BYTES	4U
#define FIRMWARE_GPIO_ANSWER_BYTES	8U

/*
 * The clock tags (Raspberry Pi firmware wiki, "Mailbox property
 * interface") and the core clock's number in them.  A question carries
 * the clock and its answer the rate; a change carries the clock, the rate
 * and a third word.
 */
#define FIRMWARE_TAG_GET_CLOCK_RATE	0x00030002U
#define FIRMWARE_TAG_GET_MAX_CLOCK_RATE	0x00030004U
#define FIRMWARE_TAG_SET_CLOCK_RATE	0x00038002U
#define FIRMWARE_CLOCK_CORE		4U
#define FIRMWARE_CLOCK_WORDS		2U
#define FIRMWARE_CLOCK_SET_WORDS	3U

/* The firmware's direction codes in a configuration answer. */
#define FIRMWARE_GPIO_DIRECTION_IN	0U
#define FIRMWARE_GPIO_DIRECTION_OUT	1U

/*
 * The steps of the mailbox's one-time preparation.
 *
 * ABSENT: nobody has prepared it, or the last attempt failed.  PREPARING:
 * one caller is finding the mailbox and allocating the buffer.  READY: the
 * mailbox, the buffer and its address are filled and never change again.
 */
#define FIRMWARE_STATE_ABSENT		0U
#define FIRMWARE_STATE_PREPARING	1U
#define FIRMWARE_STATE_READY		2U

/*
 * The mailbox registers.
 *
 * Mapped once by drv_rpi4_firmware_init() and kept for as long as the kernel
 * runs.  Only read after firmware_state has been seen READY with an acquire
 * load, which orders the read after the store that filled it.
 */
static volatile uint8_t *mailbox;

/*
 * The request buffer and its physical address.
 *
 * One page below 1 GiB, allocated with the mailbox and never freed, and
 * published by firmware_state like the mailbox.  Only the request in flight
 * uses it; busy says whether one is.
 */
static uint32_t *message;
static uint64_t message_physical;

/*
 * How far the mailbox's preparation has come: one of FIRMWARE_STATE_*.
 *
 * The GPU, the PCIe host, audio and the wireless drivers may each start the
 * preparation, on different CPUs at the same time.  The caller that moves it
 * from ABSENT to PREPARING with a compare-and-exchange is the only one that
 * fills mailbox, message and message_physical; it then stores READY with
 * release, so a CPU that loads READY with acquire also sees those three
 * filled.  A failed preparation stores ABSENT again so a later caller may
 * retry.  Zero means nothing has been prepared and every request fails with
 * ENODEV.
 */
static volatile unsigned firmware_state;

/*
 * Whether a request is in flight: zero when the buffer is free.
 *
 * It is taken with an atomic test-and-set, so a second caller on another
 * CPU waits instead of overwriting the buffer the firmware is reading
 * (ws208-p004: with interrupts held off it excluded only callers on the
 * same CPU).
 */
static volatile unsigned busy;

/*
 * The core clock's floors, by owner (enum drv_rpi4_firmware_core_owner),
 * in hertz; zero when the owner asks nothing.
 *
 * The display asks for the rate its planes need.  The video decoder asks
 * for the highest rate while a decoder exists, so the core clock never
 * changes under a decode: the firmware's decoder stopped answering when the
 * display lowered and raised the clock in the middle of one (ws236-p011,
 * T1-685).  Changed only by the holder of core_turn.
 */
static uint32_t core_floors[DRV_RPI4_FIRMWARE_CORE_OWNERS];

/*
 * The core clock's rates: the one the firmware ran before the first floor
 * (run again when no owner asks), the one it was last asked for, and its
 * highest.  Zero until the first floor reads them (core_maximum zero means
 * not read yet); then kept for good.  Changed only by the holder of
 * core_turn.
 */
static uint32_t core_base;
static uint32_t core_rate;
static uint32_t core_maximum;

/*
 * Whether a change of the core clock is in progress: zero when the turn
 * is free.  Taken with an atomic test-and-set like busy, and held across
 * the firmware requests of one change, so two owners' changes never
 * interleave.
 */
static volatile unsigned core_turn;

static int prepare_mailbox(const struct drv_fdt *fdt);
static int claim_buffer(void);
static void release_buffer(void);
static void build_request(uint32_t tag, const uint32_t *values, unsigned request_count, unsigned capacity);
static int post_request(void);
static int wait_mailbox(unsigned status_offset, uint32_t busy_bit);
static int read_answer(uint32_t *values, unsigned capacity, uint32_t *answered);
static int expander_gpio(enum drv_rpi4_firmware_gpio line, uint32_t *gpio);
static int read_gpio_config(uint32_t gpio, uint32_t *config);
static int set_gpio_output(uint32_t gpio, uint32_t polarity, int state);
static int set_tag_without_length(uint32_t tag);
static int claim_core_turn(void);
static void release_core_turn(void);
static int read_core_limits(void);
static int set_core_rate(uint32_t hz);

/*
 * Finds the mailbox in the device tree and prepares the request buffer.
 *
 * Several drivers call it, possibly on different CPUs at once; exactly one
 * of them prepares the mailbox and the others wait until it is ready.
 */
int
drv_rpi4_firmware_init(
	const struct drv_fdt *fdt)
{
	unsigned state;
	unsigned expected;
	int claimed;
	int error;

	/*
	 * Takes the preparation, or waits while another CPU holds it.  The
	 * holder does not wait on anything but the device tree and the page
	 * allocator, so it always finishes and the wait ends.
	 */
	for (;;) {
		/* Does nothing once the mailbox is ready. */
		state = atomic_raw_load_acquire(&firmware_state);
		if (state == FIRMWARE_STATE_READY)
			return 0;

		/* Becomes the one caller that prepares the mailbox. */
		expected = FIRMWARE_STATE_ABSENT;
		claimed = atomic_raw_compare_exchange(&firmware_state, &expected, FIRMWARE_STATE_PREPARING);
		if (claimed)
			break;

		/* Waits one polling interval for the other CPU's preparation. */
		kern_usleep_range(MAILBOX_POLL_US, MAILBOX_POLL_MAX_US);
	}

	/* Finds the mailbox and allocates the buffer. */
	error = prepare_mailbox(fdt);
	if (error != 0) {
		/* Gives the preparation back, so a later caller may try again. */
		atomic_raw_store_release(&firmware_state, FIRMWARE_STATE_ABSENT);
		return error;
	}

	/*
	 * READY publishes the mailbox and the buffer: a CPU that loads it with
	 * acquire sees both filled.
	 */
	atomic_raw_store_release(&firmware_state, FIRMWARE_STATE_READY);

	/* Succeeded: requests may now be sent. */
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
	unsigned state;
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

	/*
	 * Refuses a request before the mailbox is ready.  The acquire load
	 * orders the reads of the mailbox and the buffer after their filling.
	 */
	state = atomic_raw_load_acquire(&firmware_state);
	if (state != FIRMWARE_STATE_READY)
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

#if defined(CONFIG_DRIVER_RPI4_GPU)
/*
 * Posts one word to a mailbox channel other than the property channel.
 *
 * VCHIQ hands the firmware the bus address of its shared memory this way
 * (channel 3).  The word takes the request buffer's turn, so it never
 * interleaves with a property request's post and read.
 */
int
drv_rpi4_firmware_mailbox_post(
	unsigned channel,
	uint32_t value)
{
	unsigned state;
	uint32_t word;
	int error;

	/* Refuses the property channel and channels outside the four bits. */
	if (channel == MAILBOX_CHANNEL_PROPERTY || channel > 15U)
		return EINVAL;

	/* Refuses a post before the mailbox is ready. */
	state = atomic_raw_load_acquire(&firmware_state);
	if (state != FIRMWARE_STATE_READY)
		return ENODEV;

	/* Takes the mailbox's turn from any property request. */
	error = claim_buffer();
	if (error != 0)
		return error;

	/* Waits for room in mailbox 1, then posts the word on its channel. */
	word = (value & ~0xfU) | channel;
	error = wait_mailbox(MAILBOX_WRITE_STATUS, MAILBOX_FULL);
	if (error == 0)
		kern_mmio_write32(mailbox + MAILBOX_WRITE, word);
	release_buffer();

	/* Reports a mailbox that stayed full. */
	if (error != 0)
		return error;

	/* Succeeded: the firmware has the word. */
	return 0;
}
#endif

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

/*
 * Reads whether an expander line is high.
 */
int
drv_rpi4_firmware_get_gpio(
	enum drv_rpi4_firmware_gpio line,
	int *state)
{
	uint32_t values[FIRMWARE_GPIO_STATE_WORDS];
	uint32_t answered;
	uint32_t gpio;
	int error;

	/* Refuses nowhere to put the answer. */
	if (state == NULL)
		return EINVAL;

	/* Names the line in the firmware's numbering, refusing any other line. */
	error = expander_gpio(line, &gpio);
	if (error != 0)
		return error;

	/* Asks for the line's level. */
	values[0] = gpio;
	values[1] = 0;
	answered = 0;
	error = drv_rpi4_firmware_property(
		FIRMWARE_TAG_GET_GPIO_STATE,
		values,
		FIRMWARE_GPIO_STATE_WORDS,
		FIRMWARE_GPIO_STATE_WORDS,
		&answered);
	if (error != 0)
		return error;

	/* Refuses an answer without the level in it. */
	if (answered < FIRMWARE_GPIO_ANSWER_BYTES)
		return EIO;

	/* Refuses an answer whose status word says the firmware failed. */
	if (values[0] != FIRMWARE_GPIO_STATUS_OK) {
		kern_logf("rpi4-firmware: GPIO %u state not read (%x)\n", gpio, values[0]);
		return EIO;
	}

	/* Any nonzero level is high. */
	*state = 0;
	if (values[1] != 0)
		*state = 1;

	/* Succeeded: state holds the line's level. */
	return 0;
}

/*
 * Drives an expander line low or high.
 */
int
drv_rpi4_firmware_set_gpio(
	enum drv_rpi4_firmware_gpio line,
	int state)
{
	uint32_t values[FIRMWARE_GPIO_STATE_WORDS];
	uint32_t config[FIRMWARE_GPIO_CONFIG_WORDS];
	uint32_t answered;
	uint32_t gpio;
	int error;

	/* Refuses a level other than low and high. */
	if (state != 0 && state != 1)
		return EINVAL;

	/* Names the line in the firmware's numbering, refusing any other line. */
	error = expander_gpio(line, &gpio);
	if (error != 0)
		return error;

	/* Reads whether the line is an output; a state change does not drive an input. */
	error = read_gpio_config(gpio, config);
	if (error != 0)
		return error;

	/* An input is made an output at the new level, keeping its polarity (as an output's direction change does). */
	if (config[1] != FIRMWARE_GPIO_DIRECTION_OUT) {
		error = set_gpio_output(gpio, config[2], state);
		if (error != 0)
			return error;

		/* Succeeded: the line is an output at the new level. */
		return 0;
	}

	/* Asks the firmware to drive the output. */
	values[0] = gpio;
	values[1] = (uint32_t)state;
	answered = 0;
	error = drv_rpi4_firmware_property(
		FIRMWARE_TAG_SET_GPIO_STATE,
		values,
		FIRMWARE_GPIO_STATE_WORDS,
		FIRMWARE_GPIO_STATE_WORDS,
		&answered);
	if (error != 0)
		return error;

	/* Refuses an answer without its status word. */
	if (answered < FIRMWARE_GPIO_STATUS_BYTES)
		return EIO;

	/* Refuses an answer whose status word says the firmware failed. */
	if (values[0] != FIRMWARE_GPIO_STATUS_OK) {
		kern_logf("rpi4-firmware: GPIO %u not set (%x)\n", gpio, values[0]);
		return EIO;
	}

	/* Succeeded: the line is at the new level. */
	return 0;
}

/*
 * Reads whether an expander line is an input or an output.
 */
int
drv_rpi4_firmware_get_gpio_direction(
	enum drv_rpi4_firmware_gpio line,
	enum drv_rpi4_firmware_gpio_direction *direction)
{
	uint32_t values[FIRMWARE_GPIO_CONFIG_WORDS];
	uint32_t gpio;
	int error;

	/* Refuses nowhere to put the answer. */
	if (direction == NULL)
		return EINVAL;

	/* Names the line in the firmware's numbering, refusing any other line. */
	error = expander_gpio(line, &gpio);
	if (error != 0)
		return error;

	/* Reads the line's configuration. */
	error = read_gpio_config(gpio, values);
	if (error != 0)
		return error;

	/* Names the firmware's direction; any other code is not one it defines. */
	switch (values[1]) {
	case FIRMWARE_GPIO_DIRECTION_IN:
		*direction = DRV_RPI4_FIRMWARE_GPIO_INPUT;
		break;
	case FIRMWARE_GPIO_DIRECTION_OUT:
		*direction = DRV_RPI4_FIRMWARE_GPIO_OUTPUT;
		break;
	default:
		kern_logf("rpi4-firmware: GPIO %u has direction %x\n", gpio, values[1]);
		return EIO;
	}

	/* Succeeded: direction holds the line's direction. */
	return 0;
}

/*
 * Sets one owner's floor of the core clock, in hertz; zero takes it away.
 */
int
drv_rpi4_firmware_core_clock(
	enum drv_rpi4_firmware_core_owner owner,
	uint32_t hz)
{
	uint32_t previous;
	uint32_t rate;
	unsigned index;
	int error;

	/* Refuses an owner outside the enumeration. */
	if ((unsigned)owner >= DRV_RPI4_FIRMWARE_CORE_OWNERS)
		return EINVAL;

	/* Takes the clock's turn, so the floors and the rate asked change together. */
	error = claim_core_turn();
	if (error != 0)
		return error;

	/* Learns, once, the rate the firmware runs without a floor and its highest. */
	error = 0;
	if (core_maximum == 0U)
		error = read_core_limits();
	if (error != 0) {
		release_core_turn();
		return error;
	}

	/* The owner's new floor, no higher than the firmware's highest. */
	previous = core_floors[owner];
	if (hz > core_maximum)
		hz = core_maximum;
	core_floors[owner] = hz;

	/* The highest floor, or the rate from before the first floor when nobody asks. */
	rate = 0U;
	for (index = 0; index < DRV_RPI4_FIRMWARE_CORE_OWNERS; index++) {
		if (core_floors[index] > rate)
			rate = core_floors[index];
	}

	/* Nobody asks: the rate from before the first floor. */
	if (rate == 0U)
		rate = core_base;

	/* Asks the firmware only for a change of the rate. */
	if (rate != core_rate)
		error = set_core_rate(rate);
	if (error != 0) {
		/* The firmware kept the clock: the owner's floor is what it was. */
		core_floors[owner] = previous;
		release_core_turn();
		return error;
	}

	/* The firmware runs the rate now; a change is logged with the floors that made it (they change seldom). */
	if (rate != core_rate) {
		kern_logf("rpi4-firmware: core clock %u Hz (display %u, video %u)\n", (unsigned)rate,
			  (unsigned)core_floors[DRV_RPI4_FIRMWARE_CORE_DISPLAY],
			  (unsigned)core_floors[DRV_RPI4_FIRMWARE_CORE_VIDEO]);
	}

	/* The rate asked from now on, published with the turn. */
	core_rate = rate;
	release_core_turn();

	/* Succeeded: the core clock is the highest floor. */
	return 0;
}

/*
 * Finds the mailbox, maps its registers and allocates the request buffer.
 *
 * Only the caller holding the PREPARING state runs it, so it fills the
 * shared variables without a lock; nothing reads them until READY.
 */
static int
prepare_mailbox(
	const struct drv_fdt *fdt)
{
	struct kern_pmem run;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	bool enabled;
	void *registers;
	int error;

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

	/* Fills the buffer and the mailbox; the caller publishes them. */
	message = kern_pmem_to_kernel(run.paddr);
	message_physical = run.paddr;
	mailbox = registers;

	/* Succeeded: the mailbox and the buffer are ready to publish. */
	return 0;
}

/*
 * Takes the buffer for one request, waiting while another CPU's request
 * is in flight.
 *
 * The wait gives up after twice the mailbox's polling limit, which a
 * request answered normally never approaches; a buffer still taken then is
 * reported as EBUSY without touching it.  The polling interval is a busy
 * delay, so the wait is allowed wherever a request is.
 */
static int
claim_buffer(void)
{
	unsigned polls;
	int taken;

	/* Tries to take the buffer, sleeping a polling interval between tries. */
	for (polls = 0; polls < 2U * MAILBOX_POLL_LIMIT; polls++) {
		taken = atomic_raw_try_acquire_zero(&busy);
		if (taken)
			return 0;
		kern_usleep_range(MAILBOX_POLL_US, MAILBOX_POLL_MAX_US);
	}

	/* Failed: another request held the buffer for too long. */
	kern_logf("rpi4-firmware: the request buffer stayed taken\n");
	return EBUSY;
}

/* Gives the buffer back after a request. */
static void
release_buffer(void)
{
	/* Publishes the answer's reads before the next request may write. */
	atomic_raw_store_release(&busy, 0U);
}

/*
 * Takes the core clock's turn, waiting while another thread changes it.
 *
 * A change holds the turn for at most three firmware requests, each
 * bounded by the mailbox's polling limit, so the wait is bounded the same
 * way as claim_buffer()'s.
 */
static int
claim_core_turn(void)
{
	unsigned polls;
	int taken;

	/* Tries to take the turn, sleeping a polling interval between tries. */
	for (polls = 0; polls < 6U * MAILBOX_POLL_LIMIT; polls++) {
		taken = atomic_raw_try_acquire_zero(&core_turn);
		if (taken)
			return 0;
		kern_usleep_range(MAILBOX_POLL_US, MAILBOX_POLL_MAX_US);
	}

	/* Failed: another change held the turn for too long. */
	kern_logf("rpi4-firmware: the core clock's turn stayed taken\n");
	return EBUSY;
}

/* Gives the core clock's turn back. */
static void
release_core_turn(void)
{
	/* Publishes the floors and the rate before the next change reads them. */
	atomic_raw_store_release(&core_turn, 0U);
}

/*
 * Reads the core clock's rate now (the rate without a floor) and its
 * highest rate, into core_base, core_rate and core_maximum.
 */
static int
read_core_limits(void)
{
	uint32_t values[FIRMWARE_CLOCK_WORDS];
	uint32_t answered;
	int error;

	/* The rate the firmware runs the core at now. */
	values[0] = FIRMWARE_CLOCK_CORE;
	values[1] = 0U;
	error = drv_rpi4_firmware_property(FIRMWARE_TAG_GET_CLOCK_RATE, values, 2U, 2U, &answered);
	if (error != 0)
		return error;
	if (answered < 8U ||
	    values[0] != FIRMWARE_CLOCK_CORE ||
	    values[1] == 0U)
		return EIO;
	core_base = values[1];

	/* The highest rate the firmware runs the core at. */
	values[0] = FIRMWARE_CLOCK_CORE;
	values[1] = 0U;
	error = drv_rpi4_firmware_property(FIRMWARE_TAG_GET_MAX_CLOCK_RATE, values, 2U, 2U, &answered);
	if (error != 0)
		return error;
	if (answered < 8U ||
	    values[0] != FIRMWARE_CLOCK_CORE ||
	    values[1] == 0U)
		return EIO;

	/* Both known: the rate now is the one the firmware was last asked for. */
	core_rate = core_base;
	core_maximum = values[1];
	kern_logf("rpi4-firmware: core clock %u Hz, highest %u Hz\n", (unsigned)core_base, (unsigned)core_maximum);

	/* Succeeded: the limits are known for good. */
	return 0;
}

/* Asks the firmware to run the core clock at a rate, with its turbo setting left as it is. */
static int
set_core_rate(
	uint32_t hz)
{
	uint32_t values[FIRMWARE_CLOCK_SET_WORDS];
	uint32_t answered;
	int error;

	/* The clock, the rate, and the third word zero, as the display's requests have always sent it. */
	values[0] = FIRMWARE_CLOCK_CORE;
	values[1] = hz;
	values[2] = 0U;
	error = drv_rpi4_firmware_property(FIRMWARE_TAG_SET_CLOCK_RATE, values, 3U, 3U, &answered);
	if (error != 0)
		return error;

	/* Refuses an answer about another clock. */
	if (answered < 8U || values[0] != FIRMWARE_CLOCK_CORE)
		return EIO;

	/* Succeeded: the firmware accepted the rate. */
	return 0;
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
 * Words that come back for other channels are dropped; the only other
 * user, VCHIQ's post on channel 3, expects no answer.
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
	int without_length;

	/* Drops any cached copy, so the reads see what the firmware wrote. */
	kern_dcache_invalidate_range(message, message[PROPERTY_WORD_SIZE]);

	/* Refuses a request the firmware did not handle. */
	if (message[PROPERTY_WORD_CODE] != PROPERTY_SUCCESS) {
		kern_logf("rpi4-firmware: tag %x refused (%x)\n",
			  message[PROPERTY_WORD_TAG],
			  message[PROPERTY_WORD_CODE]);
		return EIO;
	}

	/* A setting tag of the GPIO expander comes back with its tag code untouched: the whole value buffer is its answer. */
	tag_code = message[PROPERTY_WORD_TAG_CODE];
	without_length = set_tag_without_length(message[PROPERTY_WORD_TAG]);
	if (without_length && tag_code == PROPERTY_REQUEST)
		tag_code = PROPERTY_TAG_ANSWERED | message[PROPERTY_WORD_CAPACITY];

	/* Refuses a tag the firmware did not recognize. */
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

/*
 * Converts an expander line to the firmware's GPIO number.
 *
 * This is the one place that adds the expander's base, and it accepts only
 * the lines of the enumeration.
 */
static int
expander_gpio(
	enum drv_rpi4_firmware_gpio line,
	uint32_t *gpio)
{
	/* Names the line; any other number, such as the SD card's power, is refused. */
	switch (line) {
	case DRV_RPI4_FIRMWARE_GPIO_BT_ON:
	case DRV_RPI4_FIRMWARE_GPIO_WL_ON:
		break;
	default:
		return EINVAL;
	}

	/* Adds the expander's base to the line's place in the expander. */
	*gpio = FIRMWARE_GPIO_EXPANDER_BASE + (uint32_t)line;

	/* Succeeded: gpio holds the line's number in the firmware's numbering. */
	return 0;
}

/*
 * Reads an expander line's configuration: the status word, the direction,
 * the polarity, the termination and its pull, in that order.
 */
static int
read_gpio_config(
	uint32_t gpio,
	uint32_t *config)
{
	uint32_t answered;
	int error;

	/* Asks for the line's configuration; only the GPIO goes with the question. */
	kern_memset(config, 0, FIRMWARE_GPIO_CONFIG_WORDS * sizeof(config[0]));
	config[0] = gpio;
	answered = 0;
	error = drv_rpi4_firmware_property(
		FIRMWARE_TAG_GET_GPIO_CONFIG,
		config,
		1U,
		FIRMWARE_GPIO_CONFIG_WORDS,
		&answered);
	if (error != 0)
		return error;

	/* Refuses an answer without the direction in it. */
	if (answered < FIRMWARE_GPIO_ANSWER_BYTES)
		return EIO;

	/* Refuses an answer whose status word says the firmware failed. */
	if (config[0] != FIRMWARE_GPIO_STATUS_OK) {
		kern_logf("rpi4-firmware: GPIO %u configuration not read (%x)\n", gpio, config[0]);
		return EIO;
	}

	/* Succeeded: config holds the line's configuration. */
	return 0;
}

/*
 * Makes an expander line an output at a level, with the polarity it had
 * and no termination.
 */
static int
set_gpio_output(
	uint32_t gpio,
	uint32_t polarity,
	int state)
{
	uint32_t values[FIRMWARE_GPIO_SET_CONFIG_WORDS];
	uint32_t answered;
	int error;

	/* The new configuration: an output at the level, the old polarity, no termination. */
	values[0] = gpio;
	values[1] = FIRMWARE_GPIO_DIRECTION_OUT;
	values[2] = polarity;
	values[3] = 0U;
	values[4] = 0U;
	values[5] = (uint32_t)state;
	answered = 0;
	error = drv_rpi4_firmware_property(
		FIRMWARE_TAG_SET_GPIO_CONFIG,
		values,
		FIRMWARE_GPIO_SET_CONFIG_WORDS,
		FIRMWARE_GPIO_SET_CONFIG_WORDS,
		&answered);
	if (error != 0)
		return error;

	/* Refuses an answer without its status word. */
	if (answered < FIRMWARE_GPIO_STATUS_BYTES)
		return EIO;

	/* Refuses an answer whose status word says the firmware failed. */
	if (values[0] != FIRMWARE_GPIO_STATUS_OK) {
		kern_logf("rpi4-firmware: GPIO %u not made an output (%x)\n", gpio, values[0]);
		return EIO;
	}

	kern_logf("rpi4-firmware: GPIO %u made an output at level %d\n", gpio, state);

	/* Succeeded: the line is an output at the level. */
	return 0;
}

/*
 * Reports nonzero for a GPIO expander tag whose answer the firmware leaves
 * without the answered bit and the length (the two setting tags).
 */
static int
set_tag_without_length(
	uint32_t tag)
{
	/* The state and the configuration changes. */
	if (tag == FIRMWARE_TAG_SET_GPIO_STATE)
		return 1;
	if (tag == FIRMWARE_TAG_SET_GPIO_CONFIG)
		return 1;

	/* Succeeded: every other tag reports its answer. */
	return 0;
}
