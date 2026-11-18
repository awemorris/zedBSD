/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The types shared by the files of the AML interpreter.
 *
 * Nothing here is visible to drivers; they use <drivers/acpi/acpi.h>.  The
 * interpreter does not depend on the kernel: it calls kcrt for the C
 * runtime and the functions of aml-os.h for everything the operating
 * system provides, so the same files run in the host test harness.
 */

#ifndef KERN_DRIVERS_ACPI_AML_INTERNAL_H
#define KERN_DRIVERS_ACPI_AML_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/acpi/acpi.h>

/*
 * Marks a parameter a function does not use.  The kernel's headers define
 * it already; the host test harness, which does not include them, gets
 * this one.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/*
 * The one-byte opcodes of ACPI 6.5 section 20.3.
 */
#define DRV_ACPI_OP_ZERO		0x00U
#define DRV_ACPI_OP_ONE			0x01U
#define DRV_ACPI_OP_ALIAS		0x06U
#define DRV_ACPI_OP_NAME		0x08U
#define DRV_ACPI_OP_BYTE_PREFIX		0x0aU
#define DRV_ACPI_OP_WORD_PREFIX		0x0bU
#define DRV_ACPI_OP_DWORD_PREFIX	0x0cU
#define DRV_ACPI_OP_STRING_PREFIX	0x0dU
#define DRV_ACPI_OP_QWORD_PREFIX	0x0eU
#define DRV_ACPI_OP_SCOPE		0x10U
#define DRV_ACPI_OP_BUFFER		0x11U
#define DRV_ACPI_OP_PACKAGE		0x12U
#define DRV_ACPI_OP_VAR_PACKAGE		0x13U
#define DRV_ACPI_OP_METHOD		0x14U
#define DRV_ACPI_OP_EXTERNAL		0x15U
#define DRV_ACPI_OP_DUAL_NAME_PREFIX	0x2eU
#define DRV_ACPI_OP_MULTI_NAME_PREFIX	0x2fU
#define DRV_ACPI_OP_EXT_PREFIX		0x5bU
#define DRV_ACPI_OP_ROOT_CHAR		0x5cU
#define DRV_ACPI_OP_PARENT_PREFIX	0x5eU
#define DRV_ACPI_OP_LOCAL0		0x60U
#define DRV_ACPI_OP_LOCAL7		0x67U
#define DRV_ACPI_OP_ARG0		0x68U
#define DRV_ACPI_OP_ARG6		0x6eU
#define DRV_ACPI_OP_STORE		0x70U
#define DRV_ACPI_OP_REF_OF		0x71U
#define DRV_ACPI_OP_ADD			0x72U
#define DRV_ACPI_OP_CONCAT		0x73U
#define DRV_ACPI_OP_SUBTRACT		0x74U
#define DRV_ACPI_OP_INCREMENT		0x75U
#define DRV_ACPI_OP_DECREMENT		0x76U
#define DRV_ACPI_OP_MULTIPLY		0x77U
#define DRV_ACPI_OP_DIVIDE		0x78U
#define DRV_ACPI_OP_SHIFT_LEFT		0x79U
#define DRV_ACPI_OP_SHIFT_RIGHT		0x7aU
#define DRV_ACPI_OP_AND			0x7bU
#define DRV_ACPI_OP_NAND		0x7cU
#define DRV_ACPI_OP_OR			0x7dU
#define DRV_ACPI_OP_NOR			0x7eU
#define DRV_ACPI_OP_XOR			0x7fU
#define DRV_ACPI_OP_NOT			0x80U
#define DRV_ACPI_OP_FIND_SET_LEFT_BIT	0x81U
#define DRV_ACPI_OP_FIND_SET_RIGHT_BIT	0x82U
#define DRV_ACPI_OP_DEREF_OF		0x83U
#define DRV_ACPI_OP_CONCAT_RES		0x84U
#define DRV_ACPI_OP_MOD			0x85U
#define DRV_ACPI_OP_NOTIFY		0x86U
#define DRV_ACPI_OP_SIZE_OF		0x87U
#define DRV_ACPI_OP_INDEX		0x88U
#define DRV_ACPI_OP_MATCH		0x89U
#define DRV_ACPI_OP_CREATE_DWORD_FIELD	0x8aU
#define DRV_ACPI_OP_CREATE_WORD_FIELD	0x8bU
#define DRV_ACPI_OP_CREATE_BYTE_FIELD	0x8cU
#define DRV_ACPI_OP_CREATE_BIT_FIELD	0x8dU
#define DRV_ACPI_OP_OBJECT_TYPE		0x8eU
#define DRV_ACPI_OP_CREATE_QWORD_FIELD	0x8fU
#define DRV_ACPI_OP_LAND		0x90U
#define DRV_ACPI_OP_LOR			0x91U
#define DRV_ACPI_OP_LNOT		0x92U
#define DRV_ACPI_OP_LEQUAL		0x93U
#define DRV_ACPI_OP_LGREATER		0x94U
#define DRV_ACPI_OP_LLESS		0x95U
#define DRV_ACPI_OP_TO_BUFFER		0x96U
#define DRV_ACPI_OP_TO_DECIMAL_STRING	0x97U
#define DRV_ACPI_OP_TO_HEX_STRING	0x98U
#define DRV_ACPI_OP_TO_INTEGER		0x99U
#define DRV_ACPI_OP_TO_STRING		0x9cU
#define DRV_ACPI_OP_COPY_OBJECT		0x9dU
#define DRV_ACPI_OP_MID			0x9eU
#define DRV_ACPI_OP_CONTINUE		0x9fU
#define DRV_ACPI_OP_IF			0xa0U
#define DRV_ACPI_OP_ELSE		0xa1U
#define DRV_ACPI_OP_WHILE		0xa2U
#define DRV_ACPI_OP_NOOP		0xa3U
#define DRV_ACPI_OP_RETURN		0xa4U
#define DRV_ACPI_OP_BREAK		0xa5U
#define DRV_ACPI_OP_BREAK_POINT		0xccU
#define DRV_ACPI_OP_ONES		0xffU

