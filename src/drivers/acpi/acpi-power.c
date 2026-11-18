/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI power devices (ws132-p002): the lid (PNP0C0D), the AC adapter
 * (ACPI0003), the batteries (PNP0C0A) and the control-method power and
 * sleep buttons (PNP0C0C, PNP0C0E), posted as the system's events
 * (kern/system-event.h) and kept as the power's state for
 * KERN_SYSTEM_GET_POWER.
 *
 * Firmware tells of a change with Notify on the device.  A notify handler
 * runs inside the interpreter and may not evaluate AML, so it only marks
 * the device and wakes this file's thread, which evaluates _LID, _PSR or
 * _BST and _BIX (_BIF) and posts what changed.  A button's notify is a
 * press, posted at once.  The fixed power and sleep buttons of the PM1
 * registers are posted by acpi-kern.c.
 */

#include <drivers/acpi/acpi.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/sleep.h>
#include <kern/system-event.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

#include <stdbool.h>

/* The most power devices taken. */
#define POWER_DEVICES_MAX	8U

/* The identifiers, as EISA numbers (PNP0Cxx) and as a string (ACPI0003). */
#define EISA_LID		0x0d0cd041U
#define EISA_BATTERY		0x0a0cd041U
#define EISA_POWER_BUTTON	0x0c0cd041U
#define EISA_SLEEP_BUTTON	0x0e0cd041U
#define STRING_AC		"ACPI0003"

/* _STA: present, and a battery present. */
#define STATUS_PRESENT		0x01U
#define STATUS_BATTERY		0x10U

/* The Notify values of a change of state, of a battery's information, and of a button pressed. */
#define NOTIFY_STATUS		0x80U
#define NOTIFY_INFORMATION	0x81U

/*
 * How often the thread reads the batteries without a notify: much firmware
 * notifies only on plugging and on the low levels, and the charge shown
 * would otherwise stand still.
 */
#define BATTERY_PERIOD_MS	60000U

/*
 * The note of what woke the system from S0 idle (kern/sleep.c, ws052-p006).
 * It is weak so that the host test, which builds this file alone, links
 * without the kernel; power_note_wake() tests it first.
 */
extern void kern_sleep_note_wake(unsigned reason) __attribute__((weak));

/* The largest GPE number ACPI has (the blocks hold at most 256 events). */
#define GPE_NUMBER_MAX		0xffU

/* _BST: charging, and a value the battery does not know. */
#define BATTERY_CHARGING	0x02U
#define BATTERY_UNKNOWN		0xffffffffU

/* The kinds of device. */
enum power_kind {
	POWER_NONE,
	POWER_LID,
	POWER_AC,
	POWER_BATTERY,
	POWER_BUTTON_POWER,
	POWER_BUTTON_SLEEP
};

/*
 * One device: its node, its kind, its subject in the events, whether a
 * notify waits for the thread, and its last value (the lid 1 open, the
 * AC 1 plugged, a battery's percent, -1 unknown) and a battery's charging.
 * The table is filled at attach and lives as long as the kernel.
 */
struct power_device {
	struct drv_acpi_node *node;
	enum power_kind kind;
	char subject[16];
	bool pending;
	int32_t value;
	bool charging;
};

/*
 * The devices and their number, filled once by drv_acpi_power_attach()
 * before the thread starts; power_lock (with interrupts off) protects the
 * pending flags and the values afterwards, and power_waitq is where the
 * thread waits for a notify.
 */
static struct power_device power_devices[POWER_DEVICES_MAX];
static unsigned power_count;
static struct spinlock power_lock;
static struct wait_queue power_waitq;
static bool power_ready;

static int find_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static enum power_kind kind_of(struct drv_acpi_node *node);
static void notify_handler(struct drv_acpi_node *node, uint32_t value, void *argument);
static void power_note_wake(unsigned reason);
static void power_thread(void *argument);
static void enable_events(struct power_device *device);
static void refresh(struct power_device *device, bool post);
static int32_t read_lid(struct power_device *device);
static int32_t read_ac(struct power_device *device);
static int32_t read_battery(struct power_device *device, bool *charging);
static int package_integer(const struct drv_acpi_object *package, unsigned index, uint64_t *value);

/*
 * Finds the lid, the AC adapter, the batteries and the control-method
 * buttons, reads their state, and starts the thread that follows their
 * notifies.  Called once the namespace is ready and the EC is attached.
 */
