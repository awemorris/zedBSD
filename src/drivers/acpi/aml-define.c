/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Named object definitions (ACPI 6.5 section 20.2.5.2): Name, Scope,
 * Device, Method, OperationRegion, the fields, and the rest, and the
 * Package and Buffer literals.
 *
 * A definition that names an existing node while a table loads is logged
 * and skipped, so that one duplicate in firmware does not lose the rest of
 * the table.  Inside a method it is an error.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * The largest buffer AML may create, which keeps a damaged size from
 * exhausting the kernel heap.
 */
#define BUFFER_SIZE_MAX (16U * 1024U * 1024U)

/*
 * The largest package AML may create, for the same reason.
 */
#define PACKAGE_COUNT_MAX (1024U * 1024U)

/*
 * The field list entries that are not named fields.
 */
#define FIELD_ENTRY_RESERVED	0x00U
#define FIELD_ENTRY_ACCESS	0x01U
#define FIELD_ENTRY_CONNECT	0x02U
#define FIELD_ENTRY_EXTENDED	0x03U

/*
 * The longest name text a log line or a name reference holds; a longer
 * name is written as "(long name)".
 */
#define NAME_TEXT_MAX 128U

static int define_name(struct drv_acpi_eval *eval);
static int define_alias(struct drv_acpi_eval *eval);
static int define_scope(struct drv_acpi_eval *eval);
static int define_container(struct drv_acpi_eval *eval, unsigned opcode);
static int container_object(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int read_processor(struct drv_acpi_eval *eval, struct drv_acpi_object *object);
static int read_power_resource(struct drv_acpi_eval *eval, struct drv_acpi_object *object);
static int define_method(struct drv_acpi_eval *eval);
static int define_external(struct drv_acpi_eval *eval);
static int define_mutex(struct drv_acpi_eval *eval);
static int define_event(struct drv_acpi_eval *eval);
static int define_region(struct drv_acpi_eval *eval);
static int region_arguments(struct drv_acpi_eval *eval, struct drv_acpi_object *region);
static int define_data_region(struct drv_acpi_eval *eval);
static int define_field(struct drv_acpi_eval *eval, unsigned opcode);
static int field_head(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_node **first, struct drv_acpi_node **second, uint64_t *bank_value, uint64_t *flags);
static int field_list(struct drv_acpi_eval *eval, const uint8_t *end, struct drv_acpi_field *template);
static int field_access(struct drv_acpi_eval *eval, bool extended, struct drv_acpi_field *template);
static int field_named(struct drv_acpi_eval *eval, const uint8_t *end, struct drv_acpi_field *template);
static bool segment_valid(const uint8_t *segment);
static bool segment_character(uint8_t character, bool first);
static int field_unit_create(struct drv_acpi_eval *eval, const uint8_t *segment, const struct drv_acpi_field *template);
static int field_connection(struct drv_acpi_eval *eval, struct drv_acpi_field *template);
static int lookup_named(struct drv_acpi_eval *eval, struct drv_acpi_node **result);
static int create_named(struct drv_acpi_eval *eval, const struct drv_acpi_name *name, struct drv_acpi_object *object, bool *duplicate);
static void name_text(const struct drv_acpi_name *name, char *text, size_t size);
static int package_element(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int name_reference(struct drv_acpi_eval *eval, const struct drv_acpi_name *name, struct drv_acpi_object **result);
static struct drv_acpi_table *table_find(const char *signature, const char *oem_id, const char *oem_table_id);
static int string_operand(struct drv_acpi_eval *eval, char *text, size_t size);

/*
 * Reports whether an opcode defines a named object.
 */
bool
drv_acpi_is_definition(
	unsigned opcode)
{
	/* Chooses by the opcode. */
	switch (opcode) {
	case DRV_ACPI_OP_NAME:
	case DRV_ACPI_OP_ALIAS:
	case DRV_ACPI_OP_SCOPE:
	case DRV_ACPI_OP_METHOD:
	case DRV_ACPI_OP_EXTERNAL:
	case DRV_ACPI_OP_MUTEX:
	case DRV_ACPI_OP_EVENT:
	case DRV_ACPI_OP_OPERATION_REGION:
	case DRV_ACPI_OP_FIELD:
	case DRV_ACPI_OP_INDEX_FIELD:
	case DRV_ACPI_OP_BANK_FIELD:
	case DRV_ACPI_OP_DEVICE:
	case DRV_ACPI_OP_PROCESSOR:
	case DRV_ACPI_OP_POWER_RESOURCE:
	case DRV_ACPI_OP_THERMAL_ZONE:
	case DRV_ACPI_OP_DATA_REGION:
	case DRV_ACPI_OP_CREATE_FIELD:
	case DRV_ACPI_OP_CREATE_BIT_FIELD:
	case DRV_ACPI_OP_CREATE_BYTE_FIELD:
	case DRV_ACPI_OP_CREATE_WORD_FIELD:
	case DRV_ACPI_OP_CREATE_DWORD_FIELD:
	case DRV_ACPI_OP_CREATE_QWORD_FIELD:
		return true;
	default:
		break;
	}

	/* Reports an opcode that is an operator or a statement, not a definition. */
	return false;
}

/*
 * Runs one definition whose opcode has been read.
 */
int
drv_acpi_define(
	struct drv_acpi_eval *eval,
	unsigned opcode)
{
	int error;

	/* Chooses the definition by its opcode. */
	switch (opcode) {
	case DRV_ACPI_OP_NAME:
		error = define_name(eval);
		break;
	case DRV_ACPI_OP_ALIAS:
		error = define_alias(eval);
		break;
	case DRV_ACPI_OP_SCOPE:
		error = define_scope(eval);
		break;
	case DRV_ACPI_OP_DEVICE:
	case DRV_ACPI_OP_PROCESSOR:
	case DRV_ACPI_OP_POWER_RESOURCE:
	case DRV_ACPI_OP_THERMAL_ZONE:
		error = define_container(eval, opcode);
		break;
	case DRV_ACPI_OP_METHOD:
		error = define_method(eval);
		break;
	case DRV_ACPI_OP_EXTERNAL:
		error = define_external(eval);
		break;
	case DRV_ACPI_OP_MUTEX:
		error = define_mutex(eval);
		break;
	case DRV_ACPI_OP_EVENT:
		error = define_event(eval);
		break;
	case DRV_ACPI_OP_OPERATION_REGION:
		error = define_region(eval);
		break;
	case DRV_ACPI_OP_DATA_REGION:
		error = define_data_region(eval);
		break;
	case DRV_ACPI_OP_FIELD:
	case DRV_ACPI_OP_INDEX_FIELD:
	case DRV_ACPI_OP_BANK_FIELD:
		error = define_field(eval, opcode);
		break;
	default:
		error = drv_acpi_create_buffer_field(eval, opcode);
		break;
	}

	/* Reports a failed definition. */
	if (error != 0)
		return error;

	/* Succeeded: the definition's name is in the namespace. */
	return 0;
}

/*
 * Builds a Package or VarPackage literal whose opcode has been read.
 *
 * An element that is a name becomes a reference to the named node, or a
 * name reference when the name does not resolve yet.
 */
int
drv_acpi_build_package(
	struct drv_acpi_eval *eval,
	bool variable,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *package;
	struct drv_acpi_object *element;
	const uint8_t *package_end;
	const uint8_t *outer_end;
	uint64_t count;
	uint32_t index;
	int error;

	/* Reads the extent of the literal. */
	error = drv_acpi_stream_package_length(eval, &package_end);
	if (error != 0)
		return error;

	/* Reads the element count, a byte or a TermArg, inside the extent. */
	outer_end = eval->end;
	eval->end = package_end;
	if (variable) {
		error = drv_acpi_eval_integer(eval, &count);
	} else {
		error = drv_acpi_stream_integer(eval, 1, &count);
	}

	/* Reports a count that could not be read. */
	if (error != 0) {
		eval->end = outer_end;
		return error;
	}

	/* Refuses a count too large to be real. */
	if (count > PACKAGE_COUNT_MAX) {
		eval->end = outer_end;
		return ENOMEM;
	}

	/* Allocates the package. */
	package = drv_acpi_object_package_new((uint32_t)count);
	if (package == NULL) {
		eval->end = outer_end;
		return ENOMEM;
	}

	/* Evaluates the listed elements; any beyond the count are dropped. */
	index = 0;
	while (eval->position < package_end) {
		/* Evaluates one element. */
		error = package_element(eval, &element);
		if (error != 0) {
			eval->end = outer_end;
			drv_acpi_object_release(package);
			return error;
		}

		/* Keeps it when the count leaves room for it. */
		if (index < package->value.package.count) {
			package->value.package.elements[index] = element;
		} else {
			drv_acpi_object_release(element);
		}

		/* Counts the listed element, kept or not. */
		index++;
	}

	/* Continues after the literal. */
	eval->end = outer_end;
	eval->position = package_end;

	/* Hands over the package. */
	*result = package;

	/* Succeeded: the caller holds the package. */
	return 0;
}

/*
 * Builds a Buffer literal whose opcode has been read.
 *
 * The buffer is as long as its size operand or its initializer, whichever
 * is longer; bytes the initializer does not cover are zero.
 */
int
drv_acpi_build_buffer(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *buffer;
	const uint8_t *buffer_end;
	const uint8_t *outer_end;
	uint64_t size;
	size_t initializer;
	int error;

	/* Reads the extent of the literal. */
	error = drv_acpi_stream_package_length(eval, &buffer_end);
	if (error != 0)
		return error;

	/* Evaluates the size inside the extent. */
	outer_end = eval->end;
	eval->end = buffer_end;
	error = drv_acpi_eval_integer(eval, &size);
	eval->end = outer_end;
	if (error != 0)
		return error;

	/* The initializer is whatever bytes remain in the extent. */
	initializer = (size_t)(buffer_end - eval->position);
	if (size < initializer)
		size = initializer;

	/* Refuses a size too large to be real. */
	if (size > BUFFER_SIZE_MAX)
		return ENOMEM;

	/* Allocates the zero-filled buffer. */
	buffer = drv_acpi_object_buffer_new(NULL, (size_t)size);
	if (buffer == NULL)
		return ENOMEM;

	/* Copies the initializer. */
	if (initializer != 0)
		kern_memcpy(buffer->value.buffer.bytes, eval->position, initializer);

	/* Continues after the literal and hands over the buffer. */
	eval->position = buffer_end;
	*result = buffer;

	/* Succeeded: the caller holds the buffer. */
	return 0;
}

/*
 * Runs a CreateField or a Create*Field whose opcode has been read.
 */
int
drv_acpi_create_buffer_field(
	struct drv_acpi_eval *eval,
	unsigned opcode)
{
	struct drv_acpi_object *source;
	struct drv_acpi_object *field;
	struct drv_acpi_name name;
	uint64_t index;
	uint64_t bits;
	uint64_t offset;
	uint64_t available;
	bool duplicate;
	int error;

	/* Evaluates the buffer the field is made in. */
	error = drv_acpi_eval_data(eval, &source);
	if (error != 0)
		return error;

	/* Evaluates the index of the field. */
	error = drv_acpi_eval_integer(eval, &index);
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* Places the field by the kind of Create: most take a byte index. */
	offset = index * 8U;
	bits = 0;
	switch (opcode) {
	case DRV_ACPI_OP_CREATE_BIT_FIELD:
		/* One bit at a bit index. */
		offset = index;
		bits = 1;
		break;
	case DRV_ACPI_OP_CREATE_BYTE_FIELD:
		bits = 8;
		break;
	case DRV_ACPI_OP_CREATE_WORD_FIELD:
		bits = 16;
		break;
	case DRV_ACPI_OP_CREATE_DWORD_FIELD:
		bits = 32;
		break;
	case DRV_ACPI_OP_CREATE_QWORD_FIELD:
		bits = 64;
		break;
	default:
		/* CreateField takes a bit index and a bit count. */
		offset = index;
		error = drv_acpi_eval_integer(eval, &bits);
		if (error != 0) {
			drv_acpi_object_release(source);
			return error;
		}

		break;
	}

	/* Reads the name of the field. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* Refuses a source that is not a buffer. */
	if (source->type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_os_log("ACPI: buffer field over an object of type %u\n", (unsigned)source->type);
		drv_acpi_object_release(source);
		return EINVAL;
	}

	/* Refuses an empty field or one that runs past the buffer. */
	available = (uint64_t)source->value.buffer.length * 8U;
	if (bits == 0 ||
	    offset > available ||
	    bits > available - offset) {
		drv_acpi_os_log("ACPI: buffer field past the end of its buffer\n");
		drv_acpi_object_release(source);
		return EINVAL;
	}

	/* Allocates the field. */
	field = drv_acpi_object_new(DRV_ACPI_TYPE_BUFFER_FIELD);
	if (field == NULL) {
		drv_acpi_object_release(source);
		return ENOMEM;
	}

	/* Places the field; it takes over the reference to the buffer, which it keeps alive. */
	field->value.buffer_field.buffer = source;
	field->value.buffer_field.bit_offset = offset;
	field->value.buffer_field.bit_length = bits;

	/* A CreateField field always reads as a buffer. */
	if (opcode == DRV_ACPI_OP_CREATE_FIELD)
		field->value.buffer_field.reads_buffer = true;

	/* Names it. */
	error = create_named(eval, &name, field, &duplicate);
	drv_acpi_object_release(field);
	if (error != 0)
		return error;

	/* Succeeded: the field's name is in the namespace. */
	return 0;
}

/*
 * Resolves the names a package holds that did not exist when it was built.
 *
 * It runs once the tables are loaded, on nested packages too.  A name that
 * still does not resolve stays a name.
 */
void
drv_acpi_package_resolve(
	struct drv_acpi_object *package)
{
	struct drv_acpi_object *element;
	uint32_t index;

	/* Looks at each element. */
	for (index = 0; index < package->value.package.count; index++) {
		/* Skips an uninitialized element. */
		element = package->value.package.elements[index];
		if (element == NULL)
			continue;

		/* A nested package is resolved the same way. */
		if (element->type == DRV_ACPI_TYPE_PACKAGE) {
			drv_acpi_package_resolve(element);
			continue;
		}

		/* A name reference becomes a node reference when its name resolves. */
		if (element->type == DRV_ACPI_TYPE_REFERENCE)
			drv_acpi_reference_resolve(element);
	}
}

/*
 * Turns a name reference into a node reference when its name resolves.
 *
 * The name is resolved from the scope it was written in.
 */
void
drv_acpi_reference_resolve(
	struct drv_acpi_object *reference)
{
	struct drv_acpi_node *node;
	int error;

	/* Only a name reference has anything to resolve. */
	if (reference->value.reference.kind != DRV_ACPI_REFERENCE_NAME)
		return;

	/* Looks the name up from its scope. */
	error = drv_acpi_lookup_path(reference->value.reference.node, reference->value.reference.name, true, &node);
	if (error != 0)
		return;

	/* The reference now names the node. */
	reference->value.reference.kind = DRV_ACPI_REFERENCE_NODE;
	reference->value.reference.node = node;

	/* The text is no longer needed. */
	drv_acpi_os_free(reference->value.reference.name);
	reference->value.reference.name = NULL;
}

/* Runs a Name: evaluates the value and gives it to a new node. */
static int
define_name(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *value;
	struct drv_acpi_object *copy;
	struct drv_acpi_name name;
	bool duplicate;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Evaluates the value. */
	error = drv_acpi_eval_term_arg(eval, &value);
	if (error != 0)
		return error;

	/* A value someone else also holds is copied, so the name owns its own. */
	copy = value;
	if (value->references > 1) {
		error = drv_acpi_object_copy(value, &copy);
		drv_acpi_object_release(value);
		if (error != 0)
			return error;
	}

	/* Names the value. */
	error = create_named(eval, &name, copy, &duplicate);
	drv_acpi_object_release(copy);
	if (error != 0)
		return error;

	/* Succeeded: the name holds the value. */
	return 0;
}

/* Runs an Alias: a second name for an existing node. */
static int
define_alias(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *alias;
	struct drv_acpi_name source;
	struct drv_acpi_name name;
	struct drv_acpi_node *target;
	bool duplicate;
	int error;

	/* Reads the existing name. */
	error = drv_acpi_stream_name(eval, &source);
	if (error != 0)
		return error;

	/* Reads the new name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Resolves the existing name. */
	error = drv_acpi_ns_lookup(eval->scope, &source, true, &target);
	if (error != 0) {
		drv_acpi_os_log("ACPI: alias of a name that does not exist\n");

		/* A table load goes on without the alias. */
		if (eval->frame == NULL)
			return 0;

		/* Reports a missing name inside a method. */
		return error;
	}

	/* Allocates the alias object. */
	alias = drv_acpi_object_new(DRV_ACPI_TYPE_ALIAS);
	if (alias == NULL)
		return ENOMEM;

	/* Makes it stand for the existing node. */
	alias->value.alias.target = target;

	/* Names it. */
	error = create_named(eval, &name, alias, &duplicate);
	drv_acpi_object_release(alias);
	if (error != 0)
		return error;

	/* Succeeded: the new name stands for the node. */
	return 0;
}

/* Runs a Scope: the terms inside run in an existing node's scope. */
static int
define_scope(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_node *node;
	const uint8_t *scope_end;
	int error;

	/* Reads the extent of the scope. */
	error = drv_acpi_stream_package_length(eval, &scope_end);
	if (error != 0)
		return error;

	/* Resolves the scope's node; a table load skips a scope whose node is missing. */
	error = lookup_named(eval, &node);
	if (error == ENOENT && eval->frame == NULL) {
		drv_acpi_os_log("ACPI: scope target does not exist; its terms are skipped\n");
		eval->position = scope_end;
		return 0;
	} else if (error != 0) {
		return error;
	}

	/* Runs the terms inside it. */
	error = drv_acpi_exec_scope(eval, node, scope_end);
	if (error != 0)
		return error;

	/* Succeeded: the scope's terms ran. */
	return 0;
}

/* Runs a Device, Processor, PowerResource or ThermalZone. */
static int
define_container(
	struct drv_acpi_eval *eval,
	unsigned opcode)
{
	struct drv_acpi_object *object;
	struct drv_acpi_node *node;
	struct drv_acpi_name name;
	const uint8_t *container_end;
	bool duplicate;
	int error;

	/* Reads the extent of the definition. */
	error = drv_acpi_stream_package_length(eval, &container_end);
	if (error != 0)
		return error;

	/* Reads its name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Makes the object and reads the fixed data some kinds carry. */
	error = container_object(eval, opcode, &object);
	if (error != 0)
		return error;

	/* Names it. */
	error = create_named(eval, &name, object, &duplicate);
	drv_acpi_object_release(object);
	if (error != 0)
		return error;

	/* A duplicate's terms are skipped with it. */
	if (duplicate) {
		eval->position = container_end;
		return 0;
	}

	/* Finds the node just created, which is the scope of the terms inside. */
	error = drv_acpi_ns_lookup(eval->scope, &name, false, &node);
	if (error != 0)
		return error;

	/* Runs the terms inside it in its own scope. */
	error = drv_acpi_exec_scope(eval, node, container_end);
	if (error != 0)
		return error;

	/* Succeeded: the container and the terms inside it are defined. */
	return 0;
}

/* Makes the object of a container definition and reads its fixed data. */
static int
container_object(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	enum drv_acpi_type type;
	int error;

	/* Chooses the type by the opcode. */
	switch (opcode) {
	case DRV_ACPI_OP_DEVICE:
		type = DRV_ACPI_TYPE_DEVICE;
		break;
	case DRV_ACPI_OP_THERMAL_ZONE:
		type = DRV_ACPI_TYPE_THERMAL_ZONE;
		break;
	case DRV_ACPI_OP_PROCESSOR:
		type = DRV_ACPI_TYPE_PROCESSOR;
		break;
	default:
		type = DRV_ACPI_TYPE_POWER_RESOURCE;
		break;
	}

	/* Allocates the object. */
	object = drv_acpi_object_new(type);
	if (object == NULL)
		return ENOMEM;

	/* A Processor and a PowerResource carry fixed data; the others carry none. */
	error = 0;
	if (type == DRV_ACPI_TYPE_PROCESSOR) {
		error = read_processor(eval, object);
	} else if (type == DRV_ACPI_TYPE_POWER_RESOURCE) {
		error = read_power_resource(eval, object);
	}

	/* Reports fixed data that could not be read. */
	if (error != 0) {
		drv_acpi_object_release(object);
		return error;
	}

	/* Hands over the object. */
	*result = object;

	/* Succeeded: the caller holds the object. */
	return 0;
}

/* Reads the fixed data of a Processor: its ID and its P_BLK address and length. */
static int
read_processor(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *object)
{
	uint64_t value;
	int error;

	/* Reads the processor ID. */
	error = drv_acpi_stream_integer(eval, 1, &value);
	if (error != 0)
		return error;
	object->value.processor.id = (uint8_t)value;

	/* Reads the address of the processor's control block. */
	error = drv_acpi_stream_integer(eval, 4, &value);
	if (error != 0)
		return error;
	object->value.processor.block_address = (uint32_t)value;

	/* Reads the length of the control block. */
	error = drv_acpi_stream_integer(eval, 1, &value);
	if (error != 0)
		return error;
	object->value.processor.block_length = (uint8_t)value;

	/* Succeeded: the object holds the processor's data. */
	return 0;
}

/* Reads the fixed data of a PowerResource: its system level and order. */
static int
read_power_resource(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *object)
{
	uint64_t value;
	int error;

	/* Reads the deepest system sleep state in which the resource stays on. */
	error = drv_acpi_stream_integer(eval, 1, &value);
	if (error != 0)
		return error;
	object->value.power.system_level = (uint8_t)value;

	/* Reads the order in which resources are turned on and off. */
	error = drv_acpi_stream_integer(eval, 2, &value);
	if (error != 0)
		return error;
	object->value.power.resource_order = (uint16_t)value;

	/* Succeeded: the object holds the resource's data. */
	return 0;
}

/* Runs a Method: records where its body is without running it. */
static int
define_method(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *method;
	struct drv_acpi_name name;
	const uint8_t *method_end;
	uint64_t flags;
	bool duplicate;
	int error;

	/* Reads the extent. */
	error = drv_acpi_stream_package_length(eval, &method_end);
	if (error != 0)
		return error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Reads the flags. */
	error = drv_acpi_stream_integer(eval, 1, &flags);
	if (error != 0)
		return error;

	/* Allocates the method object. */
	method = drv_acpi_object_new(DRV_ACPI_TYPE_METHOD);
	if (method == NULL)
		return ENOMEM;

	/* Points it at the body and decodes the flags. */
	method->value.method.start = eval->position;
	method->value.method.end = method_end;
	method->value.method.table = eval->table;
	method->value.method.argument_count = (uint8_t)(flags & 0x07U);
	method->value.method.serialized = (uint8_t)((flags >> 3) & 0x01U);
	method->value.method.sync_level = (uint8_t)((flags >> 4) & 0x0fU);

	/* Continues after the body. */
	eval->position = method_end;

	/* Names it. */
	error = create_named(eval, &name, method, &duplicate);
	drv_acpi_object_release(method);
	if (error != 0)
		return error;

	/* Succeeded: the method can be called by its name. */
	return 0;
}

/* Runs an External: a declaration for the compiler, with no effect here. */
static int
define_external(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_name name;
	uint64_t ignored;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Reads the object type and the argument count, which nothing uses. */
	error = drv_acpi_stream_integer(eval, 2, &ignored);
	if (error != 0)
		return error;

	/* Succeeded: the stream is past the declaration. */
	return 0;
}

/* Runs a Mutex. */
static int
define_mutex(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *mutex;
	struct drv_acpi_name name;
	uint64_t flags;
	bool duplicate;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Reads the sync level. */
	error = drv_acpi_stream_integer(eval, 1, &flags);
	if (error != 0)
		return error;

	/* Allocates the free mutex. */
	mutex = drv_acpi_object_new(DRV_ACPI_TYPE_MUTEX);
	if (mutex == NULL)
		return ENOMEM;

	/* Gives it its sync level. */
	mutex->value.mutex.sync_level = (uint8_t)(flags & 0x0fU);

	/* Names it. */
	error = create_named(eval, &name, mutex, &duplicate);
	drv_acpi_object_release(mutex);
	if (error != 0)
		return error;

	/* Succeeded: the mutex can be acquired by its name. */
	return 0;
}

/* Runs an Event. */
static int
define_event(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *event;
	struct drv_acpi_name name;
	bool duplicate;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Allocates the unsignaled event. */
	event = drv_acpi_object_new(DRV_ACPI_TYPE_EVENT);
	if (event == NULL)
		return ENOMEM;

	/* Names it. */
	error = create_named(eval, &name, event, &duplicate);
	drv_acpi_object_release(event);
	if (error != 0)
		return error;

	/* Succeeded: the event can be signaled by its name. */
	return 0;
}

/*
 * Runs an OperationRegion.
 *
 * While a table loads, the offset and length stay AML to be evaluated on
 * first use.  Inside a method they may use locals and arguments, so they
 * are evaluated at once.
 */
static int
define_region(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *region;
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	uint64_t space;
	bool duplicate;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Reads the address space. */
	error = drv_acpi_stream_integer(eval, 1, &space);
	if (error != 0)
		return error;

	/* Allocates the region object. */
	region = drv_acpi_object_new(DRV_ACPI_TYPE_REGION);
	if (region == NULL)
		return ENOMEM;

	/* Records its space and where its offset and length are evaluated. */
	region->value.region.space = (uint8_t)space;
	region->value.region.table = eval->table;
	region->value.region.scope = eval->scope;
	region->value.region.arguments_start = eval->position;

	/* Evaluates or steps over the offset and the length. */
	error = region_arguments(eval, region);
	if (error != 0) {
		drv_acpi_object_release(region);
		return error;
	}

	/* Names it. */
	error = create_named(eval, &name, region, &duplicate);
	if (error != 0) {
		drv_acpi_object_release(region);
		return error;
	}

	/* A duplicate that a table load skipped leaves the first region as it is. */
	if (duplicate) {
		drv_acpi_object_release(region);
		return 0;
	}

	/* The region remembers its node, which gives a PCI region its device. */
	error = drv_acpi_ns_lookup(eval->scope, &name, false, &node);
	if (error == 0)
		region->value.region.node = node;

	/* The node holds the region; this reference goes. */
	drv_acpi_object_release(region);

	/* Succeeded: the region can be used by its name. */
	return 0;
}

/*
 * Evaluates the offset and length of a region defined in a method, or
 * steps over them for a region defined while a table loads.
 */
static int
region_arguments(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *region)
{
	int error;

	/* Inside a method the arguments may use locals, so they run now. */
	if (eval->frame != NULL) {
		/* Evaluates the offset. */
		error = drv_acpi_eval_integer(eval, &region->value.region.offset);
		if (error != 0)
			return error;

		/* Evaluates the length. */
		error = drv_acpi_eval_integer(eval, &region->value.region.length);
		if (error != 0)
			return error;
	} else {
		/* Steps over the offset. */
		error = drv_acpi_skip_term_arg(eval);
		if (error != 0)
			return error;

		/* Steps over the length. */
		error = drv_acpi_skip_term_arg(eval);
		if (error != 0)
			return error;
	}

	/*
	 * evaluated tells the first access that the region has its place
	 * already; otherwise the AML between the start and here runs then.
	 */
	if (eval->frame != NULL)
		region->value.region.evaluated = 1;
	region->value.region.arguments_end = eval->position;

	/* Succeeded: the region has its place, or knows the AML that gives it. */
	return 0;
}

/* Runs a DataTableRegion: a region over the bytes of a loaded table. */
static int
define_data_region(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *region;
	struct drv_acpi_table *table;
	struct drv_acpi_name name;
	char signature[8];
	char oem_id[16];
	char oem_table_id[16];
	bool duplicate;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Evaluates the signature of the table. */
	error = string_operand(eval, signature, sizeof(signature));
	if (error != 0)
		return error;

	/* Evaluates its OEM ID. */
	error = string_operand(eval, oem_id, sizeof(oem_id));
	if (error != 0)
		return error;

	/* Evaluates its OEM table ID. */
	error = string_operand(eval, oem_table_id, sizeof(oem_table_id));
	if (error != 0)
		return error;

	/* Finds the table. */
	table = table_find(signature, oem_id, oem_table_id);
	if (table == NULL) {
		drv_acpi_os_log("ACPI: DataTableRegion names table %s that is not loaded\n", signature);
		return ENOENT;
	}

	/* Allocates the region. */
	region = drv_acpi_object_new(DRV_ACPI_TYPE_REGION);
	if (region == NULL)
		return ENOMEM;

	/*
	 * Lays the region over the table's bytes; evaluated tells every
	 * access that it has its place, as it has no AML to evaluate.
	 */
	region->value.region.space = DRV_ACPI_SPACE_SYSTEM_MEMORY;
	region->value.region.data = table->data;
	region->value.region.length = table->length;
	region->value.region.evaluated = 1;

	/* Names it. */
	error = create_named(eval, &name, region, &duplicate);
	drv_acpi_object_release(region);
	if (error != 0)
		return error;

	/* Succeeded: the region can be used by its name. */
	return 0;
}

/* Runs a Field, IndexField or BankField. */
static int
define_field(
	struct drv_acpi_eval *eval,
	unsigned opcode)
{
	struct drv_acpi_field template;
	struct drv_acpi_node *first;
	struct drv_acpi_node *second;
	const uint8_t *field_end;
	const uint8_t *outer_end;
	uint64_t flags;
	uint64_t bank_value;
	int error;

	/* Reads the extent of the field list. */
	error = drv_acpi_stream_package_length(eval, &field_end);
	if (error != 0)
		return error;

	/* Starts the template every unit of the list copies empty. */
	kern_memset(&template, 0, sizeof(template));

	/* Reads the names, the bank value and the flags that head the list, inside its extent. */
	outer_end = eval->end;
	eval->end = field_end;
	error = field_head(eval, opcode, &first, &second, &bank_value, &flags);
	eval->end = outer_end;

	/* A table load skips a field list whose region is missing. */
	if (error == ENOENT && eval->frame == NULL) {
		drv_acpi_os_log("ACPI: field list of a missing region is skipped\n");
		eval->position = field_end;
		return 0;
	} else if (error != 0) {
		return error;
	}

	/* Fills in the flags and the bank value every field unit of the list shares. */
	template.flags = (uint8_t)flags;
	template.bank_value = bank_value;

	/* Fills in the kind and the nodes the units reach their bits through. */
	if (opcode == DRV_ACPI_OP_FIELD) {
		template.kind = DRV_ACPI_FIELD_REGION;
		template.region = first;
	} else if (opcode == DRV_ACPI_OP_INDEX_FIELD) {
		template.kind = DRV_ACPI_FIELD_INDEX;
		template.index = first;
		template.data = second;
	} else {
		template.kind = DRV_ACPI_FIELD_BANK;
		template.region = first;
		template.index = second;
	}

	/* Creates the field units inside the list's extent. */
	eval->end = field_end;
	error = field_list(eval, field_end, &template);
	eval->end = outer_end;

	/* The template's connection is done with; each unit holds its own. */
	drv_acpi_object_release(template.connection);

	/* Reports a list that could not be read. */
	if (error != 0)
		return error;

	/* Continues after the list. */
	eval->position = field_end;

	/* Succeeded: every field unit of the list is defined. */
	return 0;
}

/*
 * Reads what heads a field list: the region (or the index field), the data
 * field of an IndexField, the bank field and value of a BankField, and the
 * flags.
 */
static int
field_head(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_node **first,
	struct drv_acpi_node **second,
	uint64_t *bank_value,
	uint64_t *flags)
{
	int error;

	/* Starts with no second node and no bank value, which a plain Field does not have. */
	*second = NULL;
	*bank_value = 0;

	/* Resolves the region, or the index field. */
	error = lookup_named(eval, first);
	if (error != 0)
		return error;

	/* An IndexField also names its data field, and a BankField its bank field. */
	if (opcode != DRV_ACPI_OP_FIELD) {
		error = lookup_named(eval, second);
		if (error != 0)
			return error;
	}

	/* A BankField has the value that selects its bank. */
	if (opcode == DRV_ACPI_OP_BANK_FIELD) {
		error = drv_acpi_eval_integer(eval, bank_value);
		if (error != 0)
			return error;
	}

	/* Reads the flags. */
	error = drv_acpi_stream_integer(eval, 1, flags);
	if (error != 0)
		return error;

	/* Succeeded: the caller has what heads the list. */
	return 0;
}

/* Reads a field list and creates a field unit for each named entry. */
static int
field_list(
	struct drv_acpi_eval *eval,
	const uint8_t *end,
	struct drv_acpi_field *template)
{
	uint32_t length;
	uint8_t byte;
	int error;

	/* Reads the entries in order, keeping the running bit offset. */
	template->bit_offset = 0;
	while (eval->position < end) {
		/* Reads the byte that says what kind of entry follows. */
		error = drv_acpi_stream_byte(eval, &byte);
		if (error != 0)
			return error;

		/* Chooses the entry by its first byte. */
		switch (byte) {
		case FIELD_ENTRY_RESERVED:
			/* Unnamed bits: the offset moves on. */
			error = drv_acpi_stream_field_length(eval, &length);
			if (error != 0)
				break;
			template->bit_offset += length;
			break;
		case FIELD_ENTRY_ACCESS:
			/* A new access type and attribute for the entries after it. */
			error = field_access(eval, false, template);
			break;
		case FIELD_ENTRY_EXTENDED:
			/* The same with an access length. */
			error = field_access(eval, true, template);
			break;
		case FIELD_ENTRY_CONNECT:
			/* The connection resource for the entries after it. */
			error = field_connection(eval, template);
			break;
		default:
			/* A named field, whose first character is the byte read. */
			error = field_named(eval, end, template);
			break;
		}

		/* Stops at the first entry that could not be read. */
		if (error != 0)
			return error;
	}

	/* Succeeded: every entry of the list was read. */
	return 0;
}

/* Reads an AccessAs entry, extended or not, into the template. */
static int
field_access(
	struct drv_acpi_eval *eval,
	bool extended,
	struct drv_acpi_field *template)
{
	uint64_t access_type;
	uint64_t attribute;
	uint64_t access_length;
	int error;

	/* Reads the access type. */
	error = drv_acpi_stream_integer(eval, 1, &access_type);
	if (error != 0)
		return error;

	/* Reads the access attribute. */
	error = drv_acpi_stream_integer(eval, 1, &attribute);
	if (error != 0)
		return error;

	/* Reads the access length of an extended entry; the plain one has none. */
	access_length = 0;
	if (extended) {
		error = drv_acpi_stream_integer(eval, 1, &access_length);
		if (error != 0)
			return error;
	}

	/* Replaces the access type in the flags, and the attribute and length. */
	template->flags = (uint8_t)((template->flags & ~DRV_ACPI_FIELD_ACCESS_MASK) |
	    (access_type & DRV_ACPI_FIELD_ACCESS_MASK));
	template->access_attribute = (uint8_t)attribute;
	template->access_length = (uint8_t)access_length;

	/* Succeeded: the entries after this one use the new access. */
	return 0;
}

/* Reads a named field entry, whose first character was read, and creates its unit. */
static int
field_named(
	struct drv_acpi_eval *eval,
	const uint8_t *end,
	struct drv_acpi_field *template)
{
	const uint8_t *segment;
	uint32_t length;
	bool valid;
	int error;

	/* Refuses a segment that runs past the list. */
	segment = eval->position - 1;
	if ((size_t)(end - segment) < 4U)
		return EIO;

	/* Refuses a segment with characters no name may contain, as the stream's names do. */
	valid = segment_valid(segment);
	if (!valid)
		return EIO;

	/* Reads the field's length after the segment. */
	eval->position = segment + 4;
	error = drv_acpi_stream_field_length(eval, &length);
	if (error != 0)
		return error;

	/* Creates the unit. */
	template->bit_length = length;
	error = field_unit_create(eval, segment, template);
	if (error != 0)
		return error;

	/* Moves past its bits. */
	template->bit_offset += length;

	/* Succeeded: the unit is defined. */
	return 0;
}

/* Reports whether four characters form a name segment. */
static bool
segment_valid(
	const uint8_t *segment)
{
	unsigned index;
	bool valid;

	/* The first character is an upper-case letter or an underscore. */
	valid = segment_character(segment[0], true);
	if (!valid)
		return false;

	/* The others may also be digits. */
	for (index = 1; index < 4U; index++) {
		/* Refuses a character no name may contain. */
		valid = segment_character(segment[index], false);
		if (!valid)
			return false;
	}

	/* Reports a valid segment. */
	return true;
}

/* Reports whether a character may stand in a name segment, first or later. */
static bool
segment_character(
	uint8_t character,
	bool first)
{
	/* An upper-case letter may stand anywhere. */
	if (character >= 'A' && character <= 'Z')
		return true;

	/* So may the underscore. */
	if (character == '_')
		return true;

	/* A digit may stand anywhere but first. */
	if (!first && character >= '0' && character <= '9')
		return true;

	/* Reports a character no name may contain there. */
	return false;
}

/* Creates one field unit from the template at the current offset. */
static int
field_unit_create(
	struct drv_acpi_eval *eval,
	const uint8_t *segment,
	const struct drv_acpi_field *template)
{
	struct drv_acpi_object *field;
	struct drv_acpi_name name;
	bool duplicate;
	int error;

	/* Allocates the field unit object. */
	field = drv_acpi_object_new(DRV_ACPI_TYPE_FIELD_UNIT);
	if (field == NULL)
		return ENOMEM;

	/* Copies the template. */
	field->value.field = *template;

	/* Shares the connection resource with the other units. */
	if (field->value.field.connection != NULL)
		drv_acpi_object_ref(field->value.field.connection);

	/* Names it in the current scope. */
	kern_memset(&name, 0, sizeof(name));
	name.segments = segment;
	name.count = 1;
	error = create_named(eval, &name, field, &duplicate);
	drv_acpi_object_release(field);
	if (error != 0)
		return error;

	/* Succeeded: the unit can be used by its name. */
	return 0;
}

/* Reads the resource of a Connection entry: a buffer or a name. */
static int
field_connection(
	struct drv_acpi_eval *eval,
	struct drv_acpi_field *template)
{
	struct drv_acpi_object *connection;
	struct drv_acpi_node *node;
	uint8_t byte;
	int error;

	/* Looks at how the resource is given. */
	error = drv_acpi_stream_peek(eval, &byte);
	if (error != 0)
		return error;

	/* A buffer is evaluated; a name is resolved to its object. */
	if (byte == DRV_ACPI_OP_BUFFER) {
		error = drv_acpi_eval_term_arg(eval, &connection);
		if (error != 0)
			return error;
	} else {
		/* Resolves the name. */
		error = lookup_named(eval, &node);
		if (error != 0)
			return error;

		/* Refuses a name without an object. */
		connection = node->object;
		if (connection == NULL)
			return EINVAL;

		/* The template takes a reference of its own. */
		drv_acpi_object_ref(connection);
	}

	/* Replaces the previous connection. */
	drv_acpi_object_release(template->connection);
	template->connection = connection;

	/* Succeeded: the entries after this one use the connection. */
	return 0;
}

/* Reads a name and resolves it with the search rules of references. */
static int
lookup_named(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node **result)
{
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	char text[NAME_TEXT_MAX];
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Resolves it; a missing name is logged. */
	error = drv_acpi_ns_lookup(eval->scope, &name, true, &node);
	if (error != 0) {
		name_text(&name, text, sizeof(text));
		drv_acpi_os_log("ACPI: %s does not exist\n", text);
		return error;
	}

	/* Hands over the node; an alias stands for its node. */
	*result = drv_acpi_ns_resolve_alias(node);

	/* Succeeded: result names the node. */
	return 0;
}

/*
 * Creates a node for a definition and gives it the object.
 *
 * A duplicate while a table loads is logged and reported through
 * duplicate with success; inside a method it is an error.
 */
static int
create_named(
	struct drv_acpi_eval *eval,
	const struct drv_acpi_name *name,
	struct drv_acpi_object *object,
	bool *duplicate)
{
	struct drv_acpi_node *node;
	char text[NAME_TEXT_MAX];
	int error;

	/* Creates the node. */
	*duplicate = false;
	error = drv_acpi_ns_create(eval, name, &node);
	if (error != 0) {
		/* Logs the name that is defined twice or whose scope is missing. */
		name_text(name, text, sizeof(text));
		drv_acpi_os_log("ACPI: %s cannot be created (error %d)\n", text, error);

		/* A table load goes on without the second definition. */
		if (error == EEXIST && eval->frame == NULL) {
			*duplicate = true;
			return 0;
		}

		/* Reports why the node could not be created. */
		return error;
	}

	/* The node takes the object. */
	drv_acpi_ns_attach(node, object);

	/* Succeeded: the node holds the object. */
	return 0;
}

/* Writes a NameString in text, or "(long name)" when it does not fit. */
static void
name_text(
	const struct drv_acpi_name *name,
	char *text,
	size_t size)
{
	int text_error;

	/* Writes the name; one too long for the buffer gets the mark. */
	text_error = drv_acpi_ns_name_text(name, text, size);
	if (text_error != 0)
		kern_strcpy(text, "(long name)");
}

/* Evaluates one element of a Package literal. */
static int
package_element(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *reference;
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	bool named;
	int error;

	/* Anything but a name is evaluated as data. */
	named = drv_acpi_stream_at_name(eval);
	if (!named) {
		error = drv_acpi_eval_term_arg(eval, result);
		if (error != 0)
			return error;

		/* Succeeded: result is the element's value. */
		return 0;
	}

	/* Reads the name, which is kept as a reference and not evaluated. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* A name that does not exist yet becomes a name reference. */
	error = drv_acpi_ns_lookup(eval->scope, &name, true, &node);
	if (error != 0) {
		error = name_reference(eval, &name, result);
		if (error != 0)
			return error;

		/* Succeeded: result names the name, to resolve once the tables are loaded. */
		return 0;
	}

	/* Allocates the node reference. */
	reference = drv_acpi_object_reference_new(DRV_ACPI_REFERENCE_NODE);
	if (reference == NULL)
		return ENOMEM;

	/* Points it at the node and hands it over. */
	reference->value.reference.node = node;
	*result = reference;

	/* Succeeded: result refers to the node. */
	return 0;
}

/*
 * Makes the name reference of a Package element whose name does not exist
 * yet: the scope it is resolved from later and the text of the name.  A
 * name too long for the text is kept as "(long name)", which never
 * resolves, so that the rest of the package still loads.
 */
static int
name_reference(
	struct drv_acpi_eval *eval,
	const struct drv_acpi_name *name,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *reference;
	char text[NAME_TEXT_MAX];
	size_t length;

	/* Writes the name's text. */
	name_text(name, text, sizeof(text));

	/* Allocates the reference. */
	reference = drv_acpi_object_reference_new(DRV_ACPI_REFERENCE_NAME);
	if (reference == NULL)
		return ENOMEM;

	/* Records the scope the name is resolved from later. */
	reference->value.reference.node = eval->scope;

	/* Allocates room for the text and its terminator. */
	length = kern_strlen(text);
	reference->value.reference.name = drv_acpi_os_alloc(length + 1U);
	if (reference->value.reference.name == NULL) {
		drv_acpi_object_release(reference);
		return ENOMEM;
	}

	/* Copies the text with its terminator. */
	kern_memcpy(reference->value.reference.name, text, length + 1U);

	/* Hands over the reference. */
	*result = reference;

	/* Succeeded: the caller holds the name reference. */
	return 0;
}

/* Finds a loaded table by its signature and OEM identifiers. */
static struct drv_acpi_table *
table_find(
	const char *signature,
	const char *oem_id,
	const char *oem_table_id)
{
	struct drv_acpi_table *table;
	int compared;

	/* Compares each loaded table, oldest first; an empty identifier matches any. */
	table = drv_acpi_table_first();
	for (;
	     table != NULL;
	     table = table->next) {
		/* Skips a table with another signature. */
		compared = kern_strcmp(table->signature, signature);
		if (compared != 0)
			continue;

		/* Skips a table with another OEM ID. */
		if (oem_id[0] != '\0') {
			compared = kern_strcmp(table->oem_id, oem_id);
			if (compared != 0)
				continue;
		}

		/* Skips a table with another OEM table ID. */
		if (oem_table_id[0] != '\0') {
			compared = kern_strcmp(table->oem_table_id, oem_table_id);
			if (compared != 0)
				continue;
		}

		/* Reports the matching table. */
		return table;
	}

	/* Reports that no loaded table matches. */
	return NULL;
}

/* Evaluates a TermArg that must be a string and copies it out. */
static int
string_operand(
	struct drv_acpi_eval *eval,
	char *text,
	size_t size)
{
	struct drv_acpi_object *object;
	size_t length;
	int error;

	/* Evaluates the operand. */
	error = drv_acpi_eval_data(eval, &object);
	if (error != 0)
		return error;

	/* Refuses anything but a string. */
	if (object->type != DRV_ACPI_TYPE_STRING) {
		drv_acpi_object_release(object);
		return EINVAL;
	}

	/* Copies as much as fits, terminated. */
	length = object->value.string.length;
	if (length >= size)
		length = size - 1U;
	kern_memcpy(text, object->value.string.text, length);
	text[length] = '\0';

	/* The operand is no longer needed. */
	drv_acpi_object_release(object);

	/* Succeeded: text holds the string. */
	return 0;
}