/*
 * The two-byte opcodes, written as 0x5b00 plus the second byte.
 */
#define DRV_ACPI_EXT(code)		(0x5b00U | (code))
#define DRV_ACPI_OP_MUTEX		DRV_ACPI_EXT(0x01U)
#define DRV_ACPI_OP_EVENT		DRV_ACPI_EXT(0x02U)
#define DRV_ACPI_OP_COND_REF_OF		DRV_ACPI_EXT(0x12U)
#define DRV_ACPI_OP_CREATE_FIELD	DRV_ACPI_EXT(0x13U)
#define DRV_ACPI_OP_LOAD_TABLE		DRV_ACPI_EXT(0x1fU)
#define DRV_ACPI_OP_LOAD		DRV_ACPI_EXT(0x20U)
#define DRV_ACPI_OP_STALL		DRV_ACPI_EXT(0x21U)
#define DRV_ACPI_OP_SLEEP		DRV_ACPI_EXT(0x22U)
#define DRV_ACPI_OP_ACQUIRE		DRV_ACPI_EXT(0x23U)
#define DRV_ACPI_OP_SIGNAL		DRV_ACPI_EXT(0x24U)
#define DRV_ACPI_OP_WAIT		DRV_ACPI_EXT(0x25U)
#define DRV_ACPI_OP_RESET		DRV_ACPI_EXT(0x26U)
#define DRV_ACPI_OP_RELEASE		DRV_ACPI_EXT(0x27U)
#define DRV_ACPI_OP_FROM_BCD		DRV_ACPI_EXT(0x28U)
#define DRV_ACPI_OP_TO_BCD		DRV_ACPI_EXT(0x29U)
#define DRV_ACPI_OP_UNLOAD		DRV_ACPI_EXT(0x2aU)
#define DRV_ACPI_OP_REVISION		DRV_ACPI_EXT(0x30U)
#define DRV_ACPI_OP_DEBUG		DRV_ACPI_EXT(0x31U)
#define DRV_ACPI_OP_FATAL		DRV_ACPI_EXT(0x32U)
#define DRV_ACPI_OP_TIMER		DRV_ACPI_EXT(0x33U)
#define DRV_ACPI_OP_OPERATION_REGION	DRV_ACPI_EXT(0x80U)
#define DRV_ACPI_OP_FIELD		DRV_ACPI_EXT(0x81U)
#define DRV_ACPI_OP_DEVICE		DRV_ACPI_EXT(0x82U)
#define DRV_ACPI_OP_PROCESSOR		DRV_ACPI_EXT(0x83U)
#define DRV_ACPI_OP_POWER_RESOURCE	DRV_ACPI_EXT(0x84U)
#define DRV_ACPI_OP_THERMAL_ZONE	DRV_ACPI_EXT(0x85U)
#define DRV_ACPI_OP_INDEX_FIELD		DRV_ACPI_EXT(0x86U)
#define DRV_ACPI_OP_BANK_FIELD		DRV_ACPI_EXT(0x87U)
#define DRV_ACPI_OP_DATA_REGION		DRV_ACPI_EXT(0x88U)

