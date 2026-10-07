/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI transport of the UCSI driver (ws050-p003).
 *
 * A PC describes its UCSI interface as an ACPI device (_HID USBC000, _CID
 * PNP0CA0) whose _CRS is the mailbox in memory and whose _DSM moves the
 * mailbox to and from the platform's PPM (the Embedded Controller on the
 * Latitude 5330): function 1 sends CONTROL and MESSAGE OUT, function 2
 * fetches CCI and MESSAGE IN.  The platform tells of an answer or a change
 * with Notify (device, 0x80), after copying CCI and MESSAGE IN to the
 * mailbox itself.
 *
 * Firmware's own mailbox accesses (the _DSM functions and the EC query
 * that notifies) run under the root mutex \ECMU on the 5330.  This file
 * takes the same mutex for each whole exchange (mailbox writes and _DSM,
 * or _DSM and mailbox reads), so the EC query cannot copy a new answer
 * between them; the _DSM inside takes the mutex again as the same
 * interpreter thread.  CCI is read before and after MESSAGE IN, and the
 * read is repeated when the two differ.
 *
 * The notification handler runs inside the interpreter: it only raises the
 * signal the driver's thread waits on (typec-os.h).  The thread runs every
 * command; it never holds the Type-C layer's lock while it evaluates AML.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>
#include <drivers/typec/typec.h>
#include <drivers/typec/ucsi-acpi.h>

#include "typec-os.h"
#include "ucsi.h"

/*
 * The identifiers of a UCSI device, as the EISA numbers _HID and _CID
 * encode (USBC000, PNP0CA0) and as strings.
 */
#define UCSI_ACPI_EISA_HID 0x00c06256U
#define UCSI_ACPI_EISA_CID 0xa00cd041U
#define UCSI_ACPI_STRING_HID "USBC000"
#define UCSI_ACPI_STRING_CID "PNP0CA0"

/* _STA: the device is present. */
#define UCSI_ACPI_STATUS_PRESENT 0x01U

/*
 * The UCSI _DSM: its revision, its functions (0 lists the others, 1
 * writes, 2 reads), and how many arguments a _DSM takes.  The UUID is
 * 6f8398c2-7ca4-11e4-ad36-631042b5008f.
 */
#define UCSI_ACPI_DSM_REVISION 1U
#define UCSI_ACPI_DSM_QUERY 0U
#define UCSI_ACPI_DSM_WRITE 1U
#define UCSI_ACPI_DSM_READ 2U
#define UCSI_ACPI_DSM_ARGUMENTS 4U
#define UCSI_ACPI_DSM_UUID_LENGTH 16U

/* The Notify value of a new answer or change in the mailbox. */
#define UCSI_ACPI_NOTIFY 0x80U

/* The mutex firmware holds over its own mailbox accesses (the 5330's). */
#define UCSI_ACPI_MUTEX "\\ECMU"

/*
 * The descriptor of a fixed 32-bit memory range in a resource template,
 * and what the resource visitor reports when it found the mailbox (a
 * negative value, which the walk tells apart from its own errors).
 */
#define UCSI_ACPI_MEMORY32_FIXED 0x86U
#define UCSI_ACPI_RESOURCE_FOUND (-1)

/* The bytes of CONTROL and of CCI. */
#define UCSI_ACPI_CONTROL_BYTES 8U
#define UCSI_ACPI_CCI_BYTES 4U

/* How many times a read looks for a CCI that held still across MESSAGE IN. */
#define UCSI_ACPI_READ_TRIES 3U

/* How long the thread waits for a notification before it waits again. */
#define UCSI_ACPI_IDLE_MS 60000U

/*
 * The UCSI device the driver attached to: where its mailbox is, the
 * arrangement and version the PPM reported, and the core's instance.
 *
 * Filled once by drv_ucsi_acpi_attach(); after that only the driver's
 * thread uses it, but for notified: the notification handler raises it
 * (atomically, inside the interpreter) and the thread lowers it, so the
 * thread tells a notification from a wake-up for an operation.
 */
struct ucsi_acpi {
	struct drv_acpi_node *device;
	uint64_t base;
	size_t length;
	volatile uint8_t *mailbox;
	const struct drv_ucsi_layout *layout;
	uint16_t version;
	bool firmware_mutex;
	int notified;
	struct drv_ucsi_transport transport;
	struct drv_ucsi ucsi;
};

/*
 * One exchange with the mailbox, as the work run under \ECMU sees it: a
 * write (CONTROL and MESSAGE OUT, then _DSM 1) or a read (_DSM 2 when
 * refresh asks for it, then CCI and MESSAGE IN).
 */
