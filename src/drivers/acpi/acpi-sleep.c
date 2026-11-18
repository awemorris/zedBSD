/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI side of S0 idle (ws052-p003): the LPS0 device's notifications
 * and the device power helpers that the suspend of the devices uses.
 *
 * The LPS0 device (_HID INT33A1, or PNP0D80 as an identifier) is told
 * through its _DSM that the displays went off and that the platform
 * enters its low-power idle, and the reverse on the way back.  Two _DSM
 * families exist, Intel's and Microsoft's; firmware implements either or
 * both, and function 0 of each says which of its functions exist.  The
 * Microsoft family adds the modern standby entry and exit around the
 * low-power entry and exit.
 *
 * A device's power goes through _PS0 to _PS3 and the power resources its
 * _PR0 to _PR3 name.  A power resource is shared among devices: it is
 * turned on (_ON) when the first device needs it and off (_OFF) when the
 * last one lets it go.  A device's wake goes through _PRW (its GPE and the
 * power resources the wake needs) and _DSW, or the older _PSW.
 *
 * Each public function holds the interpreter while it runs, and the
 * tables below change only with it held.  A method that sleeps (Sleep)
 * lets the interpreter go meanwhile; the S0 idle path calls these from its
 * one thread, which is what keeps one device's transitions in order.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <drivers/acpi/acpi.h>
#include "aml-internal.h"
#include "aml-os.h"

/* The most devices and power resources the helpers follow. */
#define DEVICES_MAX		64U
#define RESOURCES_MAX		32U

/* The most power resources of one _PRx or _PRW that the helpers take. */
#define DEVICE_RESOURCES_MAX	8U

/* _PRW: the GPE first, then the deepest sleep state, then the power resources. */
#define PRW_GPE			0U
#define PRW_RESOURCES		2U

/* _STA: the device is present. */
#define STATUS_PRESENT		0x01U

/* The LPS0 device: its _HID, and PNP0D80 as a string and as an EISA identifier. */
#define LPS0_HID		"INT33A1"
#define LPS0_COMPATIBLE		"PNP0D80"
#define LPS0_COMPATIBLE_EISA	0x800dd041U

/*
 * The _DSM revision of each family.  Microsoft's documents revision 0 and
 * the Latitude 5330's Microsoft _DSM answers only to it; the 5330's Intel
 * _DSM does not look at the revision, and Intel's documents revision 0.
 */
#define LPS0_REVISION_INTEL	0U
#define LPS0_REVISION_MICROSOFT	0U

/*
 * The _DSM functions: 0 asks which functions exist, 3 and 4 tell of the
 * displays going off and on, 5 and 6 of the low-power entry and exit, and
 * 7 and 8 (Microsoft's only) of the modern standby entry and exit.
 */
#define LPS0_QUERY		0U
#define LPS0_DISPLAY_OFF	3U
#define LPS0_DISPLAY_ON		4U
#define LPS0_ENTRY		5U
#define LPS0_EXIT		6U
#define LPS0_SLEEP_ENTRY	7U
#define LPS0_SLEEP_EXIT		8U

/* The functions a family's mask can name: the first four bytes of function 0's buffer. */
#define LPS0_MASK_BYTES		4U

/* The length of a _DSM UUID, and how many arguments a _DSM takes. */
#define DSM_UUID_LENGTH		16U
#define DSM_ARGUMENTS		4U

/* How many arguments _DSW takes: enable, the target system state, the target device state. */
#define DSW_ARGUMENTS		3U

/* The system state _DSW is told for S0 idle. */
#define SYSTEM_STATE_S0		0U

/*
 * The two _DSM families of the LPS0 device.
 */
enum lps0_family {
	LPS0_INTEL = 0,
	LPS0_MICROSOFT = 1,
	LPS0_FAMILIES = 2
};

/*
 * One notification of the sequence into or out of the low-power idle: a
 * _DSM family and its function.  It is called only when the family's
 * function 0 named the function.
 */
struct lps0_step {
	uint8_t family;
	uint8_t function;
};

/*
 * The power resources one method named, in its order: the ones a device
 * holds for its D-state or for its wake, or the ones it is about to take.
 */
struct resource_list {
	struct drv_acpi_node *nodes[DEVICE_RESOURCES_MAX];
	unsigned count;
};

/*
 * One device whose power the helpers changed: its D-state, the power
 * resources it holds for that state, and, while its wake is enabled, the
 * GPE it armed and the power resources the wake holds.  A device enters
 * the table at its first change, taken to be in D0 with the resources of
 * its _PR0, and stays as long as the kernel.
 */
struct power_device {
	struct drv_acpi_node *node;
	struct resource_list held;
	struct resource_list wake_held;
	unsigned wake_gpe;
	uint8_t state;
	uint8_t wake;
};

/*
 * One power resource and how many devices (or wakes) hold it: _ON ran when
 * the count left zero and _OFF runs when it returns to zero.  A resource
 * enters the table when it is first taken and stays as long as the kernel.
 */
struct power_resource {
	struct drv_acpi_node *node;
	unsigned references;
};

