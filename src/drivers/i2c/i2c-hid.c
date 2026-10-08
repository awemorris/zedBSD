/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * HID over I2C (ws159-p003): the touch pads (and touch screens) the ACPI
 * tables name as PNP0C50 devices on an I2C bus.
 *
 * Each device's _CRS gives its bus (the controller's ACPI path), its
 * address and its speed, and its _DSM gives the register of its HID
 * descriptor.  A thread of its own reads the descriptor, powers the device
 * on, resets it, reads its report descriptor and describes it with the HID
 * input glue shared with USB and the input bridge (hid-input.c,
 * ws143-p005).  A Windows Precision Touchpad is then put in its touch pad
 * mode (Device Mode 3) with its surface and its button switched on, which
 * is what makes it report its fingers rather than a mouse's motion.  The
 * glue's touch state machine (hid-touch.c) takes the fingers, and the
 * touch device alone is published as an evdev node that speaks multitouch
 * protocol B (the pad's mouse collection is not).
 *
 * The device's interrupt line (its GpioInt) is taken as an interrupt when
 * it is on an Intel PCH GPIO pad that can interrupt (intel-gpio.c,
 * ws159-p006): the thread sleeps until the pad fires, reads the input
 * register while the line stays asserted, and turns the pad's interrupt on
 * again; it also wakes every I2C_HID_IRQ_CHECK_MS to read a line that is
 * asserted without a firing, and goes back to watching the line if the
 * controller's interrupt was given up.  A pad that cannot interrupt is
 * watched instead: the thread looks at it every LINE_POLL_MS and reads the
 * input register only while the line is asserted, so a still pad costs no
 * I2C transfer.
 *
 * A device whose _CRS gives an Interrupt (an APIC line, ws183-p001: the
 * Latitude 5320's touchpad, IRQ 51, level, active low) instead of a
 * GpioInt is read on that line: its handler masks the line and wakes the
 * thread, which reads the input register until the device has nothing
 * more to say (an empty report), then unmasks the line, so a level that
 * stays asserted meanwhile does not fire again and again.  The thread also
 * reads every I2C_HID_IRQ_CHECK_MS, in case a firing was lost; a line that
 * fires without end with nothing to read is given up, and the device
 * sampled.
 *
 * Without either the
 * thread reads the input register every few milliseconds while fingers
 * move and less often when the pad has been still for a second; a device
 * with nothing to say answers with an empty report (a length of zero), as
 * the Latitude 5330's touchpad does (plan/ws159/phase001).
 */

#include <drivers/acpi/acpi.h>
#include <drivers/generic/hid-report.h>
#include <drivers/generic/hid-input.h>
#include <drivers/gpio/intel-gpio.h>
#include <drivers/i2c/i2c.h>
#include <drivers/i2c/i2c-hid.h>
#include <kern/clock.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/input.h>

#include <stdbool.h>

/* The most HID over I2C devices the driver takes. */
#define I2C_HID_DEVICES_MAX		4U

/* PNP0C50 as a string and as the EISA identifier _HID and _CID may give instead. */
#define I2C_HID_ID_STRING		"PNP0C50"
#define I2C_HID_ID_EISA			0x500cd041U

/* _STA: the device is present. */
#define ACPI_STATUS_PRESENT		0x01U

/*
 * The _DSM of HID over I2C: its UUID 3cdff6f7-4267-4555-ad05-b30a3d8938de
 * in the byte order of an ACPI buffer, its revision, and the function
 * that gives the HID descriptor's register.
 */
#define I2C_HID_DSM_REVISION		1U
#define I2C_HID_DSM_DESCRIPTOR		1U

/* The HID descriptor: its length and the specification's version 1.00. */
#define I2C_HID_DESCRIPTOR_LENGTH	30U
#define I2C_HID_VERSION			0x0100U

/* The commands (opcodes) sent to the command register, and the power state "on". */
#define I2C_HID_OPCODE_RESET		0x01U
#define I2C_HID_OPCODE_SET_REPORT	0x03U
#define I2C_HID_OPCODE_SET_POWER	0x08U
#define I2C_HID_POWER_ON		0x00U

/* A feature report's type in a command, and the report identifier that needs a byte of its own. */
#define I2C_HID_REPORT_FEATURE		0x30U
#define I2C_HID_REPORT_ID_EXTENDED	0x0fU

/* The largest input report the driver reads, and the largest feature report it writes. */
#define I2C_HID_INPUT_MAX		512U
#define I2C_HID_FEATURE_MAX		32U

/* The Precision Touchpad's touch pad mode, and its switches on. */
#define I2C_HID_DEVICE_MODE_TOUCHPAD	3U
#define I2C_HID_SWITCH_ON		1U

/*
 * How long the device is given after a reset before it is read, how often
 * it is read while fingers move and when it is still, and how long after
 * the last report it counts as moving (milliseconds).
 */
#define I2C_HID_RESET_SETTLE_MS		100U
#define I2C_HID_POLL_ACTIVE_MS		6U
#define I2C_HID_POLL_IDLE_MS		25U
#define I2C_HID_ACTIVE_HOLD_MS		1000U

/* How often the interrupt line is looked at, and the most reports read while it stays asserted. */
#define I2C_HID_LINE_POLL_MS		4U
#define I2C_HID_LINE_READS_MAX		8U

/* With the pad's interrupt, how long the thread sleeps at most before it looks at the line itself. */
#define I2C_HID_IRQ_CHECK_MS		1000U

/* With an APIC line: the most reports read at one firing, and the firings in a row with nothing to read that give the line up. */
#define I2C_HID_IRQ_READS_MAX		16U
#define I2C_HID_IRQ_EMPTY_MAX		200U

/* After this many failed reads in a row the thread says so and waits a second. */
#define I2C_HID_ERRORS_BEFORE_PAUSE	50U
#define I2C_HID_ERROR_PAUSE_MS		1000U

/* The longest ACPI path and device name the driver keeps. */
#define I2C_HID_TEXT_MAX		64U

/*
 * One HID over I2C device: where it is (its ACPI node, bus, address and
 * speed), its HID descriptor's registers, its parsed reports, the touch
 * state machine and the input device it is published as.
 *
 * It is allocated by the probe and lives for the kernel's life: the
 * touchpad of a laptop does not go away, and its thread never ends after a
 * successful start.
 */
struct i2c_hid_device {
	struct drv_acpi_node *node;
	char path[I2C_HID_TEXT_MAX];
	char bus_path[DRV_ACPI_RESOURCE_SOURCE_MAX];
	struct drv_i2c_bus *bus;
	uint16_t address;
	uint32_t speed;
	uint16_t descriptor_register;
	/* The interrupt line: its GPIO controller, pin and polarity, and the pad when it is an Intel one. */
	char line_path[DRV_ACPI_RESOURCE_SOURCE_MAX];
	uint32_t line_pin;
	uint8_t line_active_low;
	struct drv_intel_gpio_pad *line;
	/* The Interrupt of its _CRS (ws183-p001): its number (-1 for none), level or edge, active low or high. */
	int irq;
	uint8_t irq_level;
	uint8_t irq_active_low;
	/* The pad's or the line's interrupt: the lock (with interrupts off) over fired, whether it fired, and where the thread waits. */
	struct spinlock irq_lock;
	uint32_t irq_fired;
	struct wait_queue irq_queue;
	uint16_t report_descriptor_length;
	uint16_t report_descriptor_register;
	uint16_t input_register;
	uint16_t max_input_length;
	uint16_t command_register;
	uint16_t data_register;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
	/*
	 * The devices its report descriptor declares and its reports' events
	 * (the HID input glue, ws143-p005), and what the descriptor says about
	 * its fingers.
	 */
	struct hid_input *hidinput;
	struct hid_report_touch_info touch;
	char name[I2C_HID_TEXT_MAX];
	uint8_t input_buffer[I2C_HID_INPUT_MAX];
	struct thread *thread;
};

/*
 * What the probe's walk of the namespace found: the PNP0C50 devices that
 * are present, in the namespace's order.  It lives on the probe's stack.
 */
struct i2c_hid_found {
	struct drv_acpi_node *nodes[I2C_HID_DEVICES_MAX];
	unsigned count;
};

/* The UUID of the HID over I2C _DSM, as the bytes of its ACPI buffer. */
static const uint8_t i2c_hid_dsm_uuid[16] = {
	0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
	0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde
};

static int find_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool is_hid_over_i2c(struct drv_acpi_node *node);
static bool id_matches(const struct drv_acpi_object *object);
static bool device_present(struct drv_acpi_node *node);
static int device_from_acpi(struct i2c_hid_device *device);
static int resource_visitor(const struct drv_acpi_resource *resource, void *argument);
static int descriptor_register(struct i2c_hid_device *device);
static void worker(void *argument);
static void watch_line(struct i2c_hid_device *device);
static void wait_interrupt(struct i2c_hid_device *device);
static void line_interrupt(void *argument);
static int irq_take(struct i2c_hid_device *device);
static void wait_irq(struct i2c_hid_device *device);
static void irq_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void read_while_asserted(struct i2c_hid_device *device);
static void sample(struct i2c_hid_device *device);
static bool line_asserted(const struct i2c_hid_device *device);
static int device_start(struct i2c_hid_device *device);
static int read_hid_descriptor(struct i2c_hid_device *device);
static int send_command(struct i2c_hid_device *device, uint8_t opcode, uint8_t argument);
static int read_report_descriptor(struct i2c_hid_device *device);
static void set_touchpad_mode(struct i2c_hid_device *device);
static int set_feature(struct i2c_hid_device *device, uint32_t usage, uint32_t value);
static int publish(struct i2c_hid_device *device);
static int poll_input(struct i2c_hid_device *device, bool *reported);
static void take_report(struct i2c_hid_device *device, const uint8_t *report, size_t length);
static uint16_t le16(const uint8_t *bytes);
static void put_bits(uint8_t *data, uint32_t offset, uint32_t bits, uint32_t value);
static void sleep_ms(unsigned milliseconds);

/*
 * Finds the HID over I2C devices the ACPI tables name and starts a thread
 * for each, which brings it up and publishes it.  Reports how many were
 * started (0 is no error: most machines have none).
 */
int
drv_i2c_hid_probe(void)
{
	struct i2c_hid_found found;
	struct i2c_hid_device *device;
	unsigned index;
	unsigned started;
	int error;

	/* Walks the namespace for present PNP0C50 devices. */
	kern_memset(&found, 0, sizeof(found));
	error = drv_acpi_walk(NULL, find_visitor, &found);
	if (error < 0 || found.count == 0U)
		return 0;

	/* Starts each device. */
	started = 0;
	for (index = 0; index < found.count; index++) {
		/* Allocates the device's state. */
		device = kern_calloc(1U, sizeof(*device));
		if (device == NULL) {
			kern_logf("i2c-hid: no memory for a device\n");
			break;
		}

		/* The device's node; no Interrupt until its _CRS gives one. */
		device->node = found.nodes[index];
		device->irq = -1;

		/* Reads where the device is from its ACPI objects, and finds its bus. */
		error = device_from_acpi(device);
		if (error != 0) {
			kern_logf("i2c-hid: %s not taken (%d)\n", device->path, error);
			kern_free(device);
			continue;
		}

		/* Its thread brings it up and reads it. */
		error = kthread_create(worker, device, SCHED_PRIORITY_DEFAULT, &device->thread);
		if (error != 0) {
			kern_logf("i2c-hid: %s thread not started (%d)\n", device->path, error);
			kern_free(device);
			continue;
		}

		/* A created thread stays new until it is started. */
		thread_start(device->thread);

		/* One more device runs. */
		started++;
	}

	/* Succeeded: the number of devices started. */
	return (int)started;
}

/* Collects each present PNP0C50 device of the namespace. */
static int
find_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct i2c_hid_found *found;
	enum drv_acpi_type type;
	bool matched;
	bool present;

	/* The walk's depth does not matter. */
	(void)depth;

	/* Only a device can be one. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* A device that is not PNP0C50, or that is not present, is passed over. */
	matched = is_hid_over_i2c(node);
	if (!matched)
		return 0;
	present = device_present(node);
	if (!present)
		return 0;

	/* Keeps the device; a full table ends the walk. */
	found = argument;
	found->nodes[found->count] = node;
	found->count++;
	if (found->count >= I2C_HID_DEVICES_MAX)
		return -1;

	/* Goes on with the next device. */
	return 0;
}

/* Tells whether a device's _HID or _CID names PNP0C50. */
static bool
is_hid_over_i2c(
	struct drv_acpi_node *node)
{
	struct drv_acpi_object *object;
	struct drv_acpi_object *element;
	enum drv_acpi_type type;
	unsigned count;
	unsigned index;
	bool matched;
	int error;

	/* The hardware identifier first. */
	object = NULL;
	error = drv_acpi_evaluate(node, "_HID", NULL, 0, &object);
	if (error == 0) {
		/* A _HID of PNP0C50 is enough. */
		matched = id_matches(object);
		drv_acpi_object_release(object);
		if (matched)
			return true;
	}

	/* Then the compatible identifiers: one, or a package of them. */
	object = NULL;
	error = drv_acpi_evaluate(node, "_CID", NULL, 0, &object);
	if (error != 0 || object == NULL)
		return false;

	/* A package names several; any of them may be PNP0C50. */
	matched = false;
	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_PACKAGE) {
		count = drv_acpi_object_package_count(object);
		for (index = 0; index < count; index++) {
			/* The first PNP0C50 among them is enough. */
			element = drv_acpi_object_package_element(object, index);
			matched = id_matches(element);
			if (matched)
				break;
		}
	} else {
		matched = id_matches(object);
	}

	/* The object is no longer needed. */
	drv_acpi_object_release(object);

	/* Succeeded: whether the device is HID over I2C. */
	return matched;
}