/*
 * The AML revision the Revision operator reports.
 */
#define DRV_ACPI_AML_REVISION		0x20260927ULL

/*
 * The fixed sizes of a method invocation.
 */
#define DRV_ACPI_LOCAL_COUNT		8U
#define DRV_ACPI_ARGUMENT_COUNT		7U

/*
 * How deep method invocations may nest before the interpreter refuses
 * another one.  Real firmware stays far below; a loop that recurses without
 * end stops here instead of on the stack budget.
 */
#define DRV_ACPI_CALL_DEPTH_MAX		48U

/*
 * The owner of a namespace node that the interpreter itself created at
 * startup, as opposed to a table or a method invocation.
 */
#define DRV_ACPI_OWNER_PREDEFINED	0U

/*
 * The field flags byte of ACPI 6.5 section 19.6.48.
 */
#define DRV_ACPI_FIELD_ACCESS_MASK	0x0fU
#define DRV_ACPI_FIELD_ACCESS_ANY	0U
#define DRV_ACPI_FIELD_ACCESS_BYTE	1U
#define DRV_ACPI_FIELD_ACCESS_WORD	2U
#define DRV_ACPI_FIELD_ACCESS_DWORD	3U
#define DRV_ACPI_FIELD_ACCESS_QWORD	4U
#define DRV_ACPI_FIELD_ACCESS_BUFFER	5U
#define DRV_ACPI_FIELD_LOCK		0x10U
#define DRV_ACPI_FIELD_UPDATE_MASK	0x60U
#define DRV_ACPI_FIELD_UPDATE_PRESERVE	0x00U
#define DRV_ACPI_FIELD_UPDATE_ONES	0x20U
#define DRV_ACPI_FIELD_UPDATE_ZEROS	0x40U

/*
 * The kinds of reference object.
 */
enum drv_acpi_reference_kind {
	DRV_ACPI_REFERENCE_NODE = 1,
	DRV_ACPI_REFERENCE_INDEX = 2,
	DRV_ACPI_REFERENCE_OBJECT = 3,
	DRV_ACPI_REFERENCE_NAME = 4
};

/*
 * The kinds of field unit.
 */
enum drv_acpi_field_kind {
	DRV_ACPI_FIELD_REGION = 1,
	DRV_ACPI_FIELD_INDEX = 2,
	DRV_ACPI_FIELD_BANK = 3
};

/*
 * What a term asked the enclosing term lists to do next.
 */
enum drv_acpi_control {
	DRV_ACPI_CONTROL_NEXT = 0,
	DRV_ACPI_CONTROL_RETURN = 1,
	DRV_ACPI_CONTROL_BREAK = 2,
	DRV_ACPI_CONTROL_CONTINUE = 3
};

/*
 * Where a store goes: nowhere, a local, an argument, a namespace node, the
 * debug object, or through a reference object.
 */
enum drv_acpi_target_kind {
	DRV_ACPI_TARGET_NONE = 0,
	DRV_ACPI_TARGET_LOCAL = 1,
	DRV_ACPI_TARGET_ARGUMENT = 2,
	DRV_ACPI_TARGET_NODE = 3,
	DRV_ACPI_TARGET_DEBUG = 4,
	DRV_ACPI_TARGET_REFERENCE = 5
};