/* The UUIDs of the two families, as the bytes of their ACPI buffers. */
static const uint8_t lps0_uuids[LPS0_FAMILIES][DSM_UUID_LENGTH] = {
	/* c4eb40a0-6cd2-11e2-bcfd-0800200c9a66 */
	{
		0xa0, 0x40, 0xeb, 0xc4, 0xd2, 0x6c, 0xe2, 0x11,
		0xbc, 0xfd, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
	},
	/* 11e00d56-ce64-47ce-837b-1f898f9aa461 */
	{
		0x56, 0x0d, 0xe0, 0x11, 0x64, 0xce, 0xce, 0x47,
		0x83, 0x7b, 0x1f, 0x89, 0x8f, 0x9a, 0xa4, 0x61
	}
};

/* The _DSM revision each family is called with. */
static const uint8_t lps0_revisions[LPS0_FAMILIES] = {
	LPS0_REVISION_INTEL,
	LPS0_REVISION_MICROSOFT
};

/*
 * The notifications into the low-power idle, in order: the displays off,
 * the modern standby entry, then the low-power entry, each family's in
 * turn.
 */
static const struct lps0_step lps0_enter_steps[] = {
	{ LPS0_INTEL, LPS0_DISPLAY_OFF },
	{ LPS0_MICROSOFT, LPS0_DISPLAY_OFF },
	{ LPS0_MICROSOFT, LPS0_SLEEP_ENTRY },
	{ LPS0_INTEL, LPS0_ENTRY },
	{ LPS0_MICROSOFT, LPS0_ENTRY }
};

/*
 * The notifications out of the low-power idle: the reverse of the entry,
 * the low-power exit, the modern standby exit, then the displays on.
 */
static const struct lps0_step lps0_exit_steps[] = {
	{ LPS0_INTEL, LPS0_EXIT },
	{ LPS0_MICROSOFT, LPS0_EXIT },
	{ LPS0_MICROSOFT, LPS0_SLEEP_EXIT },
	{ LPS0_INTEL, LPS0_DISPLAY_ON },
	{ LPS0_MICROSOFT, LPS0_DISPLAY_ON }
};

/*
 * The LPS0 device drv_acpi_lps0_attach() found, and the functions each
 * family has (bit n set: function n exists; zero: the family is absent).
 * Attach fills it with the interpreter held, before the S0 idle path can
 * run; afterwards it is only read.  device is NULL until one was found.
 */
static struct {
	struct drv_acpi_node *device;
	uint32_t functions[LPS0_FAMILIES];
} lps0;

/*
 * The devices whose power the helpers changed, the first device_count
 * slots used.  Read and written with the interpreter held.
 */
static struct power_device devices[DEVICES_MAX];
static unsigned device_count;

/*
 * The power resources the helpers took at least once, the first
 * resource_count slots used.  Read and written with the interpreter held.
 */
static struct power_resource resources[RESOURCES_MAX];
static unsigned resource_count;

static int lps0_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool lps0_identified(struct drv_acpi_node *node);
static bool lps0_id_matches(const struct drv_acpi_object *object);
static bool device_present(struct drv_acpi_node *node);
static uint32_t lps0_query(struct drv_acpi_node *device, unsigned family);
static int lps0_call(struct drv_acpi_node *device, unsigned family, unsigned function, struct drv_acpi_object **result);
static int lps0_run(const struct lps0_step *steps, unsigned count);
static struct power_device *device_record(struct drv_acpi_node *node, int *error);
static struct power_device *device_find(struct drv_acpi_node *node);
static int resources_of_state(struct drv_acpi_node *device, enum drv_acpi_device_state state, struct resource_list *list);
static int resources_from_package(const struct drv_acpi_object *package, unsigned first, struct resource_list *list);
static int resources_take(const struct resource_list *list);
static void resources_drop(const struct resource_list *list, unsigned count);
static struct power_resource *resource_find(struct drv_acpi_node *node);
static struct power_resource *resource_record(struct drv_acpi_node *node);
static int resource_take(struct drv_acpi_node *node);
static void resource_drop(struct drv_acpi_node *node);
static int state_method(struct drv_acpi_node *device, enum drv_acpi_device_state state);
static int evaluate_optional(struct drv_acpi_node *device, const char *name, struct drv_acpi_object **arguments, unsigned count);
static int wake_method(struct drv_acpi_node *device, bool enable, enum drv_acpi_device_state state);
static int prw_read(struct drv_acpi_node *device, unsigned *gpe, struct resource_list *list);
static void log_device(const char *what, struct drv_acpi_node *node, int error);

/*
 * Finds the LPS0 device and learns which _DSM functions each family has.
 *
 * It reports ENODEV when no present LPS0 device exists or neither family
 * answers function 0, which is a platform without S0 idle notifications.
 */
