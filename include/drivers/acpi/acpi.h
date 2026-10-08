/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI namespace and AML interpreter interface for drivers.
 *
 * The interpreter loads the DSDT and the SSDTs into one namespace.  A driver
 * finds a device node by path or by walking the namespace, evaluates its
 * control methods and reads the returned objects.  Every call that returns an
 * object hands the caller one reference, which drv_acpi_object_release()
 * gives back.  Errors are reported as zedBSD errno values.
 */

#ifndef KERN_DRIVERS_ACPI_ACPI_H
#define KERN_DRIVERS_ACPI_ACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The object types.
 *
 * The numbers from 0 to 16 are the ones the ObjectType operator reports.
 * The rest are kinds of namespace entry that ObjectType folds into one of
 * those, or objects that only live inside the interpreter.
 */
enum drv_acpi_type {
	DRV_ACPI_TYPE_UNINITIALIZED = 0,
	DRV_ACPI_TYPE_INTEGER = 1,
	DRV_ACPI_TYPE_STRING = 2,
	DRV_ACPI_TYPE_BUFFER = 3,
	DRV_ACPI_TYPE_PACKAGE = 4,
	DRV_ACPI_TYPE_FIELD_UNIT = 5,
	DRV_ACPI_TYPE_DEVICE = 6,
	DRV_ACPI_TYPE_EVENT = 7,
	DRV_ACPI_TYPE_METHOD = 8,
	DRV_ACPI_TYPE_MUTEX = 9,
	DRV_ACPI_TYPE_REGION = 10,
	DRV_ACPI_TYPE_POWER_RESOURCE = 11,
	DRV_ACPI_TYPE_PROCESSOR = 12,
	DRV_ACPI_TYPE_THERMAL_ZONE = 13,
	DRV_ACPI_TYPE_BUFFER_FIELD = 14,
	DRV_ACPI_TYPE_DDB_HANDLE = 15,
	DRV_ACPI_TYPE_DEBUG = 16,
	DRV_ACPI_TYPE_SCOPE = 32,
	DRV_ACPI_TYPE_REFERENCE = 33,
	DRV_ACPI_TYPE_ALIAS = 34
};

/*
 * The address spaces of an operation region.
 */
enum drv_acpi_space {
	DRV_ACPI_SPACE_SYSTEM_MEMORY = 0,
	DRV_ACPI_SPACE_SYSTEM_IO = 1,
	DRV_ACPI_SPACE_PCI_CONFIG = 2,
	DRV_ACPI_SPACE_EMBEDDED_CONTROL = 3,
	DRV_ACPI_SPACE_SMBUS = 4,
	DRV_ACPI_SPACE_SYSTEM_CMOS = 5,
	DRV_ACPI_SPACE_PCI_BAR_TARGET = 6,
	DRV_ACPI_SPACE_IPMI = 7,
	DRV_ACPI_SPACE_GPIO = 8,
	DRV_ACPI_SPACE_GENERIC_SERIAL_BUS = 9,
	DRV_ACPI_SPACE_PCC = 10,
	DRV_ACPI_SPACE_PRM = 11,
	DRV_ACPI_SPACE_COUNT = 12
};

/*
 * The kinds of resource a resource template (_CRS) describes.
 */
enum drv_acpi_resource_kind {
	DRV_ACPI_RESOURCE_IO = 1,
	DRV_ACPI_RESOURCE_MEMORY = 2,
	DRV_ACPI_RESOURCE_IRQ = 3,
	DRV_ACPI_RESOURCE_I2C = 4,
	DRV_ACPI_RESOURCE_GPIO_INT = 5
};

/*
 * The device power states of _PSx and _PRx (ACPI 6.5 section 2.3).  D3hot
 * keeps the power resources _PR3 names on; D3cold turns every power
 * resource of the device off.
 */
enum drv_acpi_device_state {
	DRV_ACPI_D0 = 0,
	DRV_ACPI_D1 = 1,
	DRV_ACPI_D2 = 2,
	DRV_ACPI_D3_HOT = 3,
	DRV_ACPI_D3_COLD = 4
};

/* What drv_acpi_events_sleep_end() reports when no GPE fired during the sleep. */
#define DRV_ACPI_GPE_NONE	0xffffffffU

/* The longest controller path a serial bus or GPIO resource keeps, with its terminating NUL. */
#define DRV_ACPI_RESOURCE_SOURCE_MAX	64U

struct drv_acpi_node;
struct drv_acpi_object;

/*
 * One resource of a device, as drv_acpi_resources_walk() decodes it from a
 * resource template.
 *
 * base and length are the range of an I/O or memory resource (the minimum
 * address and the length the descriptor gives; an address space
 * descriptor's translation is not added), and base is the interrupt number
 * of an IRQ, whose length is 1.  descriptor is the tag byte the resource
 * came from (0x47 an I/O port, 0x86 a fixed 32-bit memory range, and so
 * on), for a driver that cares which form firmware used.  writable says a
 * memory range may be written; producer says an address space descriptor
 * gives the range to its children rather than using it.  level, active_low
 * and shared describe an interrupt.
 *
 * An I2C serial bus connection (I2cSerialBus) puts the device's address on
 * its bus in base, its connection speed in hertz in speed, sets ten_bit
 * for a 10-bit address, and names the bus controller in source.  A GPIO
 * interrupt connection (GpioInt) puts its first pin in base (length is the
 * number of pins), describes the interrupt with level, active_low, shared
 * and wake, and names the GPIO controller in source.  source is empty for
 * the other kinds, and a path too long for it is cut short.
 */
