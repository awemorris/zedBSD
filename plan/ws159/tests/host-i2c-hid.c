/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the HID over I2C driver (ws159-p003).
 *
 * The unchanged driver (src/drivers/i2c/i2c-hid.c, compiled freestanding)
 * runs against stand-ins: an ACPI namespace with the Latitude 5330's
 * \_SB.PC00.I2C1.TPD0 (_HID VEN_06CB, _CID PNP0C50, _STA 0x0f, its
 * I2cSerialBus at 0x2c and 400 kHz, a _DSM that gives the HID descriptor's
 * register 0x20), and an I2C bus with the 5330's touchpad on it, which
 * answers with the HID descriptor and the report descriptor taken from its
 * Linux and then with scripted input reports.  The test checks:
 *   - the probe takes the device and starts its thread;
 *   - the commands and feature reports the driver writes are the ones the
 *     Linux log shows (power on "22 00 00 08", reset "22 00 00 01", and the
 *     SET_REPORTs of the latency mode, the switches and the device mode
 *     "22 00 34 03 23 00 04 00 04 03");
 *   - the input device is a touch pad on the I2C bus, a pointer and a
 *     button pad;
 *   - one finger down, the pad pressed and the finger lifted come out as
 *     protocol B events with BTN_TOOL_FINGER and BTN_LEFT, and an empty
 *     read (length 0) emits nothing.
 * With the argument "line" the device's GpioInt is an Intel GPIO pad
 * (the stand-in pad is low, asserted, while a scripted report waits): the
 * driver reads only while the line is asserted, so no empty read happens;
 * with "sample" no pad is found and the driver samples the input register.
 * In the "line" run the pad cannot interrupt (the enable fails) and the
 * driver watches the line; with "irq" the pad's interrupt is enabled, the
 * stand-in fires it while a scripted report waits, and the driver reads
 * the reports and arms the pad again, sleeping at most a second between.
 * The thread's endless loop is left with a longjmp from the sleep once the
 * script is done.
 *
 *   plan/ws159/tests/run-host-i2c-hid.sh
 */

#include <drivers/acpi/acpi.h>
#include <drivers/i2c/i2c.h>
#include <kern/input-device.h>
#include <kern/irq.h>
#include <uapi/input.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The touchpad's registers and address, as its HID descriptor and _CRS give them. */
#define PAD_ADDRESS		0x2cU
#define PAD_DESCRIPTOR_REGISTER	0x20U
#define PAD_REPORT_REGISTER	0x21U

/* The most writes, events and scripted reports the test keeps. */
#define WRITES_MAX		32U
#define WRITE_BYTES_MAX		32U
#define EVENTS_MAX		256U
#define REPORTS_MAX		8U
#define REPORT_BYTES		32U

/* The sleeps the thread may take after the script before the test ends it. */
#define SLEEPS_AFTER_SCRIPT	3U

/*
 * An ACPI object as the stand-in namespace makes it: an integer, a string,
 * a buffer or a package (of no elements here).
 */
struct drv_acpi_object {
	enum drv_acpi_type type;
	uint64_t integer;
	char string[16];
	uint8_t buffer[16];
	size_t length;
};

/* One write the bus saw: its bytes. */
struct bus_write {
	uint8_t bytes[WRITE_BYTES_MAX];
	size_t length;
};

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

/* The touchpad's report descriptor, read from the file. */
static uint8_t report_descriptor[1024];
static size_t report_descriptor_length;

/* The writes the bus saw, in order. */
static struct bus_write writes[WRITES_MAX];
static size_t write_count;

/* The scripted input reports, each with its length prefix, and the next one to give. */
static uint8_t reports[REPORTS_MAX][REPORT_BYTES];
static size_t report_count;
static size_t report_next;
static size_t empty_reads;

/* The input device the driver registered, and the events it emitted. */
static struct input_device_info registered;
static int registered_count;
static struct input_event events[EVENTS_MAX];
static size_t event_count;