struct drv_acpi_eval;
struct drv_acpi_frame;
struct drv_acpi_table;
struct drv_acpi_thread;

/*
 * A native method: a predefined method the interpreter implements in C,
 * such as \_OSI.
 */
typedef int (*drv_acpi_native_method_t)(struct drv_acpi_eval *eval, struct drv_acpi_object **arguments, unsigned argument_count, struct drv_acpi_object **result);

/*
 * The AML of one control method.
 *
 * The bytes belong to the table that defined the method, which outlives
 * every node the table created.  The serialization mutex is created on the
 * first invocation of a serialized method.
 */
struct drv_acpi_method {
	const uint8_t *start;
	const uint8_t *end;
	struct drv_acpi_table *table;
	drv_acpi_native_method_t native;
	struct drv_acpi_object *serialization;
	uint8_t argument_count;
	uint8_t serialized;
	uint8_t sync_level;
};

/*
 * One operation region.
 *
 * The offset and length are TermArgs that are evaluated when the region is
 * first used, the way firmware expects: their AML stays in the table and
 * is run in the scope of the region's parent.  A data table region keeps
 * the bytes of the table it names.
 */
struct drv_acpi_region {
	uint64_t offset;
	uint64_t length;
	const uint8_t *arguments_start;
	const uint8_t *arguments_end;
	struct drv_acpi_table *table;
	struct drv_acpi_node *node;
	struct drv_acpi_node *scope;
	const uint8_t *data;
	uint16_t pci_segment;
	uint8_t pci_bus;
	uint8_t pci_device;
	uint8_t pci_function;
	uint8_t space;
	uint8_t evaluated;
	uint8_t pci_resolved;
	uint8_t connected;
};

/*
 * One field unit: a run of bits in a region, or behind an index and data
 * pair, or behind a bank selector.
 */
struct drv_acpi_field {
	struct drv_acpi_node *region;
	struct drv_acpi_node *index;
	struct drv_acpi_node *data;
	struct drv_acpi_object *connection;
	uint64_t bank_value;
	uint32_t bit_offset;
	uint32_t bit_length;
	uint8_t kind;
	uint8_t flags;
	uint8_t access_attribute;
	uint8_t access_length;
};

/*
 * One buffer field: a run of bits in a buffer object, which the field holds
 * a reference to.  A field made by CreateField always reads as a buffer;
 * the fixed-size Create*Field ones read as integers.
 */
struct drv_acpi_buffer_field {
	struct drv_acpi_object *buffer;
	uint64_t bit_offset;
	uint64_t bit_length;
	bool reads_buffer;
};

/*
 * One AML mutex.
 *
 * owner is the thread that holds it and depth counts how many times that
 * thread acquired it; both are zero when the mutex is free.  While it is
 * held, next_held links the older mutexes of the same thread and
 * original_sync_level keeps the thread's level from before the
 * acquisition, which the last Release restores.
 */
struct drv_acpi_mutex {
	struct drv_acpi_thread *owner;
	struct drv_acpi_object *next_held;
	uint32_t depth;
	uint8_t sync_level;
	uint8_t original_sync_level;
};

/*
 * One reference.
 *
 * A node reference names a namespace node.  An index reference is one
 * element of a package, one byte of a buffer, or one character of a string;
 * it holds the container.  An object reference holds the object a local or
 * argument contained.  A name reference is a package element whose name did
 * not resolve when the package was built; it keeps the path text.
 */
struct drv_acpi_reference {
	struct drv_acpi_node *node;
	struct drv_acpi_object *target;
	char *name;
	uint32_t index;
	uint8_t kind;
};

/*
 * One AML object.
 *
 * Every holder -- a namespace node, a local or argument, a package element,
 * a reference, a buffer field, or a caller -- owns one count of references.
 * The object is freed when the last one is released.
 */
struct drv_acpi_object {
	uint32_t references;
	uint8_t type;
	union {
		uint64_t integer;

		/* A string: its characters, terminated, and their count. */
		struct {
			char *text;
			size_t length;
		} string;

		/* A buffer: its bytes, which a buffer field may write in place, and their count. */
		struct {
			uint8_t *bytes;
			size_t length;
		} buffer;