struct drv_acpi_resource {
	enum drv_acpi_resource_kind kind;
	uint64_t base;
	uint64_t length;
	uint8_t descriptor;
	uint8_t writable;
	uint8_t producer;
	uint8_t level;
	uint8_t active_low;
	uint8_t shared;
	uint8_t wake;
	uint8_t ten_bit;
	uint32_t speed;
	char source[DRV_ACPI_RESOURCE_SOURCE_MAX];
};

/*
 * A walk visitor.
 *
 * It returns 0 to continue into the node's children, a positive value to
 * skip them, and a negative value to stop the walk.
 */
typedef int (*drv_acpi_walk_visitor_t)(struct drv_acpi_node *node, unsigned depth, void *argument);

/*
 * One access an operation region makes to its address space, as the
 * interpreter hands it to the space's handler.
 *
 * address is the byte address inside the space (for PCI configuration
 * space, the offset in the function's space, with the function in the pci
 * fields), width is the access width in bits: 8, 16, 32 or 64, and write
 * says whether the handler stores the value or reads it.
 */
struct drv_acpi_region_access {
	uint64_t address;
	unsigned width;
	bool write;
	uint16_t pci_segment;
	uint8_t pci_bus;
	uint8_t pci_device;
	uint8_t pci_function;
};

/*
 * An address space handler: makes one access and reports zero, or the
 * error that fails the AML which made it.
 */
typedef int (*drv_acpi_region_handler_t)(const struct drv_acpi_region_access *access, uint64_t *value, void *argument);

/*
 * A notification handler: Notify (node, value) reached the driver.
 */
typedef void (*drv_acpi_notify_handler_t)(struct drv_acpi_node *node, uint32_t value, void *argument);

/*
 * The fixed events of the PM1 registers a driver may handle; the numbers
 * are their bits in PM1_STS and PM1_EN.
 */
enum drv_acpi_fixed_event {
	DRV_ACPI_EVENT_TIMER = 0,
	DRV_ACPI_EVENT_GLOBAL_LOCK = 5,
	DRV_ACPI_EVENT_POWER_BUTTON = 8,
	DRV_ACPI_EVENT_SLEEP_BUTTON = 9,
	DRV_ACPI_EVENT_RTC = 10
};

/*
 * A fixed event handler, called in the event thread.
 */
typedef void (*drv_acpi_fixed_handler_t)(enum drv_acpi_fixed_event event, void *argument);

/*
 * A GPE handler a driver installs in place of the GPE's method, called in
 * the event thread.
 */
typedef void (*drv_acpi_gpe_handler_t)(unsigned gpe, void *argument);

/*
 * The work drv_acpi_run_locked() runs inside the interpreter with an AML
 * mutex held: it reports zero or the error the call reports.
 */
typedef int (*drv_acpi_locked_work_t)(void *argument);

/*
 * A resource visitor: called once for each resource of a template, in the
 * template's order.  It returns 0 to go on and anything else to stop the
 * walk, which then reports that value; a negative value tells a stop the
 * visitor chose from the positive errno values the walk reports itself.
 */
typedef int (*drv_acpi_resource_visitor_t)(const struct drv_acpi_resource *resource, void *argument);

int
drv_acpi_attach(void);

int
drv_acpi_initialize_namespace(void);

int
drv_acpi_load_table(
	const void *table,
	size_t length);

int
drv_acpi_initialize_objects(void);

void
drv_acpi_initialize_devices(void);

void
drv_acpi_reset(void);

int
drv_acpi_lookup(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_node **result);

struct drv_acpi_node *
drv_acpi_root(void);

int
drv_acpi_walk(
	struct drv_acpi_node *scope,
	drv_acpi_walk_visitor_t visitor,
	void *argument);

int
drv_acpi_node_path(
	const struct drv_acpi_node *node,
	char *buffer,
	size_t size);

enum drv_acpi_type
drv_acpi_node_type(
	const struct drv_acpi_node *node);

int
drv_acpi_evaluate(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result);

int
drv_acpi_evaluate_integer(
	struct drv_acpi_node *scope,
	const char *path,
	uint64_t *value);

struct drv_acpi_object *
drv_acpi_object_integer_new(
	uint64_t value);

struct drv_acpi_object *
drv_acpi_object_string_new(
	const char *text);

struct drv_acpi_object *
drv_acpi_object_buffer_new(
	const void *bytes,
	size_t length);

struct drv_acpi_object *
drv_acpi_object_package_new(
	uint32_t count);

