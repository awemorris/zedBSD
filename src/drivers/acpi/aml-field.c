/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Operation regions and the fields over them (ACPI 6.5 sections 5.5.2.4
 * and 19.6.46): address space handlers, reading and writing field units
 * by their access width and update rule, index and bank fields, and
 * buffer fields.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * The widest access a field unit makes, in bytes.
 */
#define ACCESS_BYTES_MAX 8U

/*
 * The largest field read or written at once, in bytes; larger ones are
 * refused rather than allocated.
 */
#define FIELD_BYTES_MAX 4096U

/*
 * The EISA identifiers of PNP0A03 (a PCI host bridge) and PNP0A08 (a PCI
 * Express host bridge), as _HID and _CID encode them.
 */
#define PCI_ROOT_EISA_ID	0x030ad041ULL
#define PCIE_ROOT_EISA_ID	0x080ad041ULL

/*
 * How many levels below its host bridge a region's device may lie.  The
 * bridges of a deeper device are not followed, and it keeps the host
 * bridge's bus; no real platform nests PCI bridges that deep.
 */
#define PCI_PATH_MAX 16U

/*
 * The configuration space offsets of the header type, whose low seven bits
 * say whether the function is a bridge, and of a bridge's secondary bus;
 * the mask that keeps those seven bits, and the layouts of a PCI-to-PCI
 * bridge and of a CardBus bridge, the two that lead to another bus.
 */
#define PCI_HEADER_TYPE		0x0eU
#define PCI_SECONDARY_BUS	0x19U
#define PCI_HEADER_KIND		0x7fU
#define PCI_HEADER_BRIDGE	0x01U
#define PCI_HEADER_CARDBUS	0x02U

/*
 * One installed address space handler.
 */
struct region_handler {
	drv_acpi_region_handler_t handler;
	void *argument;
};

/*
 * The handler of each address space.
 *
 * A driver installs one with drv_acpi_region_install() before the regions
 * of that space are used; the entry stays for the life of the system.  An
 * empty entry makes every access to the space fail.
 */
static struct region_handler region_handlers[DRV_ACPI_SPACE_COUNT];

/*
 * Whether the loaded namespace has had its regions connected.
 *
 * drv_acpi_region_connect_all() sets it once the tables are loaded; from
 * then on a handler installed later (the embedded controller's) connects
 * its space at once.  drv_acpi_region_reset() clears it.
 */
static bool regions_connected;

static void field_lock_release(struct drv_acpi_eval *eval, const struct drv_acpi_field *unit);
static int region_access(struct drv_acpi_eval *eval, struct drv_acpi_object *region, uint64_t offset, unsigned bytes, bool write, uint64_t *value);
static int table_region_read(const struct drv_acpi_region *place, uint64_t offset, unsigned bytes, bool write, uint64_t *value);
static int region_resolve_pci(struct drv_acpi_object *region);
static int evaluate_found(struct drv_acpi_node *device, const char *name, bool own, uint64_t *value);
static struct drv_acpi_node *pci_root_bridge(struct drv_acpi_node *device);
static bool pci_root_named(struct drv_acpi_node *node, const char *name);
static bool pci_root_id(const struct drv_acpi_object *id);
static void pci_follow_bridges(struct drv_acpi_node *root, struct drv_acpi_node *device, uint16_t segment, uint64_t *bus);
static int pci_config_read8(uint16_t segment, uint8_t bus, uint64_t address, unsigned offset, uint8_t *value);
static unsigned field_access_bytes(const struct drv_acpi_field *field, uint64_t region_length);
static int field_span(struct drv_acpi_eval *eval, struct drv_acpi_object *field, unsigned *access, uint64_t *first, uint64_t *last);
static int field_read_bits(struct drv_acpi_eval *eval, struct drv_acpi_object *field, unsigned access, uint64_t first, uint64_t last, struct drv_acpi_object **result);
static int field_write_bits(struct drv_acpi_eval *eval, struct drv_acpi_object *field, unsigned access, uint64_t first, uint64_t last, struct drv_acpi_object *value);
static int unit_read(struct drv_acpi_eval *eval, struct drv_acpi_object *field, uint64_t byte_offset, unsigned bytes, uint64_t *value);
static int unit_write(struct drv_acpi_eval *eval, struct drv_acpi_object *field, uint64_t byte_offset, unsigned bytes, uint64_t value);
static int bits_to_object(const uint8_t *bytes, uint64_t bit_length, struct drv_acpi_object **result);
static int object_to_bits(struct drv_acpi_object *value, uint64_t bit_length, uint8_t *bytes);
static void bits_extract(const uint8_t *source, uint64_t bit_offset, uint64_t bit_length, uint8_t *destination);
static void bits_insert(uint8_t *destination, uint64_t bit_offset, uint64_t bit_length, const uint8_t *source);
static int named_field_write(struct drv_acpi_eval *eval, struct drv_acpi_node *node, uint64_t value);
static int named_field_read(struct drv_acpi_eval *eval, struct drv_acpi_node *node, uint64_t *value);
static int region_connect(enum drv_acpi_space space);
static int connect_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static int run_reg(struct drv_acpi_node *region, unsigned space);


/*
 * Installs the handler of an address space.
 */
int
drv_acpi_region_install(
	enum drv_acpi_space space,
	drv_acpi_region_handler_t handler,
	void *argument)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	int error;

	/* Refuses a space ACPI does not define. */
	if ((unsigned)space >= DRV_ACPI_SPACE_COUNT)
		return EINVAL;

	/* Enters the interpreter, so that no AML uses the space while its handler changes. */
	thread = drv_acpi_enter(&storage);

	/* Refuses a second handler for the same space. */
	if (region_handlers[space].handler != NULL) {
		drv_acpi_leave(thread);
		return EBUSY;
	}

	/* Installs the handler. */
	region_handlers[space].handler = handler;
	region_handlers[space].argument = argument;

	/* A handler installed after the tables were connected tells their regions at once. */
	error = 0;
	if (regions_connected)
		error = region_connect(space);

	/* Leaves the interpreter. */
	drv_acpi_leave(thread);

	/* Reports a failed connection. */
	if (error != 0)
		return error;

	/* Succeeded: the space's regions reach the handler. */
	return 0;
}

