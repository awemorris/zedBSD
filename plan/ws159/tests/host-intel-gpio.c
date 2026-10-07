/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Intel GPIO pad lookup (ws159-p006,
 * src/drivers/gpio/intel-gpio.c compiled unchanged).
 *
 * The stand-in ACPI namespace holds the Latitude 5330's pad group table
 * \_SB.GPCL (the 18 groups of its DSDT), \SBRG 0xfd000000 and the four
 * 64 KiB memory ranges of \_SB.GPI0's _CRS (0xfd6e0000, 0xfd6d0000,
 * 0xfd6a0000, 0xfd690000, as the 5330's Linux shows them).  The touchpad's
 * GpioInt pin 327 must be found in group 14 (first GPIO number 320) as pad
 * 7, with its DW0 at 0xfd6a0ae0; the pad's level is read from DW0's bit 1
 * (the 5330's DW0 at rest, 0x80800102, reads high).  A pin in no group and
 * a configuration outside the controller's ranges are refused.
 *
 * The pad's interrupt (the controller's _HID INTC1055, its _CRS Interrupt
 * 14, level, active low): another family is refused, a pad the firmware
 * keeps (HOSTSW_OWN clear) is refused, then the pad's interrupt is enabled
 * (IRQ 14 registered, set level and low, unmasked; GPI_IE bit 7 of the
 * group's register at 0x12c set, GPI_IS 0x10c cleared); a firing with the
 * pad's GPI_IS bit set calls the user's handler once, turns GPI_IE's bit
 * off, clears GPI_IS's and sends the EOI; arming turns it on again; a
 * firing for no pad does not mask IRQ 14 (BUG-261).  Taking the line
 * turns off the GPI interrupts of the controller's fifteen groups whose
 * registers lie in its memory and clears their status (a pad the firmware
 * left enabled); a firing of a pad not the driver's, in another group or in
 * the pad's own, has that pad's interrupt turned off and does not reach the
 * user; 200 firings in a row that nothing explains mask IRQ 14 and the
 * pad's interrupt is no longer alive, and a firing the pad explains ends
 * such a run.  With the argument "nomode" the HAL cannot set the mode: the
 * enable fails and the interrupt is not alive.
 *
 *   plan/ws159/tests/run-host-intel-gpio.sh
 */

#include <drivers/acpi/acpi.h>
#include <drivers/gpio/intel-gpio.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The 5330's sideband base and the address its touchpad pad's DW0 must have. */
#define SIDEBAND_BASE		0xfd000000ULL
#define PAD_DW0			0xfd6a0ae0ULL

/* The groups of the 5330's \_SB.GPCL, and the fields of each. */
#define GROUPS			18U
#define FIELDS			9U

/*
 * An ACPI object of the stand-in namespace: an integer, or a package of
 * objects.
 */
struct drv_acpi_object {
	enum drv_acpi_type type;
	uint64_t integer;
	unsigned count;
	struct drv_acpi_object *elements[GROUPS];
};

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

/* The 5330's \_SB.GPCL: community, pads, pad offset, three registers, GPE, and first GPIO number. */
static const uint64_t gpcl[GROUPS][FIELDS] = {
	{ 0x6e0000, 0x1a, 0x700, 0xb0, 0x20, 0x140, 0x80, 0x84, 0x0 },
	{ 0x6e0000, 0x10, 0x8a0, 0xb4, 0x30, 0x144, 0x88, 0x8c, 0x20 },
	{ 0x6e0000, 0x19, 0x9a0, 0xb8, 0x38, 0x148, 0x90, 0x94, 0x40 },
	{ 0x690000, 0x8, 0x700, 0xb0, 0x20, 0x140, 0x80, 0x84, 0x160 },
	{ 0x690000, 0x9, 0x780, 0xb4, 0x24, 0xffff, 0x88, 0x8c, 0xffff },
	{ 0x6c0000, 0x11, 0x700, 0xb0, 0x20, 0x140, 0x80, 0x84, 0xffff },
	{ 0x6d0000, 0x8, 0x700, 0xb0, 0x20, 0x140, 0x80, 0x84, 0x60 },
	{ 0x6d0000, 0x18, 0x780, 0xb4, 0x24, 0x144, 0x88, 0x8c, 0x80 },
	{ 0x6d0000, 0x15, 0x900, 0xb8, 0x30, 0x148, 0x90, 0x94, 0xa0 },
	{ 0x6d0000, 0x18, 0xa50, 0xbc, 0x3c, 0x14c, 0x98, 0x9c, 0xc0 },
	{ 0x6d0000, 0x1d, 0xbd0, 0xc0, 0x48, 0x150, 0xa0, 0xa4, 0xe0 },
	{ 0x6a0000, 0x18, 0x700, 0xb0, 0x20, 0x140, 0x80, 0x84, 0x100 },
	{ 0x6a0000, 0x19, 0x880, 0xb4, 0x2c, 0x144, 0x88, 0x8c, 0x120 },
	{ 0x6a0000, 0x6, 0xa10, 0xb8, 0x3c, 0xffff, 0x90, 0x94, 0xffff },
	{ 0x6a0000, 0x19, 0xa70, 0xbc, 0x40, 0x14c, 0x98, 0x9c, 0x140 },
	{ 0x6a0000, 0xa, 0xc00, 0xc0, 0x50, 0xffff, 0xa0, 0xa4, 0xffff },
	{ 0x6b0000, 0xf, 0x700, 0xb0, 0x20, 0xffff, 0x80, 0x84, 0xffff },
	{ 0x6b0000, 0x5b, 0x7f0, 0xb4, 0x28, 0xffff, 0x88, 0x8c, 0xffff }
};

/*
 * A "tgl" run (ws183-p001) gives the same groups as Tiger Lake's seven-field
 * packages (no first GPIO number: the groups are numbered 32 apart in the
 * table's order).
 */
#define FIELDS_TGL		7U
static int tgl;

/* The controller's memory ranges, the second of which a "narrow" run leaves out. */
static const uint64_t ranges[4] = { 0xfd6e0000ULL, 0xfd6d0000ULL, 0xfd6a0000ULL, 0xfd690000ULL };
static int narrow;

/*
 * The pages the driver mapped (each address its own memory, BUG-261), the
 * last address mapped, and the DW0 value the stand-in hardware gives.
 * page_memory is the touchpad's community page, 0xfd6a0000.
 */
#define PAGES			8U
#define TOUCHPAD_PAGE		0xfd6a0000ULL
static uint64_t mapped_page;
static uint64_t page_addresses[PAGES];
static uint8_t pages[PAGES][4096];
static unsigned page_count;
static uint8_t *page_memory;
static uint32_t dw0_value;

/* The GPI_IS and GPI_IE of the community's first group, at 0x100 and 0x120 of a page, and how many groups take the line. */
#define REG_FIRST_STATUS	0x100U
#define REG_FIRST_ENABLE	0x120U
#define WATCHED_GROUPS		15U

/* How many firings in a row that nothing explains give the line up (intel-gpio.c's IRQ_UNEXPLAINED_MAX). */
#define UNEXPLAINED_MAX		200

/* The group's interrupt registers in the community's page: HOSTSW_OWN, GPI_IS, GPI_IE (group 14 is the community's fourth). */
#define REG_HOSTSW		0xbcU
#define REG_STATUS		0x10cU
#define REG_ENABLE		0x12cU
#define PAD_BIT			(1U << 7)

/* The controller's _HID, the HAL's answer to the mode, and what the stand-in IRQ calls saw. */
static const char *controller_hid = "INTC1055";
static int mode_error;
static void (*irq_handler)(int, uintptr_t, void *);
static void *irq_argument;
static int irq_registered;
static int irq_masked;
static int irq_unmasked;
static unsigned irq_trigger;
static unsigned irq_polarity;
static int eoi_count;
static int user_calls;


/* The node the stand-in lookup hands out (compared, never followed). */
static int fake_node;

void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);
void kern_logf(const char *format, ...);
int kern_device_map(uint64_t address, size_t size, unsigned attributes, void **mapped);
uint32_t kern_mmio_read32(const volatile void *address);
void kern_mmio_write32(volatile void *address, uint32_t value);
int kern_strcmp(const char *left, const char *right);
int kern_irq_register(int irq, void (*handler)(int, uintptr_t, void *), void *argument);
int kern_irq_unregister(int irq, void (*handler)(int, uintptr_t, void *), void *argument);
int kern_irq_set_mode(int irq, unsigned trigger, unsigned polarity);
void kern_irq_mask(int irq);
void kern_irq_unmask(int irq);
void kern_irq_send_eoi(uintptr_t acknowledge);
void spin_init(void *lock, int rank, const char *name);
void spin_lock(void *lock);
void spin_unlock(void *lock);
unsigned long spin_lock_irqsave(void *lock);
void spin_unlock_irqrestore(void *lock, unsigned long state);
static void user_handler(void *argument);
static uint8_t *page_of(uint64_t address);
static uint8_t *page_holding(const volatile void *address);
static uint32_t reg(unsigned offset);
static void set_reg(unsigned offset, uint32_t value);
static uint32_t page_reg(uint64_t page, unsigned offset);
static void set_page_reg(uint64_t page, unsigned offset, uint32_t value);
static void fire(void);
static void check(int condition, const char *what);
static struct drv_acpi_object *integer_object(uint64_t value);
static struct drv_acpi_object *gpcl_object(void);