int
drv_acpi_power_attach(void)
{
	struct thread *thread;
	unsigned index;
	int error;

	/* The lock and the queue, before any notify. */
	spin_init(&power_lock, LOCK_RANK_DEVICE, "acpi power");
	waitq_init(&power_waitq, "acpi power");

	/* The devices. */
	(void)drv_acpi_walk(NULL, find_visitor, NULL);
	if (power_count == 0U)
		return ENODEV;

	/* Their state now, and their notifies from now on. */
	for (index = 0; index < power_count; index++) {
		refresh(&power_devices[index], false);
		error = drv_acpi_notify_install(power_devices[index].node, notify_handler, &power_devices[index]);
		if (error != 0)
			kern_logf("acpi: %s notify not taken (%d)\n", power_devices[index].subject, error);

		/* The GPE a lid's or a button's notifies come through raises SCIs while the system runs. */
		enable_events(&power_devices[index]);
	}

	/* Reads of the state may start. */
	power_ready = true;

	/* The thread that evaluates the changes. */
	error = kthread_create(power_thread, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;
	thread_start(thread);

	/* Succeeded: the log lists what was found. */
	for (index = 0; index < power_count; index++)
		kern_logf("acpi: power device %s value %d\n", power_devices[index].subject, power_devices[index].value);
	return 0;
}

/*
 * Copies the power's present state for KERN_SYSTEM_GET_POWER: the lid, the
 * AC adapter and the first battery, when there are such devices.
 */
void
drv_acpi_power_get(
	struct system_power_info *info)
{
	const struct power_device *device;
	unsigned long irq;
	unsigned index;
	bool battery_seen;

	/* Nothing is known before the devices are read. */
	if (!power_ready)
		return;

	/* The values the thread keeps. */
	irq = spin_lock_irqsave(&power_lock);

	battery_seen = false;
	for (index = 0; index < power_count; index++) {
		device = &power_devices[index];

		/* An unknown value says nothing. */
		if (device->value < 0)
			continue;

		/* Each kind into its fields. */
		if (device->kind == POWER_LID) {
			info->known |= KERN_SYSTEM_POWER_HAS_LID;
			info->lid_open = (uint32_t)device->value;
		} else if (device->kind == POWER_AC) {
			info->known |= KERN_SYSTEM_POWER_HAS_AC;
			info->ac_online = (uint32_t)device->value;
		} else if (device->kind == POWER_BATTERY && !battery_seen) {
			info->known |= KERN_SYSTEM_POWER_HAS_BATTERY;
			info->battery_percent = (uint32_t)device->value;
			info->battery_charging = 0;
			if (device->charging)
				info->battery_charging = 1;
			battery_seen = true;
		}
	}

	spin_unlock_irqrestore(&power_lock, irq);
}

/* Takes each present power device of the namespace. */
static int
find_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct power_device *device;
	enum drv_acpi_type type;
	enum power_kind kind;
	uint64_t status;
	unsigned same_kind;
	unsigned index;
	int error;

	/* The depth and the argument do not matter. */
	(void)depth;
	(void)argument;

	/* Only a device of a known kind. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;
	kind = kind_of(node);
	if (kind == POWER_NONE)
		return 0;

	/* A device that is not present is passed over (no _STA is present). */
	error = drv_acpi_evaluate_integer(node, "_STA", &status);
	if (error == 0 && (status & STATUS_PRESENT) == 0U)
		return 0;

	/* A full table takes no more. */
	if (power_count >= POWER_DEVICES_MAX)
		return -1;

	/* How many of its kind came before it, for a battery's number. */
	same_kind = 0;
	for (index = 0; index < power_count; index++) {
		if (power_devices[index].kind == kind)
			same_kind++;
	}

	/* The device, its value unknown until it is read. */
	device = &power_devices[power_count];
	device->node = node;
	device->kind = kind;
	device->value = -1;
	switch (kind) {
	case POWER_LID:
		(void)kern_snprintf(device->subject, sizeof(device->subject), "lid");
		break;
	case POWER_AC:
		(void)kern_snprintf(device->subject, sizeof(device->subject), "ac");
		break;
	case POWER_BATTERY:
		(void)kern_snprintf(device->subject, sizeof(device->subject), "battery%u", same_kind);
		break;
	case POWER_BUTTON_POWER:
		(void)kern_snprintf(device->subject, sizeof(device->subject), "power-button");
		break;
	default:
		(void)kern_snprintf(device->subject, sizeof(device->subject), "sleep-button");
		break;
	}

	/* The table holds it. */
	power_count++;

	/* Goes on with the next device. */
	return 0;
}