/*
 * Tells firmware which address spaces have handlers, once the tables are loaded.
 *
 * It runs _REG (space, 1) for each region of those spaces.  System memory
 * and system I/O are always available and get no _REG, as ACPI 6.5 section
 * 6.5.4 lets the operating system choose.  A handler installed afterwards
 * connects its space when it is installed.
 */
int
drv_acpi_region_connect_all(void)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	unsigned space;
	int error;

	/* Enters the interpreter for the whole connection. */
	thread = drv_acpi_enter(&storage);

	/* regions_connected makes the handlers installed from now on connect at once. */
	regions_connected = true;

	/* Connects each space that has a handler. */
	error = 0;
	for (space = 0; space < DRV_ACPI_SPACE_COUNT; space++) {
		/* Skips a space without a handler. */
		if (region_handlers[space].handler == NULL)
			continue;

		/* Runs the _REG methods of the space's regions. */
		error = region_connect((enum drv_acpi_space)space);
		if (error != 0)
			break;
	}

	/* Leaves the interpreter. */
	drv_acpi_leave(thread);

	/* Reports a failed connection. */
	if (error != 0)
		return error;

	/* Succeeded: firmware knows every space that has a handler. */
	return 0;
}

/*
 * Removes every address space handler.
 *
 * The host tests use it to start the interpreter over.
 */
void
drv_acpi_region_reset(void)
{
	/* Forgets every handler, and that the regions were connected. */
	kern_memset(region_handlers, 0, sizeof(region_handlers));
	regions_connected = false;
}

/*
 * Reads a field unit and reports its value.
 *
 * The value is an integer when it fits in one, a buffer otherwise.
 */
int
drv_acpi_field_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	struct drv_acpi_object **result)
{
	struct drv_acpi_field *unit;
	uint64_t first;
	uint64_t last;
	unsigned access;
	int error;

	/* Covers the field's bits with aligned accesses of one width. */
	unit = &field->value.field;
	error = field_span(eval, field, &access, &first, &last);
	if (error != 0)
		return error;

	/* A field whose lock rule asks for it is read under the global lock. */
	if ((unit->flags & DRV_ACPI_FIELD_LOCK) != 0) {
		error = drv_acpi_global_lock(eval, true);
		if (error != 0)
			return error;
	}

	/* Reads the field's bits. */
	error = field_read_bits(eval, field, access, first, last, result);

	/* Lets the global lock go. */
	field_lock_release(eval, unit);

	/* Reports a failed read. */
	if (error != 0)
		return error;

	/* Succeeded: result is the field's value. */
	return 0;
}

/*
 * Writes a value into a field unit.
 *
 * Accesses that cover bits outside the field fill those bits by the
 * field's update rule: the current contents, ones, or zeros.
 */
int
drv_acpi_field_write(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	struct drv_acpi_object *value)
{
	struct drv_acpi_field *unit;
	uint64_t first;
	uint64_t last;
	unsigned access;
	int error;

	/* Covers the field's bits with aligned accesses of one width. */
	unit = &field->value.field;
	error = field_span(eval, field, &access, &first, &last);
	if (error != 0)
		return error;

	/* A field whose lock rule asks for it is written under the global lock. */
	if ((unit->flags & DRV_ACPI_FIELD_LOCK) != 0) {
		error = drv_acpi_global_lock(eval, true);
		if (error != 0)
			return error;
	}

	/* Writes the field's bits. */
	error = field_write_bits(eval, field, access, first, last, value);

	/* Lets the global lock go. */
	field_lock_release(eval, unit);

	/* Reports a failed write. */
	if (error != 0)
		return error;

	/* Succeeded: the field holds the value. */
	return 0;
}

/*
 * Reads a buffer field.
 *
 * The value is an integer when it fits in one, a buffer otherwise.
 */
int
drv_acpi_buffer_field_read(
	struct drv_acpi_object *field,
	struct drv_acpi_object **result)
{
	struct drv_acpi_buffer_field *unit;
	struct drv_acpi_object *buffer;
	uint8_t *bits;
	size_t size;
	int error;

	/* Allocates the bytes of the field. */
	unit = &field->value.buffer_field;
	size = (size_t)((unit->bit_length + 7U) / 8U);
	bits = drv_acpi_os_alloc(size + 1U);
	if (bits == NULL)
		return ENOMEM;

	/* Starts the bytes at zero, so that the bits above the field stay clear. */
	kern_memset(bits, 0, size + 1U);

	/* Takes the bits out of the buffer. */
	bits_extract(unit->buffer->value.buffer.bytes, unit->bit_offset, unit->bit_length, bits);

	/* A CreateField field is always a buffer; the others are integers when they fit. */
	if (unit->reads_buffer) {
		buffer = drv_acpi_object_buffer_new(bits, size);
		error = 0;
		if (buffer == NULL)
			error = ENOMEM;
		*result = buffer;
	} else {
		error = bits_to_object(bits, unit->bit_length, result);
	}

	/* Frees the staging bytes. */
	drv_acpi_os_free(bits);

	/* Reports a value that could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: result is the field's value. */
	return 0;
}

/*
 * Writes a value into a buffer field.
 */
int
drv_acpi_buffer_field_write(
	struct drv_acpi_object *field,
	struct drv_acpi_object *value)
{
	struct drv_acpi_buffer_field *unit;
	uint8_t *bits;
	size_t size;
	int error;

	/* Allocates the bytes of the field. */
	unit = &field->value.buffer_field;
	size = (size_t)((unit->bit_length + 7U) / 8U);
	bits = drv_acpi_os_alloc(size + 1U);
	if (bits == NULL)
		return ENOMEM;

	/* Starts the bytes at zero, so that bits the value lacks are zero. */
	kern_memset(bits, 0, size + 1U);

