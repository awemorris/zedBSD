/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Embedded Controller (ACPI 6.5 section 12): the EmbeddedControl
 * address space its operation regions live in, and the _Qxx queries it
 * raises through its GPE (lid, buttons, battery, thermal events).
 *
 * The EC is the PNP0C09 device of the namespace: its _CRS gives the data
 * port and the command/status port, _GPE its GPE, and _GLK whether the
 * Global Lock guards it.  Every transaction runs under the interpreter
 * lock, which keeps AML and the query handler from mixing their bytes.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * The bits of the EC status register.
 */
#define EC_STATUS_OBF		0x01U
#define EC_STATUS_IBF		0x02U
#define EC_STATUS_SCI_EVT	0x20U

/* How many times each query's run is logged (BUG-255). */
#define EC_QUERY_LOGGED		8U

/*
 * The EC commands.
 */
#define EC_COMMAND_READ		0x80U
#define EC_COMMAND_WRITE	0x81U
#define EC_COMMAND_QUERY	0x84U

/*
 * How long the EC may take to empty or fill a buffer, in 10-microsecond
 * polls: half a second, as slow controllers need.
 */
#define EC_POLLS		50000U

/*
 * How many queries one GPE may drain, which bounds an EC that raises
 * SCI_EVT without end.
 */
#define EC_QUERIES_MAX		32U

/*
 * The EISA identifier of PNP0C09 as _HID encodes it.
 */
#define EC_EISA_ID		0x090cd041ULL

/*
 * The value port_visitor() stops the walk of _CRS with once it has both
 * ports; negative, as the walk's own errors are positive.
 */
#define PORTS_COMPLETE		(-1)

/*
 * The ECDT fields (ACPI 6.5 table 5.123): the command/status and data
 * registers as Generic Address Structures, the GPE, and the path of the
 * EC device; and the GAS fields read.
 */
#define ECDT_EC_CONTROL		36U
#define ECDT_EC_DATA		48U
#define ECDT_GPE_BIT		64U
#define ECDT_EC_ID		65U
#define GAS_ADDRESS		4U
#define GAS_SPACE_SYSTEM_IO	1U

/*
 * The ports read_ports() collects from _CRS while it walks it: the base of
 * each I/O range, the data port first, and how many are known.
 */
struct ec_ports {
	uint16_t base[2];
	unsigned count;
};

/*
 * The embedded controller the driver found.
 *
 * drv_acpi_ec_ecdt() and drv_acpi_ec_attach() fill it; from_ecdt says
 * the ECDT installed the address space handler, so that the attachment
 * does not install it a second time.  The ports are used and changed only
 * under the interpreter lock.
 */
static struct {
	struct drv_acpi_node *device;
	uint16_t data_port;
	uint16_t command_port;
	unsigned gpe;
	uint8_t global_lock;
	uint8_t has_gpe;
	uint8_t from_ecdt;
} ec;

/*
 * How many times each query (_Q00 to _QFF) has run, up to EC_QUERY_LOGGED:
 * only the query handler, which runs one query at a time, counts them, and
 * they live as long as the kernel (BUG-255).
 */
static uint8_t ec_queries_logged[256];

static int attach_device(void);
static uint64_t load_u64(const uint8_t *bytes);
static int find_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool is_ec(struct drv_acpi_node *node);
static int read_ports(struct drv_acpi_node *device, uint16_t *data, uint16_t *command);
static int port_visitor(const struct drv_acpi_resource *resource, void *argument);
static int ec_region(const struct drv_acpi_region_access *access, uint64_t *value, void *argument);
static void ec_gpe(unsigned gpe, void *argument);
static int ec_transaction(uint8_t command, uint8_t address, bool has_address, bool write, uint8_t *data);
static int ec_exchange(uint8_t command, uint8_t address, bool has_address, bool write, uint8_t *data);
static int ec_send(uint16_t port, uint8_t byte);
static int ec_receive(uint8_t *byte);
static int wait_status(uint8_t mask, uint8_t wanted);
static uint8_t read_status(void);
static void run_query(uint8_t query);