/* Allocates zeroed memory for the driver. */
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

/* Frees the driver's memory. */
void
kern_free(
	void *pointer)
{
	/* The host's allocator stands in for the kernel's. */
	free(pointer);
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

/* Maps a device page: the stand-in memory of its address, its address recorded. */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	/* One page, uncached. */
	check(size == 4096U, "one page is mapped");
	check(attributes == 0U, "uncached");
	mapped_page = address;
	*mapped = page_of(address);
	return 0;
}

/* Reads the stand-in DW0, or a register of a mapped page. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uint8_t *page;
	uint32_t value;

	/* The pad's DW0. */
	if ((const uint8_t *)address == page_memory + (PAD_DW0 & 0xfffU))
		return dw0_value;

	/* A register of a mapped page. */
	page = page_holding(address);
	check(page != NULL, "the read is of a mapped page");
	if (page == NULL)
		return 0U;
	memcpy(&value, (const uint8_t *)address, sizeof(value));
	return value;
}

/* Writes a register of a mapped page; GPI_IS (0x100 to 0x11c) clears the bits written as ones. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uint8_t *page;
	unsigned offset;
	uint32_t old;

	/* Within a mapped page. */
	page = page_holding(address);
	check(page != NULL, "the write is of a mapped page");
	if (page == NULL)
		return;
	offset = (unsigned)((uint8_t *)address - page);

	/* GPI_IS: write one to clear. */
	if (offset >= REG_FIRST_STATUS && offset < REG_FIRST_ENABLE) {
		memcpy(&old, page + offset, sizeof(old));
		old &= ~value;
		memcpy(page + offset, &old, sizeof(old));
		return;
	}

	/* Any other register keeps what is written. */
	memcpy(page + offset, &value, sizeof(value));
}