int
drv_acpi_lps0_attach(void)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct drv_acpi_node *device;
	char path[64];
	unsigned family;
	int error;

	/* Finds the device and asks each family which functions it has, with the interpreter held. */
	thread = drv_acpi_enter(&storage);

	/* Walks the namespace for the first present LPS0 device. */
	device = NULL;
	(void)drv_acpi_walk(NULL, lps0_visitor, &device);

	/* Asks function 0 of each family of a device found; zero means the family is absent. */
	kern_memset(lps0.functions, 0, sizeof(lps0.functions));
	if (device != NULL) {
		for (family = 0; family < LPS0_FAMILIES; family++)
			lps0.functions[family] = lps0_query(device, family);
	}

	/* Lets the interpreter go. */
	drv_acpi_leave(thread);

	/* Refuses a platform without the device. */
	if (device == NULL) {
		drv_acpi_os_log("ACPI: no LPS0 device; S0 idle has no platform notifications\n");
		return ENODEV;
	}

	/* Names the device in the log. */
	error = drv_acpi_node_path(device, path, sizeof(path));
	if (error != 0)
		path[0] = '\0';

	/* Refuses a device neither family answers for. */
	if (lps0.functions[LPS0_INTEL] == 0 && lps0.functions[LPS0_MICROSOFT] == 0) {
		drv_acpi_os_log("ACPI: LPS0 %s has no _DSM functions\n", path);
		return ENODEV;
	}

	/* Keeps the device for the entry and the exit, and logs the functions each family has. */
	lps0.device = device;
	drv_acpi_os_log("ACPI: LPS0 %s, Intel functions 0x%x, Microsoft functions 0x%x\n", path, (unsigned)lps0.functions[LPS0_INTEL], (unsigned)lps0.functions[LPS0_MICROSOFT]);

	/* Succeeded: the entry and the exit can tell the platform. */
	return 0;
}

/*
 * Reports whether an LPS0 device with _DSM functions attached (ws052-p006):
 * a platform without one has no S0 idle to enter.  Returns 1 or 0.
 */
int
drv_acpi_lps0_present(void)
{
	/* The device drv_acpi_lps0_attach() kept. */
	if (lps0.device == NULL)
		return 0;

	/* Succeeded: S0 idle can be told to the platform. */
	return 1;
}

/*
 * Tells the platform that the displays are off and that it enters its
 * low-power idle.
 *
 * Every notification the device has is made, in order, even after one
 * failed; the first failure is reported.  It reports ENODEV without an
 * LPS0 device.
 */
int
drv_acpi_lps0_enter(void)
{
	int error;

	/* Makes the notifications into the low-power idle. */
	error = lps0_run(lps0_enter_steps, sizeof(lps0_enter_steps) / sizeof(lps0_enter_steps[0]));
	if (error != 0)
		return error;

	/* Succeeded: the platform knows it may enter its low-power idle. */
	return 0;
}

/*
 * Tells the platform that it leaves its low-power idle and that the
 * displays are on again.
 *
 * Every notification the device has is made, in order, even after one
 * failed; the first failure is reported.  It reports ENODEV without an
 * LPS0 device.
 */
int
drv_acpi_lps0_exit(void)
{
	int error;

	/* Makes the notifications out of the low-power idle. */
	error = lps0_run(lps0_exit_steps, sizeof(lps0_exit_steps) / sizeof(lps0_exit_steps[0]));
	if (error != 0)
		return error;

	/* Succeeded: the platform knows it runs again. */
	return 0;
}

/*
 * Puts a device into a D-state through its _PSx and the power resources
 * its _PRx name.
 *
 * Toward less power, _PSx runs first, then the resources of the new state
 * are taken and those of the old one let go.  Toward more power, the new
 * resources are taken first, so that the device has power when _PSx runs.
 * D3cold is D3hot with no power resource held, and between the two only
 * the resources change.  D1 and D2 are refused from D3 with EINVAL.  A
 * D-state the device has neither _PSx nor _PRx for is refused with
 * ENOTSUP, except D0, D3hot and D3cold, which every device has.  On a
 * failure the device keeps its former state and resources.
 */
int
drv_acpi_device_power_set(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state state)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct power_device *record;
	struct resource_list wanted;
	bool tell;
	int error;

	/* Refuses no device and a state beyond D3cold. */
	if (device == NULL || state > DRV_ACPI_D3_COLD)
		return EINVAL;

	/* Changes the state with the interpreter held, so that the tables stay consistent. */
	thread = drv_acpi_enter(&storage);

	/* Finds the device's record, which takes the resources of D0 at the first change. */
	record = device_record(device, &error);
	if (record == NULL) {
		drv_acpi_leave(thread);
		log_device("power", device, error);
		return error;
	}

	/* A device already in the state has nothing to do. */
	if (record->state == (uint8_t)state) {
		drv_acpi_leave(thread);
		return 0;
	}

	/* Refuses D1 or D2 from D3: a device leaves D3 only for D0. */
	if (record->state >= DRV_ACPI_D3_HOT &&
	    (state == DRV_ACPI_D1 ||
	     state == DRV_ACPI_D2)) {
		drv_acpi_leave(thread);
		return EINVAL;
	}

	/* Between D3hot and D3cold only the resources change: the device was told D3 already. */
	tell = true;
	if (record->state >= DRV_ACPI_D3_HOT && state >= DRV_ACPI_D3_HOT)
		tell = false;

	/* Reads the resources the new state needs. */
	error = resources_of_state(device, state, &wanted);
	if (error != 0) {
		drv_acpi_leave(thread);
		log_device("power", device, error);
		return error;
	}

	/* Toward less power the device is told first, while it still has its power. */
	if (tell && (uint8_t)state > record->state) {
		error = state_method(device, state);
		if (error != 0) {
			drv_acpi_leave(thread);
			log_device("power", device, error);
			return error;
		}
	}

	/* Takes the new state's resources before the old ones go, so that a shared one stays on. */
	error = resources_take(&wanted);
	if (error != 0) {
		drv_acpi_leave(thread);
		log_device("power", device, error);
		return error;
	}

	/* Toward more power the device is told once its power is back. */
	if (tell && (uint8_t)state < record->state) {
		error = state_method(device, state);
		if (error != 0) {
			resources_drop(&wanted, wanted.count);
			drv_acpi_leave(thread);
			log_device("power", device, error);
			return error;
		}
	}

	/* Lets the old state's resources go; the record now holds the new state's. */
	resources_drop(&record->held, record->held.count);
	record->held = wanted;
	record->state = (uint8_t)state;

	/* Lets the interpreter go. */
	drv_acpi_leave(thread);

	/* Succeeded: the device is in the state. */
	return 0;
}

