/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The namespace and evaluation results as text: one line per node
 * ("PATH TYPE ATTRIBUTES") and one line per evaluation ("PATH = VALUE").
 * /dev/acpi gives this text to user programs, and the host tests print
 * the same, so a namespace read inside a guest compares with one loaded
 * on the host line for line.
 */

#include <stdarg.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>

#include "acpi-text.h"
#include "aml-internal.h"
#include "aml-os.h"

/*
 * The longest path written.
 */
#define PATH_LENGTH_MAX 512U

/*
 * The longest formatted piece appended at once.
 */
#define PIECE_LENGTH_MAX 256U

/*
 * The first size of a text's storage; it doubles as it fills.
 */
#define TEXT_INITIAL_CAPACITY 4096U

/*
 * The largest text written, which bounds a namespace a damaged table
 * blew up.
 */
#define TEXT_CAPACITY_MAX (64U * 1024U * 1024U)

static int namespace_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static void node_line(struct drv_acpi_text *text, struct drv_acpi_node *node);
static void field_line(struct drv_acpi_text *text, const char *path, const struct drv_acpi_object *object);
static void object_text(struct drv_acpi_text *text, const struct drv_acpi_object *object, unsigned depth);
static void segment_text(const struct drv_acpi_node *node, char *name);
static const char *space_name(unsigned space);
static bool text_reserve(struct drv_acpi_text *text, size_t more);

/*
 * Starts an empty text.
 */
void
drv_acpi_text_init(
	struct drv_acpi_text *text)
{
	/* Nothing is stored yet. */
	kern_memset(text, 0, sizeof(*text));
}

/*
 * Frees what a text holds.
 */
void
drv_acpi_text_release(
	struct drv_acpi_text *text)
{
	/* Frees the storage and forgets it. */
	drv_acpi_os_free(text->data);
	kern_memset(text, 0, sizeof(*text));
}

/*
 * Appends formatted characters to a text.
 *
 * A text that ran out of memory keeps ENOMEM in error and takes nothing
 * more.
 */
void
drv_acpi_text_printf(
	struct drv_acpi_text *text,
	const char *format,
	...)
{
	char piece[PIECE_LENGTH_MAX];
	va_list arguments;
	size_t length;
	bool room;

	/* Formats the piece. */
	va_start(arguments, format);
	kern_vsnprintf(piece, sizeof(piece), format, arguments);
	va_end(arguments);

	/* Measures it. */
	length = kern_strlen(piece);

	/* Makes room for it and its terminator. */
	room = text_reserve(text, length + 1U);
	if (!room)
		return;

	/* Appends it, terminated. */
	kern_memcpy(text->data + text->length, piece, length + 1U);
	text->length += length;
}

/*
 * Writes one line per node of the namespace.
 */
int
drv_acpi_text_namespace(
	struct drv_acpi_text *text)
{
	int error;

	/* Walks the whole namespace. */
	error = drv_acpi_walk(NULL, namespace_visitor, text);
	if (error != 0)
		return error;

	/* Reports a text that ran out of memory. */
	if (text->error != 0)
		return text->error;

	/* Succeeded: the text holds the whole namespace. */
	return 0;
}

/*
 * Evaluates a path and writes "PATH = VALUE", or "PATH = error N".
 */
int
drv_acpi_text_evaluate(
	struct drv_acpi_text *text,
	struct drv_acpi_node *scope,
	const char *path,
	const char *label)
{
	struct drv_acpi_object *result;
	int error;

	/* Evaluates the path; a failure is written as the line's value. */
	error = drv_acpi_evaluate(scope, path, NULL, 0, &result);
	if (error != 0) {
		drv_acpi_text_printf(text, "%s = error %d\n", label, error);
		return error;
	}

	/* Writes the result. */
	drv_acpi_text_printf(text, "%s = ", label);
	object_text(text, result, 0);
	drv_acpi_text_printf(text, "\n");

	/* The result is no longer needed. */
	drv_acpi_object_release(result);

	/* Reports a text that ran out of memory. */
	if (text->error != 0)
		return text->error;

	/* Succeeded: the text holds the evaluation's line. */
	return 0;
}

/* Writes one node's line during the walk. */
static int
namespace_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct drv_acpi_text *text;

	UNUSED_PARAMETER(depth);

	/* Stops the walk when the text ran out of memory. */
	text = argument;
	if (text->error != 0)
		return -1;

	/* Writes the line. */
	node_line(text, node);

	/* Goes on into the children. */
	return 0;
}

/* Writes "path type attributes" for one node. */
static void
node_line(
	struct drv_acpi_text *text,
	struct drv_acpi_node *node)
{
	const struct drv_acpi_object *object;
	const char *space;
	char path[PATH_LENGTH_MAX];
	char name[5];
	int error;

	/* Writes the path. */
	error = drv_acpi_node_path(node, path, sizeof(path));
	if (error != 0)
		kern_strcpy(path, "(long)");

	/* A node without an object is untyped. */
	object = node->object;
	if (object == NULL) {
		drv_acpi_text_printf(text, "%s Untyped\n", path);
		return;
	}