/* Compares two strings. */
int
kern_strcmp(
	const char *left,
	const char *right)
{
	/* The host's. */
	return strcmp(left, right);
}

/* Records the controller's handler. */
int
kern_irq_register(
	int irq,
	void (*handler)(int, uintptr_t, void *),
	void *argument)
{
	/* IRQ 14, once. */
	check(irq == 14, "the controller's IRQ is 14");
	irq_handler = handler;
	irq_argument = argument;
	irq_registered++;
	return 0;
}

/* Forgets the controller's handler. */
int
kern_irq_unregister(
	int irq,
	void (*handler)(int, uintptr_t, void *),
	void *argument)
{
	/* The same registration. */
	check(irq == 14 && handler == irq_handler && argument == irq_argument, "the registration removed is the one made");
	irq_registered--;
	return 0;
}

/* Records the mode asked for; the "nomode" run refuses it. */
int
kern_irq_set_mode(
	int irq,
	unsigned trigger,
	unsigned polarity)
{
	/* What was asked. */
	(void)irq;
	irq_trigger = trigger;
	irq_polarity = polarity;
	return mode_error;
}

/* Counts the masks. */
void
kern_irq_mask(
	int irq)
{
	/* IRQ 14. */
	(void)irq;
	irq_masked++;
}

/* Counts the unmasks. */
void
kern_irq_unmask(
	int irq)
{
	/* IRQ 14. */
	(void)irq;
	irq_unmasked++;
}

/* Counts the EOIs. */
void
kern_irq_send_eoi(
	uintptr_t acknowledge)
{
	/* The token handed to the handler. */
	check(acknowledge == 0x1234U, "the EOI is of the firing's token");
	eoi_count++;
}

/* The locks: one thread here. */
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