/* Tells whether an identifier object is PNP0C50, as a string or an EISA identifier. */
static bool
id_matches(
	const struct drv_acpi_object *object)
{
	enum drv_acpi_type type;
	const char *text;
	uint64_t value;
	size_t length;
	int compared;

	/* No object names nothing. */
	if (object == NULL)
		return false;

	/* The EISA form. */
	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_INTEGER) {
		value = drv_acpi_object_integer(object);
		if (value == I2C_HID_ID_EISA)
			return true;
		return false;
	}

	/* The string form. */
	if (type != DRV_ACPI_TYPE_STRING)
		return false;
	text = drv_acpi_object_string(object, &length);
	if (text == NULL)
		return false;
	compared = kern_strcmp(text, I2C_HID_ID_STRING);
	if (compared != 0)
		return false;

	/* Succeeded: the identifier is PNP0C50. */
	return true;
}

/* Tells whether a device is present: its _STA says so, or it has none. */
static bool
device_present(
	struct drv_acpi_node *node)
{
	uint64_t status;
	int error;

	/* A device without _STA is present. */
	error = drv_acpi_evaluate_integer(node, "_STA", &status);
	if (error != 0)
		return true;

	/* The present bit. */
	if ((status & ACPI_STATUS_PRESENT) == 0U)
		return false;