	/* Writes the type and what identifies the object. */
	switch (object->type) {
	case DRV_ACPI_TYPE_SCOPE:
		drv_acpi_text_printf(text, "%s Scope\n", path);
		break;
	case DRV_ACPI_TYPE_DEVICE:
		drv_acpi_text_printf(text, "%s Device\n", path);
		break;
	case DRV_ACPI_TYPE_THERMAL_ZONE:
		drv_acpi_text_printf(text, "%s Thermal\n", path);
		break;
	case DRV_ACPI_TYPE_POWER_RESOURCE:
		drv_acpi_text_printf(text, "%s Power\n", path);
		break;
	case DRV_ACPI_TYPE_EVENT:
		drv_acpi_text_printf(text, "%s Event\n", path);
		break;
	case DRV_ACPI_TYPE_MUTEX:
		drv_acpi_text_printf(text, "%s Mutex\n", path);
		break;
	case DRV_ACPI_TYPE_PROCESSOR:
		drv_acpi_text_printf(text,
				     "%s Processor id=%02X len=%02X addr=%llX\n",
				     path,
				     (unsigned)object->value.processor.id,
				     (unsigned)object->value.processor.block_length,
				     (unsigned long long)object->value.processor.block_address);
		break;
	case DRV_ACPI_TYPE_INTEGER:
		drv_acpi_text_printf(text, "%s Integer %llX\n", path, (unsigned long long)object->value.integer);
		break;
	case DRV_ACPI_TYPE_STRING:
		drv_acpi_text_printf(text, "%s String \"%s\"\n", path, object->value.string.text);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		drv_acpi_text_printf(text, "%s Buffer len=%zX\n", path, object->value.buffer.length);
		break;
	case DRV_ACPI_TYPE_PACKAGE:
		drv_acpi_text_printf(text, "%s Package count=%X\n", path, (unsigned)object->value.package.count);
		break;
	case DRV_ACPI_TYPE_METHOD:
		drv_acpi_text_printf(text, "%s Method args=%u\n", path, (unsigned)object->value.method.argument_count);
		break;
	case DRV_ACPI_TYPE_REGION:
		space = space_name(object->value.region.space);
		drv_acpi_text_printf(text,
				     "%s Region %s addr=%llX len=%llX\n",
				     path,
				     space,
				     (unsigned long long)object->value.region.offset,
				     (unsigned long long)object->value.region.length);
		break;
	case DRV_ACPI_TYPE_FIELD_UNIT:
		field_line(text, path, object);
		break;
	case DRV_ACPI_TYPE_BUFFER_FIELD:
		drv_acpi_text_printf(text,
				     "%s BufferField off=%llX len=%llX\n",
				     path,
				     (unsigned long long)object->value.buffer_field.bit_offset,
				     (unsigned long long)object->value.buffer_field.bit_length);
		break;
	case DRV_ACPI_TYPE_ALIAS:
		segment_text(object->value.alias.target, name);
		drv_acpi_text_printf(text, "%s Alias target=%s\n", path, name);
		break;
	default:
		drv_acpi_text_printf(text, "%s Type%u\n", path, (unsigned)object->type);
		break;
	}
}

/* Writes a field unit's line by its kind. */
static void
field_line(
	struct drv_acpi_text *text,
	const char *path,
	const struct drv_acpi_object *object)
{
	const struct drv_acpi_field *field;
	char first[5];
	char second[5];

	/* Names the nodes the field refers to. */
	field = &object->value.field;
	segment_text(field->region, first);
	segment_text(field->index, second);

	/* Writes the line by the kind of field. */
	if (field->kind == DRV_ACPI_FIELD_INDEX) {
		segment_text(field->data, first);
		drv_acpi_text_printf(text,
				     "%s IndexField idx=%s dat=%s off=%X len=%X\n",
				     path,
				     second,
				     first,
				     (unsigned)field->bit_offset,
				     (unsigned)field->bit_length);
	} else if (field->kind == DRV_ACPI_FIELD_BANK) {
		drv_acpi_text_printf(text,
				     "%s BankField rgn=%s bnk=%s off=%X len=%X\n",
				     path,
				     first,
				     second,
				     (unsigned)field->bit_offset,
				     (unsigned)field->bit_length);
	} else {
		drv_acpi_text_printf(text,
				     "%s RegionField rgn=%s off=%X len=%X\n",
				     path,
				     first,
				     (unsigned)field->bit_offset,
				     (unsigned)field->bit_length);
	}
}