/*
 * Lets a device wake the system from S0 idle.
 *
 * The power resources its _PRW names are taken, its _DSW is told the wake
 * is enabled for S0 with the D-state the device will be in (or its _PSW,
 * when it has no _DSW), and the GPE its _PRW names is armed.  A device
 * without _PRW is refused with ENOENT, and a _PRW whose GPE is in a GPE
 * block device with ENOTSUP.  Enabling the wake of a device whose wake is
 * enabled does nothing.  On a failure nothing stays changed.
 */
int
drv_acpi_device_wake_enable(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state state)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct power_device *record;
	struct resource_list wake_resources;
	unsigned gpe;
	int error;

	/* Refuses no device and a state beyond D3cold. */
	if (device == NULL || state > DRV_ACPI_D3_COLD)
		return EINVAL;

	/* Enables the wake with the interpreter held, so that the tables stay consistent. */
	thread = drv_acpi_enter(&storage);

	/* Finds the device's record. */
	record = device_record(device, &error);
	if (record == NULL) {
		drv_acpi_leave(thread);
		log_device("wake", device, error);
		return error;
	}

	/* A wake already enabled has nothing to do. */
	if (record->wake) {
		drv_acpi_leave(thread);
		return 0;
	}

	/* Reads the GPE and the resources of the wake. */
	error = prw_read(device, &gpe, &wake_resources);
	if (error != 0) {
		drv_acpi_leave(thread);
		log_device("wake", device, error);
		return error;
	}

	/* Takes the resources the wake needs, which stay on in every D-state. */
	error = resources_take(&wake_resources);
	if (error != 0) {
		drv_acpi_leave(thread);
		log_device("wake", device, error);
		return error;
	}

	/* Tells the device to signal its wake. */
	error = wake_method(device, true, state);
	if (error != 0) {
		resources_drop(&wake_resources, wake_resources.count);
		drv_acpi_leave(thread);
		log_device("wake", device, error);
		return error;
	}

	/* Arms the GPE, so that it raises an SCI during a sleep. */
	error = drv_acpi_gpe_wake_set(gpe, true);
	if (error != 0) {
		/* Undoes the device's wake signal and the wake's resources. */
		(void)wake_method(device, false, DRV_ACPI_D0);
		resources_drop(&wake_resources, wake_resources.count);
		drv_acpi_leave(thread);
		log_device("wake", device, error);
		return error;
	}

	/* wake makes the disable undo exactly this: the GPE armed and the resources held. */
	record->wake = 1;
	record->wake_gpe = gpe;
	record->wake_held = wake_resources;

	/* Lets the interpreter go. */
	drv_acpi_leave(thread);

	/* Succeeded: the device wakes the system. */
	return 0;
}

/*
 * Stops a device from waking the system.
 *
 * It undoes drv_acpi_device_wake_enable(): _DSW (or _PSW) is told the wake
 * is disabled, the GPE disarmed and the wake's power resources let go.  A
 * device whose wake is not enabled is refused with EINVAL.
 */
int
drv_acpi_device_wake_disable(
	struct drv_acpi_node *device)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct power_device *record;
	int error;

	/* Refuses no device. */
	if (device == NULL)
		return EINVAL;

	/* Disables the wake with the interpreter held, so that the tables stay consistent. */
	thread = drv_acpi_enter(&storage);

	/* Refuses a device whose wake the helpers did not enable. */
	record = device_find(device);
	if (record == NULL || !record->wake) {
		drv_acpi_leave(thread);
		return EINVAL;
	}

	/* Tells the device to stop signalling its wake; a failure is logged and the rest still undone. */
	error = wake_method(device, false, DRV_ACPI_D0);
	if (error != 0)
		log_device("wake disable", device, error);

	/* Disarms the GPE the enable armed. */
	error = drv_acpi_gpe_wake_set(record->wake_gpe, false);
	if (error != 0)
		log_device("wake disable", device, error);

	/* Lets the wake's resources go; the device no longer wakes the system. */
	resources_drop(&record->wake_held, record->wake_held.count);
	record->wake_held.count = 0;
	record->wake = 0;

	/* Lets the interpreter go. */
	drv_acpi_leave(thread);

	/* Succeeded: the device no longer wakes the system. */
	return 0;
}

/*
 * Reports the deepest D-state from which a device can wake the system
 * from S0 idle, from its _S0W.
 *
 * It reports ENOENT when the device has no _S0W, which leaves the choice
 * to the driver.
 */
int
drv_acpi_device_wake_state(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state *state)
{
	uint64_t value;
	int error;

	/* Refuses no device or nowhere to put the answer. */
	if (device == NULL || state == NULL)
		return EINVAL;

	/* Reads _S0W. */
	error = drv_acpi_evaluate_integer(device, "_S0W", &value);
	if (error != 0)
		return error;

	/* Refuses a state beyond D3cold. */
	if (value > DRV_ACPI_D3_COLD)
		return EINVAL;

	/* Succeeded: reports the deepest state the device wakes from. */
	*state = (enum drv_acpi_device_state)value;
	return 0;
}