struct ucsi_acpi_exchange {
	struct ucsi_acpi *driver;
	uint64_t control;
	const uint8_t *message_out;
	size_t message_out_length;
	bool refresh;
	uint32_t *cci;
	uint8_t *message_in;
	size_t message_in_size;
};

/*
 * The connectors' locations the display mapping reads (ws050-p005): one
 * per device under the UCSI device, in the namespace's order.
 */
struct ucsi_acpi_locations {
	struct drv_typec_location connector[DRV_TYPEC_CONNECTOR_MAX];
	unsigned count;
};

/*
 * The USB 3 ports of the Type-C subsystem's xHCI that share the display's
 * Type-C ports TC1 to TC4 (Intel's TCSS: port n of the subsystem is the
 * display's TCn, on the same lanes), whose _PLD names the USB-C connector
 * they sit on.
 */
static const char *const ucsi_acpi_display_ports[DRV_TYPEC_DISPLAY_PORT_MAX] = {
	"\\_SB.PC00.TXHC.RHUB.SS01",
	"\\_SB.PC00.TXHC.RHUB.SS02",
	"\\_SB.PC00.TXHC.RHUB.SS03",
	"\\_SB.PC00.TXHC.RHUB.SS04"
};

/*
 * The attached device.
 *
 * The kernel has at most one UCSI interface; device is NULL until
 * drv_ucsi_acpi_attach() found one, and the record lives as long as the
 * kernel.
 */
static struct ucsi_acpi ucsi_acpi;

/*
 * The UUID of the UCSI _DSM, as the bytes of its ACPI buffer (ToUUID's
 * order: the first three fields little-endian, the rest as written).
 */
static const uint8_t ucsi_acpi_uuid[UCSI_ACPI_DSM_UUID_LENGTH] = {
	0xc2, 0x98, 0x83, 0x6f, 0xa4, 0x7c, 0xe4, 0x11,
	0xad, 0x36, 0x63, 0x10, 0x42, 0xb5, 0x00, 0x8f
};