/* The pad's user's handler: counted. */
static void
user_handler(
	void *argument)
{
	/* The argument given at the enable. */
	check(argument == &user_calls, "the handler's argument");
	user_calls++;
}

/* Gives the stand-in memory of a page's address, making it the first time. */
static uint8_t *
page_of(
	uint64_t address)
{
	unsigned index;

	/* A page met before. */
	for (index = 0; index < page_count; index++) {
		if (page_addresses[index] == address)
			return pages[index];
	}

	/* A new page (the test never needs more than PAGES). */
	if (page_count >= PAGES) {
		printf("FAIL: more than %u pages mapped\n", PAGES);
		exit(1);
	}
	page_addresses[page_count] = address;
	page_count++;
	return pages[page_count - 1U];
}

/* Gives the stand-in page an address points into, or NULL. */
static uint8_t *
page_holding(
	const volatile void *address)
{
	unsigned index;

	/* Looks through the pages made. */
	for (index = 0; index < page_count; index++) {
		if ((const uint8_t *)address >= pages[index] && (const uint8_t *)address < pages[index] + sizeof(pages[index]))
			return pages[index];
	}

	/* Not a mapped page. */
	return NULL;
}

/* Reads a register of the touchpad's community page. */
static uint32_t
reg(
	unsigned offset)
{
	/* The page at 0xfd6a0000. */
	return page_reg(TOUCHPAD_PAGE, offset);
}

/* Writes a register of the touchpad's community page. */
static void
set_reg(
	unsigned offset,
	uint32_t value)
{
	/* The page at 0xfd6a0000. */
	set_page_reg(TOUCHPAD_PAGE, offset, value);
}

/* Reads a register of a page. */
static uint32_t
page_reg(
	uint64_t page,
	unsigned offset)
{
	uint32_t value;

	/* Little-endian, as the host. */
	memcpy(&value, page_of(page) + offset, sizeof(value));
	return value;
}

/* Writes a register of a page. */
static void
set_page_reg(
	uint64_t page,
	unsigned offset,
	uint32_t value)
{
	/* Little-endian, as the host. */
	memcpy(page_of(page) + offset, &value, sizeof(value));
}

/* Fires the controller's line once. */
static void
fire(void)
{
	/* The token the EOI must carry. */
	irq_handler(14, 0x1234U, irq_argument);
}

/* Finds the controller. */
int
drv_acpi_lookup(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_node **result)
{
	int same;

	/* From the root, the controller GPI0. */
	(void)scope;
	same = strcmp(path, "\\_SB.GPI0");
	check(same == 0, "the controller is \\_SB.GPI0");
	*result = (struct drv_acpi_node *)&fake_node;
	return 0;
}

/* Gives \SBRG. */
int
drv_acpi_evaluate_integer(
	struct drv_acpi_node *scope,
	const char *path,
	uint64_t *value)
{
	int same;

	/* Only \SBRG is asked. */
	(void)scope;
	same = strcmp(path, "\\SBRG");
	check(same == 0, "the integer asked is \\SBRG");
	*value = SIDEBAND_BASE;
	return 0;
}

/* Gives \_SB.GPCL. */
int
drv_acpi_evaluate(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result)
{
	int same;

	/* The controller's _HID, a string. */
	(void)arguments;
	same = strcmp(path, "_HID");
	if (same == 0) {
		check(scope == (struct drv_acpi_node *)&fake_node, "the _HID is the controller's");
		*result = calloc(1, sizeof(**result));
		(*result)->type = DRV_ACPI_TYPE_STRING;
		return 0;
	}

	/* Otherwise only \_SB.GPCL is asked, without arguments. */
	(void)scope;
	same = strcmp(path, "\\_SB.GPCL");
	check(same == 0 && argument_count == 0U, "the object asked is \\_SB.GPCL");
	*result = gpcl_object();
	return 0;
}