/* The thread the driver started, and where its endless loop is left. */
static void (*thread_entry)(void *);
static void *thread_argument;
static jmp_buf thread_exit;
static int thread_started;
static unsigned sleeps_after_script;

/* Whether the device's line is a pad (the "line" run), and the pin the driver asked for. */
static int line_mode;
static int irq_mode;

/* The pad's interrupt in the "irq" run: its handler, how often it was enabled, armed and fired, and the longest sleep asked. */
static void (*pad_handler)(void *);
static void *pad_argument;
static int irq_enables;
static int irq_arms;
static int irq_fires;
static uint64_t longest_wait_ms;
static uint32_t line_pin_asked;
static int fake_pad;

/* The fake clock, in milliseconds and ticks. */
static uint64_t now_ms;

/* The node and the bus the stand-ins hand out (only compared, never followed). */
static int fake_node;
static int fake_bus;

/* The 5330's HID descriptor, from the Linux log. */
static const uint8_t hid_descriptor[30] = {
	0x1e, 0x00, 0x00, 0x01, 0x99, 0x02, 0x21, 0x00, 0x24, 0x00,
	0x40, 0x00, 0x25, 0x00, 0x17, 0x00, 0x22, 0x00, 0x23, 0x00,
	0xcb, 0x06, 0x65, 0xce, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00
};

void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);
void *kern_memcpy(void *destination, const void *source, size_t length);
void *kern_memset(void *destination, int value, size_t length);
int kern_strcmp(const char *left, const char *right);
int kern_snprintf(char *buffer, size_t size, const char *format, ...);
void kern_logf(const char *format, ...);
uint64_t clock_milliseconds(void *context);
uint64_t sched_ticks(void);
void sched_sleep(uint64_t timeout_tick);
int kthread_create(void (*entry)(void *), void *argument, int priority, void **result);
void thread_start(void *thread);
int drv_i2c_hid_probe(void);
int drv_intel_gpio_pad_find(const char *controller, uint32_t pin, void **result);
int drv_intel_gpio_pad_level(const void *pad);
static void check(int condition, const char *what);
static int written(size_t index, const uint8_t *bytes, size_t length);
static int has_event(uint16_t type, uint16_t code, int32_t value);
static void put_bits(uint8_t *report, unsigned offset, unsigned bits, unsigned value);
static void script_report(unsigned tip, unsigned button, unsigned count, unsigned scan);

/* Allocates zeroed memory for the driver and the HID layer. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *object;

	/* The host's allocator stands in for the kernel's. */
	object = calloc(count, size);
	return object;
}

/* Frees memory of the driver and the HID layer. */
void
kern_free(
	void *pointer)
{
	/* The host's allocator stands in for the kernel's. */
	free(pointer);
}

/* Copies bytes for the HID layer. */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t length)
{
	void *copied;

	/* The host's copy stands in for the kernel's. */
	copied = memcpy(destination, source, length);
	return copied;
}

/* Fills bytes for the driver and the HID layer. */
void *
kern_memset(
	void *destination,
	int value,
	size_t length)
{
	void *filled;

	/* The host's fill stands in for the kernel's. */
	filled = memset(destination, value, length);
	return filled;
}

/* Compares two strings for the driver. */
int
kern_strcmp(
	const char *left,
	const char *right)
{
	int compared;

	/* The host's comparison stands in for the kernel's. */
	compared = strcmp(left, right);
	return compared;
}

/* Formats a string for the driver. */
int
kern_snprintf(
	char *buffer,
	size_t size,
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* The host's formatting stands in for the kernel's. */
	va_start(arguments, format);
	length = vsnprintf(buffer, size, format, arguments);
	va_end(arguments);
	return length;
}

/* Prints the driver's log lines. */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* The log goes to the test's output. */
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

/* Gives the fake clock. */
uint64_t
clock_milliseconds(
	void *context)
{
	/* No context is used. */
	(void)context;
	return now_ms;
}

/* Gives the fake clock in ticks (one tick a millisecond). */
uint64_t
sched_ticks(void)
{
	/* The clock in ticks. */
	return now_ms;
}