		/* A package: its elements, each holding one reference or NULL, and their count. */
		struct {
			struct drv_acpi_object **elements;
			uint32_t count;
		} package;

		struct drv_acpi_method method;
		struct drv_acpi_region region;
		struct drv_acpi_field field;
		struct drv_acpi_buffer_field buffer_field;
		struct drv_acpi_mutex mutex;

		/* An event: how many Signals no Wait has taken yet. */
		struct {
			uint32_t pending;
		} event;

		/* A processor: the block of its P_BLK registers and its processor ID. */
		struct {
			uint32_t block_address;
			uint8_t id;
			uint8_t block_length;
		} processor;

		/* A power resource: the order it is turned on in, and the deepest sleep state it serves. */
		struct {
			uint16_t resource_order;
			uint8_t system_level;
		} power;

		struct drv_acpi_reference reference;

		/* A DDB handle: the loaded table it names, or NULL after Unload. */
		struct {
			struct drv_acpi_table *table;
		} ddb;

		/* An alias: the node it stands for. */
		struct {
			struct drv_acpi_node *target;
		} alias;
	} value;
};

/*
 * One notification handler installed on a node.
 */
struct drv_acpi_notify {
	drv_acpi_notify_handler_t handler;
	void *argument;
	struct drv_acpi_notify *next;
};

/*
 * One namespace node.
 *
 * Children are kept in creation order.  owner is the table that created
 * the node, or zero for the predefined ones; a node a method invocation
 * created is also on that invocation's list through temporary_next and is
 * removed when the invocation ends.
 */
struct drv_acpi_node {
	uint32_t name;
	uint32_t owner;
	struct drv_acpi_node *parent;
	struct drv_acpi_node *child;
	struct drv_acpi_node *last_child;
	struct drv_acpi_node *next;
	struct drv_acpi_node *temporary_next;
	struct drv_acpi_object *object;
	struct drv_acpi_notify *notify;
};

/*
 * One loaded definition block.
 *
 * The interpreter keeps its own copy of the table; method bodies and
 * deferred region arguments point into it.
 */
struct drv_acpi_table {
	uint8_t *data;
	size_t length;
	uint32_t id;
	uint8_t revision;
	char signature[5];
	char oem_id[7];
	char oem_table_id[9];
	struct drv_acpi_table *next;
};

/*
 * A NameString as it appears in the AML: an optional root prefix, a
 * number of parent prefixes, and the name segments, which stay in the
 * AML bytes.
 */
struct drv_acpi_name {
	const uint8_t *segments;
	uint32_t count;
	uint32_t parents;
	bool root;
};

/*
 * One method invocation.
 *
 * It lives on the heap so that nested invocations cost the C stack only
 * the evaluation context.  created lists the nodes the invocation made,
 * newest first.
 */
struct drv_acpi_frame {
	struct drv_acpi_object *locals[DRV_ACPI_LOCAL_COUNT];
	struct drv_acpi_object *arguments[DRV_ACPI_ARGUMENT_COUNT];
	struct drv_acpi_node *method;
	struct drv_acpi_node *created;
	struct drv_acpi_frame *caller;
	unsigned depth;
};

/*
 * One entry into the interpreter: a driver's evaluation, a table load, or
 * the preparation of a region.
 *
 * AML mutexes and serialized methods are owned by it.  held lists the
 * mutexes it holds, newest first, each with a reference; sync_level is the
 * level the newest one set, zero when it holds none (ACPI 6.5 section
 * 19.6.2).  stack_base is where the stack budget is measured from, and
 * nesting counts the entries of the same thread.  It lives on the stack
 * of the outermost entry function, whose leave releases whatever is still
 * held.
 */
struct drv_acpi_thread {
	struct drv_acpi_object *held;
	uintptr_t stack_base;
	unsigned nesting;
	uint8_t sync_level;
};

/*
 * The state of one run through AML: a table load or a method invocation.
 *
 * position walks the bytes; end bounds the term list being run.  control
 * carries a Return, Break or Continue out of nested term lists, and
 * return_value holds what Return produced.
 */