/*
 * Starts the embedded controller the ECDT describes, before the namespace
 * is initialized (ACPI 6.5 section 5.2.16).
 *
 * Firmware lists the EC in the ECDT when _REG and _INI need its address
 * space: the handler is installed now, so drv_acpi_region_connect_all()
 * runs the EC's _REG with the other spaces and _INI can reach the EC.
 * drv_acpi_ec_attach() later adds the device's GPE and queries.  It is
 * called after the tables are loaded.
 */
int
drv_acpi_ec_ecdt(
	const uint8_t *ecdt,
	size_t length)
{
	struct drv_acpi_node *device;
	uint64_t control;
	uint64_t data;
	uint64_t value;
	size_t index;
	int error;

	/* Refuses a table too short for its EC_ID. */
	if (ecdt == NULL || length < ECDT_EC_ID + 1U)
		return EINVAL;

	/* Refuses a table that is not the ECDT. */
	if (ecdt[0] != 'E' ||
	    ecdt[1] != 'C' ||
	    ecdt[2] != 'D' ||
	    ecdt[3] != 'T')
		return EINVAL;

	/* The ports must be system I/O ports. */
	control = load_u64(ecdt + ECDT_EC_CONTROL + GAS_ADDRESS);
	data = load_u64(ecdt + ECDT_EC_DATA + GAS_ADDRESS);
	if (ecdt[ECDT_EC_CONTROL] != GAS_SPACE_SYSTEM_IO || ecdt[ECDT_EC_DATA] != GAS_SPACE_SYSTEM_IO)
		return ENOTSUP;

	/* Refuses a port of zero or one beyond the 64 KiB space. */
	if (control == 0 ||
	    data == 0 ||
	    control > 0xffffU ||
	    data > 0xffffU)
		return EINVAL;

	/* The EC_ID must end within the table. */
	for (index = ECDT_EC_ID; index < length; index++) {
		/* Stops at its terminator. */
		if (ecdt[index] == 0)
			break;
	}

	/* Refuses an EC_ID without its terminator. */
	if (index == length)
		return EINVAL;

	/* Takes the ports and the GPE. */
	ec.data_port = (uint16_t)data;
	ec.command_port = (uint16_t)control;
	ec.gpe = ecdt[ECDT_GPE_BIT];
	ec.has_gpe = 1;
	ec.from_ecdt = 1;

	/* Finds the device EC_ID names, whose _GLK says whether the Global Lock guards it. */
	error = drv_acpi_lookup(NULL, (const char *)ecdt + ECDT_EC_ID, &device);
	if (error == 0) {
		ec.device = device;
		error = drv_acpi_evaluate_integer(device, "_GLK", &value);
		if (error == 0 && value != 0)
			ec.global_lock = 1;
	}

	/* Installs the address space; its _REG runs with the others. */
	error = drv_acpi_region_install(DRV_ACPI_SPACE_EMBEDDED_CONTROL, ec_region, NULL);
	if (error != 0) {
		ec.from_ecdt = 0;
		return error;
	}

	/* Succeeded: AML reaches the EC's space before the namespace is initialized. */
	drv_acpi_os_log("ACPI: EC from the ECDT at ports 0x%x/0x%x, GPE 0x%x\n", ec.data_port, ec.command_port, ec.gpe);
	return 0;
}

/*
 * Reports the GPE of the Embedded Controller (ws052-p006): its queries
 * (a battery's level, a key) wake the system through it, and the sleep's
 * coordinator tells such a wake from a real one.  It reports ENODEV
 * without an EC that has a GPE.
 */