/* Sleeps: moves the fake clock on, and leaves the thread once the script is done. */
void
sched_sleep(
	uint64_t timeout_tick)
{
	/* Time passes. */
	if (timeout_tick > now_ms)
		now_ms = timeout_tick;

	/* After the script, a few more sleeps, then the thread is left. */
	if (report_next >= report_count) {
		sleeps_after_script++;
		if (sleeps_after_script > SLEEPS_AFTER_SCRIPT)
			longjmp(thread_exit, 1);
	}
}

/* Records the thread the probe starts, which the test then runs. */
int
kthread_create(
	void (*entry)(void *),
	void *argument,
	int priority,
	void **result)
{
	/* The priority does not matter here. */
	(void)priority;

	/* The test runs the thread itself. */
	thread_entry = entry;
	thread_argument = argument;
	*result = NULL;
	return 0;
}

/* Starts the recorded thread: the test runs it itself, and checks that the driver started it. */
void
thread_start(
	void *thread)
{
	/* The thread the driver made (the stand-in hands out none). */
	(void)thread;
	thread_started++;
}

/* Walks the stand-in namespace: one device, the touchpad. */
int
drv_acpi_walk(
	struct drv_acpi_node *scope,
	drv_acpi_walk_visitor_t visitor,
	void *argument)
{
	int stop;

	/* From the root. */
	check(scope == NULL, "the probe walks from the root");

	/* The one device. */
	stop = visitor((struct drv_acpi_node *)&fake_node, 3U, argument);
	if (stop < 0)
		return stop;
	return 0;
}

/* Every node of the stand-in namespace is a device. */
enum drv_acpi_type
drv_acpi_node_type(
	const struct drv_acpi_node *node)
{
	/* The touchpad. */
	(void)node;
	return DRV_ACPI_TYPE_DEVICE;
}

/* The touchpad's path. */
int
drv_acpi_node_path(
	const struct drv_acpi_node *node,
	char *buffer,
	size_t size)
{
	/* The 5330's path. */
	(void)node;
	snprintf(buffer, size, "\\_SB_.PC00.I2C1.TPD0");
	return 0;
}

/* Evaluates the touchpad's _HID, _CID and _DSM. */
int
drv_acpi_evaluate(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result)
{
	static const uint8_t uuid[16] = {
		0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
		0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde
	};
	struct drv_acpi_object *object;
	int is_hid;
	int is_cid;
	int is_dsm;
	int same_uuid;

	/* Every evaluation is of the touchpad. */
	check(scope == (struct drv_acpi_node *)&fake_node, "evaluations are of the touchpad");
	object = calloc(1, sizeof(*object));

	/* Which object is asked for. */
	is_hid = strcmp(path, "_HID") == 0;
	is_cid = strcmp(path, "_CID") == 0;
	is_dsm = strcmp(path, "_DSM") == 0;

	/* _HID: the vendor's string. */
	if (is_hid) {
		object->type = DRV_ACPI_TYPE_STRING;
		strcpy(object->string, "VEN_06CB");
	} else if (is_cid) {
		/* _CID: PNP0C50. */
		object->type = DRV_ACPI_TYPE_STRING;
		strcpy(object->string, "PNP0C50");
	} else if (is_dsm) {
		/* _DSM: the HID over I2C UUID, revision 1, function 1, a package; the register. */
		check(argument_count == 4U, "the _DSM has four arguments");
		same_uuid = memcmp(arguments[0]->buffer, uuid, 16) == 0;
		check(arguments[0]->type == DRV_ACPI_TYPE_BUFFER && same_uuid, "the _DSM's UUID");
		check(arguments[1]->integer == 1U && arguments[2]->integer == 1U, "revision 1, function 1");
		check(arguments[3]->type == DRV_ACPI_TYPE_PACKAGE, "an empty package");
		object->type = DRV_ACPI_TYPE_INTEGER;
		object->integer = PAD_DESCRIPTOR_REGISTER;
	} else {
		free(object);
		return 6;
	}

	/* Hands the object over. */
	*result = object;
	return 0;
}