/* Stops the walk at the first present LPS0 device and keeps it. */
static int
lps0_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct drv_acpi_node **found;
	enum drv_acpi_type type;
	bool identified;
	bool present;

	UNUSED_PARAMETER(depth);

	/* Only a device can be the LPS0 device. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* Skips a device of another identifier. */
	identified = lps0_identified(node);
	if (!identified)
		return 0;

	/* Skips a device firmware reports absent. */
	present = device_present(node);
	if (!present)
		return 0;

	/* Keeps the device and ends the walk. */
	found = argument;
	*found = node;
	return -1;
}

/* Tells whether a device's _HID or _CID names the LPS0 device. */
static bool
lps0_identified(
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
		/* A _HID of the LPS0 device is enough. */
		matched = lps0_id_matches(object);
		drv_acpi_object_release(object);
		if (matched)
			return true;
	}

	/* Then the compatible identifiers: one, or a package of them. */
	object = NULL;
	error = drv_acpi_evaluate(node, "_CID", NULL, 0, &object);
	if (error != 0 || object == NULL)
		return false;

	/* A package names several; any of them may be the LPS0 device's. */
	matched = false;
	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_PACKAGE) {
		count = drv_acpi_object_package_count(object);
		for (index = 0; index < count; index++) {
			/* The first match among them is enough. */
			element = drv_acpi_object_package_element(object, index);
			matched = lps0_id_matches(element);
			if (matched)
				break;
		}
	} else {
		matched = lps0_id_matches(object);
	}

	/* The identifiers are no longer needed. */
	drv_acpi_object_release(object);

	/* Succeeded: reports whether the device is the LPS0 device. */
	return matched;
}

/* Tells whether one identifier is the LPS0 device's, as a string or an EISA identifier. */
static bool
lps0_id_matches(
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

	/* The EISA form names only PNP0D80. */
	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_INTEGER) {
		value = drv_acpi_object_integer(object);
		if (value == LPS0_COMPATIBLE_EISA)
			return true;

		/* Any other EISA identifier is another device's. */
		return false;
	}

	/* Anything but a string names nothing else. */
	if (type != DRV_ACPI_TYPE_STRING)
		return false;

	/* The string form: INT33A1. */
	text = drv_acpi_object_string(object, &length);
	compared = kern_strcmp(text, LPS0_HID);
	if (compared == 0)
		return true;

	/* Or PNP0D80. */
	compared = kern_strcmp(text, LPS0_COMPATIBLE);
	if (compared == 0)
		return true;

	/* The identifier is some other device's. */
	return false;
}

/* Tells whether firmware reports a device present; a device without _STA is. */
static bool
device_present(
	struct drv_acpi_node *node)
{
	struct drv_acpi_node *method;
	uint64_t status;
	int error;

	/* A device without _STA is present. */
	error = drv_acpi_lookup(node, "_STA", &method);
	if (error != 0)
		return true;

	/* Reads _STA; one that fails says nothing usable. */
	error = drv_acpi_evaluate_integer(method, NULL, &status);
	if (error != 0)
		return false;

	/* The present bit decides. */
	if ((status & STATUS_PRESENT) != 0)
		return true;

	/* The device is absent. */
	return false;
}

/* Asks function 0 of a family which functions exist, and reports them, or zero for an absent family. */
static uint32_t
lps0_query(
	struct drv_acpi_node *device,
	unsigned family)
{
	struct drv_acpi_object *result;
	enum drv_acpi_type type;
	const uint8_t *bytes;
	uint32_t functions;
	size_t length;
	unsigned index;
	int error;

	/* Calls function 0; a family that fails or answers nothing is absent. */
	error = lps0_call(device, family, LPS0_QUERY, &result);
	if (error != 0 || result == NULL)
		return 0;

	/* Anything but a buffer names no function. */
	bytes = NULL;
	length = 0;
	type = drv_acpi_object_type(result);
	if (type == DRV_ACPI_TYPE_BUFFER)
		bytes = drv_acpi_object_buffer(result, &length);

	/* Gathers the mask, its first byte the lowest. */
	functions = 0;
	for (index = 0; index < length && index < LPS0_MASK_BYTES; index++)
		functions |= (uint32_t)bytes[index] << (index * 8U);

	/* The answer is no longer needed. */
	drv_acpi_object_release(result);

	/* Bit 0 clear means the family has no function at all. */
	if ((functions & 1U) == 0)
		return 0;

	/* Succeeded: reports the functions the family has. */
	return functions;
}

/* Calls one function of a family's _DSM with an empty package as its argument. */
static int
lps0_call(
	struct drv_acpi_node *device,
	unsigned family,
	unsigned function,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *arguments[DSM_ARGUMENTS];
	unsigned index;
	int error;

	/* The UUID, the revision, the function, and an empty package. */
	arguments[0] = drv_acpi_object_buffer_new(lps0_uuids[family], DSM_UUID_LENGTH);
	arguments[1] = drv_acpi_object_integer_new(lps0_revisions[family]);
	arguments[2] = drv_acpi_object_integer_new(function);
	arguments[3] = drv_acpi_object_package_new(0U);

	/* Finds whether every argument was made. */
	error = 0;
	for (index = 0; index < DSM_ARGUMENTS; index++) {
		/* One argument missing is enough to refuse. */
		if (arguments[index] == NULL)
			error = ENOMEM;
	}

	/* Evaluates the _DSM when every argument was made. */
	*result = NULL;
	if (error == 0)
		error = drv_acpi_evaluate(device, "_DSM", arguments, DSM_ARGUMENTS, result);

	/* The arguments are no longer needed. */
	for (index = 0; index < DSM_ARGUMENTS; index++)
		drv_acpi_object_release(arguments[index]);

	/* Reports why the function could not be called. */
	if (error != 0)
		return error;

	/* Succeeded: result holds the function's answer, or NULL. */
	return 0;
}