	/* Converts the value to the field's bits. */
	error = object_to_bits(value, unit->bit_length, bits);
	if (error != 0) {
		drv_acpi_os_free(bits);
		return error;
	}

	/* Puts the bits into the buffer. */
	bits_insert(unit->buffer->value.buffer.bytes, unit->bit_offset, unit->bit_length, bits);

	/* Frees the staging bytes. */
	drv_acpi_os_free(bits);

	/* Succeeded: the buffer holds the value in the field's bits. */
	return 0;
}

/*
 * Reads bytes of a region through its handler, one byte at a time.
 *
 * Load reads a table from a region this way.
 */
int
drv_acpi_region_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *region,
	uint64_t offset,
	size_t length,
	uint8_t *bytes)
{
	uint64_t value;
	size_t index;
	int error;

	/* Reads each byte. */
	for (index = 0; index < length; index++) {
		/* Reads the byte at the next offset through the space's handler. */
		error = region_access(eval, region, offset + index, 1, false, &value);
		if (error != 0)
			return error;

		/* Puts it in its place. */
		bytes[index] = (uint8_t)value;
	}

	/* Succeeded: bytes holds the region's bytes. */
	return 0;
}

/*
 * Evaluates the offset and length of a region, the first time it is used.
 *
 * The region was defined while a table loaded.
 */
int
drv_acpi_region_prepare(
	struct drv_acpi_object *region)
{
	struct drv_acpi_eval arguments;
	struct drv_acpi_table *table;
	int error;

	/* A region is prepared once. */
	if (region->value.region.evaluated)
		return 0;

	/* Prepares to run the offset and length AML in the scope the region was defined in. */
	table = region->value.region.table;
	kern_memset(&arguments, 0, sizeof(arguments));
	arguments.position = region->value.region.arguments_start;
	arguments.end = region->value.region.arguments_end;
	arguments.scope = region->value.region.scope;
	arguments.table = table;

	/* Evaluates the offset. */
	error = drv_acpi_eval_integer(&arguments, &region->value.region.offset);
	if (error != 0) {
		drv_acpi_os_log("ACPI: region arguments could not be evaluated (error %d)\n", error);
		return error;
	}

	/* Evaluates the length. */
	error = drv_acpi_eval_integer(&arguments, &region->value.region.length);
	if (error != 0) {
		drv_acpi_os_log("ACPI: region arguments could not be evaluated (error %d)\n", error);
		return error;
	}

	/* evaluated tells every later use that the region has its place. */
	region->value.region.evaluated = 1;

	/* Succeeded: the region has its place. */
	return 0;
}

/*
 * Finds the PCI function a device of the namespace stands for.
 *
 * The device gives the device and function (_ADR, zero without one), and
 * the nearest scope above it that has them gives the bus (_BBN) and the
 * segment (_SEG).  A device behind PCI-to-PCI bridges is on the bus the
 * last of them leads to, so the bridges between the host bridge and the
 * device are followed, as ACPICA does (AcpiHwDerivePciId).
 */
int
drv_acpi_pci_location(
	struct drv_acpi_node *device,
	uint16_t *segment,
	uint8_t *bus,
	uint8_t *slot,
	uint8_t *function)
{
	struct drv_acpi_node *root;
	uint64_t address;
	uint64_t bus_number;
	uint64_t segment_number;
	int error;

	/* Reads the device and function from its address, zero when it has none. */
	address = 0;
	error = evaluate_found(device, "_ADR", true, &address);
	if (error != 0)
		return error;

	/* Reads the bus, zero when no scope has one. */
	bus_number = 0;
	error = evaluate_found(device, "_BBN", false, &bus_number);
	if (error != 0)
		return error;

	/* Reads the segment, zero when no scope has one. */
	segment_number = 0;
	error = evaluate_found(device, "_SEG", false, &segment_number);
	if (error != 0)
		return error;

	/* Moves to the bus behind the bridges between the host bridge and the device. */
	root = pci_root_bridge(device);
	if (root != NULL)
		pci_follow_bridges(root, device, (uint16_t)segment_number, &bus_number);

	/* Succeeded: reports the function. */
	*segment = (uint16_t)segment_number;
	*bus = (uint8_t)bus_number;
	*slot = (uint8_t)((address >> 16) & 0x1fU);
	*function = (uint8_t)(address & 0x07U);
	return 0;
}

/*
 * Tells whether a device of the namespace is below a PCI host bridge.
 */
bool
drv_acpi_pci_below_root(
	struct drv_acpi_node *device)
{
	struct drv_acpi_node *root;

	/* Looks for the host bridge above the device. */
	root = pci_root_bridge(device);
	if (root == NULL)
		return false;

	/* The device is below one. */
	return true;
}

/* Lets the global lock go after the access of a field whose lock rule took it. */
static void
field_lock_release(
	struct drv_acpi_eval *eval,
	const struct drv_acpi_field *unit)
{
	int error;

	/* A field without the lock rule took no lock. */
	if ((unit->flags & DRV_ACPI_FIELD_LOCK) == 0)
		return;

	/* Lets the lock go; a release that fails is logged, as the access itself is done. */
	error = drv_acpi_global_lock(eval, false);
	if (error != 0)
		drv_acpi_os_log("ACPI: a field's global lock release failed (error %d)\n", error);
}

/* Writes the access units that cover a field, filling their other bits by the update rule. */
static int
field_write_bits(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	unsigned access,
	uint64_t first,
	uint64_t last,
	struct drv_acpi_object *value)
{
	struct drv_acpi_field *unit;
	uint8_t *units;
	uint8_t *bits;
	uint64_t offset;
	uint64_t word;
	uint64_t unit_start;
	uint64_t unit_end;
	uint64_t field_start;
	uint64_t field_end;
	unsigned update;
	unsigned index;
	size_t span;
	size_t size;
	int error;

	/* Measures the accesses and the field. */
	unit = &field->value.field;
	span = (size_t)(last - first);
	size = (size_t)((unit->bit_length + 7U) / 8U);