	/* Succeeded: the device is present. */
	return true;
}

/*
 * Reads a device's place from ACPI: its path, its I2C connection (bus,
 * address, speed), its HID descriptor's register, and the bus's driver.
 */
static int
device_from_acpi(
	struct i2c_hid_device *device)
{
	int error;

	/* The path, for the log. */
	error = drv_acpi_node_path(device->node, device->path, sizeof(device->path));
	if (error != 0)
		(void)kern_snprintf(device->path, sizeof(device->path), "(unnamed)");

	/* The I2C connection from _CRS. */
	error = drv_acpi_resources_walk(device->node, NULL, resource_visitor, device);
	if (error != 0)
		return error;
	if (device->bus_path[0] == '\0')
		return ENODEV;

	/* The HID descriptor's register from _DSM. */
	error = descriptor_register(device);
	if (error != 0)
		return error;

	/* The bus, which the LPSS driver registered under the controller's path. */
	error = drv_i2c_bus_find_acpi(device->bus_path, &device->bus);
	if (error != 0)
		return error;

	/* Succeeded: the device can be reached. */
	return 0;
}

/* Takes the first I2C connection, the first GPIO interrupt and the first Interrupt of a device's resources. */
static int
resource_visitor(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	struct i2c_hid_device *device;

	/* The first Interrupt it consumes (an APIC line, ws183-p001). */
	device = argument;
	if (resource->kind == DRV_ACPI_RESOURCE_IRQ && !resource->producer && device->irq < 0) {
		device->irq = (int)resource->base;
		device->irq_level = resource->level;
		device->irq_active_low = resource->active_low;
		return 0;
	}

	/* The first GPIO interrupt is the device's line. */
	if (resource->kind == DRV_ACPI_RESOURCE_GPIO_INT && device->line_path[0] == '\0') {
		(void)kern_snprintf(device->line_path, sizeof(device->line_path), "%s", resource->source);
		device->line_pin = (uint32_t)resource->base;
		device->line_active_low = resource->active_low;
		return 0;
	}

	/* Only the first I2C connection matters. */
	if (resource->kind != DRV_ACPI_RESOURCE_I2C || device->bus_path[0] != '\0')
		return 0;

	/* The address, the speed and the controller. */
	device->address = (uint16_t)resource->base;
	device->speed = resource->speed;
	(void)kern_snprintf(device->bus_path, sizeof(device->bus_path), "%s", resource->source);

	/* Goes on: the other resources are not needed. */
	return 0;
}