int
drv_acpi_ec_gpe(
	unsigned *gpe)
{
	/* An EC without a GPE (or none at all). */
	if (!ec.has_gpe)
		return ENODEV;

	/* Succeeded: the GPE. */
	*gpe = ec.gpe;
	return 0;
}

/*
 * Finds the embedded controller in the namespace, installs the handler of
 * its address space (which runs its _REG) and the handler of its GPE.
 *
 * After drv_acpi_ec_ecdt() the space is already there; the device's _CRS
 * and _GPE are preferred when they differ from the ECDT, as firmware
 * whose ECDT disagrees with its namespace is known.
 */
int
drv_acpi_ec_attach(void)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	int error;

	/* Finds the device and installs the space, holding the interpreter so that no AML reaches the EC while it changes. */
	thread = drv_acpi_enter(&storage);
	error = attach_device();
	drv_acpi_leave(thread);
	if (error != 0)
		return error;

	/* Installs the GPE handler, which drains the EC's queries. */
	if (ec.has_gpe && ec.device != NULL) {
		error = drv_acpi_gpe_install(ec.gpe, true, ec_gpe, NULL);
		if (error != 0)
			drv_acpi_os_log("ACPI: the EC's GPE 0x%x cannot be handled (error %d)\n", ec.gpe, error);

		/*
		 * Arms a handled GPE as a wake source of S0 idle for as long as
		 * the kernel runs: the lid, the power button and the AC reach
		 * many machines only as EC queries (the Latitude 5330's power
		 * button is its query 0x66).
		 */
		if (error == 0) {
			error = drv_acpi_gpe_wake_set(ec.gpe, true);
			if (error != 0)
				drv_acpi_os_log("ACPI: the EC's GPE 0x%x cannot wake the system (error %d)\n", ec.gpe, error);
		}
	}

	/* Succeeded: AML reaches the EC, and its queries run their methods. */
	drv_acpi_os_log("ACPI: EC at ports 0x%x/0x%x, GPE 0x%x\n", ec.data_port, ec.command_port, ec.gpe);
	return 0;
}

/* Finds the EC's device and reads its ports, GPE and _GLK; installs the space unless the ECDT did. */
static int
attach_device(void)
{
	struct drv_acpi_node *found;
	uint16_t data;
	uint16_t command;
	uint64_t value;
	int error;

	/* Finds the PNP0C09 device, or keeps the one the ECDT named. */
	found = NULL;
	drv_acpi_walk(NULL, find_visitor, &found);
	if (found == NULL)
		found = ec.device;

	/* Refuses a platform with no EC at all. */
	if (found == NULL && !ec.from_ecdt)
		return ENODEV;

	/* An EC only the ECDT describes keeps its space, without queries. */
	if (found == NULL) {
		drv_acpi_os_log("ACPI: the ECDT's EC has no device; no queries\n");
		return 0;
	}

	/* The device is the EC's from now on. */
	ec.device = found;

	/* Reads its ports from _CRS; an ECDT's stay when _CRS gives none. */
	error = read_ports(found, &data, &command);
	if (error != 0 && !ec.from_ecdt) {
		drv_acpi_os_log("ACPI: the EC's _CRS gives no ports (error %d)\n", error);
		return error;
	}

	/* Takes _CRS's ports, noting when they differ from the ECDT's. */
	if (error == 0) {
		if (ec.from_ecdt &&
		    (data != ec.data_port ||
		     command != ec.command_port))
			drv_acpi_os_log("ACPI: the ECDT's EC ports differ from _CRS; _CRS is used\n");

		/* Uses _CRS's ports from now on. */
		ec.data_port = data;
		ec.command_port = command;
	}

	/* Reads its GPE, which the ECDT may have given already. */
	error = drv_acpi_evaluate_integer(found, "_GPE", &value);
	if (error == 0) {
		ec.gpe = (unsigned)value;
		ec.has_gpe = 1;
	}

	/* _GLK says whether the firmware shares the EC under the Global Lock. */
	error = drv_acpi_evaluate_integer(found, "_GLK", &value);
	if (error == 0 && value != 0)
		ec.global_lock = 1;

	/* The ECDT installed the space already. */
	if (ec.from_ecdt)
		return 0;

	/* Installs the address space; the EC's _REG runs now. */
	error = drv_acpi_region_install(DRV_ACPI_SPACE_EMBEDDED_CONTROL, ec_region, NULL);
	if (error != 0)
		return error;

	/* Succeeded: AML reaches the EC's space. */
	return 0;
}