	/* Allocates the bytes of the accesses and of the field. */
	units = drv_acpi_os_alloc(span + size + 1U);
	if (units == NULL)
		return ENOMEM;

	/* Starts both at zero; the field's bytes follow the accesses' bytes. */
	kern_memset(units, 0, span + size + 1U);
	bits = units + span;

	/* Converts the value to the field's bits. */
	error = object_to_bits(value, unit->bit_length, bits);
	if (error != 0) {
		drv_acpi_os_free(units);
		return error;
	}

	/* Fills each unit the field covers only in part, by the update rule. */
	update = unit->flags & DRV_ACPI_FIELD_UPDATE_MASK;
	field_start = unit->bit_offset;
	field_end = field_start + unit->bit_length;
	for (offset = first;
	     offset < last;
	     offset += access) {
		/* A unit entirely inside the field needs nothing of its own. */
		unit_start = offset * 8U;
		unit_end = unit_start + access * 8U;
		if (unit_start >= field_start && unit_end <= field_end)
			continue;

		/* Keeps, sets or clears the bits outside the field. */
		if (update == DRV_ACPI_FIELD_UPDATE_PRESERVE) {
			error = unit_read(eval, field, offset, access, &word);
			if (error != 0) {
				drv_acpi_os_free(units);
				return error;
			}
		} else if (update == DRV_ACPI_FIELD_UPDATE_ONES) {
			word = ~0ULL;
		} else {
			word = 0;
		}

		/* Lays the unit's bytes out in order. */
		for (index = 0; index < access; index++)
			units[offset - first + index] = (uint8_t)(word >> (index * 8U));
	}

	/* Puts the field's bits in place. */
	bits_insert(units, field_start - first * 8U, unit->bit_length, bits);

	/* Writes each unit. */
	for (offset = first;
	     offset < last;
	     offset += access) {
		/* Assembles one unit, lowest byte first. */
		word = 0;
		for (index = 0; index < access; index++)
			word |= (uint64_t)units[offset - first + index] << (index * 8U);

		/* Writes the unit through the field's region, index or bank. */
		error = unit_write(eval, field, offset, access, word);
		if (error != 0) {
			drv_acpi_os_free(units);
			return error;
		}
	}

	/* Frees the staging bytes. */
	drv_acpi_os_free(units);

	/* Succeeded: the field holds the value. */
	return 0;
}

/* Reads the access units that cover a field and makes its value from its bits. */
static int
field_read_bits(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	unsigned access,
	uint64_t first,
	uint64_t last,
	struct drv_acpi_object **result)
{
	struct drv_acpi_field *unit;
	uint8_t *units;
	uint8_t *bits;
	uint64_t offset;
	uint64_t value;
	unsigned index;
	size_t span;
	size_t size;
	int error;

	/* Measures the accesses and the field. */
	unit = &field->value.field;
	span = (size_t)(last - first);
	size = (size_t)((unit->bit_length + 7U) / 8U);

	/* Allocates the bytes the accesses fill and the bytes of the field. */
	units = drv_acpi_os_alloc(span + size + 1U);
	if (units == NULL)
		return ENOMEM;

	/* Starts both at zero; the field's bytes follow the accesses' bytes. */
	kern_memset(units, 0, span + size + 1U);
	bits = units + span;

	/* Reads each access unit into its place, lowest byte first. */
	for (offset = first;
	     offset < last;
	     offset += access) {
		/* Reads the unit through the field's region, index or bank. */
		error = unit_read(eval, field, offset, access, &value);
		if (error != 0) {
			drv_acpi_os_free(units);
			return error;
		}

		/* Lays its bytes out in order. */
		for (index = 0; index < access; index++)
			units[offset - first + index] = (uint8_t)(value >> (index * 8U));
	}

	/* Takes the field's bits out. */
	bits_extract(units, unit->bit_offset - first * 8U, unit->bit_length, bits);

	/* Makes the value from the bits. */
	error = bits_to_object(bits, unit->bit_length, result);
	if (error != 0) {
		drv_acpi_os_free(units);
		return error;
	}

	/* Frees the staging bytes. */
	drv_acpi_os_free(units);

	/* Succeeded: result is the field's value. */
	return 0;
}

/* Reads or writes one access unit of a region through its space's handler. */
static int
region_access(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *region,
	uint64_t offset,
	unsigned bytes,
	bool write,
	uint64_t *value)
{
	struct drv_acpi_region_access access;
	struct drv_acpi_region *place;
	struct region_handler *handler;
	int error;

	UNUSED_PARAMETER(eval);

	/* Prepares the region's place. */
	error = drv_acpi_region_prepare(region);
	if (error != 0)
		return error;

	/* Takes the place the region now has. */
	place = &region->value.region;

	/* Refuses an access outside the region. */
	if (offset > place->length || bytes > place->length - offset) {
		drv_acpi_os_log(
			"ACPI: field access at 0x%llx past the region's 0x%llx bytes\n",
			(unsigned long long)offset,
			(unsigned long long)place->length);
		return EFAULT;
	}

	/* A data table region reads the table's bytes, which never change. */
	if (place->data != NULL) {
		error = table_region_read(place, offset, bytes, write, value);
		if (error != 0)
			return error;

		/* Succeeded: value holds the table's bytes. */
		return 0;
	}

	/* Refuses a space ACPI does not define. */
	if (place->space >= DRV_ACPI_SPACE_COUNT) {
		drv_acpi_os_log("ACPI: region of unknown space 0x%x\n", (unsigned)place->space);
		return ENODEV;
	}

	/* Refuses a space that has no handler. */
	handler = &region_handlers[place->space];
	if (handler->handler == NULL) {
		drv_acpi_os_log("ACPI: no handler for address space %u\n", (unsigned)place->space);
		return ENODEV;
	}

	/* Starts the access with every field zero, the PCI function among them. */
	kern_memset(&access, 0, sizeof(access));

	/* Finds the PCI function of a configuration region. */
	if (place->space == DRV_ACPI_SPACE_PCI_CONFIG) {
		error = region_resolve_pci(region);
		if (error != 0)
			return error;

		/* Names the function in the access. */
		access.pci_segment = place->pci_segment;
		access.pci_bus = place->pci_bus;
		access.pci_device = place->pci_device;
		access.pci_function = place->pci_function;
	}

	/* Hands the access to the handler. */
	access.address = place->offset + offset;
	access.width = bytes * 8U;
	access.write = write;
	error = handler->handler(&access, value, handler->argument);
	if (error != 0)
		return error;

	/* Succeeded: a read leaves the space's value in value. */
	return 0;
}