/* Makes each notification of a sequence that the device has, and reports the first failure. */
static int
lps0_run(
	const struct lps0_step *steps,
	unsigned count)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct drv_acpi_object *result;
	unsigned index;
	unsigned family;
	unsigned function;
	int first_error;
	int error;

	/* Refuses a platform whose LPS0 device was not found. */
	if (lps0.device == NULL)
		return ENODEV;

	/* Makes the notifications with the interpreter held, so that no AML runs between them. */
	thread = drv_acpi_enter(&storage);

	/* Calls each function of the sequence the family has. */
	first_error = 0;
	for (index = 0; index < count; index++) {
		/* Skips a function the family does not have. */
		family = steps[index].family;
		function = steps[index].function;
		if ((lps0.functions[family] & (1U << function)) == 0)
			continue;

		/* Calls it; a failure is logged and the sequence goes on. */
		error = lps0_call(lps0.device, family, function, &result);
		if (error != 0) {
			drv_acpi_os_log("ACPI: LPS0 function %u of family %u failed (error %d)\n", function, family, error);

			/* Keeps the first failure for the caller. */
			if (first_error == 0)
				first_error = error;

			continue;
		}

		/* A notification's answer means nothing. */
		drv_acpi_object_release(result);
	}

	/* Lets the interpreter go. */
	drv_acpi_leave(thread);

	/* Reports the first notification that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: every notification was made. */
	return 0;
}

/*
 * Finds a device's record or makes one: a new device is taken to be in D0
 * and takes the power resources of its _PR0.  error receives why no
 * record could be made.
 */
static struct power_device *
device_record(
	struct drv_acpi_node *node,
	int *error)
{
	struct power_device *record;
	struct resource_list d0;

	/* A device already followed has its record. */
	record = device_find(node);
	if (record != NULL)
		return record;

	/* Refuses a device beyond the table. */
	if (device_count == DEVICES_MAX) {
		*error = ENOSPC;
		return NULL;
	}

	/* Reads the resources of D0, which the device is taken to hold. */
	*error = resources_of_state(node, DRV_ACPI_D0, &d0);
	if (*error != 0)
		return NULL;

	/* Takes them, which turns on any that firmware left off. */
	*error = resources_take(&d0);
	if (*error != 0)
		return NULL;

	/* Fills the record: in D0 with those resources, its wake disabled. */
	record = &devices[device_count];
	kern_memset(record, 0, sizeof(*record));
	record->node = node;
	record->held = d0;
	record->state = DRV_ACPI_D0;
	record->wake_gpe = DRV_ACPI_GPE_NONE;

	/* Publishes the record: the table now follows the device. */
	device_count++;

	/* Succeeded: the record follows the device. */
	return record;
}

/* Finds a device's record, or NULL when the helpers never changed the device. */
static struct power_device *
device_find(
	struct drv_acpi_node *node)
{
	unsigned index;

	/* Looks through the records in use. */
	for (index = 0; index < device_count; index++) {
		/* The device's own record. */
		if (devices[index].node == node)
			return &devices[index];
	}

	/* The device has no record. */
	return NULL;
}

/*
 * Reads the power resources a D-state needs: _PR0 to _PR3 for D0 to
 * D3hot, none for D3cold.  A D1 or D2 the device has neither _PSx nor
 * _PRx for is refused with ENOTSUP.
 */
static int
resources_of_state(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state state,
	struct resource_list *list)
{
	static const char *const resource_methods[DRV_ACPI_D3_HOT + 1] = { "_PR0", "_PR1", "_PR2", "_PR3" };
	static const char *const state_methods[DRV_ACPI_D3_HOT + 1] = { "_PS0", "_PS1", "_PS2", "_PS3" };
	struct drv_acpi_object *package;
	struct drv_acpi_node *method;
	int error;

	/* D3cold holds no resource. */
	list->count = 0;
	if (state == DRV_ACPI_D3_COLD)
		return 0;

	/* Finds the state's _PRx. */
	error = drv_acpi_lookup(device, resource_methods[state], &method);
	if (error != 0) {
		/* D0 and D3hot need nothing when the device has no _PRx. */
		if (state == DRV_ACPI_D0 || state == DRV_ACPI_D3_HOT)
			return 0;

		/* D1 and D2 exist only when the device has a _PSx for them. */
		error = drv_acpi_lookup(device, state_methods[state], &method);
		if (error != 0)
			return ENOTSUP;

		/* The state needs no resource. */
		return 0;
	}

	/* Evaluates it. */
	error = drv_acpi_evaluate(method, NULL, NULL, 0, &package);
	if (error != 0)
		return error;

	/* Takes the resources the package names. */
	error = resources_from_package(package, 0, list);
	drv_acpi_object_release(package);
	if (error != 0)
		return error;