/* The touchpad's _STA: present and working. */
int
drv_acpi_evaluate_integer(
	struct drv_acpi_node *scope,
	const char *path,
	uint64_t *value)
{
	/* Only _STA is asked. */
	(void)scope;
	check(strcmp(path, "_STA") == 0, "the integer asked is _STA");
	*value = 0x0fU;
	return 0;
}

/* Walks the touchpad's _CRS: its I2cSerialBus and its GpioInt. */
int
drv_acpi_resources_walk(
	struct drv_acpi_node *device,
	const char *method,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	int stop;

	/* The _CRS of the touchpad. */
	(void)device;
	check(method == NULL, "the walk is of _CRS");

	/* The I2C connection. */
	memset(&resource, 0, sizeof(resource));
	resource.kind = DRV_ACPI_RESOURCE_I2C;
	resource.base = PAD_ADDRESS;
	resource.speed = 400000U;
	strcpy(resource.source, "\\_SB.PC00.I2C1");
	stop = visitor(&resource, argument);
	if (stop != 0)
		return stop;

	/* The GPIO interrupt, which the driver does not use yet. */
	memset(&resource, 0, sizeof(resource));
	resource.kind = DRV_ACPI_RESOURCE_GPIO_INT;
	resource.base = 327U;
	resource.level = 1;
	resource.active_low = 1;
	strcpy(resource.source, "\\_SB.GPI0");
	stop = visitor(&resource, argument);
	return stop;
}

/* Makes an integer object. */
struct drv_acpi_object *
drv_acpi_object_integer_new(
	uint64_t value)
{
	struct drv_acpi_object *object;

	/* An integer. */
	object = calloc(1, sizeof(*object));
	object->type = DRV_ACPI_TYPE_INTEGER;
	object->integer = value;
	return object;
}

/* Makes a buffer object. */
struct drv_acpi_object *
drv_acpi_object_buffer_new(
	const void *bytes,
	size_t length)
{
	struct drv_acpi_object *object;

	/* A buffer of at most 16 bytes. */
	object = calloc(1, sizeof(*object));
	object->type = DRV_ACPI_TYPE_BUFFER;
	object->length = length;
	if (length <= sizeof(object->buffer))
		memcpy(object->buffer, bytes, length);
	return object;
}

/* Makes a package object (of no elements here). */
struct drv_acpi_object *
drv_acpi_object_package_new(
	uint32_t count)
{
	struct drv_acpi_object *object;

	/* A package. */
	object = calloc(1, sizeof(*object));
	object->type = DRV_ACPI_TYPE_PACKAGE;
	object->length = count;
	return object;
}

/* Releases an object. */
void
drv_acpi_object_release(
	struct drv_acpi_object *object)
{
	/* NULL is allowed. */
	free(object);
}

/* Gives an object's type. */
enum drv_acpi_type
drv_acpi_object_type(
	const struct drv_acpi_object *object)
{
	/* The type it was made with. */
	return object->type;
}

/* Gives an integer object's value. */
uint64_t
drv_acpi_object_integer(
	const struct drv_acpi_object *object)
{
	/* The value it was made with. */
	return object->integer;
}

/* Gives a string object's text. */
const char *
drv_acpi_object_string(
	const struct drv_acpi_object *object,
	size_t *length)
{
	/* The text it was made with. */
	*length = strlen(object->string);
	return object->string;
}

/* Gives a package's count (none here are filled). */
unsigned
drv_acpi_object_package_count(
	const struct drv_acpi_object *object)
{
	/* The count it was made with. */
	return (unsigned)object->length;
}

/* Gives a package's element (none here are filled). */
struct drv_acpi_object *
drv_acpi_object_package_element(
	const struct drv_acpi_object *object,
	unsigned index)
{
	/* No element. */
	(void)object;
	(void)index;
	return NULL;
}