/*
 * Asks the device's _DSM for its HID descriptor's register.  Evaluating it
 * also tells a Dell's firmware that the OS drives the touchpad over I2C
 * (its PS/2 mouse then reports itself absent, plan/ws159/phase001 D7).
 */
static int
descriptor_register(
	struct i2c_hid_device *device)
{
	struct drv_acpi_object *arguments[4];
	struct drv_acpi_object *result;
	enum drv_acpi_type type;
	unsigned index;
	int error;

	/* The UUID, the revision, the function, and an empty package. */
	arguments[0] = drv_acpi_object_buffer_new(i2c_hid_dsm_uuid, sizeof(i2c_hid_dsm_uuid));
	arguments[1] = drv_acpi_object_integer_new(I2C_HID_DSM_REVISION);
	arguments[2] = drv_acpi_object_integer_new(I2C_HID_DSM_DESCRIPTOR);
	arguments[3] = drv_acpi_object_package_new(0U);

	/* Evaluates the _DSM when every argument was made. */
	result = NULL;
	error = 0;
	for (index = 0; index < 4U; index++) {
		if (arguments[index] == NULL)
			error = ENOMEM;
	}

	/* The evaluation itself. */
	if (error == 0)
		error = drv_acpi_evaluate(device->node, "_DSM", arguments, 4U, &result);

	/* The arguments are no longer needed. */
	for (index = 0; index < 4U; index++)
		drv_acpi_object_release(arguments[index]);

	/* A failed evaluation, or an answer that is not a register. */
	if (error != 0)
		return error;
	if (result == NULL)
		return EINVAL;
	type = drv_acpi_object_type(result);
	if (type != DRV_ACPI_TYPE_INTEGER) {
		drv_acpi_object_release(result);
		return EINVAL;
	}

	/* The register. */
	device->descriptor_register = (uint16_t)drv_acpi_object_integer(result);
	drv_acpi_object_release(result);

	/* Succeeded: the descriptor can be read. */
	return 0;
}

/*
 * Brings one device up and reads it for as long as the kernel runs: when
 * its interrupt line is an Intel GPIO pad, while the line is asserted;
 * otherwise often while fingers move and less often while the pad is still.
 */
static void
worker(
	void *argument)
{
	struct i2c_hid_device *device;
	int error;
	int line;

	/* The device the probe started this thread for. */
	device = argument;

	/* Brings it up; a device that does not come up is left alone. */
	error = device_start(device);
	if (error != 0) {
		kern_logf("i2c-hid: %s did not start (%d); its PS/2 mouse, if any, stays\n", device->path, error);
		return;
	}

	/* Where the thread waits for an interrupt, the pad's or the line's. */
	spin_init(&device->irq_lock, LOCK_RANK_DEVICE, "i2c-hid irq");
	waitq_init(&device->irq_queue, "i2c-hid irq");

	/* The line, when it is a pad this kernel can read. */
	error = ENOENT;
	if (device->line_path[0] != '\0')
		error = drv_intel_gpio_pad_find(device->line_path, device->line_pin, &device->line);

	/* Without a pad, the Interrupt of its _CRS when it has one (until it is given up), then sampling. */
	if (error != 0) {
		if (device->irq >= 0) {
			line = irq_take(device);
			if (line == 0) {
				kern_logf("i2c-hid: %s reads on its interrupt (irq %d level=%u active_low=%u)\n", device->path, device->irq, (unsigned)device->irq_level, (unsigned)device->irq_active_low);
				wait_irq(device);
			}

			/* The line could not be had, or was given up. */
			kern_logf("i2c-hid: %s samples its input (irq %d: %d)\n", device->path, device->irq, line);
		} else {
			kern_logf("i2c-hid: %s samples its input (line: %d)\n", device->path, error);
		}

		/* Sampling, for as long as the kernel runs. */
		sample(device);
		return;
	}

	/* The pad's interrupt, when it can interrupt; until the interrupt is given up, if ever. */
	error = drv_intel_gpio_pad_irq_enable(device->line, line_interrupt, device);
	if (error == 0) {
		kern_logf("i2c-hid: %s reads on its interrupt (%s pin %u)\n", device->path, device->line_path, device->line_pin);
		wait_interrupt(device);
	}

	/* Watching the line, for as long as the kernel runs. */
	if (error == 0) {
		kern_logf("i2c-hid: %s: the interrupt was given up; watches its line\n", device->path);
	} else {
		kern_logf("i2c-hid: %s reads on its line (%s pin %u; interrupt: %d)\n", device->path, device->line_path, device->line_pin, error);
	}

	/* The line's watch never ends. */
	watch_line(device);
}

/*
 * Reads the device each time its pad fires, and when the line is found
 * asserted without a firing; returns when the controller's interrupt was
 * given up.
 */
static void
wait_interrupt(
	struct i2c_hid_device *device)
{
	unsigned long state;
	uint64_t observed;
	uint64_t deadline;
	uint64_t now;
	int alive;
	int error;