/* Walks GPI0's _CRS: its four memory ranges (three in a narrow run). */
int
drv_acpi_resources_walk(
	struct drv_acpi_node *device,
	const char *method,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	unsigned index;
	int stop;

	/* The controller's _CRS. */
	(void)device;
	(void)method;
	for (index = 0; index < 4U; index++) {
		/* The narrow run leaves out the range of the touchpad's community. */
		if (narrow && ranges[index] == 0xfd6a0000ULL)
			continue;
		memset(&resource, 0, sizeof(resource));
		resource.kind = DRV_ACPI_RESOURCE_MEMORY;
		resource.base = ranges[index];
		resource.length = 0x10000U;
		stop = visitor(&resource, argument);
		if (stop != 0)
			return stop;
	}

	/* Its interrupt: 14, level, active low, shared. */
	memset(&resource, 0, sizeof(resource));
	resource.kind = DRV_ACPI_RESOURCE_IRQ;
	resource.base = 14U;
	resource.level = 1U;
	resource.active_low = 1U;
	resource.shared = 1U;
	stop = visitor(&resource, argument);
	if (stop != 0)
		return stop;

	/* Every range was walked. */
	return 0;
}

/* Gives a string object's text: the controller's _HID. */
const char *
drv_acpi_object_string(
	const struct drv_acpi_object *object,
	size_t *length)
{
	/* The _HID of the run. */
	(void)object;
	*length = strlen(controller_hid);
	return controller_hid;
}

/* Releases an object and its elements. */
void
drv_acpi_object_release(
	struct drv_acpi_object *object)
{
	unsigned index;