/* Tells which kind of power device a device is, by its _HID. */
static enum power_kind
kind_of(
	struct drv_acpi_node *node)
{
	struct drv_acpi_object *hid;
	enum drv_acpi_type type;
	enum power_kind kind;
	const char *text;
	uint64_t value;
	size_t length;
	int compared;
	int error;

	/* The hardware identifier. */
	hid = NULL;
	error = drv_acpi_evaluate(node, "_HID", NULL, 0U, &hid);
	if (error != 0 || hid == NULL)
		return POWER_NONE;

	/* An EISA identifier names the PNP0Cxx devices. */
	kind = POWER_NONE;
	type = drv_acpi_object_type(hid);
	if (type == DRV_ACPI_TYPE_INTEGER) {
		value = drv_acpi_object_integer(hid);
		if (value == EISA_LID) {
			kind = POWER_LID;
		} else if (value == EISA_BATTERY) {
			kind = POWER_BATTERY;
		} else if (value == EISA_POWER_BUTTON) {
			kind = POWER_BUTTON_POWER;
		} else if (value == EISA_SLEEP_BUTTON) {
			kind = POWER_BUTTON_SLEEP;
		}
	} else if (type == DRV_ACPI_TYPE_STRING) {
		/* The AC adapter's is a string. */
		text = drv_acpi_object_string(hid, &length);
		if (text != NULL) {
			compared = kern_strcmp(text, STRING_AC);
			if (compared == 0)
				kind = POWER_AC;
		}
	}

	/* The identifier is no longer needed. */
	drv_acpi_object_release(hid);

	/* Succeeded: the kind, or none. */
	return kind;
}

/*
 * Takes a Notify on a power device, inside the interpreter: a button's is
 * a press, posted at once; any other marks the device for the thread.
 */
static void
notify_handler(
	struct drv_acpi_node *node,
	uint32_t value,
	void *argument)
{
	struct power_device *device;
	unsigned long irq;

	/* The device the handler was installed for. */
	(void)node;
	device = argument;

	/* A button pressed (which also wakes the system from S0 idle). */
	if (device->kind == POWER_BUTTON_POWER || device->kind == POWER_BUTTON_SLEEP) {
		if (value == NOTIFY_STATUS) {
			power_note_wake(KERN_SLEEP_WAKE_POWER_BUTTON);
			kern_system_event_post(KERN_SYSTEM_EVENT_POWER, KERN_SYSTEM_EVENT_PRESS, 1, device->subject, "");
		}

		/* Nothing to read for a button. */
		return;
	}

	/* Only a change of state or information needs reading. */
	if (value != NOTIFY_STATUS && value != NOTIFY_INFORMATION)
		return;

	/* A lid or an AC adapter that changed wakes the system from S0 idle; a battery's news does not. */
	if (device->kind == POWER_LID)
		power_note_wake(KERN_SLEEP_WAKE_LID);
	if (device->kind == POWER_AC)
		power_note_wake(KERN_SLEEP_WAKE_AC);
	if (device->kind == POWER_BATTERY)
		power_note_wake(KERN_SLEEP_NOTE_BATTERY);

	/* The thread reads the device. */
	irq = spin_lock_irqsave(&power_lock);

	device->pending = true;
	waitq_wake_all(&power_waitq);

	spin_unlock_irqrestore(&power_lock, irq);
}

/*
 * Enables at runtime the GPE a lid's or a button's _PRW names.
 *
 * Firmware may notify the lid and the buttons from the method of the GPE
 * their _PRW names, which is kept masked at runtime as a wake-only event
 * until a driver asks for it; without it a lid's closing never comes
 * (BUG-253, the Latitude 5330's GPE 0x18).  A _PRW whose GPE is in a GPE
 * block device (a package, not an integer) is left as it is.
 */
static void
enable_events(
	struct power_device *device)
{
	struct drv_acpi_object *wake;
	uint64_t gpe;
	int error;

	/* Only a lid and the buttons, whose events the operating system is to take while it runs. */
	if (device->kind != POWER_LID &&
	    device->kind != POWER_BUTTON_POWER &&
	    device->kind != POWER_BUTTON_SLEEP)
		return;

	/* The device's wake event: a package whose first element is the GPE (none: nothing to enable). */
	wake = NULL;
	error = drv_acpi_evaluate(device->node, "_PRW", NULL, 0U, &wake);
	if (error != 0 || wake == NULL)
		return;