	/* For as long as the interrupt comes. */
	for (;;) {
		/* Sleeps until the pad fires, or the time to look comes. */
		deadline = sched_ticks() + kern_ms_to_ticks(I2C_HID_IRQ_CHECK_MS) + 1U;
		state = spin_lock_irqsave(&device->irq_lock);

		while (device->irq_fired == 0U) {
			/* The time to look at the line came. */
			now = sched_ticks();
			if (now >= deadline)
				break;

			/* Sleeps until the handler wakes it, or the deadline. */
			observed = waitq_sequence(&device->irq_queue);
			error = waitq_sleep(&device->irq_queue, &device->irq_lock, observed, deadline, 0U);
			(void)error;
		}

		/* The firing is taken. */
		device->irq_fired = 0U;

		spin_unlock_irqrestore(&device->irq_lock, state);

		/* The reports while the line is asserted, then the pad's interrupt on again. */
		read_while_asserted(device);
		drv_intel_gpio_pad_irq_arm(device->line);

		/* The controller's interrupt given up: the caller watches the line. */
		alive = drv_intel_gpio_pad_irq_alive(device->line);
		if (!alive)
			return;
	}
}

/* The pad fired (in interrupt context): the thread wakes. */
static void
line_interrupt(
	void *argument)
{
	struct i2c_hid_device *device;

	/* Marked under the lock the thread sleeps with, and woken. */
	device = argument;
	spin_lock(&device->irq_lock);

	device->irq_fired = 1U;
	waitq_wake_all(&device->irq_queue);

	spin_unlock(&device->irq_lock);
}

/*
 * Takes the device's Interrupt (an APIC line): its handler, its trigger
 * mode and polarity as its _CRS says, and unmasked.  Returns 0, or why it
 * cannot be had (the handler is given back then).
 */
static int
irq_take(
	struct i2c_hid_device *device)
{
	unsigned trigger;
	unsigned polarity;
	int error;

	/* The handler, while the line is still masked. */
	error = kern_irq_register(device->irq, irq_interrupt, device);
	if (error != 0)
		return error;

	/* The line's trigger mode and polarity. */
	trigger = KERN_IRQ_TRIGGER_EDGE;
	if (device->irq_level != 0U)
		trigger = KERN_IRQ_TRIGGER_LEVEL;
	polarity = KERN_IRQ_POLARITY_HIGH;
	if (device->irq_active_low != 0U)
		polarity = KERN_IRQ_POLARITY_LOW;
	error = kern_irq_set_mode(device->irq, trigger, polarity);
	if (error != 0) {
		(void)kern_irq_unregister(device->irq, irq_interrupt, device);
		return error;
	}

	/* Succeeded: the line is live. */
	kern_irq_unmask(device->irq);
	return 0;
}

/*
 * Reads the device each time its line fires (and every IRQ_CHECK_MS in
 * case a firing was lost): its reports until an empty one, then the line
 * unmasked.  Returns when the line fired too often with nothing to read,
 * the line given back.
 */
static void
wait_irq(
	struct i2c_hid_device *device)
{
	unsigned long state;
	uint64_t observed;
	uint64_t deadline;
	uint64_t now;
	unsigned empty;
	unsigned reads;
	uint32_t fired;
	bool reported;
	bool any;
	int error;

	/* For as long as the line is worth it. */
	empty = 0;
	for (;;) {
		/* Sleeps until the line fires, or the time to look comes. */
		deadline = sched_ticks() + kern_ms_to_ticks(I2C_HID_IRQ_CHECK_MS) + 1U;
		state = spin_lock_irqsave(&device->irq_lock);

		while (device->irq_fired == 0U) {
			/* The time to look came. */
			now = sched_ticks();
			if (now >= deadline)
				break;

			/* Sleeps until the handler wakes it, or the deadline. */
			observed = waitq_sequence(&device->irq_queue);
			error = waitq_sleep(&device->irq_queue, &device->irq_lock, observed, deadline, 0U);
			(void)error;
		}

		/* The firing is taken. */
		fired = device->irq_fired;
		device->irq_fired = 0U;

		spin_unlock_irqrestore(&device->irq_lock, state);

		/* The reports until the device has nothing more (a failed read ends the turn). */
		any = false;
		for (reads = 0; reads < I2C_HID_IRQ_READS_MAX; reads++) {
			reported = false;
			error = poll_input(device, &reported);
			if (error != 0 || !reported)
				break;
			any = true;
		}

		/* A look without a firing leaves the line as it is. */
		if (fired == 0U)
			continue;

		/* Firings with nothing to read, without end: the line is given back, and the device sampled. */
		empty++;
		if (any)
			empty = 0;
		if (empty >= I2C_HID_IRQ_EMPTY_MAX) {
			kern_logf("i2c-hid: %s: irq %d fired %u times with nothing to read; gives it up\n", device->path, device->irq, empty);
			(void)kern_irq_unregister(device->irq, irq_interrupt, device);
			kern_irq_unmask(device->irq);
			return;
		}

		/* The line again, which fires at once while it stays asserted. */
		kern_irq_unmask(device->irq);
	}
}

/* The line fired (in interrupt context): masked until the thread has read the device, and the thread wakes. */
static void
irq_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct i2c_hid_device *device;

	/* Masked first, so a level that stays asserted does not come back before the reads. */
	device = argument;
	kern_irq_mask(irq);

	/* Marked under the lock the thread sleeps with, and woken. */
	spin_lock(&device->irq_lock);

	device->irq_fired = 1U;
	waitq_wake_all(&device->irq_queue);

	spin_unlock(&device->irq_lock);

	/* The interrupt is over. */
	kern_irq_send_eoi(acknowledge);
}