	/* Succeeded: list names the resources the state needs. */
	return 0;
}

/*
 * Collects the power resources a package names from one element on.
 *
 * Each element must be a reference to a power resource; anything else is
 * refused with EINVAL, and more than DEVICE_RESOURCES_MAX with E2BIG.
 */
static int
resources_from_package(
	const struct drv_acpi_object *package,
	unsigned first,
	struct resource_list *list)
{
	struct drv_acpi_object *element;
	struct drv_acpi_node *node;
	enum drv_acpi_type type;
	unsigned count;
	unsigned index;

	/* Refuses anything but a package. */
	list->count = 0;
	type = drv_acpi_object_type(package);
	if (type != DRV_ACPI_TYPE_PACKAGE)
		return EINVAL;

	/* Collects each power resource the package names. */
	count = drv_acpi_object_package_count(package);
	for (index = first; index < count; index++) {
		/* Refuses an element that is no power resource. */
		element = drv_acpi_object_package_element(package, index);
		node = drv_acpi_object_reference_node(element);
		if (node == NULL)
			return EINVAL;

		/* Refuses a node that is no power resource. */
		type = drv_acpi_node_type(node);
		if (type != DRV_ACPI_TYPE_POWER_RESOURCE)
			return EINVAL;

		/* Refuses more resources than a list holds. */
		if (list->count == DEVICE_RESOURCES_MAX)
			return E2BIG;

		/* Keeps the resource. */
		list->nodes[list->count] = node;
		list->count++;
	}

	/* Succeeded: list names the package's resources. */
	return 0;
}

/* Takes every resource of a list; on a failure the ones already taken are let go again. */
static int
resources_take(
	const struct resource_list *list)
{
	unsigned index;
	int error;

	/* Takes the resources in the list's order. */
	for (index = 0; index < list->count; index++) {
		/* Takes one; a failure lets the ones before it go. */
		error = resource_take(list->nodes[index]);
		if (error != 0) {
			resources_drop(list, index);
			return error;
		}
	}

	/* Succeeded: every resource of the list is held once more. */
	return 0;
}

/* Lets the first count resources of a list go, in the reverse order. */
static void
resources_drop(
	const struct resource_list *list,
	unsigned count)
{
	unsigned index;

	/* Lets each go, the last taken first. */
	for (index = count; index > 0; index--)
		resource_drop(list->nodes[index - 1U]);
}

/* Finds a power resource's record, or NULL when it was never taken. */
static struct power_resource *
resource_find(
	struct drv_acpi_node *node)
{
	unsigned index;

	/* Looks through the records in use. */
	for (index = 0; index < resource_count; index++) {
		/* The resource's own record. */
		if (resources[index].node == node)
			return &resources[index];
	}

	/* The resource has no record. */
	return NULL;
}

/* Finds a power resource's record or makes one; NULL when the table is full. */
static struct power_resource *
resource_record(
	struct drv_acpi_node *node)
{
	struct power_resource *record;

	/* A resource already taken has its record. */
	record = resource_find(node);
	if (record != NULL)
		return record;

	/* Refuses a resource beyond the table. */
	if (resource_count == RESOURCES_MAX)
		return NULL;

	/* Makes the record, held by nobody yet, and publishes it. */
	record = &resources[resource_count];
	record->node = node;
	record->references = 0;
	resource_count++;

	/* Succeeded: the record follows the resource. */
	return record;
}

/* Takes one reference to a power resource, turning it on when it is the first. */
static int
resource_take(
	struct drv_acpi_node *node)
{
	struct power_resource *record;
	struct drv_acpi_object *result;
	int error;

	/* Finds the resource's record. */
	record = resource_record(node);
	if (record == NULL)
		return ENOSPC;

	/* The first reference turns the resource on. */
	if (record->references == 0) {
		error = drv_acpi_evaluate(node, "_ON", NULL, 0, &result);
		if (error != 0) {
			log_device("power resource on", node, error);
			return error;
		}

		/* _ON returns nothing meaningful. */
		drv_acpi_object_release(result);
	}

	/* One more device or wake keeps the resource on. */
	record->references++;

	/* Succeeded: the resource is on and held. */
	return 0;
}

/* Lets one reference to a power resource go, turning it off when it was the last. */
static void
resource_drop(
	struct drv_acpi_node *node)
{
	struct power_resource *record;
	struct drv_acpi_object *result;
	int error;

	/* Finds the resource's record; a resource nobody holds has nothing to let go. */
	record = resource_find(node);
	if (record == NULL || record->references == 0)
		return;

	/* One holder fewer; zero means nothing needs the resource any more. */
	record->references--;
	if (record->references != 0)
		return;

	/* The last reference turns the resource off; a failure leaves it on, which only costs power. */
	error = drv_acpi_evaluate(node, "_OFF", NULL, 0, &result);
	if (error != 0) {
		log_device("power resource off", node, error);
		return;
	}

	/* _OFF returns nothing meaningful. */
	drv_acpi_object_release(result);
}

/* Runs the _PSx of a D-state (_PS3 for D3cold) when the device has one. */
static int
state_method(
	struct drv_acpi_node *device,
	enum drv_acpi_device_state state)
{
	static const char *const methods[DRV_ACPI_D3_COLD + 1] = { "_PS0", "_PS1", "_PS2", "_PS3", "_PS3" };
	int error;