static int ucsi_acpi_find(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool ucsi_acpi_identifies(struct drv_acpi_node *node, const char *method);
static bool ucsi_acpi_names(const struct drv_acpi_object *identifier);
static int ucsi_acpi_probe(struct ucsi_acpi *driver);
static int ucsi_acpi_resource(const struct drv_acpi_resource *resource, void *argument);
static int ucsi_acpi_functions(struct ucsi_acpi *driver);
static int ucsi_acpi_dsm(struct ucsi_acpi *driver, unsigned function, struct drv_acpi_object **result);
static void ucsi_acpi_notify(struct drv_acpi_node *node, uint32_t value, void *argument);
static void ucsi_acpi_thread(void *argument);
static void ucsi_acpi_kick(void *argument);
static int ucsi_acpi_write(void *context, uint64_t control, const uint8_t *message_out, size_t length);
static int ucsi_acpi_read(void *context, bool refresh, uint32_t *cci, uint8_t *message_in, size_t size);
static int ucsi_acpi_wait(void *context, uint32_t milliseconds);
static int ucsi_acpi_exchange(struct ucsi_acpi_exchange *exchange);
static int ucsi_acpi_write_work(void *argument);
static int ucsi_acpi_read_work(void *argument);
static uint32_t ucsi_acpi_cci(struct ucsi_acpi *driver);
static void ucsi_acpi_map_displays(struct ucsi_acpi *driver);
static int ucsi_acpi_connector_location(struct drv_acpi_node *node, unsigned depth, void *argument);
static int ucsi_acpi_location(struct drv_acpi_node *node, struct drv_typec_location *location);

/*
 * Finds the platform's UCSI device in the ACPI namespace, maps its mailbox,
 * makes sure its _DSM can write and read, and starts the driver's thread.
 *
 * Called once the namespace is ready and the Embedded Controller attached
 * (the 5330's _STA and _DSM read the EC).  Returns 0, ENODEV when there is
 * no usable device, or another errno value.
 */
int
drv_ucsi_acpi_attach(void)
{
	struct ucsi_acpi *driver;
	int error;

	/* The UCSI device; a platform without one has nothing to start. */
	driver = &ucsi_acpi;
	kern_memset(driver, 0, sizeof(*driver));
	(void)drv_acpi_walk(NULL, ucsi_acpi_find, driver);
	if (driver->device == NULL)
		return ENODEV;

	/* Its presence, its mailbox, its version and its _DSM functions; an unusable device is not kept. */
	error = ucsi_acpi_probe(driver);
	if (error != 0) {
		driver->device = NULL;
		return error;
	}

	/* Which display port drives which connector, where the firmware's locations tell it. */
	ucsi_acpi_map_displays(driver);

	/* The core's operations over this device. */
	driver->transport.write = ucsi_acpi_write;
	driver->transport.read = ucsi_acpi_read;
	driver->transport.wait = ucsi_acpi_wait;
	driver->transport.context = driver;

	/* The signal, before the notifications that raise it. */
	drv_typec_os_signal_init();
	error = drv_acpi_notify_install(driver->device, ucsi_acpi_notify, driver);
	if (error != 0) {
		drv_typec_os_log("ucsi: the notifications were not taken (error %d)\n", error);
		return error;
	}

	/* The operations other drivers ask wake the thread. */
	drv_typec_operator_set(ucsi_acpi_kick, driver);

	/* The diagnostic text; the driver works without it. */
	error = drv_typec_os_device_register();
	if (error != 0)
		drv_typec_os_log("ucsi: /dev/typec was not published (error %d)\n", error);

	/* The thread that runs every command. */
	error = drv_typec_os_thread_start(ucsi_acpi_thread, driver);
	if (error != 0) {
		drv_typec_os_log("ucsi: the thread did not start (error %d)\n", error);
		return error;
	}

	/* Succeeded: the thread reads the connectors from now on. */
	return 0;
}

/*
 * Starts the UCSI core over the attached device: resets the PPM, reads
 * every connector and turns the notifications on.
 *
 * Returns 0, ENODEV when no device was attached, or the core's errno value.
 */
int
drv_ucsi_acpi_start(void)
{
	struct ucsi_acpi *driver;
	int error;

	/* Refuses to start without a device. */
	driver = &ucsi_acpi;
	if (driver->device == NULL)
		return ENODEV;

	/* The core's start. */
	error = drv_ucsi_start(&driver->ucsi, &driver->transport, driver->layout, driver->version);
	if (error != 0) {
		drv_typec_os_log("ucsi: the PPM did not start (error %d)\n", error);
		return error;
	}

	/* Succeeded: the records are current. */
	drv_typec_os_log("ucsi: %u connectors\n", driver->ucsi.connector_count);
	return 0;
}

/*
 * Waits at most some milliseconds for a notification or an operation;
 * then reads and acknowledges the connector changes a notification tells
 * of, and carries out the operations other drivers asked, oldest first.
 *
 * Returns 1 when something was done, 0 when nothing came, or a negative
 * errno value when the PPM could not be read.
 */
int
drv_ucsi_acpi_step(
	uint32_t milliseconds)
{
	struct drv_typec_request request;
	struct ucsi_acpi *driver;
	bool requested;
	int notified;
	int done;
	int error;

	/* A notification or an operation, or the time. */
	driver = &ucsi_acpi;
	(void)drv_typec_os_wait(milliseconds);
	done = 0;

	/* The changes a notification tells of (a wake-up for an operation reads nothing). */
	notified = __atomic_exchange_n(&driver->notified, 0, __ATOMIC_ACQ_REL);
	if (notified != 0) {
		done = 1;
		error = drv_ucsi_service(&driver->ucsi);
		if (error != 0) {
			drv_typec_os_log("ucsi: a notification was not handled (error %d)\n", error);
			return -error;
		}
	}

	/* Each operation waiting. */
	for (;;) {
		requested = drv_typec_request_take(&request);
		if (!requested)
			break;

		/* Carried out; its outcome is in the connector's record. */
		done = 1;
		error = drv_ucsi_request(&driver->ucsi, &request);
		if (error != 0) {
			drv_typec_os_log("ucsi: an operation's connector was not read (error %d)\n", error);
			return -error;
		}
	}

	/* Succeeded: 1 when a notification or an operation was handled. */
	return done;
}

/* Takes the first present device the namespace names as a UCSI interface. */
static int
ucsi_acpi_find(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct ucsi_acpi *driver;
	enum drv_acpi_type type;
	bool identified;

	/* The depth does not matter. */
	(void)depth;
	driver = argument;

	/* Only a device. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* A UCSI device by its hardware identifier, else by a compatible one. */
	identified = ucsi_acpi_identifies(node, "_HID");
	if (!identified)
		identified = ucsi_acpi_identifies(node, "_CID");
	if (!identified)
		return 0;

	/* Succeeded: the device is found, and the walk stops. */
	driver->device = node;
	return -1;
}

/* Tells whether an identifier object of a device (_HID or _CID) names UCSI. */
static bool
ucsi_acpi_identifies(
	struct drv_acpi_node *node,
	const char *method)
{
	struct drv_acpi_object *identifier;
	struct drv_acpi_object *element;
	enum drv_acpi_type type;
	unsigned count;
	unsigned index;
	bool named;
	int error;

	/* The identifier; a device without it names nothing. */
	identifier = NULL;
	error = drv_acpi_evaluate(node, method, NULL, 0U, &identifier);
	if (error != 0 || identifier == NULL)
		return false;

	/* One identifier, or (for _CID) a package of them. */
	named = false;
	type = drv_acpi_object_type(identifier);
	if (type == DRV_ACPI_TYPE_PACKAGE) {
		count = drv_acpi_object_package_count(identifier);
		for (index = 0; index < count && !named; index++) {
			element = drv_acpi_object_package_element(identifier, index);
			named = ucsi_acpi_names(element);
		}
	} else {
		named = ucsi_acpi_names(identifier);
	}

	/* The identifier is no longer needed. */
	drv_acpi_object_release(identifier);

	/* Succeeded: whether it named UCSI. */
	return named;
}

/* Tells whether one identifier (an EISA number or a string) is UCSI's. */
static bool
ucsi_acpi_names(
	const struct drv_acpi_object *identifier)
{
	enum drv_acpi_type type;
	const char *text;
	uint64_t value;
	size_t length;
	int compared;

	/* A missing element names nothing. */
	if (identifier == NULL)
		return false;

	/* An EISA number. */
	type = drv_acpi_object_type(identifier);
	if (type == DRV_ACPI_TYPE_INTEGER) {
		value = drv_acpi_object_integer(identifier);
		if (value == UCSI_ACPI_EISA_HID)
			return true;
		if (value == UCSI_ACPI_EISA_CID)
			return true;
		return false;
	}

	/* Anything but a string names nothing. */
	if (type != DRV_ACPI_TYPE_STRING)
		return false;

	/* A string. */
	text = drv_acpi_object_string(identifier, &length);
	if (text == NULL)
		return false;
	compared = kern_strcmp(text, UCSI_ACPI_STRING_HID);
	if (compared == 0)
		return true;
	compared = kern_strcmp(text, UCSI_ACPI_STRING_CID);
	if (compared == 0)
		return true;

	/* Some other device's identifier. */
	return false;
}

/*
 * Reads the device's _STA (which, on the 5330, also puts VERSION in the
 * mailbox), maps the mailbox _CRS gives, picks the arrangement VERSION
 * names and makes sure _DSM has the write and read functions.
 */
static int
ucsi_acpi_probe(
	struct ucsi_acpi *driver)
{
	struct drv_acpi_node *mutex;
	char path[64];
	uint64_t status;
	size_t mapped;
	unsigned major;
	int error;

	/* The device's path, for the log. */
	error = drv_acpi_node_path(driver->device, path, sizeof(path));
	if (error != 0)
		(void)kern_snprintf(path, sizeof(path), "(device)");

	/* Present, or nothing to do; a device without _STA is present. */
	error = drv_acpi_evaluate_integer(driver->device, "_STA", &status);
	if (error == 0 && (status & UCSI_ACPI_STATUS_PRESENT) == 0U) {
		drv_typec_os_log("ucsi: %s is not present (_STA 0x%llx)\n", path, (unsigned long long)status);
		return ENODEV;
	}

	/* The mailbox's range. */
	error = drv_acpi_resources_walk(driver->device, "_CRS", ucsi_acpi_resource, driver);
	if (error == UCSI_ACPI_RESOURCE_FOUND)
		error = 0;
	if (error != 0 || driver->length == 0) {
		drv_typec_os_log("ucsi: %s has no mailbox in _CRS (error %d)\n", path, error);
		return ENODEV;
	}

	/* Refuses a range too small for the 1.x mailbox. */
	if (driver->length < drv_ucsi_layout_1.size) {
		drv_typec_os_log("ucsi: %s's mailbox of 0x%zx bytes is too small\n", path, driver->length);
		return ENODEV;
	}

	/*
	 * Maps as much of the range as the largest arrangement uses; a range
	 * in RAM (which the kernel would hand out) is refused, and the driver
	 * does not attach.
	 */
	mapped = driver->length;
	if (mapped > drv_ucsi_layout_2.size)
		mapped = drv_ucsi_layout_2.size;
	error = drv_typec_os_map(driver->base, mapped, &driver->mailbox);
	if (error != 0) {
		drv_typec_os_log("ucsi: %s's mailbox at 0x%llx was not mapped (error %d)\n", path, (unsigned long long)driver->base, error);
		return ENODEV;
	}

	/* VERSION (BCD: major, minor, sub-minor), which names the arrangement. */
	driver->version = (uint16_t)(drv_typec_os_read8(driver->mailbox) | ((unsigned)drv_typec_os_read8(driver->mailbox + 1) << 8));
	major = driver->version >> 8;
	driver->layout = &drv_ucsi_layout_1;
	if (major >= 2U)
		driver->layout = &drv_ucsi_layout_2;

	/* Refuses an arrangement the mapped range cannot hold. */
	if (mapped < driver->layout->size) {
		drv_typec_os_log("ucsi: %s's version 0x%04x needs 0x%zx bytes, the mailbox has 0x%zx\n", path, (unsigned)driver->version, driver->layout->size, mapped);
		return ENODEV;
	}

	/* Refuses a _DSM without the functions the transport needs. */
	error = ucsi_acpi_functions(driver);
	if (error != 0)
		return error;

	/* Whether firmware's mutex is there to hold over each exchange. */
	driver->firmware_mutex = false;
	error = drv_acpi_lookup(NULL, UCSI_ACPI_MUTEX, &mutex);
	if (error == 0) {
		driver->firmware_mutex = true;
		drv_typec_os_log("ucsi: the mailbox is exchanged under %s\n", UCSI_ACPI_MUTEX);
	}

	/* Succeeded: the device is usable. */
	drv_typec_os_log("ucsi: %s, mailbox 0x%llx (0x%zx bytes), version 0x%04x, the %s arrangement\n", path, (unsigned long long)driver->base, driver->length, (unsigned)driver->version, driver->layout->name);
	return 0;
}

/* Keeps the first fixed 32-bit memory range of _CRS as the mailbox. */
static int
ucsi_acpi_resource(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	struct ucsi_acpi *driver;

	/* Only a memory range. */
	driver = argument;
	if (resource->kind != DRV_ACPI_RESOURCE_MEMORY)
		return 0;

	/* A range other than the fixed 32-bit kind is not the mailbox. */
	if (resource->descriptor != UCSI_ACPI_MEMORY32_FIXED)
		return 0;

	/* The first one is the mailbox: the walk stops. */
	driver->base = resource->base;
	driver->length = (size_t)resource->length;
	return UCSI_ACPI_RESOURCE_FOUND;
}

/* Makes sure the _DSM function 0 lists the write and read functions. */
static int
ucsi_acpi_functions(
	struct ucsi_acpi *driver)
{
	struct drv_acpi_object *result;
	enum drv_acpi_type type;
	const uint8_t *bytes;
	uint64_t functions;
	size_t length;
	int error;

	/* The list of functions. */
	error = ucsi_acpi_dsm(driver, UCSI_ACPI_DSM_QUERY, &result);
	if (error != 0) {
		drv_typec_os_log("ucsi: _DSM function 0 failed (error %d)\n", error);
		return ENODEV;
	}

	/* A buffer whose first byte holds the bits of the first functions (or an integer, from some firmware). */
	functions = 0;
	type = DRV_ACPI_TYPE_UNINITIALIZED;
	if (result != NULL)
		type = drv_acpi_object_type(result);
	if (type == DRV_ACPI_TYPE_BUFFER) {
		bytes = drv_acpi_object_buffer(result, &length);
		if (bytes != NULL && length != 0)
			functions = bytes[0];
	} else if (type == DRV_ACPI_TYPE_INTEGER) {
		functions = drv_acpi_object_integer(result);
	}

	/* The answer is no longer needed. */
	if (result != NULL)
		drv_acpi_object_release(result);

	/*
	 * Refuses a _DSM without the write or the read function: a UUID given
	 * in the wrong byte order also lands here, as firmware then answers
	 * function 0 with no bits.
	 */
	if ((functions & (1U << UCSI_ACPI_DSM_WRITE)) == 0 || (functions & (1U << UCSI_ACPI_DSM_READ)) == 0) {
		drv_typec_os_log("ucsi: _DSM lists functions 0x%llx, without write and read\n", (unsigned long long)functions);
		return ENODEV;
	}

	/* Succeeded: the transport can write and read. */
	return 0;
}

/* Calls a function of the UCSI _DSM with an empty package as its argument. */
static int
ucsi_acpi_dsm(
	struct ucsi_acpi *driver,
	unsigned function,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *arguments[UCSI_ACPI_DSM_ARGUMENTS];
	unsigned index;
	int error;

	/* The UUID, the revision, the function, and an empty package. */
	arguments[0] = drv_acpi_object_buffer_new(ucsi_acpi_uuid, UCSI_ACPI_DSM_UUID_LENGTH);
	arguments[1] = drv_acpi_object_integer_new(UCSI_ACPI_DSM_REVISION);
	arguments[2] = drv_acpi_object_integer_new(function);
	arguments[3] = drv_acpi_object_package_new(0U);

	/* Refuses to call without every argument. */
	error = 0;
	for (index = 0; index < UCSI_ACPI_DSM_ARGUMENTS; index++) {
		/* One missing argument is enough. */
		if (arguments[index] == NULL)
			error = ENOMEM;
	}

	/* The call, when every argument was made. */
	*result = NULL;
	if (error == 0)
		error = drv_acpi_evaluate(driver->device, "_DSM", arguments, UCSI_ACPI_DSM_ARGUMENTS, result);

	/* The arguments are no longer needed. */
	for (index = 0; index < UCSI_ACPI_DSM_ARGUMENTS; index++)
		drv_acpi_object_release(arguments[index]);

	/* Reports why the function could not be called. */
	if (error != 0)
		return error;

	/* Succeeded: result holds the function's answer, or NULL. */
	return 0;
}

/*
 * Takes a Notify on the UCSI device, inside the interpreter: a new answer
 * or change is in the mailbox, and the thread is told.
 */
static void
ucsi_acpi_notify(
	struct drv_acpi_node *node,
	uint32_t value,
	void *argument)
{
	/* The device and the driver do not matter: there is one. */
	(void)node;
	(void)argument;

	/* Only the mailbox's notification. */
	if (value != UCSI_ACPI_NOTIFY)
		return;

	/* The thread reads the mailbox. */
	__atomic_store_n(&ucsi_acpi.notified, 1, __ATOMIC_RELEASE);
	drv_typec_os_signal();
}

/* Wakes the driver's thread for an operation another driver asked. */
static void
ucsi_acpi_kick(
	void *argument)
{
	/* The driver does not matter: there is one. */
	(void)argument;

	/* The thread takes the operation. */
	drv_typec_os_signal();
}

/* Starts the core, then reads the connectors each notification tells of, for as long as the kernel runs. */
static void
ucsi_acpi_thread(
	void *argument)
{
	uint32_t milliseconds;
	int error;

	/* No argument: the driver is the attached one. */
	(void)argument;

	/* The core's start; a PPM that does not start leaves the driver off. */
	error = drv_ucsi_acpi_start();
	if (error != 0)
		return;

	/*
	 * Each notification, waiting no longer than the Type-C layer's next
	 * comparison of the display driver's DisplayPort report with UCSI's.
	 */
	for (;;) {
		milliseconds = drv_typec_display_check();
		if (milliseconds == 0)
			milliseconds = UCSI_ACPI_IDLE_MS;
		(void)drv_ucsi_acpi_step(milliseconds);
	}
}

/* Writes CONTROL and MESSAGE OUT and tells the PPM (_DSM function 1). */
static int
ucsi_acpi_write(
	void *context,
	uint64_t control,
	const uint8_t *message_out,
	size_t length)
{
	struct ucsi_acpi_exchange exchange;
	int error;

	/* The exchange. */
	kern_memset(&exchange, 0, sizeof(exchange));
	exchange.driver = context;
	exchange.control = control;
	exchange.message_out = message_out;
	exchange.message_out_length = length;

	/* Runs it under firmware's mutex. */
	error = ucsi_acpi_exchange(&exchange);
	if (error != 0)
		return -error;

	/* Succeeded: the PPM has the command. */
	return 0;
}

/* Gives CCI and MESSAGE IN, fetched from the PPM first (_DSM function 2) when refresh asks. */
static int
ucsi_acpi_read(
	void *context,
	bool refresh,
	uint32_t *cci,
	uint8_t *message_in,
	size_t size)
{
	struct ucsi_acpi_exchange exchange;
	int error;

	/* The exchange. */
	kern_memset(&exchange, 0, sizeof(exchange));
	exchange.driver = context;
	exchange.refresh = refresh;
	exchange.cci = cci;
	exchange.message_in = message_in;
	exchange.message_in_size = size;

	/* Runs it under firmware's mutex. */
	error = ucsi_acpi_exchange(&exchange);
	if (error != 0)
		return -error;

	/* Succeeded: CCI and MESSAGE IN are the PPM's. */
	return 0;
}

/* Waits for the notification for at most some milliseconds. */
static int
ucsi_acpi_wait(
	void *context,
	uint32_t milliseconds)
{
	struct ucsi_acpi *driver;
	int notified;

	/* The signal, or the time. */
	driver = context;
	notified = drv_typec_os_wait(milliseconds);

	/*
	 * A command's wait takes the notifications that came with it: the
	 * core reads CCI next and keeps any connector change it shows, which
	 * it handles before its operation ends.
	 */
	__atomic_store_n(&driver->notified, 0, __ATOMIC_RELEASE);

	/* Succeeded: 1 for a notification, 0 for none. */
	return notified;
}

/* Runs a write or a read under \ECMU when firmware has it, else as it is. */
static int
ucsi_acpi_exchange(
	struct ucsi_acpi_exchange *exchange)
{
	int (*work)(void *argument);
	int error;

	/* The work of a write or of a read. */
	work = ucsi_acpi_read_work;
	if (exchange->cci == NULL)
		work = ucsi_acpi_write_work;

	/* Under firmware's mutex, or without it on a platform that has none. */
	if (exchange->driver->firmware_mutex) {
		error = drv_acpi_run_locked(UCSI_ACPI_MUTEX, work, exchange);
	} else {
		error = work(exchange);
	}

	/* Reports a failed exchange. */
	if (error != 0)
		return error;

	/* Succeeded: the exchange ran whole. */
	return 0;
}

/* Puts MESSAGE OUT and CONTROL in the mailbox and calls _DSM function 1. */
static int
ucsi_acpi_write_work(
	void *argument)
{
	struct ucsi_acpi_exchange *exchange;
	struct drv_acpi_object *result;
	const struct drv_ucsi_layout *layout;
	struct ucsi_acpi *driver;
	size_t length;
	size_t index;
	int error;

	/* The exchange and its device. */
	exchange = argument;
	driver = exchange->driver;
	layout = driver->layout;

	/* MESSAGE OUT first, no longer than the mailbox holds. */
	length = exchange->message_out_length;
	if (length > layout->message_out_size)
		length = layout->message_out_size;
	for (index = 0; index < length; index++)
		drv_typec_os_write8(driver->mailbox + layout->message_out_offset + index, exchange->message_out[index]);

	/* CONTROL, little-endian, which the PPM reads as the command. */
	for (index = 0; index < UCSI_ACPI_CONTROL_BYTES; index++)
		drv_typec_os_write8(driver->mailbox + layout->control_offset + index, (uint8_t)(exchange->control >> (index * 8U)));

	/* Tells the PPM. */
	error = ucsi_acpi_dsm(driver, UCSI_ACPI_DSM_WRITE, &result);
	if (result != NULL)
		drv_acpi_object_release(result);
	if (error != 0)
		return error;

	/* Succeeded: the PPM has the command. */
	return 0;
}

/*
 * Calls _DSM function 2 when the read is a refresh, then reads CCI,
 * MESSAGE IN and CCI again, until the two CCIs agree.
 */
static int
ucsi_acpi_read_work(
	void *argument)
{
	struct ucsi_acpi_exchange *exchange;
	struct drv_acpi_object *result;
	const struct drv_ucsi_layout *layout;
	struct ucsi_acpi *driver;
	uint32_t before;
	uint32_t after;
	unsigned tries;
	size_t length;
	size_t index;
	int error;

	/* The exchange and its device. */
	exchange = argument;
	driver = exchange->driver;
	layout = driver->layout;

	/* Fetches CCI and MESSAGE IN from the PPM when nothing put them in the mailbox. */
	if (exchange->refresh) {
		error = ucsi_acpi_dsm(driver, UCSI_ACPI_DSM_READ, &result);
		if (result != NULL)
			drv_acpi_object_release(result);
		if (error != 0)
			return error;
	}

	/* No more MESSAGE IN than the mailbox holds. */
	length = exchange->message_in_size;
	if (length > layout->message_in_size)
		length = layout->message_in_size;

	/*
	 * CCI, MESSAGE IN and CCI again: two CCIs that differ mean the
	 * platform changed the mailbox meanwhile, and the read is repeated.
	 */
	after = 0;
	for (tries = 0; tries < UCSI_ACPI_READ_TRIES; tries++) {
		before = ucsi_acpi_cci(driver);
		for (index = 0; index < length; index++)
			exchange->message_in[index] = drv_typec_os_read8(driver->mailbox + layout->message_in_offset + index);
		after = ucsi_acpi_cci(driver);

		/* The mailbox held still. */
		if (before == after)
			break;
	}

	/* Logs a mailbox that never held still; the last read is taken. */
	if (tries == UCSI_ACPI_READ_TRIES)
		drv_typec_os_log("ucsi: CCI kept changing while it was read (0x%08x)\n", (unsigned)after);

	/* Succeeded: CCI goes with MESSAGE IN. */
	*exchange->cci = after;
	return 0;
}

/* Reads CCI from the mailbox (little-endian). */
static uint32_t
ucsi_acpi_cci(
	struct ucsi_acpi *driver)
{
	uint32_t cci;
	size_t index;

	/* Its four bytes, the lowest first. */
	cci = 0;
	for (index = 0; index < UCSI_ACPI_CCI_BYTES; index++)
		cci |= (uint32_t)drv_typec_os_read8(driver->mailbox + driver->layout->cci_offset + index) << (index * 8U);

	/* Succeeded: the CCI the mailbox holds. */
	return cci;
}

/*
 * Binds each of the display's Type-C ports to the USB-C connector it is
 * wired to (ws050-p005), where the firmware tells it: the connectors are
 * the devices under the UCSI device in the namespace's order (connector n
 * is the n-th, as Linux's ucsi_find_fwnode() takes them), and a display
 * port is the connector whose _PLD has the group token and position of
 * the _PLD of the subsystem's USB 3 port on the same lanes.  A port
 * without a location, or without exactly one connector at it, stays
 * unbound: its reports reach no connector.  Each port is logged.
 */
static void
ucsi_acpi_map_displays(
	struct ucsi_acpi *driver)
{
	struct ucsi_acpi_locations locations;
	struct drv_typec_location port;
	struct drv_acpi_node *node;
	unsigned connector;
	unsigned index;
	int error;

	/* The connectors' locations, in order. */
	kern_memset(&locations, 0, sizeof(locations));
	(void)drv_acpi_walk(driver->device, ucsi_acpi_connector_location, &locations);

	/* Each display port's location, and the connector at it. */
	for (index = 0U; index < DRV_TYPEC_DISPLAY_PORT_MAX; index++) {
		/* A port the firmware does not name has no location. */
		error = drv_acpi_lookup(NULL, ucsi_acpi_display_ports[index], &node);
		if (error != 0)
			continue;

		/* Its location; one that cannot be read binds nothing. */
		error = ucsi_acpi_location(node, &port);
		if (error != 0) {
			drv_typec_os_log("typec: display port TC%u: no location (error %d), not bound\n", index + 1U, error);
			continue;
		}

		/* The connector at the place, if exactly one is there. */
		connector = drv_typec_location_match(locations.connector, locations.count, &port);
		if (connector == DRV_TYPEC_CONNECTOR_NONE) {
			drv_typec_os_log("typec: display port TC%u: no single connector at group %u position %u (visible %d), not bound\n",
					 index + 1U,
					 port.group_token,
					 port.group_position,
					 port.visible ? 1 : 0);
			continue;
		}

		/* Bound: its reports go into that connector's record. */
		error = drv_typec_display_bind(index, connector);
		drv_typec_os_log("typec: display port TC%u: connector %u (group %u position %u), bound (error %d)\n",
				 index + 1U,
				 connector,
				 port.group_token,
				 port.group_position,
				 error);
	}
}

/* Takes the location of each device directly under the UCSI device, in order: the connectors. */
static int
ucsi_acpi_connector_location(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct ucsi_acpi_locations *locations;
	enum drv_acpi_type type;

	locations = argument;

	/* Only the devices right under the UCSI device. */
	type = drv_acpi_node_type(node);
	if (depth != 0U || type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* No room for more connectors: the walk stops. */
	if (locations->count >= DRV_TYPEC_CONNECTOR_MAX)
		return -1;

	/* Its location, unknown when it has none. */
	(void)ucsi_acpi_location(node, &locations->connector[locations->count]);
	locations->count++;
	return 0;
}

/*
 * Reads a device's _PLD into a location: 0, ENOENT without one, EINVAL
 * for an answer that is not a buffer of the format (in a package, or
 * bare), or the evaluation's errno value.
 */
static int
ucsi_acpi_location(
	struct drv_acpi_node *node,
	struct drv_typec_location *location)
{
	struct drv_acpi_object *result;
	struct drv_acpi_object *element;
	const uint8_t *bytes;
	enum drv_acpi_type type;
	size_t length;
	int error;

	/* Nothing is known until the _PLD is read. */
	kern_memset(location, 0, sizeof(*location));

	/* The device's _PLD. */
	result = NULL;
	error = drv_acpi_evaluate(node, "_PLD", NULL, 0U, &result);
	if (error != 0)
		return error;
	if (result == NULL)
		return ENOENT;

	/*
	 * The buffer in the package's first element (no reference of its own);
	 * a buffer returned bare is taken as well (the 5330's xHCI ports
	 * return their _PLD so, against ACPI's package of buffers).
	 */
	error = EINVAL;
	element = result;
	type = drv_acpi_object_type(result);
	if (type == DRV_ACPI_TYPE_PACKAGE) {
		element = drv_acpi_object_package_element(result, 0U);
		type = DRV_ACPI_TYPE_UNINITIALIZED;
		if (element != NULL)
			type = drv_acpi_object_type(element);
	}
	if (type == DRV_ACPI_TYPE_BUFFER) {
		bytes = drv_acpi_object_buffer(element, &length);
		error = drv_typec_location_decode(bytes, length, location);
	}

	/* The answer goes. */
	drv_acpi_object_release(result);
	return error;
}