/* Reads reports while the device's line is asserted, a few at a time between looks at the line. */
static void
read_while_asserted(
	struct i2c_hid_device *device)
{
	unsigned reads;
	bool asserted;
	bool reported;
	int error;

	/* While the line says a report waits. */
	reads = 0;
	asserted = line_asserted(device);
	while (asserted) {
		/* One report; a failed read ends the turn (the interrupt or the next look comes back). */
		reported = false;
		error = poll_input(device, &reported);
		if (error != 0)
			break;

		/* A long run gives the processor up now and then. */
		reads++;
		if (reads % I2C_HID_LINE_READS_MAX == 0U)
			sleep_ms(1U);
		asserted = line_asserted(device);
	}
}

/*
 * Reads the device while its interrupt line is asserted, looking at the
 * line every LINE_POLL_MS: a still pad costs no transfer.
 */
static void
watch_line(
	struct i2c_hid_device *device)
{
	unsigned reads;
	bool asserted;
	bool reported;
	int error;

	/* For as long as the kernel runs. */
	for (;;) {
		/* Reads while the line says a report waits, a few at a time. */
		reads = 0;
		asserted = line_asserted(device);
		while (asserted && reads < I2C_HID_LINE_READS_MAX) {
			/* One report; a failed read leaves the line to be looked at again. */
			reported = false;
			error = poll_input(device, &reported);
			if (error != 0)
				break;
			reads++;
			asserted = line_asserted(device);
		}

		/* Looks at the line again a little later. */
		sleep_ms(I2C_HID_LINE_POLL_MS);
	}
}

/* Tells whether the device's interrupt line is asserted. */
static bool
line_asserted(
	const struct i2c_hid_device *device)
{
	int level;

	/* The line's level on the wire. */
	level = drv_intel_gpio_pad_level(device->line);

	/* An active low line is asserted low, an active high one high. */
	if (device->line_active_low) {
		if (level == 0)
			return true;
		return false;
	}

	/* An active high line is asserted high. */
	if (level != 0)
		return true;

	/* Succeeded: the line is not asserted. */
	return false;
}

/*
 * Reads the device without its line: often while fingers move, less often
 * while the pad is still.
 */
static void
sample(
	struct i2c_hid_device *device)
{
	uint64_t last_report_ms;
	uint64_t now;
	unsigned interval;
	unsigned errors;
	bool reported;
	int error;

	/* For as long as the kernel runs. */
	last_report_ms = 0;
	errors = 0;
	for (;;) {
		/* One read of the input register. */
		reported = false;
		error = poll_input(device, &reported);
		now = clock_milliseconds(NULL);

		/* Too many failures in a row: says so and waits before trying again. */
		if (error != 0) {
			errors++;
			if (errors >= I2C_HID_ERRORS_BEFORE_PAUSE) {
				kern_logf("i2c-hid: %s: %u reads failed (last %d), pausing\n", device->path, errors, error);
				errors = 0;
				sleep_ms(I2C_HID_ERROR_PAUSE_MS);
			}
		} else {
			errors = 0;
		}

		/* A report keeps the reads frequent for a while. */
		if (reported)
			last_report_ms = now;

		/* Reads often while fingers move, less often when the pad is still. */
		interval = I2C_HID_POLL_IDLE_MS;
		if (now - last_report_ms < I2C_HID_ACTIVE_HOLD_MS)
			interval = I2C_HID_POLL_ACTIVE_MS;
		sleep_ms(interval);
	}
}

/*
 * Brings a device up: its HID descriptor, power on, reset, its report
 * descriptor, the touch pad mode, and its input device.
 */
static int
device_start(
	struct i2c_hid_device *device)
{
	int error;

	/* The HID descriptor gives every register. */
	error = read_hid_descriptor(device);
	if (error != 0)
		return error;

	/* Powers the device on, and resets it. */
	error = send_command(device, I2C_HID_OPCODE_SET_POWER, I2C_HID_POWER_ON);
	if (error != 0)
		return error;
	error = send_command(device, I2C_HID_OPCODE_RESET, 0U);
	if (error != 0)
		return error;

	/* Gives the reset its time, and takes the empty report that ends it. */
	sleep_ms(I2C_HID_RESET_SETTLE_MS);
	(void)drv_i2c_transfer(device->bus, device->address, device->speed, NULL, 0U, device->input_buffer, device->max_input_length);

	/* The report descriptor, parsed. */
	error = read_report_descriptor(device);
	if (error != 0)
		return error;

	/* A device with no fingers is not one this driver publishes. */
	error = drv_hid_input_touch(device->hidinput, &device->touch);
	if (error != 0) {
		kern_logf("i2c-hid: %s %04x:%04x has no touch pad or touch screen\n", device->path, device->vendor, device->product);
		return ENODEV;
	}

	/* A Precision Touchpad reports its fingers only in its touch pad mode. */
	if (device->touch.pad)
		set_touchpad_mode(device);

	/* Publishes the input device. */
	error = publish(device);
	if (error != 0)
		return error;

	/* Succeeded: the log says what came up. */
	kern_logf("i2c-hid: %s %04x:%04x at 0x%02x on %s: %s, %u fingers, %u buttons\n",
		  device->path,
		  device->vendor,
		  device->product,
		  device->address,
		  device->bus_path,
		  device->name,
		  (unsigned)device->touch.contacts,
		  (unsigned)device->touch.buttons);
	return 0;
}

/* Reads and checks the HID descriptor, and keeps its registers. */
static int
read_hid_descriptor(
	struct i2c_hid_device *device)
{
	uint8_t request[2];
	uint8_t descriptor[I2C_HID_DESCRIPTOR_LENGTH];
	uint16_t length;
	uint16_t version;
	int error;