/* Reads one access unit of a data table region from the table's bytes. */
static int
table_region_read(
	const struct drv_acpi_region *place,
	uint64_t offset,
	unsigned bytes,
	bool write,
	uint64_t *value)
{
	const uint8_t *data;
	unsigned index;

	/* Refuses a write to a table. */
	if (write)
		return EPERM;

	/* Assembles the unit from the table's bytes, lowest first. */
	data = place->data + offset;
	*value = 0;
	for (index = 0; index < bytes; index++)
		*value |= (uint64_t)data[index] << (index * 8U);

	/* Succeeded: value holds the table's bytes. */
	return 0;
}

/*
 * Finds the PCI function a configuration region belongs to.
 *
 * The device that contains the region is the function; see
 * drv_acpi_pci_location().
 */
static int
region_resolve_pci(
	struct drv_acpi_object *region)
{
	struct drv_acpi_node *device;
	uint16_t segment;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
	int error;

	/* The function is found once. */
	if (region->value.region.pci_resolved)
		return 0;

	/* The device is the region's parent. */
	device = NULL;
	if (region->value.region.node != NULL)
		device = region->value.region.node->parent;

	/* Refuses a region outside any device. */
	if (device == NULL)
		return EINVAL;

	/* Finds the device's PCI function. */
	error = drv_acpi_pci_location(device, &segment, &bus, &slot, &function);
	if (error != 0)
		return error;

	/* Remembers the function; pci_resolved tells every later access it is known. */
	region->value.region.pci_segment = segment;
	region->value.region.pci_bus = bus;
	region->value.region.pci_device = slot;
	region->value.region.pci_function = function;
	region->value.region.pci_resolved = 1;

	/* Succeeded: the region knows its PCI function. */
	return 0;
}

/*
 * Evaluates an integer object found from a device by the search rules, or
 * leaves the value alone when there is none.  With own set, only an object
 * of the device itself counts.
 */
static int
evaluate_found(
	struct drv_acpi_node *device,
	const char *name,
	bool own,
	uint64_t *value)
{
	struct drv_acpi_node *found;
	int error;

	/* Searches from the device toward the root; a name no scope has leaves the value alone. */
	error = drv_acpi_lookup_path(device, name, true, &found);
	if (error == ENOENT)
		return 0;

	/* Reports a lookup that failed otherwise. */
	if (error != 0)
		return error;

	/* An object of another scope does not count when the device's own is asked for. */
	if (own && found->parent != device)
		return 0;

	/* Evaluates the object as an integer. */
	error = drv_acpi_evaluate_integer(found, NULL, value);
	if (error != 0)
		return error;

	/* Succeeded: value holds the object's integer. */
	return 0;
}

/* Finds the PCI host bridge (PNP0A03 or PNP0A08) above a device, or NULL. */
static struct drv_acpi_node *
pci_root_bridge(
	struct drv_acpi_node *device)
{
	struct drv_acpi_node *node;
	bool root;

	/* Looks at each scope above the device, nearest first. */
	for (node = device->parent;
	     node != NULL;
	     node = node->parent) {
		/* Only a device can be a host bridge. */
		if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_DEVICE)
			continue;

		/* A host bridge says so in its _HID. */
		root = pci_root_named(node, "_HID");
		if (root)
			return node;

		/* Or it says so in its _CID. */
		root = pci_root_named(node, "_CID");
		if (root)
			return node;
	}

	/* The device is not below a PCI host bridge. */
	return NULL;
}

/* Reports whether a device's own _HID or _CID names a PCI host bridge. */
static bool
pci_root_named(
	struct drv_acpi_node *node,
	const char *name)
{
	struct drv_acpi_node *found;
	struct drv_acpi_object *id;
	bool root;
	int error;

	/* Finds the device's own object of the name. */
	error = drv_acpi_lookup_path(node, name, false, &found);
	if (error != 0 || found == NULL)
		return false;

	/* Evaluates it; a _HID or _CID that fails names nothing. */
	id = NULL;
	error = drv_acpi_evaluate(found, NULL, NULL, 0, &id);
	if (error != 0 || id == NULL)
		return false;

	/* Compares the identifier with a host bridge's. */
	root = pci_root_id(id);

	/* The identifier is no longer needed. */
	drv_acpi_object_release(id);

	/* Reports whether the identifier is a host bridge's. */
	return root;
}

/* Reports whether an identifier, or a package of them (_CID), is a PCI host bridge's. */
static bool
pci_root_id(
	const struct drv_acpi_object *id)
{
	const struct drv_acpi_object *element;
	uint32_t index;
	bool root;
	int compared;

	/* Compares an identifier by its form. */
	switch (id->type) {
	case DRV_ACPI_TYPE_INTEGER:
		/* Compares an EISA identifier with PNP0A03, then with PNP0A08. */
		if (id->value.integer == PCI_ROOT_EISA_ID)
			return true;
		if (id->value.integer == PCIE_ROOT_EISA_ID)
			return true;
		break;
	case DRV_ACPI_TYPE_STRING:
		/* Compares a string identifier with PNP0A03. */
		compared = kern_strcmp(id->value.string.text, "PNP0A03");
		if (compared == 0)
			return true;

		/* Compares it with PNP0A08. */
		compared = kern_strcmp(id->value.string.text, "PNP0A08");
		if (compared == 0)
			return true;
		break;
	case DRV_ACPI_TYPE_PACKAGE:
		/* A _CID package lists several; any of them will do. */
		for (index = 0; index < id->value.package.count; index++) {
			/* Skips an empty slot and a nested package, which names nothing. */
			element = id->value.package.elements[index];
			if (element == NULL || element->type == DRV_ACPI_TYPE_PACKAGE)
				continue;

			/* Compares one identifier of the list. */
			root = pci_root_id(element);
			if (root)
				return true;
		}

		break;
	default:
		/* Any other object names no device. */
		break;
	}

	/* The identifier is some other device's. */
	return false;
}