	/* NULL is allowed. */
	if (object == NULL)
		return;
	for (index = 0; index < object->count; index++)
		drv_acpi_object_release(object->elements[index]);
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

/* Gives a package's count. */
unsigned
drv_acpi_object_package_count(
	const struct drv_acpi_object *object)
{
	/* The count it was made with. */
	return object->count;
}

/* Gives a package's element, without a reference. */
struct drv_acpi_object *
drv_acpi_object_package_element(
	const struct drv_acpi_object *object,
	unsigned index)
{
	/* An index past the end names nothing. */
	if (index >= object->count)
		return NULL;
	return object->elements[index];
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

/* Makes an integer object. */
static struct drv_acpi_object *
integer_object(
	uint64_t value)
{
	struct drv_acpi_object *object;

	/* An integer. */
	object = calloc(1, sizeof(*object));
	object->type = DRV_ACPI_TYPE_INTEGER;
	object->integer = value;
	return object;
}

/* Makes the 5330's \_SB.GPCL. */
static struct drv_acpi_object *
gpcl_object(void)
{
	struct drv_acpi_object *table;
	struct drv_acpi_object *group;
	unsigned index;
	unsigned field;

	/* A package of the groups, each a package of its fields. */
	table = calloc(1, sizeof(*table));
	table->type = DRV_ACPI_TYPE_PACKAGE;
	table->count = GROUPS;
	for (index = 0; index < GROUPS; index++) {
		group = calloc(1, sizeof(*group));
		group->type = DRV_ACPI_TYPE_PACKAGE;
		group->count = FIELDS;
		if (tgl)
			group->count = FIELDS_TGL;
		for (field = 0; field < group->count; field++)
			group->elements[field] = integer_object(gpcl[index][field]);
		table->elements[index] = group;
	}

	/* The table. */
	return table;
}

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	struct drv_intel_gpio_pad *pad;
	struct drv_intel_gpio_pad *irq_pad;
	int index;
	int nomode;
	int error;
	int level;
	int alive;

	/* The touchpad's community page is the first the test makes. */
	page_memory = page_of(TOUCHPAD_PAGE);

	/* The run: the HAL sets the mode, or ("nomode") it cannot. */
	nomode = 0;
	if (argc > 1) {
		error = strcmp(argv[1], "nomode");
		if (error == 0)
			nomode = 1;
	}

	/* The "nomode" HAL refuses the mode. */
	if (nomode)
		mode_error = 95;

	/* The "tgl" run: pin 327 is the eleventh group's (320 = 10 x 32) pad 7, its DW0 at 0xfd6d0c40; pin 700 is past the table. */
	if (argc > 1) {
		error = strcmp(argv[1], "tgl");
		if (error == 0)
			tgl = 1;
	}

	/* The "tgl" run is these checks alone. */
	if (tgl) {
		pad = NULL;
		error = drv_intel_gpio_pad_find("\\_SB.GPI0", 327U, &pad);
		check(error == 0 && pad != NULL, "with seven fields pin 327 is found by the group's place");
		check(mapped_page == 0xfd6d0000ULL, "the page of 0xfd6d0c40 is mapped");
		free(pad);
		error = drv_intel_gpio_pad_find("\\_SB.GPI0", 700U, &pad);
		check(error != 0, "pin 700 is past the eighteen groups");
		if (failures != 0) {
			printf("host-intel-gpio: FAIL (tgl, %d of %d checks)\n", failures, checks);
			return 1;
		}

		/* Succeeded: every check of the run held. */
		printf("host-intel-gpio: ok (tgl, %d checks)\n", checks);
		return 0;
	}

	/* 1. The touchpad's pin 327: group 14's pad 7, its DW0 at 0xfd6a0ae0. */
	pad = NULL;
	error = drv_intel_gpio_pad_find("\\_SB.GPI0", 327U, &pad);
	check(error == 0 && pad != NULL, "pin 327 is found");
	check(mapped_page == (PAD_DW0 & ~0xfffULL), "the page of 0xfd6a0ae0 is mapped");

	/* 2. Its level: DW0 0x80800102 (at rest) is high, with bit 1 clear it is low. */
	if (pad != NULL) {
		dw0_value = 0x80800102U;
		level = drv_intel_gpio_pad_level(pad);
		check(level == 1, "the 5330's DW0 at rest reads high");
		dw0_value = 0x80800100U;
		level = drv_intel_gpio_pad_level(pad);
		check(level == 0, "with its input low it reads low");
	}

	/* 5. The pad's interrupt: another family is refused, then a pad the firmware keeps. */
	irq_pad = pad;
	if (irq_pad != NULL) {
		controller_hid = "INTC1056";
		error = drv_intel_gpio_pad_irq_enable(irq_pad, user_handler, &user_calls);
		check(error != 0 && irq_registered == 0, "another family's pad does not interrupt");
		controller_hid = "INTC1055";
		set_reg(REG_HOSTSW, 0U);
		error = drv_intel_gpio_pad_irq_enable(irq_pad, user_handler, &user_calls);
		check(error != 0 && irq_registered == 0, "a pad the firmware keeps does not interrupt");
		set_reg(REG_HOSTSW, PAD_BIT);
	}

	/* 6. The "nomode" run: the enable fails, and the interrupt is not alive. */
	if (irq_pad != NULL && nomode) {
		error = drv_intel_gpio_pad_irq_enable(irq_pad, user_handler, &user_calls);
		alive = drv_intel_gpio_pad_irq_alive(irq_pad);
		check(error != 0 && alive == 0, "without the mode the pad does not interrupt");
		check(irq_registered == 0 && irq_unmasked == 0, "and IRQ 14 is given back, never unmasked");
	}

	/* 7. The enable: IRQ 14 registered, level and low, unmasked; GPI_IE's bit 7 set. */
	if (irq_pad != NULL && !nomode) {
		/* Pads the firmware left enabled and asking: in the touchpad's first group, and in another community (BUG-261). */
		set_reg(REG_FIRST_STATUS, 0x8U);
		set_reg(REG_FIRST_ENABLE, 0x8U);
		set_page_reg(0xfd6d0000ULL, 0x10cU, 0x80U);
		set_page_reg(0xfd6d0000ULL, 0x12cU, 0x80U);
		set_reg(REG_STATUS, PAD_BIT);
		error = drv_intel_gpio_pad_irq_enable(irq_pad, user_handler, &user_calls);
		check(error == 0, "the pad's interrupt is enabled");
		check(reg(REG_FIRST_ENABLE) == 0U && reg(REG_FIRST_STATUS) == 0U, "a pad the firmware left enabled in the pad's community is turned off and cleared");
		check(page_reg(0xfd6d0000ULL, 0x12cU) == 0U && page_reg(0xfd6d0000ULL, 0x10cU) == 0U, "and one in another community too");
		check(page_reg(0xfd6e0000ULL, 0x100U) == 0U && page_reg(0xfd690000ULL, 0x100U) == 0U, "the other communities in the controller's memory are quieted");
		check(irq_registered == 1 && irq_trigger == 1U && irq_polarity == 1U && irq_unmasked == 1, "IRQ 14 taken level, active low, unmasked");
		check((reg(REG_ENABLE) & PAD_BIT) != 0U, "GPI_IE's bit 7 is set");
		check((reg(REG_STATUS) & PAD_BIT) == 0U, "GPI_IS's bit 7 was cleared first");
		alive = drv_intel_gpio_pad_irq_alive(irq_pad);
		check(alive == 1, "the interrupt is alive");

		/* 8. A firing: the user's handler once, GPI_IE's bit off, GPI_IS's cleared, the EOI. */
		set_reg(REG_STATUS, PAD_BIT | 0x1U);
		irq_handler(14, 0x1234U, irq_argument);
		check(user_calls == 1, "the user's handler is called once");
		check((reg(REG_ENABLE) & PAD_BIT) == 0U, "GPI_IE's bit 7 is off until armed");
		check((reg(REG_STATUS) & PAD_BIT) == 0U && (reg(REG_STATUS) & 0x1U) != 0U, "only GPI_IS's bit 7 is cleared");
		check(eoi_count == 1 && irq_masked == 0, "the EOI is sent, IRQ 14 stays unmasked");

		/* 9. Armed again. */
		drv_intel_gpio_pad_irq_arm(irq_pad);
		check((reg(REG_ENABLE) & PAD_BIT) != 0U, "arming sets GPI_IE's bit 7 again");

		/* 10. A pad not the driver's in another community fires: its interrupt is turned off, the user does not hear it, the line stays. */
		set_page_reg(0xfd6d0000ULL, 0x104U, 0x4U);
		set_page_reg(0xfd6d0000ULL, 0x124U, 0x4U);
		fire();
		alive = drv_intel_gpio_pad_irq_alive(irq_pad);
		check(page_reg(0xfd6d0000ULL, 0x124U) == 0U && page_reg(0xfd6d0000ULL, 0x104U) == 0U, "a stray pad of another community is turned off and acknowledged");
		check(user_calls == 1 && irq_masked == 0 && eoi_count == 2 && alive == 1, "the user does not hear it, and IRQ 14 stays");

		/* 11. One in the pad's own group, while the pad's own status is set but not enabled: only the stray bit goes. */
		set_reg(REG_ENABLE, 0x2U);
		set_reg(REG_STATUS, PAD_BIT | 0x2U);
		fire();
		check(reg(REG_ENABLE) == 0U, "the stray pad of the pad's group is turned off");
		check(reg(REG_STATUS) == PAD_BIT, "the pad's own status is left for its user");
		check(user_calls == 1 && irq_masked == 0 && eoi_count == 3, "the user does not hear it, and IRQ 14 stays");
		set_reg(REG_STATUS, 0U);
		drv_intel_gpio_pad_irq_arm(irq_pad);

		/* 12. A firing nothing explains does not give the line up. */
		set_reg(REG_STATUS, 0x1U);
		fire();
		alive = drv_intel_gpio_pad_irq_alive(irq_pad);
		check(irq_masked == 0 && alive == 1 && eoi_count == 4, "a firing for no pad keeps IRQ 14");

		/* 13. A run of them broken by the pad's own firing does not either. */
		for (index = 1; index < UNEXPLAINED_MAX - 1; index++)
			fire();
		set_reg(REG_STATUS, PAD_BIT);
		fire();
		check(user_calls == 2, "the pad's firing in the run reaches its user");
		drv_intel_gpio_pad_irq_arm(irq_pad);
		for (index = 0; index < UNEXPLAINED_MAX - 1; index++)
			fire();
		alive = drv_intel_gpio_pad_irq_alive(irq_pad);
		check(irq_masked == 0 && alive == 1, "199 in a row after it keep IRQ 14");

		/* 14. The 200th in a row: IRQ 14 masked, the interrupt no longer alive. */
		fire();
		alive = drv_intel_gpio_pad_irq_alive(irq_pad);
		check(irq_masked == 1, "200 firings in a row for no pad mask IRQ 14");
		check(alive == 0, "and the interrupt is given up");
	}

	/* The pad's state is the driver's allocation; the test lets it go. */
	free(pad);

	/* 3. A pin in no group is refused. */
	error = drv_intel_gpio_pad_find("\\_SB.GPI0", 400U, &pad);
	check(error != 0, "pin 400 is in no group");

	/* 4. A configuration outside the controller's ranges is refused. */
	narrow = 1;
	error = drv_intel_gpio_pad_find("\\_SB.GPI0", 327U, &pad);
	check(error != 0, "outside the controller's ranges it is refused");

	/* The verdict. */
	if (failures != 0) {
		printf("host-intel-gpio: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-intel-gpio: ok (nomode=%d, %d checks)\n", nomode, checks);
	return 0;
}