int
drv_acpi_object_package_set(
	struct drv_acpi_object *package,
	unsigned index,
	struct drv_acpi_object *element);

void
drv_acpi_object_release(
	struct drv_acpi_object *object);

enum drv_acpi_type
drv_acpi_object_type(
	const struct drv_acpi_object *object);

uint64_t
drv_acpi_object_integer(
	const struct drv_acpi_object *object);

const char *
drv_acpi_object_string(
	const struct drv_acpi_object *object,
	size_t *length);

const uint8_t *
drv_acpi_object_buffer(
	const struct drv_acpi_object *object,
	size_t *length);

unsigned
drv_acpi_object_package_count(
	const struct drv_acpi_object *object);

struct drv_acpi_object *
drv_acpi_object_package_element(
	const struct drv_acpi_object *object,
	unsigned index);

struct drv_acpi_node *
drv_acpi_object_reference_node(
	const struct drv_acpi_object *object);

int
drv_acpi_region_install(
	enum drv_acpi_space space,
	drv_acpi_region_handler_t handler,
	void *argument);

int
drv_acpi_region_connect_all(void);

int
drv_acpi_notify_install(
	struct drv_acpi_node *node,
	drv_acpi_notify_handler_t handler,
	void *argument);

int
drv_acpi_notify_remove(
	struct drv_acpi_node *node,
	drv_acpi_notify_handler_t handler,
	void *argument);

int
drv_acpi_run_locked(
	const char *mutex_path,
	drv_acpi_locked_work_t work,
	void *argument);

int
drv_acpi_resources_walk(
	struct drv_acpi_node *device,
	const char *method,
	drv_acpi_resource_visitor_t visitor,
	void *argument);

/*
 * The resources of a PCI host bridge (the _CRS of the PNP0A03 or PNP0A08
 * device whose _SEG and _BBN name the segment and bus), and those of the
 * motherboard resource devices (PNP0C01, PNP0C02), for the placement of a
 * BAR the firmware left unassigned (acpi-pci-root.c, BUG-210).
 */
int
drv_acpi_pci_root_resources_walk(
	uint16_t segment,
	uint8_t bus,
	drv_acpi_resource_visitor_t visitor,
	void *argument);

int
drv_acpi_system_resources_walk(
	drv_acpi_resource_visitor_t visitor,
	void *argument);

int
drv_acpi_events_init(
	const uint8_t *fadt,
	size_t length);

unsigned
drv_acpi_sci_irq(void);

int
drv_acpi_fixed_event_install(
	enum drv_acpi_fixed_event event,
	drv_acpi_fixed_handler_t handler,
	void *argument);

int
drv_acpi_gpe_install(
	unsigned gpe,
	bool edge,
	drv_acpi_gpe_handler_t handler,
	void *argument);

/* A GPE a device's _PRW names, enabled at runtime for its driver (acpi-event.c, BUG-253). */
int
drv_acpi_gpe_runtime_enable(
	unsigned gpe);

bool
drv_acpi_sci_interrupt(void);

int
drv_acpi_poweroff(void);

/* The FADT's reset register (acpi-event.c, BUG-249): 0 once the reset is asked for. */
int
drv_acpi_reset_machine(void);

void
drv_acpi_events_process(void);

int
drv_acpi_global_lock_attach(
	volatile uint32_t *word);

int
drv_acpi_ec_ecdt(
	const uint8_t *ecdt,
	size_t length);

int
drv_acpi_ec_attach(void);

/* The GPE wake sources of S0 idle (acpi-event.c, ws052-p003). */
int
drv_acpi_gpe_wake_set(
	unsigned gpe,
	bool arm);

int
drv_acpi_events_sleep_begin(void);

int
drv_acpi_events_sleep_end(
	unsigned *woken);

int
drv_acpi_events_sleep_woken(
	unsigned *woken);

int
drv_acpi_ec_gpe(
	unsigned *gpe);

int
drv_acpi_s0_idle_capable(void);

/* The LPS0 notifications and the device power of S0 idle (acpi-sleep.c, ws052-p003). */
int
drv_acpi_lps0_attach(void);

int
drv_acpi_lps0_enter(void);

int
drv_acpi_lps0_exit(void);

int
drv_acpi_lps0_present(void);

int
drv_acpi_device_power_set(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state state);

int
drv_acpi_device_wake_enable(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state state);

int
drv_acpi_device_wake_disable(
	struct drv_acpi_node *device);

int
drv_acpi_device_wake_state(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state *state);

/* The ACPI side of the PCI functions' power (acpi-pci-power.c, ws052-p004). */
int
drv_acpi_pci_power_attach(void);

/* The lid, the AC adapter, the batteries and the buttons as the system's events (acpi-power.c, ws132-p002). */
int
drv_acpi_power_attach(void);

/* The thermal zones and the devices with a _TMP, in sysctl hw.thermal (acpi-thermal.c, ws134-p009). */
int
drv_acpi_thermal_attach(void);

struct system_power_info;

void
drv_acpi_power_get(
	struct system_power_info *info);

#endif