/*
 * Follows the PCI-to-PCI bridges from a host bridge down to a device: each
 * device between them that has an _ADR is a function on the current bus,
 * and when its header says it is a bridge, the devices below it are on its
 * secondary bus.  A function that cannot be read leaves the bus as it is.
 */
static void
pci_follow_bridges(
	struct drv_acpi_node *root,
	struct drv_acpi_node *device,
	uint16_t segment,
	uint64_t *bus)
{
	struct drv_acpi_node *path[PCI_PATH_MAX];
	struct drv_acpi_node *node;
	struct drv_acpi_node *found;
	unsigned depth;
	uint64_t address;
	uint8_t header;
	uint8_t secondary;
	int error;

	/* Lists the scopes between the host bridge and the device, the device's parent first. */
	depth = 0;
	for (node = device->parent;
	     node != NULL && node != root;
	     node = node->parent) {
		/* Leaves a path too deep to follow on the host bridge's bus. */
		if (depth == PCI_PATH_MAX)
			return;

		/* Records the scope one level further up. */
		path[depth] = node;
		depth++;
	}

	/* Walks them from the host bridge down. */
	while (depth != 0) {
		/* A scope without its own _ADR is not a PCI function. */
		depth--;
		node = path[depth];
		error = drv_acpi_lookup_path(node, "_ADR", false, &found);
		if (error != 0 || found == NULL)
			continue;

		/* Reads the function's address; one that fails is not followed. */
		error = drv_acpi_evaluate_integer(found, NULL, &address);
		if (error != 0)
			continue;

		/* Reads whether the function is a bridge; an absent one reads all ones and is not. */
		error = pci_config_read8(segment, (uint8_t)*bus, address, PCI_HEADER_TYPE, &header);
		if (error != 0)
			return;

		/* Goes on past a function that is no bridge. */
		header &= PCI_HEADER_KIND;
		if (header != PCI_HEADER_BRIDGE && header != PCI_HEADER_CARDBUS)
			continue;

		/* The devices below the bridge are on its secondary bus. */
		error = pci_config_read8(segment, (uint8_t)*bus, address, PCI_SECONDARY_BUS, &secondary);
		if (error != 0)
			return;
		*bus = secondary;
	}
}

/* Reads one byte of a function's configuration space through the PCI_Config handler. */
static int
pci_config_read8(
	uint16_t segment,
	uint8_t bus,
	uint64_t address,
	unsigned offset,
	uint8_t *value)
{
	struct drv_acpi_region_access access;
	struct region_handler *handler;
	uint64_t read;
	int error;

	/* Refuses a read before the space has a handler. */
	handler = &region_handlers[DRV_ACPI_SPACE_PCI_CONFIG];
	if (handler->handler == NULL)
		return ENODEV;

	/* Describes the byte: the function is the _ADR's device and function. */
	kern_memset(&access, 0, sizeof(access));
	access.address = offset;
	access.width = 8;
	access.write = false;
	access.pci_segment = segment;
	access.pci_bus = bus;
	access.pci_device = (uint8_t)((address >> 16) & 0x1fU);
	access.pci_function = (uint8_t)(address & 0x07U);

	/* Reads the byte through the handler. */
	read = 0;
	error = handler->handler(&access, &read, handler->argument);
	if (error != 0)
		return error;

	/* Hands over the byte. */
	*value = (uint8_t)read;

	/* Succeeded: value holds the configuration byte. */
	return 0;
}

/*
 * Chooses the access width of a field in bytes.
 *
 * A field that asks for any access uses the narrowest width in which it
 * fits one aligned access inside the region, and bytes when none does.
 */
static unsigned
field_access_bytes(
	const struct drv_acpi_field *field,
	uint64_t region_length)
{
	uint64_t start;
	uint64_t end;
	unsigned bytes;

	/* Chooses by the access type. */
	switch (field->flags & DRV_ACPI_FIELD_ACCESS_MASK) {
	case DRV_ACPI_FIELD_ACCESS_WORD:
		return 2;
	case DRV_ACPI_FIELD_ACCESS_DWORD:
		return 4;
	case DRV_ACPI_FIELD_ACCESS_QWORD:
		return 8;
	case DRV_ACPI_FIELD_ACCESS_ANY:
		break;
	default:
		return 1;
	}

	/* Tries each width from the narrowest. */
	start = field->bit_offset;
	end = start + field->bit_length;
	for (bytes = 1;
	     bytes <= ACCESS_BYTES_MAX;
	     bytes *= 2U) {
		/* Takes the width when one aligned access holds the whole field. */
		if (start / (bytes * 8U) == (end - 1U) / (bytes * 8U) &&
		    (start / (bytes * 8U) + 1U) * bytes <= region_length)
			return bytes;
	}

	/* Falls back to byte accesses. */
	return 1;
}

/*
 * Chooses the access width of a field and the byte range of the accesses
 * that cover it.
 */