/* Stops the walk at the first PNP0C09 device. */
static int
find_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct drv_acpi_node **found;
	bool matched;

	UNUSED_PARAMETER(depth);

	/* Only a device can be the EC. */
	if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* Goes on past a device that is not the EC. */
	matched = is_ec(node);
	if (!matched)
		return 0;

	/* Hands the EC to the caller. */
	found = argument;
	*found = node;

	/* Stops the walk: the first EC is the one. */
	return -1;
}

/* Reports whether a device's _HID is PNP0C09, as an EISA identifier or a string. */
static bool
is_ec(
	struct drv_acpi_node *node)
{
	struct drv_acpi_object *hid;
	const char *text;
	bool matched;
	int compared;
	int error;

	/* Evaluates the _HID. */
	hid = NULL;
	error = drv_acpi_evaluate(node, "_HID", NULL, 0, &hid);
	if (error != 0 || hid == NULL)
		return false;

	/* Compares it in its form. */
	matched = false;
	if (hid->type == DRV_ACPI_TYPE_INTEGER && hid->value.integer == EC_EISA_ID) {
		matched = true;
	} else if (hid->type == DRV_ACPI_TYPE_STRING) {
		text = hid->value.string.text;
		compared = kern_strcmp(text, "PNP0C09");
		if (compared == 0)
			matched = true;
	}

	/* The _HID is no longer needed. */
	drv_acpi_object_release(hid);

	/* Reports whether the _HID named PNP0C09. */
	return matched;
}

/* Reads the data port and the command port from the EC's _CRS. */
static int
read_ports(
	struct drv_acpi_node *device,
	uint16_t *data,
	uint16_t *command)
{
	struct ec_ports ports;
	int error;

	/* Takes the first two I/O ranges of _CRS: the data port, then the command port. */
	kern_memset(&ports, 0, sizeof(ports));
	error = drv_acpi_resources_walk(device, NULL, port_visitor, &ports);
	if (error > 0)
		return error;

	/* Refuses a _CRS without both ports. */
	if (ports.count != 2U)
		return ENOENT;

	/* Hands the ports to the caller. */
	*data = ports.base[0];
	*command = ports.base[1];

	/* Succeeded: the caller has the data port and the command port. */
	return 0;
}

/* Keeps the base of each I/O range of _CRS until it has two. */
static int
port_visitor(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	struct ec_ports *ports;

	/* Only I/O ranges are ports. */
	ports = argument;
	if (resource->kind != DRV_ACPI_RESOURCE_IO)
		return 0;

	/* Keeps the range's base; count is how many ports are known. */
	ports->base[ports->count] = (uint16_t)resource->base;
	ports->count++;

	/* Stops the walk once both ports are known. */
	if (ports->count == 2U)
		return PORTS_COMPLETE;

	/* Goes on to the next resource. */
	return 0;
}

/* Reads a little-endian 64-bit value. */
static uint64_t
load_u64(
	const uint8_t *bytes)
{
	uint64_t value;
	unsigned index;

	/* Assembles it from its highest byte down. */
	value = 0;
	for (index = 8; index != 0; index--)
		value = value << 8 | bytes[index - 1U];

	/* Reports the assembled value. */
	return value;
}