	/* The GPE number, when it is an integer; the package is not needed after it. */
	error = package_integer(wake, 0U, &gpe);
	drv_acpi_object_release(wake);
	if (error != 0)
		return;

	/* A number past the last GPE ACPI has is not one. */
	if (gpe > GPE_NUMBER_MAX)
		return;

	/* The GPE at runtime; the log shows which GPE carries the device's events. */
	error = drv_acpi_gpe_runtime_enable((unsigned)gpe);
	if (error != 0) {
		kern_logf("acpi: %s GPE 0x%x not enabled (%d)\n", device->subject, (unsigned)gpe, error);
		return;
	}

	/* Succeeded: the device's notifies come while the system runs. */
	kern_logf("acpi: %s events on GPE 0x%x\n", device->subject, (unsigned)gpe);
}

/* Notes for a sleep under way what woke the system, when the kernel has the sleep's coordinator. */
static void
power_note_wake(
	unsigned reason)
{
	/* The host test has no coordinator. */
	if (kern_sleep_note_wake == NULL)
		return;

	/* The note. */
	kern_sleep_note_wake(reason);
}

/*
 * Reads the devices whose notifies came, and the batteries every
 * BATTERY_PERIOD_MS, for as long as the kernel runs.
 */
static void
power_thread(
	void *argument)
{
	unsigned long irq;
	uint64_t observed;
	uint64_t deadline;
	uint64_t now;
	unsigned index;
	bool any;
	int error;

	/* No argument. */
	(void)argument;

	/* For as long as the kernel runs. */
	deadline = sched_ticks() + kern_ms_to_ticks(BATTERY_PERIOD_MS);
	for (;;) {
		/* Waits until a notify marks a device, or the batteries' time comes. */
		irq = spin_lock_irqsave(&power_lock);

		for (;;) {
			/* A marked device. */
			any = false;
			for (index = 0; index < power_count; index++) {
				if (power_devices[index].pending)
					any = true;
			}

			/* Stops looking when one is marked. */
			if (any)
				break;

			/* The batteries' time: each of them is marked. */
			now = sched_ticks();
			if (now >= deadline) {
				for (index = 0; index < power_count; index++) {
					if (power_devices[index].kind == POWER_BATTERY)
						power_devices[index].pending = true;
				}

				/* The next time comes a period later. */
				deadline = now + kern_ms_to_ticks(BATTERY_PERIOD_MS);
				continue;
			}

			/* Sleeps until a notify or the time. */
			observed = waitq_sequence(&power_waitq);
			error = waitq_sleep(&power_waitq, &power_lock, observed, deadline, 0U);
			(void)error;
		}

		spin_unlock_irqrestore(&power_lock, irq);

		/* Reads each marked device and posts what changed. */
		for (index = 0; index < power_count; index++) {
			/* Takes its mark. */
			irq = spin_lock_irqsave(&power_lock);

			any = power_devices[index].pending;
			power_devices[index].pending = false;

			spin_unlock_irqrestore(&power_lock, irq);

			/* A marked device is read. */
			if (any)
				refresh(&power_devices[index], true);
		}
	}
}

/*
 * Reads a device's state and keeps it; with post, posts it when it
 * changed.
 */
static void
refresh(
	struct power_device *device,
	bool post)
{
	unsigned long irq;
	const char *outcome;
	uint32_t class_bit;
	int32_t value;
	bool charging;
	bool changed;

	/* The state by the device's kind. */
	charging = false;
	class_bit = 0;
	value = -1;
	switch (device->kind) {
	case POWER_LID:
		value = read_lid(device);
		class_bit = KERN_SYSTEM_EVENT_LID;
		break;
	case POWER_AC:
		value = read_ac(device);
		class_bit = KERN_SYSTEM_EVENT_AC;
		break;
	case POWER_BATTERY:
		value = read_battery(device, &charging);
		class_bit = KERN_SYSTEM_EVENT_BATTERY;
		break;
	default:
		/* A button has no state. */
		return;
	}

	/* Keeps it. */
	irq = spin_lock_irqsave(&power_lock);

	changed = false;
	if (device->value != value || device->charging != charging)
		changed = true;
	device->value = value;
	device->charging = charging;

	spin_unlock_irqrestore(&power_lock, irq);

	/* Posts a change. */
	if (!post)
		return;

	/*
	 * Every notify of the lid is logged with what _LID answered and when,
	 * changed or not: an opening the firmware reports while the lid is
	 * closed (BUG-255) is told from a real one by its time.
	 */
	if (device->kind == POWER_LID) {
		outcome = "unchanged";
		if (changed)
			outcome = "posted";
		kern_logf("acpi: lid notify: _LID %d (%s) at %llu ms\n",
		    value,
		    outcome,
		    (unsigned long long)kern_ticks_to_ms(sched_ticks()));
	}

	/* Only a change is posted. */
	if (!changed)
		return;
	if (charging) {
		kern_system_event_post(class_bit, KERN_SYSTEM_EVENT_CHANGE, value, device->subject, "charging=1");
	} else {
		kern_system_event_post(class_bit, KERN_SYSTEM_EVENT_CHANGE, value, device->subject, "charging=0");
	}
}