static int
field_span(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	unsigned *access,
	uint64_t *first,
	uint64_t *last)
{
	struct drv_acpi_field *unit;
	struct drv_acpi_object *region;
	uint64_t region_length;
	uint64_t end;
	int error;

	UNUSED_PARAMETER(eval);

	/* A region's length bounds the width; an index field has no region. */
	unit = &field->value.field;
	region_length = ~0ULL;
	if (unit->kind != DRV_ACPI_FIELD_INDEX) {
		/* Refuses a field whose region is not one. */
		region = unit->region->object;
		if (region == NULL || region->type != DRV_ACPI_TYPE_REGION)
			return EINVAL;

		/* Prepares the region, whose length is then known. */
		error = drv_acpi_region_prepare(region);
		if (error != 0)
			return error;
		region_length = region->value.region.length;
	}

	/* Refuses a field of no bits or too many. */
	if (unit->bit_length == 0 || unit->bit_length / 8U > FIELD_BYTES_MAX)
		return EINVAL;

	/* Refuses the buffer access that serial bus spaces need. */
	if ((unit->flags & DRV_ACPI_FIELD_ACCESS_MASK) == DRV_ACPI_FIELD_ACCESS_BUFFER) {
		drv_acpi_os_log("ACPI: buffer access fields are not supported\n");
		return ENOTSUP;
	}

	/* Covers the bits with aligned accesses. */
	*access = field_access_bytes(unit, region_length);
	*first = (unit->bit_offset / 8U) / *access * *access;
	end = ((uint64_t)unit->bit_offset + unit->bit_length + 7U) / 8U;
	*last = (end + *access - 1U) / *access * *access;

	/* Succeeded: the caller has the width and the range. */
	return 0;
}

/* Reads one access unit of a field. */
static int
unit_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	uint64_t byte_offset,
	unsigned bytes,
	uint64_t *value)
{
	struct drv_acpi_field *unit;
	uint64_t mask;
	int error;

	/* Chooses the access by the kind of field. */
	unit = &field->value.field;
	switch (unit->kind) {
	case DRV_ACPI_FIELD_INDEX:
		/* Selects the unit through the index. */
		error = named_field_write(eval, unit->index, byte_offset);
		if (error != 0)
			return error;

		/* Reads the data. */
		error = named_field_read(eval, unit->data, value);
		break;
	case DRV_ACPI_FIELD_BANK:
		/* Selects the bank. */
		error = named_field_write(eval, unit->index, unit->bank_value);
		if (error != 0)
			return error;

		/* Reads the region. */
		error = region_access(eval, unit->region->object, byte_offset, bytes, false, value);
		break;
	default:
		/* Reads the region. */
		error = region_access(eval, unit->region->object, byte_offset, bytes, false, value);
		break;
	}

	/* Reports a unit that could not be read. */
	if (error != 0)
		return error;

	/* Keeps only the unit's bytes. */
	mask = ~0ULL;
	if (bytes < 8U)
		mask = (1ULL << (bytes * 8U)) - 1U;
	*value &= mask;

	/* Succeeded: value holds the unit. */
	return 0;
}

/* Writes one access unit of a field. */
static int
unit_write(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	uint64_t byte_offset,
	unsigned bytes,
	uint64_t value)
{
	struct drv_acpi_field *unit;
	int error;

	/* Chooses the access by the kind of field. */
	unit = &field->value.field;
	switch (unit->kind) {
	case DRV_ACPI_FIELD_INDEX:
		/* Selects the unit through the index. */
		error = named_field_write(eval, unit->index, byte_offset);
		if (error != 0)
			return error;

		/* Writes the data. */
		error = named_field_write(eval, unit->data, value);
		break;
	case DRV_ACPI_FIELD_BANK:
		/* Selects the bank. */
		error = named_field_write(eval, unit->index, unit->bank_value);
		if (error != 0)
			return error;

		/* Writes the region. */
		error = region_access(eval, unit->region->object, byte_offset, bytes, true, &value);
		break;
	default:
		/* Writes the region. */
		error = region_access(eval, unit->region->object, byte_offset, bytes, true, &value);
		break;
	}

	/* Reports a unit that could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the unit holds the value. */
	return 0;
}

/* Makes the value of a field from its bits: an integer when it fits, else a buffer. */
static int
bits_to_object(
	const uint8_t *bytes,
	uint64_t bit_length,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	uint64_t value;
	uint64_t integer_bits;
	size_t size;
	size_t index;

	/* A field no wider than an integer reads as one; a wider one as a buffer. */
	size = (size_t)((bit_length + 7U) / 8U);
	integer_bits = drv_acpi_integer_bytes() * 8U;
	if (bit_length <= integer_bits) {
		/* Assembles the integer, lowest byte first. */
		value = 0;
		for (index = 0; index < size; index++)
			value |= (uint64_t)bytes[index] << (index * 8U);

		/* Makes the integer object. */
		object = drv_acpi_object_integer_new(value);
	} else {
		object = drv_acpi_object_buffer_new(bytes, size);
	}

	/* Reports a value that could not be allocated. */
	if (object == NULL)
		return ENOMEM;

	/* Hands over the value. */
	*result = object;

	/* Succeeded: result is the field's value. */
	return 0;
}

/*
 * Converts a value to a field's bits.
 *
 * An integer gives its bytes, anything else is converted to a buffer.
 * Bits the value does not have are zero.
 */
static int
object_to_bits(
	struct drv_acpi_object *value,
	uint64_t bit_length,
	uint8_t *bytes)
{
	struct drv_acpi_object *buffer;
	size_t size;
	size_t length;
	size_t index;
	int error;

	/* An integer is laid out lowest byte first. */
	size = (size_t)((bit_length + 7U) / 8U);
	if (value->type == DRV_ACPI_TYPE_INTEGER) {
		for (index = 0; index < size && index < 8U; index++)
			bytes[index] = (uint8_t)(value->value.integer >> (index * 8U));
		return 0;
	}

	/* Anything else becomes a buffer first. */
	error = drv_acpi_convert_buffer(value, &buffer);
	if (error != 0)
		return error;

	/* Copies as many bytes as the field has. */
	length = buffer->value.buffer.length;
	if (length > size)
		length = size;
	if (length != 0)
		kern_memcpy(bytes, buffer->value.buffer.bytes, length);

	/* The converted buffer is no longer needed. */
	drv_acpi_object_release(buffer);

	/* Succeeded: bytes holds the value's bits. */
	return 0;
}