/* Reads or writes the EC's address space for an operation region, a byte at a time. */
static int
ec_region(
	const struct drv_acpi_region_access *access,
	uint64_t *value,
	void *argument)
{
	uint8_t command;
	uint8_t byte;
	unsigned bytes;
	unsigned index;
	int error;

	UNUSED_PARAMETER(argument);

	/* Refuses an address beyond the EC's 256 bytes. */
	bytes = access->width / 8U;
	if (access->address + bytes > 256U)
		return EFAULT;

	/* A write sends Write Embedded Controller, a read Read Embedded Controller. */
	command = EC_COMMAND_READ;
	if (access->write)
		command = EC_COMMAND_WRITE;

	/* A read starts from zero and gathers its bytes. */
	if (!access->write)
		*value = 0;

	/* Moves each byte through one EC transaction. */
	for (index = 0; index < bytes; index++) {
		/* Writes or reads one byte. */
		byte = (uint8_t)(*value >> (index * 8U));
		error = ec_transaction(command, (uint8_t)(access->address + index), true, access->write, &byte);
		if (error != 0)
			return error;

		/* Puts a read byte in its place. */
		if (!access->write)
			*value |= (uint64_t)byte << (index * 8U);
	}

	/* Succeeded: every byte of the access moved. */
	return 0;
}

/*
 * Handles the EC's GPE: while the EC says it has an event, asks which one
 * and runs its _Qxx method.  It runs as an interpreter entry of its own,
 * so the queries do not mix with AML's accesses.
 */
static void
ec_gpe(
	unsigned gpe,
	void *argument)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	uint8_t query;
	uint8_t status;
	unsigned count;
	int error;

	UNUSED_PARAMETER(gpe);
	UNUSED_PARAMETER(argument);

	/* Enters the interpreter, so that the queries do not mix with AML's accesses. */
	thread = drv_acpi_enter(&storage);

	/* Drains the queries, as a bounded number of rounds. */
	for (count = 0; count < EC_QUERIES_MAX; count++) {
		/* Stops when the EC has nothing more. */
		status = read_status();
		if ((status & EC_STATUS_SCI_EVT) == 0)
			break;

		/* Asks for the event; zero means none after all. */
		query = 0;
		error = ec_transaction(EC_COMMAND_QUERY, 0, false, false, &query);
		if (error != 0 || query == 0)
			break;

		/* Runs its method. */
		run_query(query);
	}

	/* Leaves the interpreter. */
	drv_acpi_leave(thread);
}

/*
 * Makes one EC transaction: a command, an address when it has one, and a
 * byte written or read.  The Global Lock guards it when _GLK asks.
 */
static int
ec_transaction(
	uint8_t command,
	uint8_t address,
	bool has_address,
	bool write,
	uint8_t *data)
{
	int release_error;
	int error;

	/* Takes the Global Lock for a controller that shares itself with the firmware. */
	if (ec.global_lock) {
		error = drv_acpi_global_lock(NULL, true);
		if (error != 0)
			return error;
	}

	/* Exchanges the bytes. */
	error = ec_exchange(command, address, has_address, write, data);

	/* Lets the Global Lock go; a release that fails leaves nothing more to undo here. */
	if (ec.global_lock) {
		release_error = drv_acpi_global_lock(NULL, false);
		if (release_error != 0)
			drv_acpi_os_log("ACPI: the EC's Global Lock release failed (error %d)\n", release_error);
	}

	/* Reports a failed exchange. */
	if (error != 0) {
		drv_acpi_os_log("ACPI: EC command 0x%x failed (error %d)\n", command, error);
		return error;
	}

	/* Succeeded: a read leaves the EC's byte in data. */
	return 0;
}