/* Writes an object on one line, packages with their elements. */
static void
object_text(
	struct drv_acpi_text *text,
	const struct drv_acpi_object *object,
	unsigned depth)
{
	struct drv_acpi_node *node;
	struct drv_acpi_object *element;
	char path[PATH_LENGTH_MAX];
	const uint8_t *bytes;
	const char *characters;
	enum drv_acpi_type type;
	uint64_t integer;
	size_t length;
	size_t index;
	unsigned count;
	int error;

	/* A missing object writes as none. */
	if (object == NULL) {
		drv_acpi_text_printf(text, "None");
		return;
	}

	/* Writes by type. */
	type = drv_acpi_object_type(object);
	switch (type) {
	case DRV_ACPI_TYPE_INTEGER:
		/* Writes the value in hexadecimal. */
		integer = drv_acpi_object_integer(object);
		drv_acpi_text_printf(text, "Integer 0x%llX", (unsigned long long)integer);
		break;
	case DRV_ACPI_TYPE_STRING:
		/* Writes the characters in quotes. */
		characters = drv_acpi_object_string(object, NULL);
		drv_acpi_text_printf(text, "String \"%s\"", characters);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		/* Writes the length. */
		bytes = drv_acpi_object_buffer(object, &length);
		drv_acpi_text_printf(text, "Buffer [%zu]", length);

		/* Writes every byte. */
		for (index = 0; index < length; index++)
			drv_acpi_text_printf(text, " %02X", (unsigned)bytes[index]);
		break;
	case DRV_ACPI_TYPE_PACKAGE:
		/* Writes the count. */
		count = drv_acpi_object_package_count(object);
		drv_acpi_text_printf(text, "Package [%u] {", count);

		/* Writes each element separated by commas; nesting is bounded. */
		for (index = 0; index < count && depth < 8U; index++) {
			/* Separates the element from the one before. */
			if (index != 0)
				drv_acpi_text_printf(text, ",");

			/* Writes the element after a blank. */
			drv_acpi_text_printf(text, " ");
			element = drv_acpi_object_package_element(object, (unsigned)index);
			object_text(text, element, depth + 1U);
		}

		/* Closes the package. */
		drv_acpi_text_printf(text, " }");
		break;
	case DRV_ACPI_TYPE_REFERENCE:
		/* A reference to a node writes its path. */
		node = drv_acpi_object_reference_node(object);
		if (node == NULL) {
			drv_acpi_text_printf(text, "Reference");
			break;
		}

		/* Finds the path, or a mark when it does not fit. */
		error = drv_acpi_node_path(node, path, sizeof(path));
		if (error != 0)
			kern_strcpy(path, "(long)");

		/* Writes the path. */
		drv_acpi_text_printf(text, "Reference %s", path);
		break;
	default:
		/* Writes the type's number. */
		drv_acpi_text_printf(text, "Type%u", (unsigned)type);
		break;
	}
}

/* Writes the four characters of a node's name. */
static void
segment_text(
	const struct drv_acpi_node *node,
	char *name)
{
	/* A missing node has no name. */
	if (node == NULL) {
		kern_strcpy(name, "????");
		return;
	}

	/* Unpacks the name, lowest byte first. */
	name[0] = (char)(node->name & 0xffU);
	name[1] = (char)((node->name >> 8) & 0xffU);
	name[2] = (char)((node->name >> 16) & 0xffU);
	name[3] = (char)((node->name >> 24) & 0xffU);
	name[4] = '\0';
}

/* Names an address space the way acpiexec does. */
static const char *
space_name(
	unsigned space)
{
	static const char *const names[] = {
		"SystemMemory", "SystemIO", "PCI_Config", "EmbeddedControl", "SMBus",
		"SystemCMOS", "PCIBARTarget", "IPMI", "GeneralPurposeIo", "GenericSerialBus",
		"PCC", "PlatformRtMechanism",
	};

	/* Names the spaces ACPI defines. */
	if (space < sizeof(names) / sizeof(names[0]))
		return names[space];

	/* Anything above is an OEM space. */
	return "OEM";
}

/* Makes room for more characters, doubling the storage. */
static bool
text_reserve(
	struct drv_acpi_text *text,
	size_t more)
{
	char *grown;
	size_t capacity;

	/* A text that failed once stays failed. */
	if (text->error != 0)
		return false;

	/* A text with enough room already needs nothing. */
	if (text->length + more <= text->capacity)
		return true;

	/* Starts from the present size, or from the first size of an empty text. */
	capacity = text->capacity;
	if (capacity == 0)
		capacity = TEXT_INITIAL_CAPACITY;

	/* Doubles until the characters fit, within the bound. */
	while (capacity < text->length + more && capacity <= TEXT_CAPACITY_MAX)
		capacity *= 2U;

	/* Refuses a text beyond the bound; error keeps it failed from now on. */
	if (capacity > TEXT_CAPACITY_MAX) {
		text->error = E2BIG;
		return false;
	}

	/* Allocates the larger storage; error keeps a text that ran out of memory failed. */
	grown = drv_acpi_os_alloc(capacity);
	if (grown == NULL) {
		text->error = ENOMEM;
		return false;
	}

	/* Moves the characters written so far. */
	if (text->length != 0)
		kern_memcpy(grown, text->data, text->length);

	/* Replaces the old storage with the larger one. */
	drv_acpi_os_free(text->data);
	text->data = grown;
	text->capacity = capacity;

	/* Succeeded: the text has room for the characters. */
	return true;
}