/* Finds the bus the touchpad names. */
int
drv_i2c_bus_find_acpi(
	const char *path,
	struct drv_i2c_bus **result)
{
	/* The controller of the 5330's touchpad. */
	check(strcmp(path, "\\_SB.PC00.I2C1") == 0, "the bus path is I2C1's");
	*result = (struct drv_i2c_bus *)&fake_bus;
	return 0;
}

/*
 * The bus with the touchpad on it: the descriptors at their registers, the
 * commands recorded, and the scripted reports (then empty ones) on plain
 * reads.
 */
int
drv_i2c_transfer(
	struct drv_i2c_bus *bus,
	uint16_t address,
	uint32_t speed,
	const uint8_t *write,
	size_t write_length,
	uint8_t *read,
	size_t read_length)
{
	unsigned reg;
	size_t length;

	/* Everything is on the touchpad's bus, at its address and speed. */
	check(bus == (struct drv_i2c_bus *)&fake_bus, "transfers are on the touchpad's bus");
	check(address == PAD_ADDRESS, "transfers are to 0x2c");
	check(speed == 400000U, "transfers are at 400 kHz");

	/* A plain read gives the next scripted report, or an empty one. */
	if (write_length == 0U) {
		memset(read, 0, read_length);
		length = read_length;
		if (length > REPORT_BYTES)
			length = REPORT_BYTES;
		if (report_next < report_count) {
			memcpy(read, reports[report_next], length);
			report_next++;
		} else {
			empty_reads++;
		}

		/* The read is answered. */
		return 0;
	}

	/* A register read: the HID descriptor or the report descriptor. */
	reg = (unsigned)write[0] | ((unsigned)write[1] << 8);
	if (write_length == 2U && read_length != 0U) {
		/* The HID descriptor. */
		if (reg == PAD_DESCRIPTOR_REGISTER) {
			length = read_length;
			if (length > sizeof(hid_descriptor))
				length = sizeof(hid_descriptor);
			memcpy(read, hid_descriptor, length);
			return 0;
		}

		/* The report descriptor. */
		if (reg == PAD_REPORT_REGISTER) {
			length = read_length;
			if (length > report_descriptor_length)
				length = report_descriptor_length;
			memcpy(read, report_descriptor, length);
			return 0;
		}

		/* No other register is read. */
		return 5;
	}

	/* Anything else is a command (with its data): recorded. */
	if (write_count < WRITES_MAX && write_length <= WRITE_BYTES_MAX) {
		memcpy(writes[write_count].bytes, write, write_length);
		writes[write_count].length = write_length;
		write_count++;
	}

	/* The command is taken. */
	return 0;
}

/* Finds the stand-in pad in the "line" run, and none in the "sample" run. */
int
drv_intel_gpio_pad_find(
	const char *controller,
	uint32_t pin,
	void **result)
{
	int same;

	/* The touchpad's line is on GPI0. */
	same = strcmp(controller, "\\_SB.GPI0");
	check(same == 0, "the line's controller is GPI0");
	line_pin_asked = pin;

	/* No pad when sampling. */
	if (!line_mode)
		return 6;

	/* The pad. */
	*result = &fake_pad;
	return 0;
}

/* The stand-in pad's level: low (asserted) while a scripted report waits. */
int
drv_intel_gpio_pad_level(
	const void *pad)
{
	/* Only the stand-in pad is read. */
	(void)pad;

	/* A report waits: the line is low. */
	if (report_next < report_count)
		return 0;

	/* Nothing waits: the line is high. */
	return 1;
}

/* Enables the stand-in pad's interrupt in the "irq" run; the "line" run's pad cannot interrupt. */
int
drv_intel_gpio_pad_irq_enable(
	void *pad,
	void (*handler)(void *),
	void *argument)
{
	/* The stand-in pad. */
	check(pad == &fake_pad, "the interrupt asked is the pad's");
	irq_enables++;
	if (!irq_mode)
		return 95;

	/* Its handler, for the firings. */
	pad_handler = handler;
	pad_argument = argument;
	return 0;
}

/* Counts the arms. */
void
drv_intel_gpio_pad_irq_arm(
	void *pad)
{
	/* The stand-in pad. */
	(void)pad;
	irq_arms++;
}