/* Reads the lid: 1 open, 0 closed, -1 unknown. */
static int32_t
read_lid(
	struct power_device *device)
{
	uint64_t value;
	int error;

	/* _LID is nonzero when the lid is open. */
	error = drv_acpi_evaluate_integer(device->node, "_LID", &value);
	if (error != 0)
		return -1;
	if (value != 0U)
		return 1;

	/* Succeeded: the lid is closed. */
	return 0;
}

/* Reads the AC adapter: 1 plugged, 0 not, -1 unknown. */
static int32_t
read_ac(
	struct power_device *device)
{
	uint64_t value;
	int error;

	/* _PSR is 1 on line power. */
	error = drv_acpi_evaluate_integer(device->node, "_PSR", &value);
	if (error != 0)
		return -1;
	if (value != 0U)
		return 1;

	/* Succeeded: the adapter is not plugged. */
	return 0;
}

/*
 * Reads a battery's charge in percent of its last full charge (-1 when it
 * is absent or does not say), and whether it is charging.
 */
static int32_t
read_battery(
	struct power_device *device,
	bool *charging)
{
	struct drv_acpi_object *status;
	struct drv_acpi_object *information;
	uint64_t present;
	uint64_t state;
	uint64_t remaining;
	uint64_t full;
	unsigned full_index;
	int error;

	/* An absent battery says nothing. */
	*charging = false;
	error = drv_acpi_evaluate_integer(device->node, "_STA", &present);
	if (error == 0 && (present & STATUS_BATTERY) == 0U)
		return -1;

	/* The last full charge: _BIX's fourth field, or _BIF's third. */
	information = NULL;
	full_index = 3U;
	error = drv_acpi_evaluate(device->node, "_BIX", NULL, 0U, &information);
	if (error != 0) {
		full_index = 2U;
		error = drv_acpi_evaluate(device->node, "_BIF", NULL, 0U, &information);
	}

	/* Neither method answered. */
	if (error != 0)
		return -1;
	error = package_integer(information, full_index, &full);
	drv_acpi_object_release(information);
	if (error != 0 || full == 0U || full == BATTERY_UNKNOWN)
		return -1;

	/* The state and the remaining capacity: _BST's first and third fields. */
	status = NULL;
	error = drv_acpi_evaluate(device->node, "_BST", NULL, 0U, &status);
	if (error != 0)
		return -1;
	error = package_integer(status, 0U, &state);
	if (error == 0)
		error = package_integer(status, 2U, &remaining);
	drv_acpi_object_release(status);
	if (error != 0 || remaining == BATTERY_UNKNOWN)
		return -1;

	/* Charging, and the percent (a battery may report a little over its last full charge). */
	if ((state & BATTERY_CHARGING) != 0U)
		*charging = true;
	if (remaining > full)
		remaining = full;

	/* Succeeded: the percent. */
	return (int32_t)(remaining * 100U / full);
}

/* Reads one integer element of a package. */
static int
package_integer(
	const struct drv_acpi_object *package,
	unsigned index,
	uint64_t *value)
{
	struct drv_acpi_object *element;
	enum drv_acpi_type type;

	/* A package, long enough. */
	if (package == NULL)
		return EINVAL;
	type = drv_acpi_object_type(package);
	if (type != DRV_ACPI_TYPE_PACKAGE)
		return EINVAL;
	element = drv_acpi_object_package_element(package, index);
	if (element == NULL)
		return EINVAL;

	/* An integer element. */
	type = drv_acpi_object_type(element);
	if (type != DRV_ACPI_TYPE_INTEGER)
		return EINVAL;

	/* Succeeded: its value. */
	*value = drv_acpi_object_integer(element);
	return 0;
}