/* Exchanges the bytes of one EC transaction. */
static int
ec_exchange(
	uint8_t command,
	uint8_t address,
	bool has_address,
	bool write,
	uint8_t *data)
{
	int error;

	/* Sends the command. */
	error = ec_send(ec.command_port, command);
	if (error != 0)
		return error;

	/* Sends the address. */
	if (has_address) {
		error = ec_send(ec.data_port, address);
		if (error != 0)
			return error;
	}

	/* A write sends the byte and waits until the EC took it; a read waits for the EC's byte and takes it. */
	if (write) {
		error = ec_send(ec.data_port, *data);
		if (error != 0)
			return error;
		error = wait_status(EC_STATUS_IBF, 0);
	} else {
		error = ec_receive(data);
	}

	/* Reports a byte that did not move. */
	if (error != 0)
		return error;

	/* Succeeded: the transaction's bytes moved. */
	return 0;
}

/* Sends one byte once the EC's input buffer is empty. */
static int
ec_send(
	uint16_t port,
	uint8_t byte)
{
	int error;

	/* Waits for room. */
	error = wait_status(EC_STATUS_IBF, 0);
	if (error != 0)
		return error;

	/* Writes the byte. */
	error = drv_acpi_os_port_write(port, 8, byte);
	if (error != 0)
		return error;

	/* Succeeded: the EC has the byte. */
	return 0;
}

/* Takes one byte once the EC's output buffer is full. */
static int
ec_receive(
	uint8_t *byte)
{
	uint32_t value;
	int error;

	/* Waits for the byte. */
	error = wait_status(EC_STATUS_OBF, EC_STATUS_OBF);
	if (error != 0)
		return error;

	/* Reads the byte from the data port. */
	error = drv_acpi_os_port_read(ec.data_port, 8, &value);
	if (error != 0)
		return error;

	/* Hands the byte to the caller. */
	*byte = (uint8_t)value;

	/* Succeeded: the caller has the EC's byte. */
	return 0;
}

/* Waits until the masked status bits are as wanted. */
static int
wait_status(
	uint8_t mask,
	uint8_t wanted)
{
	uint8_t status;
	unsigned poll;
	bool reached;

	/* Polls the status register. */
	reached = false;
	for (poll = 0; poll < EC_POLLS; poll++) {
		/* Stops as soon as the bits are right. */
		status = read_status();
		if ((status & mask) == wanted) {
			reached = true;
			break;
		}

		/* Gives the EC 10 microseconds more. */
		drv_acpi_os_stall(10);
	}

	/* Reports an EC that did not answer. */
	if (!reached)
		return ETIMEDOUT;

	/* Succeeded: the status bits are as wanted. */
	return 0;
}

/* Reads the EC's status register. */
static uint8_t
read_status(void)
{
	uint32_t value;
	int error;

	/* A status that cannot be read reads as busy, which times out. */
	error = drv_acpi_os_port_read(ec.command_port, 8, &value);
	if (error != 0)
		return EC_STATUS_IBF;

	/* Reports the status byte. */
	return (uint8_t)value;
}

/* Runs the _Qxx method of the EC for one query. */
static void
run_query(
	uint8_t query)
{
	static const char digits[] = "0123456789ABCDEF";
	struct drv_acpi_object *result;
	char name[5];
	int error;

	/* The method is _Q and the query in two upper-case hexadecimal digits. */
	name[0] = '_';
	name[1] = 'Q';
	name[2] = digits[query >> 4];
	name[3] = digits[query & 0x0fU];
	name[4] = '\0';

	/* Logs the query's first few runs (BUG-255: which queries a lid or a button raises). */
	if (ec_queries_logged[query] < EC_QUERY_LOGGED) {
		ec_queries_logged[query]++;
		drv_acpi_os_log("ACPI: EC query %s #%u\n", name, (unsigned)ec_queries_logged[query]);
	}

	/* Runs it; a query without a method is logged. */
	result = NULL;
	error = drv_acpi_evaluate(ec.device, name, NULL, 0, &result);
	drv_acpi_object_release(result);
	if (error != 0)
		drv_acpi_os_log("ACPI: EC query %s failed (error %d)\n", name, error);
}