struct drv_acpi_eval {
	const uint8_t *position;
	const uint8_t *end;
	struct drv_acpi_node *scope;
	struct drv_acpi_frame *frame;
	struct drv_acpi_table *table;
	struct drv_acpi_thread *thread;
	struct drv_acpi_object *return_value;
	int control;
};

/*
 * A parsed SuperName or Target.  A reference target holds one count of
 * references on the reference object, released by drv_acpi_target_release().
 */
struct drv_acpi_target {
	struct drv_acpi_node *node;
	struct drv_acpi_object *reference;
	unsigned index;
	int kind;
};

/* aml-object.c */
struct drv_acpi_object *
drv_acpi_object_new(
	enum drv_acpi_type type);

struct drv_acpi_object *
drv_acpi_object_string_new_length(
	const char *text,
	size_t length);

struct drv_acpi_object *
drv_acpi_object_reference_new(
	enum drv_acpi_reference_kind kind);

void
drv_acpi_object_ref(
	struct drv_acpi_object *object);

int
drv_acpi_object_copy(
	struct drv_acpi_object *source,
	struct drv_acpi_object **result);

uint64_t
drv_acpi_integer_mask(void);

unsigned
drv_acpi_integer_bytes(void);

void
drv_acpi_integer_set_width(
	unsigned bits);

/* aml-namespace.c */
int
drv_acpi_ns_init(void);

void
drv_acpi_ns_reset(void);

uint32_t
drv_acpi_ns_segment(
	const uint8_t *bytes);

int
drv_acpi_ns_lookup(
	struct drv_acpi_node *scope,
	const struct drv_acpi_name *name,
	bool search,
	struct drv_acpi_node **result);

int
drv_acpi_ns_create(
	struct drv_acpi_eval *eval,
	const struct drv_acpi_name *name,
	struct drv_acpi_node **result);

int
drv_acpi_ns_create_child(
	struct drv_acpi_node *parent,
	uint32_t segment,
	uint32_t owner,
	struct drv_acpi_node **result);

void
drv_acpi_ns_delete(
	struct drv_acpi_node *node);

void
drv_acpi_ns_attach(
	struct drv_acpi_node *node,
	struct drv_acpi_object *object);

struct drv_acpi_node *
drv_acpi_ns_resolve_alias(
	struct drv_acpi_node *node);

int
drv_acpi_pci_location(
	struct drv_acpi_node *device,
	uint16_t *segment,
	uint8_t *bus,
	uint8_t *slot,
	uint8_t *function);

bool
drv_acpi_pci_below_root(
	struct drv_acpi_node *device);

int
drv_acpi_lookup_path(
	struct drv_acpi_node *scope,
	const char *path,
	bool search,
	struct drv_acpi_node **result);

int
drv_acpi_ns_parse_path(
	const char *text,
	uint8_t *segments,
	size_t capacity,
	struct drv_acpi_name *name);

int
drv_acpi_ns_name_text(
	const struct drv_acpi_name *name,
	char *buffer,
	size_t size);

/* aml-stream.c */
int
drv_acpi_stream_byte(
	struct drv_acpi_eval *eval,
	uint8_t *byte);

int
drv_acpi_stream_peek(
	struct drv_acpi_eval *eval,
	uint8_t *byte);

int
drv_acpi_stream_opcode(
	struct drv_acpi_eval *eval,
	unsigned *opcode);

int
drv_acpi_stream_integer(
	struct drv_acpi_eval *eval,
	unsigned bytes,
	uint64_t *value);

int
drv_acpi_stream_package_length(
	struct drv_acpi_eval *eval,
	const uint8_t **end);

int
drv_acpi_stream_field_length(
	struct drv_acpi_eval *eval,
	uint32_t *length);

int
drv_acpi_stream_name(
	struct drv_acpi_eval *eval,
	struct drv_acpi_name *name);

bool
drv_acpi_stream_at_name(
	const struct drv_acpi_eval *eval);

int
drv_acpi_stream_string(
	struct drv_acpi_eval *eval,
	const char **text,
	size_t *length);

/* aml-eval.c */
int
drv_acpi_exec_term_list(
	struct drv_acpi_eval *eval);