/* The stand-in interrupt never goes away. */
int
drv_intel_gpio_pad_irq_alive(
	const void *pad)
{
	/* Alive. */
	(void)pad;
	return 1;
}

/* The locks and the wait queue: one thread here. */
void
spin_init(
	void *lock,
	int rank,
	const char *name)
{
	/* Nothing to make. */
	(void)lock;
	(void)rank;
	(void)name;
}

void
spin_lock(
	void *lock)
{
	/* Nothing to take. */
	(void)lock;
}

void
spin_unlock(
	void *lock)
{
	/* Nothing to give. */
	(void)lock;
}

unsigned long
spin_lock_irqsave(
	void *lock)
{
	/* Nothing to take. */
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(
	void *lock,
	unsigned long state)
{
	/* Nothing to give. */
	(void)lock;
	(void)state;
}

void
waitq_init(
	void *queue,
	const char *name)
{
	/* Nothing to make. */
	(void)queue;
	(void)name;
}

uint64_t
waitq_sequence(
	const void *queue)
{
	/* Any value. */
	(void)queue;
	return 0;
}

void
waitq_wake_all(
	void *queue)
{
	/* The sleep returns by itself. */
	(void)queue;
}

/*
 * The thread waits for the pad: the stand-in fires it while a scripted
 * report waits, otherwise the deadline passes (and after the script the
 * thread is left).
 */
int
waitq_sleep(
	void *queue,
	void *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	/* The wait's length. */
	(void)queue;
	(void)lock;
	(void)observed;
	(void)flags;
	if (deadline > now_ms && deadline - now_ms > longest_wait_ms)
		longest_wait_ms = deadline - now_ms;

	/* A report waits: the pad fires. */
	if (report_next < report_count && pad_handler != NULL) {
		irq_fires++;
		pad_handler(pad_argument);
		return 0;
	}

	/* Nothing: the deadline passes. */
	sched_sleep(deadline);
	return 110;
}

/* Records the input device the driver registers. */
int
drv_input_device_register(
	const struct input_device_info *info,
	struct input_device **result)
{
	/* Keeps its description. */
	registered = *info;
	registered_count++;
	*result = (struct input_device *)&registered;
	return 0;
}

/* Takes a device out (the glue's undoing of a failed publication; the test's never fails). */
void
drv_input_device_unregister(
	struct input_device *device)
{
	/* Nothing is kept for it. */
	(void)device;
}

/* Allocates for the HID input glue, as the kernel heap does. */
void *
kern_malloc(
	size_t size)
{
	/* The host's heap. */
	return malloc(size);
}

/* Refuses an interrupt line: the test's devices are read on their GPIO pad or sampled (ws183-p001 added the lines). */
int
kern_irq_register(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	/* No line is taken. */
	(void)irq;
	(void)handler;
	(void)argument;
	return 95;
}

/* Gives a line back. */
int
kern_irq_unregister(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	/* Nothing was taken. */
	(void)irq;
	(void)handler;
	(void)argument;
	return 0;
}

/* Sets a line's trigger and polarity. */
int
kern_irq_set_mode(
	int irq,
	unsigned trigger,
	unsigned polarity)
{
	/* Nothing to set. */
	(void)irq;
	(void)trigger;
	(void)polarity;
	return 0;
}

/* Masks a line. */
void
kern_irq_mask(
	int irq)
{
	/* Nothing to mask. */
	(void)irq;
}

/* Unmasks a line. */
void
kern_irq_unmask(
	int irq)
{
	/* Nothing to unmask. */
	(void)irq;
}

/* Ends an interrupt. */
void
kern_irq_send_eoi(
	kern_irq_ack_t acknowledge)
{
	/* Nothing to end. */
	(void)acknowledge;
}

/* Records an event the driver emits. */
void
drv_input_device_emit_at(
	struct input_device *device,
	uint16_t type,
	uint16_t code,
	int32_t value,
	uint64_t milliseconds)
{
	/* No device tells nothing, as the input layer does (the glue's unpublished main device, ws143-p005). */
	if (device == NULL)
		return;

	/* The device is the registered one; the time is the fake clock's. */
	(void)milliseconds;

	/* Keeps the event. */
	if (event_count < EVENTS_MAX) {
		events[event_count].type = type;
		events[event_count].code = code;
		events[event_count].value = value;
		event_count++;
	}
}

/* Counts one check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check ran. */
	checks++;

	/* A failed check is printed and counted. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* Tells whether the write at an index has exactly some bytes. */
static int
written(
	size_t index,
	const uint8_t *bytes,
	size_t length)
{
	int compared;

	/* A write that is not there, or of another length. */
	if (index >= write_count || writes[index].length != length)
		return 0;

	/* Its bytes. */
	compared = memcmp(writes[index].bytes, bytes, length);
	if (compared != 0)
		return 0;

	/* The write is the one expected. */
	return 1;
}

/* Tells whether the driver emitted an event. */
static int
has_event(
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	size_t index;

	/* Looks through the events. */
	for (index = 0; index < event_count; index++) {
		if (events[index].type == type && events[index].code == code && events[index].value == value)
			return 1;
	}

	/* No such event. */
	return 0;
}

/* Puts some bits of a value into a report at a bit offset (after its length and identifier). */
static void
put_bits(
	uint8_t *report,
	unsigned offset,
	unsigned bits,
	unsigned value)
{
	unsigned index;

	/* One bit at a time, after the two length bytes and the identifier. */
	for (index = 0; index < bits; index++) {
		if ((value >> index) & 1U)
			report[3U + (offset + index) / 8U] |= (uint8_t)(1U << ((offset + index) % 8U));
	}
}

/* Scripts one report 3: the first finger's tip, the button, the count and the Scan Time. */
static void
script_report(
	unsigned tip,
	unsigned button,
	unsigned count,
	unsigned scan)
{
	uint8_t *report;

	/* The report's length (with its own two bytes) and identifier. */
	report = reports[report_count];
	memset(report, 0, REPORT_BYTES);
	report[0] = 32U;
	report[1] = 0U;
	report[2] = 3U;

	/* The first finger: confidence, tip, contact 0, X 600, Y 300. */
	put_bits(report, 0U, 1U, 1U);
	put_bits(report, 1U, 1U, tip);
	put_bits(report, 8U, 16U, 600U);
	put_bits(report, 24U, 16U, 300U);

	/* The Scan Time, the Contact Count and the button. */
	put_bits(report, 200U, 16U, scan);
	put_bits(report, 216U, 8U, count);
	put_bits(report, 224U, 1U, button);
	report_count++;
}

/* Runs the driver against the stand-ins and checks what it did. */
int
main(
	int argc,
	char **argv)
{
	static const uint8_t power_on[] = { 0x22, 0x00, 0x00, 0x08 };
	static const uint8_t reset[] = { 0x22, 0x00, 0x00, 0x01 };
	static const uint8_t latency[] = { 0x22, 0x00, 0x3d, 0x03, 0x23, 0x00, 0x04, 0x00, 0x0d, 0x00 };
	static const uint8_t switches[] = { 0x22, 0x00, 0x36, 0x03, 0x23, 0x00, 0x04, 0x00, 0x06, 0x03 };
	static const uint8_t mode[] = { 0x22, 0x00, 0x34, 0x03, 0x23, 0x00, 0x04, 0x00, 0x04, 0x03 };
	FILE *stream;
	int started;

	/* The report descriptor file, and the run: by the line or by sampling. */
	if (argc != 3) {
		fprintf(stderr, "usage: host-i2c-hid DESCRIPTOR line|sample\n");
		return 2;
	}

	/* The run. */
	line_mode = strcmp(argv[2], "line") == 0;
	irq_mode = strcmp(argv[2], "irq") == 0;
	if (irq_mode)
		line_mode = 1;

	/* Reads it. */
	stream = fopen(argv[1], "rb");
	if (stream == NULL)
		return 2;
	report_descriptor_length = fread(report_descriptor, 1, sizeof(report_descriptor), stream);
	fclose(stream);
	check(report_descriptor_length == 665U, "the descriptor is 665 bytes");

	/* The script: one finger down, pressed, released, lifted. */
	script_report(1U, 0U, 1U, 100U);
	script_report(1U, 1U, 1U, 200U);
	script_report(1U, 0U, 1U, 300U);
	script_report(0U, 0U, 1U, 400U);

	/* The probe takes the touchpad and starts its thread. */
	started = drv_i2c_hid_probe();
	check(started == 1, "the probe starts one device");
	check(thread_entry != NULL, "the probe made a thread");
	check(thread_started == 1, "and started it (thread_start)");
	if (thread_entry == NULL) {
		printf("host-i2c-hid: FAIL (no thread)\n");
		return 1;
	}

	/*
	 * Runs the thread until the script is done.  setjmp() may only be
	 * called in a condition (C99 7.13.1.1), so it stays in one here.
	 */
	if (setjmp(thread_exit) == 0)
		thread_entry(thread_argument);

	/* The commands, as Linux sends them. */
	check(written(0, power_on, sizeof(power_on)), "power on is 22 00 00 08");
	check(written(1, reset, sizeof(reset)), "reset is 22 00 00 01");
	check(written(2, latency, sizeof(latency)), "the latency mode is 22 00 3d 03 23 00 04 00 0d 00");
	check(written(3, switches, sizeof(switches)), "the surface switch sets both switches: 22 00 36 03 23 00 04 00 06 03");
	check(written(4, switches, sizeof(switches)), "so does the button switch");
	check(written(5, mode, sizeof(mode)), "the device mode is 22 00 34 03 23 00 04 00 04 03");

	/* The input device. */
	check(registered_count == 1, "one input device");
	check(registered.id.bustype == BUS_I2C, "on the I2C bus");
	check(registered.id.vendor == 0x06cbU && registered.id.product == 0xce65U, "Synaptics 06CB:CE65");
	check(registered.name != NULL && strcmp(registered.name, "06CB:CE65 Touchpad") == 0, "named 06CB:CE65 Touchpad");
	check(registered.properties == ((1U << INPUT_PROP_POINTER) | (1U << INPUT_PROP_BUTTONPAD)), "a pointer and a button pad");

	/* The events of the script. */
	check(has_event(EV_KEY, BTN_TOUCH, 1), "BTN_TOUCH down");
	check(has_event(EV_KEY, BTN_TOOL_FINGER, 1), "BTN_TOOL_FINGER");
	check(has_event(EV_ABS, ABS_MT_POSITION_X, 600), "the finger's X");
	check(has_event(EV_KEY, BTN_LEFT, 1), "BTN_LEFT pressed");
	check(has_event(EV_KEY, BTN_LEFT, 0), "BTN_LEFT released");
	check(has_event(EV_KEY, BTN_TOUCH, 0), "BTN_TOUCH up");
	check(has_event(EV_KEY, BTN_TOOL_FINGER, 0), "BTN_TOOL_FINGER released");
	check(line_pin_asked == 327U, "the line's pin is 327");
	if (irq_mode) {
		check(irq_enables == 1 && irq_fires >= 1 && irq_arms >= 1, "the pad's interrupt is enabled, fires and is armed again");
		check(longest_wait_ms >= 1000U && longest_wait_ms <= 1002U, "the thread sleeps at most a second between looks");
	}

	/* The line run asked for the interrupt, which failed, before it watched the line. */
	if (line_mode && !irq_mode)
		check(irq_enables == 1, "the line run asks for the interrupt first");
	if (line_mode) {
		check(empty_reads == 0U, "by the line, no read happens while the line is idle");
	} else {
		check(empty_reads >= 1U, "sampling, empty reads after the script emit nothing");
	}

	/* The verdict. */
	if (failures != 0) {
		printf("host-i2c-hid: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-i2c-hid: ok (%s, %d checks)\n", argv[2], checks);
	return 0;
}