	/* Runs the method; a device without one changes through its resources alone. */
	error = evaluate_optional(device, methods[state], NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded: the device was told its new state. */
	return 0;
}

/* Runs a method of a device when it has one; a missing method is no failure. */
static int
evaluate_optional(
	struct drv_acpi_node *device,
	const char *name,
	struct drv_acpi_object **arguments,
	unsigned count)
{
	struct drv_acpi_object *result;
	struct drv_acpi_node *method;
	int error;

	/* A device without the method has nothing to run. */
	error = drv_acpi_lookup(device, name, &method);
	if (error != 0)
		return 0;

	/* Runs the method. */
	error = drv_acpi_evaluate(method, NULL, arguments, count, &result);
	if (error != 0)
		return error;

	/* What the method returns means nothing. */
	drv_acpi_object_release(result);

	/* Succeeded: the method ran. */
	return 0;
}

/*
 * Tells a device to signal its wake or to stop: _DSW with the enable, the
 * system state S0 and the D-state, or _PSW with the enable when the device
 * has no _DSW.
 */
static int
wake_method(
	struct drv_acpi_node *device,
	bool enable,
	enum drv_acpi_device_state state)
{
	struct drv_acpi_object *arguments[DSW_ARGUMENTS];
	struct drv_acpi_node *method;
	unsigned index;
	uint64_t enabled;
	int error;

	/* The enable as the integer both methods take. */
	enabled = 0;
	if (enable)
		enabled = 1;

	/* A device without _DSW has the older _PSW, which takes the enable alone. */
	error = drv_acpi_lookup(device, "_DSW", &method);
	if (error != 0) {
		/* Makes the argument. */
		arguments[0] = drv_acpi_object_integer_new(enabled);
		if (arguments[0] == NULL)
			return ENOMEM;

		/* Runs _PSW when the device has one, and drops the argument. */
		error = evaluate_optional(device, "_PSW", arguments, 1U);
		drv_acpi_object_release(arguments[0]);
		if (error != 0)
			return error;

		/* Succeeded: the device was told through _PSW, or has no method to tell. */
		return 0;
	}

	/* _DSW: the enable, the system state S0, and the D-state the device will be in. */
	arguments[0] = drv_acpi_object_integer_new(enabled);
	arguments[1] = drv_acpi_object_integer_new(SYSTEM_STATE_S0);
	arguments[2] = drv_acpi_object_integer_new((uint64_t)state);

	/* Finds whether every argument was made. */
	error = 0;
	for (index = 0; index < DSW_ARGUMENTS; index++) {
		/* One argument missing is enough to refuse. */
		if (arguments[index] == NULL)
			error = ENOMEM;
	}

	/* Runs _DSW when every argument was made. */
	if (error == 0)
		error = evaluate_optional(device, "_DSW", arguments, DSW_ARGUMENTS);

	/* The arguments are no longer needed. */
	for (index = 0; index < DSW_ARGUMENTS; index++)
		drv_acpi_object_release(arguments[index]);

	/* Reports why the device could not be told. */
	if (error != 0)
		return error;

	/* Succeeded: the device was told. */
	return 0;
}

/*
 * Reads a device's _PRW: the GPE that signals its wake, and the power
 * resources the wake needs.  A GPE given as a package (a GPE block device
 * and a number in it) is refused with ENOTSUP.
 */
static int
prw_read(
	struct drv_acpi_node *device,
	unsigned *gpe,
	struct resource_list *list)
{
	struct drv_acpi_object *package;
	struct drv_acpi_object *first;
	enum drv_acpi_type type;
	uint64_t number;
	int error;

	/* Evaluates _PRW. */
	package = NULL;
	error = drv_acpi_evaluate(device, "_PRW", NULL, 0, &package);
	if (error != 0)
		return error;

	/* Refuses anything but a package. */
	type = drv_acpi_object_type(package);
	if (type != DRV_ACPI_TYPE_PACKAGE) {
		drv_acpi_object_release(package);
		return EINVAL;
	}

	/* Refuses a GPE in a GPE block device, which the event code does not handle. */
	first = drv_acpi_object_package_element(package, PRW_GPE);
	type = drv_acpi_object_type(first);
	if (type != DRV_ACPI_TYPE_INTEGER) {
		drv_acpi_object_release(package);
		return ENOTSUP;
	}

	/* Refuses a GPE number that no GPE block can carry. */
	number = drv_acpi_object_integer(first);
	if (number >= DRV_ACPI_GPE_NONE) {
		drv_acpi_object_release(package);
		return EINVAL;
	}

	/* Collects the power resources after the GPE and the sleep state. */
	error = resources_from_package(package, PRW_RESOURCES, list);
	drv_acpi_object_release(package);
	if (error != 0)
		return error;

	/* Succeeded: reports the GPE; list names the wake's resources. */
	*gpe = (unsigned)number;
	return 0;
}

/* Logs a failed change of a device with its path. */
static void
log_device(
	const char *what,
	struct drv_acpi_node *node,
	int error)
{
	char path[64];
	int path_error;

	/* Names the device; a path that does not fit is left empty. */
	path_error = drv_acpi_node_path(node, path, sizeof(path));
	if (path_error != 0)
		path[0] = '\0';

	/* Writes the line. */
	drv_acpi_os_log("ACPI: %s of %s failed (error %d)\n", what, path, error);
}