int
drv_acpi_exec_scope(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *scope,
	const uint8_t *end);

int
drv_acpi_eval_term_arg(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result);

int
drv_acpi_eval_integer(
	struct drv_acpi_eval *eval,
	uint64_t *value);

int
drv_acpi_eval_data(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result);

int
drv_acpi_parse_target(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target);

void
drv_acpi_target_release(
	struct drv_acpi_target *target);

int
drv_acpi_store(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *value,
	struct drv_acpi_target *target);

int
drv_acpi_invoke(
	struct drv_acpi_eval *caller,
	struct drv_acpi_node *node,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result);

int
drv_acpi_index_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *reference,
	struct drv_acpi_object **result);

int
drv_acpi_read_node(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *node,
	struct drv_acpi_object **result);

int
drv_acpi_stack_check(void);

size_t
drv_acpi_stack_deepest(void);

/* aml-skip.c */
int
drv_acpi_skip_term_arg(
	struct drv_acpi_eval *eval);

/* aml-operator.c */
int
drv_acpi_eval_operator(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result);

int
drv_acpi_convert_integer(
	struct drv_acpi_object *object,
	uint64_t *value);

int
drv_acpi_convert_buffer(
	struct drv_acpi_object *object,
	struct drv_acpi_object **result);

int
drv_acpi_convert_string(
	struct drv_acpi_object *object,
	struct drv_acpi_object **result);

/* aml-define.c */
int
drv_acpi_define(
	struct drv_acpi_eval *eval,
	unsigned opcode) __attribute__((noinline));

bool
drv_acpi_is_definition(
	unsigned opcode);

int
drv_acpi_build_package(
	struct drv_acpi_eval *eval,
	bool variable,
	struct drv_acpi_object **result);

int
drv_acpi_build_buffer(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result);

int
drv_acpi_create_buffer_field(
	struct drv_acpi_eval *eval,
	unsigned opcode);

void
drv_acpi_package_resolve(
	struct drv_acpi_object *package);

void
drv_acpi_reference_resolve(
	struct drv_acpi_object *reference);

/* aml-field.c */
int
drv_acpi_field_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	struct drv_acpi_object **result);

int
drv_acpi_field_write(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *field,
	struct drv_acpi_object *value);

int
drv_acpi_buffer_field_read(
	struct drv_acpi_object *field,
	struct drv_acpi_object **result);

int
drv_acpi_buffer_field_write(
	struct drv_acpi_object *field,
	struct drv_acpi_object *value);

int
drv_acpi_region_prepare(
	struct drv_acpi_object *region);

void
drv_acpi_region_reset(void);

int
drv_acpi_region_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *region,
	uint64_t offset,
	size_t length,
	uint8_t *bytes);

/* aml-table.c */
struct drv_acpi_table *
drv_acpi_table_first(void);

int
drv_acpi_table_operator(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result) __attribute__((noinline));

/* aml-thread.c */
struct drv_acpi_thread *
drv_acpi_enter(
	struct drv_acpi_thread *storage) __attribute__((noinline));

void
drv_acpi_leave(
	struct drv_acpi_thread *thread);

struct drv_acpi_thread *
drv_acpi_active_thread(void);

struct drv_acpi_thread *
drv_acpi_eval_thread(
	struct drv_acpi_eval *eval);

void
drv_acpi_sleep(
	uint64_t milliseconds);

/* aml-sync.c */
int
drv_acpi_notify(
	struct drv_acpi_node *node,
	uint32_t value);

int
drv_acpi_sync_operator(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result) __attribute__((noinline));

int
drv_acpi_mutex_acquire(
	struct drv_acpi_thread *thread,
	struct drv_acpi_object *mutex,
	uint64_t timeout,
	bool *timed_out);

int
drv_acpi_mutex_release(
	struct drv_acpi_thread *thread,
	struct drv_acpi_object *mutex);

void
drv_acpi_thread_end(
	struct drv_acpi_thread *thread);

int
drv_acpi_global_lock(
	struct drv_acpi_eval *eval,
	bool acquire);

void
drv_acpi_events_global_release(void);

int
drv_acpi_osi_install(void);

#endif