	/* Reads the descriptor at its register. */
	request[0] = (uint8_t)(device->descriptor_register & 0xffU);
	request[1] = (uint8_t)(device->descriptor_register >> 8);
	error = drv_i2c_transfer(device->bus, device->address, device->speed, request, sizeof(request), descriptor, sizeof(descriptor));
	if (error != 0)
		return error;

	/* Refuses a descriptor of another length or version. */
	length = le16(descriptor + 0);
	if (length != I2C_HID_DESCRIPTOR_LENGTH)
		return EIO;
	version = le16(descriptor + 2);
	if (version != I2C_HID_VERSION)
		return EIO;

	/* The registers and the lengths. */
	device->report_descriptor_length = le16(descriptor + 4);
	device->report_descriptor_register = le16(descriptor + 6);
	device->input_register = le16(descriptor + 8);
	device->max_input_length = le16(descriptor + 10);
	device->command_register = le16(descriptor + 16);
	device->data_register = le16(descriptor + 18);
	device->vendor = le16(descriptor + 20);
	device->product = le16(descriptor + 22);
	device->version = le16(descriptor + 24);

	/* Refuses an input report larger than the buffer, or too small for its length. */
	if (device->max_input_length > I2C_HID_INPUT_MAX || device->max_input_length < 2U)
		return EIO;
	if (device->report_descriptor_length == 0U || device->report_descriptor_length > HID_REPORT_DESCRIPTOR_SIZE_MAX)
		return EIO;

	/* Succeeded: the registers are known. */
	return 0;
}