/* Copies a run of bits out of a byte string into the front of another. */
static void
bits_extract(
	const uint8_t *source,
	uint64_t bit_offset,
	uint64_t bit_length,
	uint8_t *destination)
{
	uint64_t index;
	uint64_t from;

	/* Copies bit by bit; fields are short and this is not a fast path. */
	for (index = 0; index < bit_length; index++) {
		/* Sets the destination bit when the source bit is set. */
		from = bit_offset + index;
		if ((source[from / 8U] >> (from % 8U)) & 1U)
			destination[index / 8U] |= (uint8_t)(1U << (index % 8U));
	}

	/* Clears the bits above the run in the last destination byte. */
	if (bit_length % 8U != 0)
		destination[bit_length / 8U] &= (uint8_t)((1U << (bit_length % 8U)) - 1U);
}

/* Copies the front bits of a byte string into a run of bits of another. */
static void
bits_insert(
	uint8_t *destination,
	uint64_t bit_offset,
	uint64_t bit_length,
	const uint8_t *source)
{
	uint64_t index;
	uint64_t to;
	uint8_t mask;

	/* Copies bit by bit. */
	for (index = 0; index < bit_length; index++) {
		/* Sets or clears the destination bit as the source bit is. */
		to = bit_offset + index;
		mask = (uint8_t)(1U << (to % 8U));
		if ((source[index / 8U] >> (index % 8U)) & 1U) {
			destination[to / 8U] |= mask;
		} else {
			destination[to / 8U] &= (uint8_t)~mask;
		}
	}
}

/* Writes an integer into the field unit a node holds. */
static int
named_field_write(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *node,
	uint64_t value)
{
	struct drv_acpi_object *integer;
	struct drv_acpi_object *field;
	int error;

	/* Refuses a selector that is not a field unit. */
	field = node->object;
	if (field == NULL || field->type != DRV_ACPI_TYPE_FIELD_UNIT)
		return EINVAL;

	/* Makes the integer to write. */
	integer = drv_acpi_object_integer_new(value);
	if (integer == NULL)
		return ENOMEM;

	/* Writes it into the field. */
	error = drv_acpi_field_write(eval, field, integer);
	drv_acpi_object_release(integer);
	if (error != 0)
		return error;

	/* Succeeded: the field holds the value. */
	return 0;
}

/* Runs _REG (space, 1) for each region of one space not yet connected. */
static int
region_connect(
	enum drv_acpi_space space)
{
	unsigned wanted;
	int error;

	/* System memory and system I/O are always there and get no _REG. */
	if (space == DRV_ACPI_SPACE_SYSTEM_MEMORY || space == DRV_ACPI_SPACE_SYSTEM_IO)
		return 0;

	/* Visits every region in the namespace. */
	wanted = (unsigned)space;
	error = drv_acpi_walk(NULL, connect_visitor, &wanted);
	if (error != 0)
		return error;

	/* Succeeded: every region of the space is connected. */
	return 0;
}

/* Connects one region of the space the walk asks for. */
static int
connect_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct drv_acpi_object *region;
	unsigned space;
	int error;

	UNUSED_PARAMETER(depth);

	/* Only regions matter. */
	space = *(unsigned *)argument;
	region = node->object;
	if (region == NULL || region->type != DRV_ACPI_TYPE_REGION)
		return 0;

	/* Only regions of the wanted space that are not connected yet matter. */
	if (region->value.region.space != space || region->value.region.connected)
		return 0;

	/*
	 * The region is connected from now on, even when its _REG fails, so
	 * that the method runs once.
	 */
	region->value.region.connected = 1;

	/* Runs its _REG; a _REG that could not run is logged and the walk goes on to the other regions. */
	error = run_reg(node, space);
	if (error != 0)
		drv_acpi_os_log("ACPI: _REG for space %u could not run (error %d)\n", space, error);

	/* Goes on with the walk. */
	return 0;
}

/* Runs the _REG of the scope a region is in, if it has one. */
static int
run_reg(
	struct drv_acpi_node *region,
	unsigned space)
{
	struct drv_acpi_object *arguments[2];
	struct drv_acpi_object *result;
	struct drv_acpi_node *method;
	int error;

	/* _REG lives beside the region, in the device that contains it. */
	error = drv_acpi_lookup(region->parent, "_REG", &method);
	if (error != 0 || method->parent != region->parent)
		return 0;

	/* Makes the first argument, the space. */
	arguments[0] = drv_acpi_object_integer_new(space);
	if (arguments[0] == NULL)
		return ENOMEM;

	/* Makes the second argument, "connected". */
	arguments[1] = drv_acpi_object_integer_new(1);
	if (arguments[1] == NULL) {
		drv_acpi_object_release(arguments[0]);
		return ENOMEM;
	}

	/* Runs it; a failing _REG is logged and ignored, as firmware expects. */
	result = NULL;
	error = drv_acpi_evaluate(method, NULL, arguments, 2, &result);
	if (error != 0)
		drv_acpi_os_log("ACPI: _REG for space %u failed (error %d)\n", space, error);

	/* The result and the arguments are no longer needed. */
	drv_acpi_object_release(result);
	drv_acpi_object_release(arguments[0]);
	drv_acpi_object_release(arguments[1]);

	/* Succeeded: _REG ran, whatever it reported. */
	return 0;
}

/* Reads the field unit a node holds as an integer. */
static int
named_field_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *node,
	uint64_t *value)
{
	struct drv_acpi_object *field;
	struct drv_acpi_object *object;
	int error;

	/* Refuses a data register that is not a field unit. */
	field = node->object;
	if (field == NULL || field->type != DRV_ACPI_TYPE_FIELD_UNIT)
		return EINVAL;

	/* Reads the field. */
	error = drv_acpi_field_read(eval, field, &object);
	if (error != 0)
		return error;

	/* Converts its value to an integer. */
	error = drv_acpi_convert_integer(object, value);
	drv_acpi_object_release(object);
	if (error != 0)
		return error;

	/* Succeeded: value holds the field's integer. */
	return 0;
}