/* Sends one command without data: the opcode and its argument to the command register. */
static int
send_command(
	struct i2c_hid_device *device,
	uint8_t opcode,
	uint8_t argument)
{
	uint8_t command[4];
	int error;

	/* The command register, then the argument and the opcode. */
	command[0] = (uint8_t)(device->command_register & 0xffU);
	command[1] = (uint8_t)(device->command_register >> 8);
	command[2] = argument;
	command[3] = opcode;
	error = drv_i2c_transfer(device->bus, device->address, device->speed, command, sizeof(command), NULL, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the device took the command. */
	return 0;
}

/* Reads the report descriptor and describes the devices it declares. */
static int
read_report_descriptor(
	struct i2c_hid_device *device)
{
	uint8_t request[2];
	uint8_t *descriptor;
	int error;

	/* A buffer for the descriptor. */
	descriptor = kern_calloc(1U, device->report_descriptor_length);
	if (descriptor == NULL)
		return ENOMEM;

	/* Reads it at its register. */
	request[0] = (uint8_t)(device->report_descriptor_register & 0xffU);
	request[1] = (uint8_t)(device->report_descriptor_register >> 8);
	error = drv_i2c_transfer(device->bus, device->address, device->speed, request, sizeof(request), descriptor, device->report_descriptor_length);

	/* Parses it with the HID layer. */
	if (error == 0)
		error = drv_hid_input_prepare(descriptor, device->report_descriptor_length, &device->hidinput);

	/* The bytes are no longer needed; the description keeps what it uses. */
	kern_free(descriptor);

	/* Reports a descriptor that could not be read or parsed. */
	if (error != 0)
		return error;

	/* Succeeded: the description is ready. */
	return 0;
}

/*
 * Puts a Precision Touchpad in its touch pad mode with its surface and its
 * button on, and its latency normal.  A feature the descriptor does not
 * have is passed over; the log says when the mode could not be set.
 */
static void
set_touchpad_mode(
	struct i2c_hid_device *device)
{
	int error;

	/* The latency mode first, normal; then the switches; the mode last, as Linux sends them. */
	(void)set_feature(device, HID_REPORT_USAGE_LATENCY_MODE, 0U);
	(void)set_feature(device, HID_REPORT_USAGE_SURFACE_SWITCH, I2C_HID_SWITCH_ON);
	(void)set_feature(device, HID_REPORT_USAGE_BUTTON_SWITCH, I2C_HID_SWITCH_ON);
	error = set_feature(device, HID_REPORT_USAGE_DEVICE_MODE, I2C_HID_DEVICE_MODE_TOUCHPAD);
	if (error != 0)
		kern_logf("i2c-hid: %s: touch pad mode not set (%d)\n", device->path, error);
}

/*
 * Sets one field of a feature report with SET_REPORT: the command register
 * names the report, the data register takes its length, its identifier and
 * its data.  The report's other fields are written as zero, except the
 * two switches, which share a report and are both turned on.
 */
static int
set_feature(
	struct i2c_hid_device *device,
	uint32_t usage,
	uint32_t value)
{
	struct hid_report_feature_info feature;
	struct hid_report_feature_info other;
	uint8_t message[12U + I2C_HID_FEATURE_MAX];
	uint8_t *data;
	size_t length;
	size_t data_offset;
	uint16_t data_length;
	int error;

	/* Where the field is; a descriptor without it has nothing to set. */
	error = drv_hid_input_feature(device->hidinput, usage, &feature);
	if (error != 0)
		return error;
	if (feature.data_size == 0U || feature.data_size > I2C_HID_FEATURE_MAX)
		return EINVAL;

	/* The command register, the report's type and identifier, and SET_REPORT. */
	kern_memset(message, 0, sizeof(message));
	length = 0;
	message[length++] = (uint8_t)(device->command_register & 0xffU);
	message[length++] = (uint8_t)(device->command_register >> 8);
	if (feature.report_id < I2C_HID_REPORT_ID_EXTENDED) {
		message[length++] = (uint8_t)(I2C_HID_REPORT_FEATURE | feature.report_id);
		message[length++] = I2C_HID_OPCODE_SET_REPORT;
	} else {
		/* An identifier of 15 or more follows the opcode in a byte of its own. */
		message[length++] = (uint8_t)(I2C_HID_REPORT_FEATURE | I2C_HID_REPORT_ID_EXTENDED);
		message[length++] = I2C_HID_OPCODE_SET_REPORT;
		message[length++] = feature.report_id;
	}

	/* The data register, and the length of what follows it (itself, the identifier and the data). */
	message[length++] = (uint8_t)(device->data_register & 0xffU);
	message[length++] = (uint8_t)(device->data_register >> 8);
	data_length = (uint16_t)(2U + 1U + feature.data_size);
	message[length++] = (uint8_t)(data_length & 0xffU);
	message[length++] = (uint8_t)(data_length >> 8);

	/* The identifier, then the data with the field set. */
	message[length++] = feature.report_id;
	data_offset = length;
	data = message + data_offset;
	put_bits(data, feature.bit_offset, feature.bit_size, value);
	length += feature.data_size;

	/* The surface switch and the button switch share a report: both are turned on together. */
	if (usage == HID_REPORT_USAGE_SURFACE_SWITCH || usage == HID_REPORT_USAGE_BUTTON_SWITCH) {
		error = drv_hid_input_feature(device->hidinput, HID_REPORT_USAGE_SURFACE_SWITCH, &other);
		if (error == 0 && other.report_id == feature.report_id)
			put_bits(data, other.bit_offset, other.bit_size, I2C_HID_SWITCH_ON);
		error = drv_hid_input_feature(device->hidinput, HID_REPORT_USAGE_BUTTON_SWITCH, &other);
		if (error == 0 && other.report_id == feature.report_id)
			put_bits(data, other.bit_offset, other.bit_size, I2C_HID_SWITCH_ON);
	}

	/* Writes the command and the data in one transfer. */
	error = drv_i2c_transfer(device->bus, device->address, device->speed, message, length, NULL, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the device has the feature. */
	return 0;
}

/*
 * Registers the touch pad's or touch screen's device through the HID input
 * glue: the touch device alone (a Precision Touchpad's mouse collection is
 * left unpublished, its reports dropped).
 */
static int
publish(
	struct i2c_hid_device *device)
{
	struct hid_input_identity identity;
	const char *kind;
	int error;

	/* Its name, as "vendor:product Touchpad". */
	kind = "Touchscreen";
	if (device->touch.pad)
		kind = "Touchpad";
	(void)kern_snprintf(device->name, sizeof(device->name), "%04X:%04X %s", device->vendor, device->product, kind);

	/* The touch device's identity: the name and the device's ACPI path, no unique ID. */
	kern_memset(&identity, 0, sizeof(identity));
	identity.name = device->name;
	identity.physical_path = device->path;
	identity.unique_id = NULL;
	identity.touch_name = device->name;
	identity.touch_physical_path = device->path;
	identity.id.bustype = BUS_I2C;
	identity.id.vendor = device->vendor;
	identity.id.product = device->product;
	identity.id.version = device->version;
	identity.flags = HID_INPUT_TOUCH_ONLY;

	/* Registers it. */
	error = drv_hid_input_publish(device->hidinput, &identity);
	if (error != 0)
		return error;

	/* Succeeded: readers can open the device. */
	return 0;
}

/*
 * Reads the input register once: an empty report says nothing happened, a
 * report goes through the touch state machine.  *reported says whether a
 * report came.
 */
static int
poll_input(
	struct i2c_hid_device *device,
	bool *reported)
{
	uint16_t length;
	int error;

	/* A plain read gives the report the device holds, its length first. */
	error = drv_i2c_transfer(device->bus, device->address, device->speed, NULL, 0U, device->input_buffer, device->max_input_length);
	if (error != 0)
		return error;

	/* An empty report (length 0, or the two bytes of the length alone) says nothing happened. */
	length = le16(device->input_buffer);
	if (length <= 2U)
		return 0;

	/* A length past what was read is not a report. */
	if (length > device->max_input_length)
		return EIO;

	/* Takes the report: its identifier and data after the length. */
	take_report(device, device->input_buffer + 2, (size_t)length - 2U);
	*reported = true;

	/* Succeeded: the report was taken. */
	return 0;
}

/* Hands one input report to the HID input glue, at the time it arrived. */
static void
take_report(
	struct i2c_hid_device *device,
	const uint8_t *report,
	size_t length)
{
	uint64_t now;

	/* The glue decodes it and the touch state machine makes its events; other reports are dropped. */
	now = clock_milliseconds(NULL);
	drv_hid_input_report(device->hidinput, report, length, now);
}

/* Reads a 16-bit little-endian field. */
static uint16_t
le16(
	const uint8_t *bytes)
{
	uint16_t value;

	/* The low byte, then the high one. */
	value = (uint16_t)bytes[0];
	value = (uint16_t)(value | ((uint16_t)bytes[1] << 8));

	/* Succeeded: the field's value. */
	return value;
}

/* Stores a value of some bits at a bit offset, least significant bit first. */
static void
put_bits(
	uint8_t *data,
	uint32_t offset,
	uint32_t bits,
	uint32_t value)
{
	uint32_t index;
	uint32_t bit;

	/* One bit at a time; the bits of the value past 32 are zero. */
	for (index = 0; index < bits && index < 32U; index++) {
		/* A clear bit leaves the data as it is. */
		if (((value >> index) & 1U) == 0U)
			continue;

		/* Sets the data's bit. */
		bit = offset + index;
		data[bit / 8U] = (uint8_t)(data[bit / 8U] | (1U << (bit % 8U)));
	}
}

/* Sleeps the thread for a number of milliseconds. */
static void
sleep_ms(
	unsigned milliseconds)
{
	/* At least one tick. */
	sched_sleep(sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U);
}
