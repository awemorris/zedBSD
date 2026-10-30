/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared execution engine of browser: the garbage-collected heap,
 * strings, atoms, values, objects, bytecode and interpreter that JavaScript
 * and Wasm share.
 *
 * The heap is non-moving mark-and-sweep.  Its roots are found by scanning
 * the C stack conservatively (every word that points into a live cell
 * keeps it) and by the tracers and root slots its users register; inside
 * the heap each cell's type traces the cells it refers to exactly.  A cell
 * held only from memory the collector cannot see (malloc'd arrays, arenas)
 * must be reported by a tracer, or it is freed under its holder.
 *
 * One heap serves one tab; nothing is shared between heaps.
 */

#ifndef KEILAND_BROWSER_VM_H
#define KEILAND_BROWSER_VM_H

#include "base/base.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The string's characters are UTF-16 code units rather than Latin-1 bytes. */
#define VM_STRING_WIDE 0x1U

/* The string is its heap's atom for these characters. */
#define VM_STRING_ATOM 0x2U

/* The hash field holds the string's hash. */
#define VM_STRING_HASHED 0x4U

/* The small constants. */
#define VM_VALUE_EMPTY 0x00ULL
#define VM_VALUE_NULL 0x02ULL
#define VM_VALUE_FALSE 0x06ULL
#define VM_VALUE_TRUE 0x07ULL
#define VM_VALUE_UNDEFINED 0x0AULL

/* The tag every number has some of, and an int32 has all of. */
#define VM_VALUE_INT32_TAG 0xFFFE000000000000ULL

/* The bit the small constants share and cells never have. */
#define VM_VALUE_OTHER_TAG 0x02ULL

/* What is added to a double's bits. */
#define VM_VALUE_DOUBLE_OFFSET (1ULL << 49)

/* The bits of the canonical NaN, the one NaN a value holds. */
#define VM_VALUE_NAN_BITS 0x7FF8000000000000ULL

/* The attributes of a property. */
#define VM_PROPERTY_WRITABLE 0x1U
#define VM_PROPERTY_ENUMERABLE 0x2U
#define VM_PROPERTY_CONFIGURABLE 0x4U
#define VM_PROPERTY_ACCESSOR 0x8U

/* The attributes of a property made by assignment. */
#define VM_PROPERTY_DEFAULT (VM_PROPERTY_WRITABLE | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE)

/* The status a native function or an engine call reports when it threw (the value is the realm's exception). */
#define VM_THROWN (-1)

/* The object's flags: array identity, nonextensibility, and immutable BigInt TypedArray content. */
#define VM_OBJECT_ARRAY 0x1U
#define VM_OBJECT_NOT_EXTENSIBLE 0x2U
#define VM_OBJECT_TYPED_BIGINT 0x4U

/* The hints of ToPrimitive: which of valueOf and toString is tried first. */
#define VM_HINT_DEFAULT 0
#define VM_HINT_NUMBER 1
#define VM_HINT_STRING 2

/* Which fields a property descriptor has. */
#define VM_HAS_VALUE 0x01U
#define VM_HAS_WRITABLE 0x02U
#define VM_HAS_GET 0x04U
#define VM_HAS_SET 0x08U
#define VM_HAS_ENUMERABLE 0x10U
#define VM_HAS_CONFIGURABLE 0x20U

/* The states of a promise. */
#define VM_PROMISE_PENDING 0U
#define VM_PROMISE_FULFILLED 1U
#define VM_PROMISE_REJECTED 2U

/* How a generator or an async function is resumed: with a value, with an exception, or told to return. */
#define VM_RESUME_NEXT 0
#define VM_RESUME_THROW 1
#define VM_RESUME_RETURN 2

/*
 * The operators of vm_numeric: the arithmetic ones on numbers and the
 * bitwise ones on their int32 or uint32 values.
 */
enum vm_numeric_operator {
	VM_NUMERIC_SUB,
	VM_NUMERIC_MUL,
	VM_NUMERIC_DIV,
	VM_NUMERIC_MOD,
	VM_NUMERIC_EXP,
	VM_NUMERIC_AND,
	VM_NUMERIC_OR,
	VM_NUMERIC_XOR,
	VM_NUMERIC_SHL,
	VM_NUMERIC_SAR,
	VM_NUMERIC_SHR,
	VM_NUMERIC_ADD
};

/*
 * The relations of vm_relation.
 */
enum vm_relation {
	VM_RELATION_LESS,
	VM_RELATION_LESS_EQUAL,
	VM_RELATION_GREATER,
	VM_RELATION_GREATER_EQUAL
};

/*
 * The objects of a realm that the engine itself uses, beyond the three
 * prototypes every realm has: the built-ins fill them (js_install_builtins),
 * and until then they are NULL (errors are then thrown as strings).
 */
enum vm_intrinsic {
	VM_INTRINSIC_ERROR_PROTOTYPE,
	VM_INTRINSIC_TYPE_ERROR_PROTOTYPE,
	VM_INTRINSIC_RANGE_ERROR_PROTOTYPE,
	VM_INTRINSIC_REFERENCE_ERROR_PROTOTYPE,
	VM_INTRINSIC_SYNTAX_ERROR_PROTOTYPE,
	VM_INTRINSIC_EVAL_ERROR_PROTOTYPE,
	VM_INTRINSIC_URI_ERROR_PROTOTYPE,
	VM_INTRINSIC_BOOLEAN_PROTOTYPE,
	VM_INTRINSIC_NUMBER_PROTOTYPE,
	VM_INTRINSIC_STRING_PROTOTYPE,
	VM_INTRINSIC_SYMBOL_PROTOTYPE,
	VM_INTRINSIC_BIGINT_PROTOTYPE,
	VM_INTRINSIC_ARRAY_BUFFER_PROTOTYPE,
	VM_INTRINSIC_TYPED_ARRAY_PROTOTYPE,
	VM_INTRINSIC_DATA_VIEW_PROTOTYPE,
	VM_INTRINSIC_TYPED_PROTOTYPE_0,
	VM_INTRINSIC_TYPED_PROTOTYPE_1,
	VM_INTRINSIC_TYPED_PROTOTYPE_2,
	VM_INTRINSIC_TYPED_PROTOTYPE_3,
	VM_INTRINSIC_TYPED_PROTOTYPE_4,
	VM_INTRINSIC_TYPED_PROTOTYPE_5,
	VM_INTRINSIC_TYPED_PROTOTYPE_6,
	VM_INTRINSIC_TYPED_PROTOTYPE_7,
	VM_INTRINSIC_TYPED_PROTOTYPE_8,
	VM_INTRINSIC_TYPED_PROTOTYPE_9,
	VM_INTRINSIC_TYPED_PROTOTYPE_10,
	VM_INTRINSIC_THROW_TYPE_ERROR,
	VM_INTRINSIC_REGEXP_PROTOTYPE,
	VM_INTRINSIC_REGEXP,
	VM_INTRINSIC_DATE_PROTOTYPE,
	VM_INTRINSIC_PROMISE_PROTOTYPE,
	VM_INTRINSIC_PROMISE,
	VM_INTRINSIC_GENERATOR_PROTOTYPE,
	VM_INTRINSIC_GENERATOR_FUNCTION_PROTOTYPE,
	VM_INTRINSIC_ASYNC_FUNCTION_PROTOTYPE,
	VM_INTRINSIC_ITERATOR_PROTOTYPE,
	VM_INTRINSIC_ARRAY_ITERATOR_PROTOTYPE,
	VM_INTRINSIC_ARRAY_VALUES,
	VM_INTRINSIC_STRING_ITERATOR,
	VM_INTRINSIC_HAS_INSTANCE,
	VM_INTRINSIC_SYMBOL_REGISTRY,
	VM_INTRINSIC_MAP_ITERATOR_PROTOTYPE,
	VM_INTRINSIC_SET_ITERATOR_PROTOTYPE,
	VM_INTRINSIC_STRING_ITERATOR_PROTOTYPE,
	VM_INTRINSIC_MAP_PROTOTYPE,
	VM_INTRINSIC_SET_PROTOTYPE,
	VM_INTRINSIC_WEAK_MAP_PROTOTYPE,
	VM_INTRINSIC_WEAK_SET_PROTOTYPE,
	VM_INTRINSICS
};

/*
 * The well-known symbols, which every realm of a heap makes
 * when it is made (the engine's own operations look them up, before any
 * built-in exists).
 */
enum vm_well_known {
	VM_SYMBOL_ASYNC_ITERATOR,
	VM_SYMBOL_HAS_INSTANCE,
	VM_SYMBOL_IS_CONCAT_SPREADABLE,
	VM_SYMBOL_ITERATOR,
	VM_SYMBOL_MATCH,
	VM_SYMBOL_MATCH_ALL,
	VM_SYMBOL_REPLACE,
	VM_SYMBOL_SEARCH,
	VM_SYMBOL_SPECIES,
	VM_SYMBOL_SPLIT,
	VM_SYMBOL_TO_PRIMITIVE,
	VM_SYMBOL_TO_STRING_TAG,
	VM_SYMBOL_UNSCOPABLES,
	VM_SYMBOLS
};

/* The error kinds of vm_throw_error, in the order of their intrinsic prototypes. */
enum vm_error_kind {
	VM_ERROR_PLAIN,
	VM_ERROR_TYPE,
	VM_ERROR_RANGE,
	VM_ERROR_REFERENCE,
	VM_ERROR_SYNTAX,
	VM_ERROR_EVAL,
	VM_ERROR_URI
};

/*
 * The kinds of object the built-ins tell apart (Object.prototype.toString,
 * the methods that need a Boolean, a Number, a String or an Error).
 */
enum vm_object_kind {
	VM_KIND_OBJECT,
	VM_KIND_ARRAY,
	VM_KIND_FUNCTION,
	VM_KIND_ARGUMENTS,
	VM_KIND_ERROR,
	VM_KIND_BOOLEAN,
	VM_KIND_NUMBER,
	VM_KIND_STRING,
	VM_KIND_SYMBOL,
	VM_KIND_BIGINT,
	VM_KIND_ARRAY_BUFFER,
	VM_KIND_TYPED_ARRAY,
	VM_KIND_DATA_VIEW,
	VM_KIND_DATE,
	VM_KIND_REGEXP,
	VM_KIND_PLATFORM,
	VM_KIND_PROMISE,
	VM_KIND_GENERATOR,
	VM_KIND_ITERATOR,
	VM_KIND_MAP,
	VM_KIND_SET,
	VM_KIND_WEAK_MAP,
	VM_KIND_WEAK_SET,
	VM_KIND_PROXY
};

/* Heap and cell identity precede the callbacks that refer to them. */
struct vm_heap;
struct vm_cell;

/*
 * The behavior every cell of one kind shares.
 *
 * trace marks the cells a cell refers to (with vm_heap_mark); finalize
 * frees what a dead cell owns outside the heap and must not allocate in
 * the heap.  Either may be NULL.  A type lives as long as the program:
 * cells point to it.
 */
struct vm_cell_type {
	const char *name;
	void (*trace)(struct vm_heap *heap, struct vm_cell *cell);
	void (*finalize)(struct vm_heap *heap, struct vm_cell *cell);
};

/*
 * The header at the start of every cell.
 *
 * A cell's own structure begins with this header, so a pointer to the
 * structure is a pointer to its cell.
 */
struct vm_cell {
	const struct vm_cell_type *type;
};

/*
 * A string in the heap: Latin-1 (one byte per unit) or UTF-16.
 *
 * The characters follow the header; vm_string_latin1 and vm_string_units
 * find them.  Strings never change after they are made.  An atom is a
 * string interned in its heap's atom table: two atoms with the same
 * characters are the same cell, so atoms compare by pointer.
 */
struct vm_string {
	struct vm_cell cell;
	uint32_t length;
	uint32_t hash;
	uint32_t flags;
	uint32_t reserved;
};

/*
 * A function that marks cells a subsystem holds outside the heap.
 */
typedef void (*vm_tracer)(struct vm_heap *heap, void *context);

/*
 * What the heap has done, for tests and diagnostics.
 */
struct vm_heap_stats {
	size_t live_bytes;
	size_t live_cells;
	size_t heap_bytes;
	size_t collections;
	size_t freed_cells;
};

/*
 * A value of the engine: 64 bits, NaN-boxed as in JavaScriptCore.
 *
 * A cell (an object, a string, a symbol) is its pointer, whose top 16 bits
 * are zero and whose low 4 bits are zero (cells are 16-byte aligned).  An
 * int32 is VM_VALUE_INT32_TAG with the number in the low 32 bits.  A double
 * is its bits plus 2^49, which puts every double (NaN made canonical) above
 * the pointers and below the int32s.  undefined, null, true, false and the
 * empty value (an array's hole, a slot never written) are small constants
 * no cell can have.  Wasm keeps its values unboxed in registers, so
 * this form is only JavaScript's.
 */
typedef uint64_t vm_value;

/* Shapes, execution owners and lexical environments have private definitions. */
struct vm_shape;
struct vm_realm;
struct vm_code;
struct vm_env;

/*
 * A symbol: a unique property key with a description (a string or
 * undefined).  A private name (a class's #x) is a symbol too,
 * marked private_name: it is never listed among an object's keys, and
 * only the class's code can read it.  registered marks a symbol of the
 * global registry (Symbol.for), whose key is its description.
 */
struct vm_symbol {
	struct vm_cell cell;
	vm_value description;
	int private_name;
	int registered;
};

/*
 * A BigInt: a sign and a magnitude of 32-bit limbs (least
 * significant first, no leading zero limb) that follow the structure.
 */
struct vm_bigint {
	struct vm_cell cell;
	uint32_t length;
	uint32_t negative;
};

/*
 * An object: its shape (which names its properties and where each one's
 * value is), its prototype (NULL for none), the values of its named
 * properties, and its elements (the values of its index properties with
 * the default attributes, densely, VM_VALUE_EMPTY for a hole).
 *
 * The slots and the elements are malloc'd and freed with the cell.  For an
 * array, length is the array's length; for other objects it is one past
 * the highest element in use.  kind is an enum vm_object_kind, and
 * internal the value a wrapper holds (a Boolean's, Number's or String's
 * primitive, or a platform object's cell: the DOM node or the event it
 * stands for).  Every kind of object (functions, the DOM's wrappers, and
 * later Wasm instances) begins with this structure.
 */
struct vm_object {
	struct vm_cell cell;
	struct vm_shape *shape;
	struct vm_object *prototype;
	vm_value *slots;
	uint32_t slot_capacity;
	uint32_t flags;
	vm_value *elements;
	uint32_t length;
	uint32_t element_capacity;
	uint32_t kind;
	uint32_t reserved;
	vm_value internal;
	/* Optional C property operations expose native live views without stored fake slots. */
	const struct vm_native_operations *native_operations;
};

/*
 * An accessor property's getter and setter (each a function, or
 * undefined), kept in the property's slot.
 */
struct vm_accessor {
	struct vm_cell cell;
	vm_value getter;
	vm_value setter;
};

/*
 * A property found on an object: the object that has it, its attributes
 * and where its value is (a slot or an element).
 */
struct vm_property {
	struct vm_object *holder;
	vm_value *value;
	uint32_t attributes;
	/* Virtual properties publish their data in the caller's property record. */
	vm_value temporary;
};

/*
 * A property descriptor (Object.defineProperty's argument, or what
 * getOwnPropertyDescriptor reports): the fields present (VM_HAS_*), the
 * value or the getter and setter, and the attributes among
 * VM_PROPERTY_WRITABLE, VM_PROPERTY_ENUMERABLE and VM_PROPERTY_CONFIGURABLE
 * (meaningful where present).
 */
struct vm_descriptor {
	uint32_t has;
	uint32_t attributes;
	vm_value value;
	vm_value getter;
	vm_value setter;
};

/*
 * Optional native property policy is shared immutable C code, not a GC owner.
 * Get-own reports missing 0, found 1 or negative errno; other hooks return errno.
 * Virtual data belongs in property.temporary with value pointing to that member.
 * Define receives NULL realm for the heap-level assignment API; it cannot run JS.
 * Define/delete may leave handled false for ordinary fallback; own keys append
 * a prefix whose ordering is native policy. Callbacks cannot run user script.
 */
struct vm_native_operations {
	int (*get_own)(struct vm_object *object, vm_value key, struct vm_property *property);
	int (*own_keys)(struct vm_heap *heap, struct vm_object *object, struct wb_vector *keys);
	int (*define)(struct vm_realm *realm, struct vm_object *object, vm_value key, const struct vm_descriptor *descriptor, int *handled, int *done);
	int (*delete)(struct vm_heap *heap, struct vm_object *object, vm_value key, int *handled, int *deleted);
	int (*prevent_extensions)(struct vm_object *object, int *allowed);
};

/*
 * A native function: what it does when called, with the this value and
 * the arguments; it stores its result, and returns 0, VM_THROWN with the
 * realm's exception set, or an errno value.
 */
typedef int (*vm_native)(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * A function: an object that can be called, with its realm and what runs
 * when it is called: a code unit of bytecode (with the environment of the
 * code it was made in, NULL for none), or a native function.  A native
 * constructor also has what runs for new (it makes its own object from
 * the realm's new_target); data is what a native function keeps for
 * itself (a bound function's target, this and arguments).
 */
struct vm_function {
	struct vm_object object;
	struct vm_realm *realm;
	/* Only managed owner cells are traced; raw manual realms may be retired. */
	struct vm_cell *realm_owner;
	vm_native native;
	vm_native construct;
	struct vm_code *code;
	struct vm_env *env;
	vm_value data;
	/* Embedding-owned immutable source metadata is inherited by nested closures. */
	vm_value source;
	/* VM-owned BoundTargetFunction is undefined for ordinary functions and traced for bound functions. */
	vm_value bound_target;
};

/*
 * A realm: the global object and the intrinsic objects a script's objects
 * are made from, the exception being thrown, and the VM stack its code
 * runs on.
 *
 * A primary realm registers a tracer that keeps its objects and
 * marks every word of the used stack conservatively (Wasm's raw values sit
 * beside JavaScript's boxed ones there), and lives until vm_realm_destroy.
 * A managed child is itself a cell: functions, global/prototype internal values
 * and embedding Documents retain it. Only GC releases its stack and host hooks.
 * depth counts how many times the interpreter is entered from C, which a
 * native function calling a script function does.  host is what the
 * embedder keeps with the realm (the DOM binding's window), and jobs the
 * queue of microtasks (struct vm_job) the next checkpoint runs.
 * lexicals holds the top-level let and const of every script run in the
 * realm (the global declarative record, which the scripts share and which
 * is not the global object): a const is a property that is not writable,
 * and a declaration that has not run yet holds the empty value.
 * throw_value, throw_line and throw_column are where the exception
 * throw_value was last seen leaving a bytecode frame without a handler
 * (throw_line 0 when that place is not known); the embedder reads them
 * with vm_throw_site to report an uncaught exception's place.
 * symbols are the well-known symbols (enum vm_well_known).
 * rejections lists the promises rejected while nothing handled them
 * which the end of a checkpoint reports unless a handler
 * came meanwhile; reporting_rejection is set while the report of one runs,
 * so the embedder writes it as a promise's ("Uncaught (in promise)").
 */
struct vm_realm {
	struct vm_cell cell;
	struct vm_heap *heap;
	/* Managed child realms and their host are retained by traced references. */
	int managed;
	vm_tracer host_trace;
	void (*host_release)(void *context);
	struct vm_object *global;
	struct vm_object *object_prototype;
	struct vm_object *function_prototype;
	struct vm_object *array_prototype;
	struct vm_object *intrinsics[VM_INTRINSICS];
	struct vm_symbol *symbols[VM_SYMBOLS];
	struct vm_object *lexicals;
	vm_value exception;
	vm_value throw_value;
	/* The first throwing frame contributes its opaque defining-source metadata. */
	vm_value throw_source;
	uint32_t throw_line;
	char throw_stack[200];
	uint32_t throw_column;
	vm_value callee;
	vm_value new_target;
	vm_value *stack;
	uint32_t stack_capacity;
	uint32_t stack_top;
	unsigned depth;
	/* Counts realm-aware property calls so native fallback chains cannot recurse without a bound. */
	unsigned property_depth;
	void *host;
	struct wb_vector jobs;
	/* Where the microtasks of a retired realm are queued instead (a live realm that drains them), or NULL. */
	struct vm_realm *job_target;
	struct wb_vector rejections;
	int reporting_rejection;
};

/*
 * One microtask: a function to call with one argument (queueMicrotask's
 * callback), or a promise's job (a cell of vm_promise_job_type: a
 * reaction, or the resolution of a promise with a thenable) with its
 * argument.
 */
struct vm_job {
	vm_value callback;
	vm_value argument;
};

/*
 * What a microtask checkpoint does with an exception a job threw: the
 * embedder reports it (the console's "Uncaught ...") and the checkpoint
 * goes on with the next job.
 */
typedef void (*vm_job_report)(struct vm_realm *realm, vm_value exception, void *context);

/* Strings (string.c). */
extern const struct vm_cell_type vm_string_type;

/* Objects (object.c). */
extern const struct vm_cell_type vm_object_type;
extern const struct vm_cell_type vm_array_type;
extern const struct vm_cell_type vm_symbol_type;
extern const struct vm_cell_type vm_bigint_type;
extern const struct vm_cell_type vm_accessor_type;

/* Functions (function.c). */
extern const struct vm_cell_type vm_function_type;

/* Promises (promise.c). */
extern const struct vm_cell_type vm_promise_type;
extern const struct vm_cell_type vm_promise_job_type;

/* The heap (heap.c). */
int vm_heap_create(struct vm_heap **heap, size_t limit);
void vm_heap_destroy(struct vm_heap *heap);
void vm_heap_set_stack_base(struct vm_heap *heap, const void *base);
void *vm_heap_alloc(struct vm_heap *heap, const struct vm_cell_type *type, size_t size);
void vm_heap_collect(struct vm_heap *heap);
void vm_heap_mark(struct vm_heap *heap, struct vm_cell *cell);
void vm_heap_mark_word(struct vm_heap *heap, uintptr_t word);
int vm_heap_add_root(struct vm_heap *heap, struct vm_cell **slot);
void vm_heap_remove_root(struct vm_heap *heap, struct vm_cell **slot);
int vm_heap_add_tracer(struct vm_heap *heap, vm_tracer tracer, void *context);
void vm_heap_remove_tracer(struct vm_heap *heap, vm_tracer tracer, void *context);
struct vm_cell *vm_heap_find_cell(struct vm_heap *heap, uintptr_t word);
void vm_heap_stats(const struct vm_heap *heap, struct vm_heap_stats *stats);

/* Strings (string.c). */
struct vm_string *vm_string_from_latin1(struct vm_heap *heap, const unsigned char *bytes, size_t length);
struct vm_string *vm_string_from_units(struct vm_heap *heap, const uint16_t *units, size_t length);
struct vm_string *vm_string_from_utf8(struct vm_heap *heap, const char *bytes, size_t length);
struct vm_string *vm_string_concat(struct vm_heap *heap, const struct vm_string *left, const struct vm_string *right);
const unsigned char *vm_string_latin1(const struct vm_string *string);
const uint16_t *vm_string_units(const struct vm_string *string);
uint16_t vm_string_at(const struct vm_string *string, size_t index);
uint32_t vm_string_hash(struct vm_string *string);
int vm_string_equal(const struct vm_string *left, const struct vm_string *right);
int vm_string_equal_ascii(const struct vm_string *string, const char *ascii);
int vm_string_equal_units(const struct vm_string *string, const uint16_t *units, size_t length);
int vm_string_compare(const struct vm_string *left, const struct vm_string *right);
int vm_string_to_utf8(const struct vm_string *string, struct wb_buffer *buffer);
int vm_string_append_units(const struct vm_string *string, struct wb_units *units);

/* Atoms (atom.c). */
struct vm_string *vm_atom(struct vm_heap *heap, struct vm_string *string);
struct vm_string *vm_atom_from_ascii(struct vm_heap *heap, const char *ascii);
struct vm_string *vm_atom_from_units(struct vm_heap *heap, const uint16_t *units, size_t length);
struct vm_string *vm_atom_find_units(struct vm_heap *heap, const uint16_t *units, size_t length);

/* Values and keys (object.c). */
void vm_heap_mark_value(struct vm_heap *heap, vm_value value);
int vm_value_is_object(vm_value value);
int vm_value_is_string(vm_value value);
int vm_value_is_array_index(vm_value key, uint32_t *index);
int vm_key_from_string(struct vm_heap *heap, struct vm_string *string, vm_value *key);
vm_value vm_key_from_ascii(struct vm_heap *heap, const char *ascii);

/* Objects (object.c). */
struct vm_object *vm_object_create(struct vm_heap *heap, struct vm_object *prototype);
struct vm_object *vm_array_create(struct vm_heap *heap, struct vm_object *prototype);
int vm_proxy_create(struct vm_realm *realm, vm_value target, vm_value handler, vm_value *result);
int vm_proxy_revoke(struct vm_realm *realm, vm_value proxy);
int vm_object_init(struct vm_heap *heap, struct vm_object *object, struct vm_object *prototype);
struct vm_symbol *vm_symbol_create(struct vm_heap *heap, vm_value description);
struct vm_accessor *vm_accessor_create(struct vm_heap *heap, vm_value getter, vm_value setter);

/* Lookups report missing 0, found 1, or negative errno; status APIs remain positive errno. */
int vm_object_get_own(struct vm_object *object, vm_value key, struct vm_property *property);
int vm_object_get_own_ordinary(struct vm_object *object, vm_value key, struct vm_property *property);
int vm_object_find(struct vm_object *object, vm_value key, struct vm_property *property);
int vm_object_define(struct vm_heap *heap, struct vm_object *object, vm_value key, vm_value value, uint32_t attributes);
int vm_object_get(struct vm_object *object, vm_value key, vm_value *value);
int vm_object_set(struct vm_heap *heap, struct vm_object *object, vm_value key, vm_value value, int *done);
int vm_object_delete(struct vm_heap *heap, struct vm_object *object, vm_value key, int *deleted);
/* EPERM reports a fixed-index shrink refusal after higher deletions and length restoration to the failed index plus one. */
int vm_array_set_length(struct vm_heap *heap, struct vm_object *array, uint32_t length);
int vm_object_own_keys(struct vm_heap *heap, struct vm_object *object, struct wb_vector *keys);
int vm_object_prevent_extensions(struct vm_object *object, int *done);
void vm_object_trace(struct vm_heap *heap, struct vm_cell *cell);
void vm_object_finalize(struct vm_heap *heap, struct vm_cell *cell);

/* Shapes (shape.c). */
struct vm_shape *vm_shape_root(struct vm_heap *heap);
struct vm_shape *vm_shape_add(struct vm_heap *heap, struct vm_shape *shape, vm_value key, uint32_t attributes);
int vm_shape_find(const struct vm_shape *shape, vm_value key, uint32_t *slot, uint32_t *attributes);
uint32_t vm_shape_count(const struct vm_shape *shape);
int vm_shape_keys(const struct vm_shape *shape, vm_value *keys, uint32_t *slots, uint32_t *attributes);

/* Functions (function.c). */
struct vm_function *vm_function_create_native(struct vm_realm *realm, const char *name, unsigned length, vm_native native);
int vm_value_is_callable(vm_value value);
int vm_value_is_constructor(vm_value value);
int vm_construct_this(struct vm_realm *realm, vm_value constructor, vm_value *object);
int vm_construct_prototype(struct vm_realm *realm, vm_value new_target, struct vm_object *fallback, struct vm_object **prototype);
int vm_construct(struct vm_realm *realm, vm_value constructor, const vm_value *args, unsigned count, vm_value new_target, vm_value *result);
int vm_call(struct vm_realm *realm, vm_value callee, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int vm_throw(struct vm_realm *realm, vm_value exception);
int vm_throw_site(const struct vm_realm *realm, vm_value exception, uint32_t *line, uint32_t *column);
const char *vm_throw_stack(const struct vm_realm *realm, vm_value exception);

/* Realms (realm.c). */
int vm_realm_create(struct vm_heap *heap, struct vm_realm **realm);

/* Makes a collectible child realm; only the heap destroys its resources. */
int vm_realm_create_managed(struct vm_heap *heap, struct vm_realm **realm);
void vm_realm_destroy(struct vm_realm *realm);
int vm_enqueue_job(struct vm_realm *realm, vm_value callback, vm_value argument);
int vm_run_jobs(struct vm_realm *realm, vm_job_report report, void *context);

/* Numbers and their decimal text (number.c). */
int vm_number_to_text(double number, int radix, struct wb_buffer *out);
int vm_number_to_fixed(double number, int fraction_digits, struct wb_buffer *out);
int vm_number_to_exponential(double number, int fraction_digits, struct wb_buffer *out);
int vm_number_to_precision(double number, int precision, struct wb_buffer *out);
double vm_number_parse(const char *text, size_t length);
double vm_number_parse_radix(const char *text, size_t length, int radix);

/* JavaScript's operations on values (operation.c). */
int vm_to_boolean(vm_value value);
int vm_to_primitive(struct vm_realm *realm, vm_value value, int hint, vm_value *result);
int vm_ordinary_to_primitive(struct vm_realm *realm, vm_value value, int hint, vm_value *result);
double vm_string_to_number(const struct vm_string *string);
int vm_canonical_numeric_index(vm_value key, double *number, int *canonical);
int vm_to_bigint(struct vm_realm *realm, vm_value value, vm_value *result);
int vm_to_number(struct vm_realm *realm, vm_value value, double *number);
int vm_to_numeric(struct vm_realm *realm, vm_value value, vm_value *numeric);

/* BigInt (bigint.c). */
int vm_bigint_from_double(struct vm_heap *heap, double number, vm_value *result);
int vm_bigint_from_int64(struct vm_heap *heap, int64_t value, vm_value *result);
int vm_bigint_from_digits(struct vm_heap *heap, const char *digits, size_t length, int radix, int negative, vm_value *result);
int vm_bigint_from_text(struct vm_heap *heap, const char *text, size_t length, vm_value *result);
int vm_bigint_to_text(const struct vm_bigint *big, int radix, struct wb_buffer *out);
double vm_bigint_to_double(const struct vm_bigint *big);
int vm_bigint_is_zero(const struct vm_bigint *big);
int vm_bigint_compare(const struct vm_bigint *a, const struct vm_bigint *b);
int vm_bigint_compare_double(struct vm_heap *heap, const struct vm_bigint *a, double number, int *order);
int vm_bigint_binary(struct vm_realm *realm, int operator, const struct vm_bigint *left, const struct vm_bigint *right, vm_value *result);
int vm_bigint_negate(struct vm_heap *heap, const struct vm_bigint *big, vm_value *result);
int vm_bigint_add_small(struct vm_heap *heap, const struct vm_bigint *big, int64_t amount, vm_value *result);
int vm_bigint_not(struct vm_heap *heap, const struct vm_bigint *big, vm_value *result);
int vm_bigint_as_n(struct vm_realm *realm, uint64_t bits, int is_signed, const struct vm_bigint *big, vm_value *result);
struct vm_bigint *vm_value_as_bigint(vm_value value);
uint32_t vm_bigint_hash(const struct vm_bigint *big);
uint64_t vm_bigint_to_uint64(const struct vm_bigint *big);
int vm_bigint_from_uint64(struct vm_heap *heap, uint64_t value, vm_value *result);
int vm_to_int32(struct vm_realm *realm, vm_value value, int32_t *number);
int vm_to_uint32(struct vm_realm *realm, vm_value value, uint32_t *number);
int vm_to_string(struct vm_realm *realm, vm_value value, struct vm_string **string);
int vm_to_key(struct vm_realm *realm, vm_value value, vm_value *key);
int vm_strict_equals(vm_value left, vm_value right);
int vm_loose_equals(struct vm_realm *realm, vm_value left, vm_value right, int *equal);
int vm_add(struct vm_realm *realm, vm_value left, vm_value right, vm_value *result);
int vm_numeric(struct vm_realm *realm, int operator, vm_value left, vm_value right, vm_value *result);
int vm_less(struct vm_realm *realm, vm_value left, vm_value right, vm_value *result);
int vm_relation(struct vm_realm *realm, int relation, vm_value left, vm_value right, vm_value *result);
int vm_typeof(struct vm_realm *realm, vm_value value, vm_value *result);
int vm_throw_error(struct vm_realm *realm, int kind, const char *message);
int vm_error_create(struct vm_realm *realm, int kind, const char *message, vm_value *error_value);
int vm_throw_type_error(struct vm_realm *realm, const char *message);
int vm_throw_range_error(struct vm_realm *realm, const char *message);
int vm_throw_reference_error(struct vm_realm *realm, const char *message);
int vm_throw_not_defined(struct vm_realm *realm, vm_value key);
int vm_throw_uninitialized(struct vm_realm *realm, vm_value key);
int vm_throw_redeclared(struct vm_realm *realm, vm_value key);
int vm_is_space(uint16_t unit);

/* Properties of any value, globals and enumeration (access.c). */
int vm_get(struct vm_realm *realm, vm_value base, vm_value key, vm_value *result);
int vm_get_receiver(struct vm_realm *realm, vm_value base, vm_value key, vm_value receiver, vm_value *result);
int vm_put(struct vm_realm *realm, vm_value base, vm_value key, vm_value value);
int vm_set(struct vm_realm *realm, vm_value base, vm_value key, vm_value value, int strict);
int vm_set_receiver(struct vm_realm *realm, struct vm_object *object, vm_value key, vm_value value, vm_value receiver, int *done);
int vm_delete_property(struct vm_realm *realm, struct vm_object *object, vm_value key, int *deleted);
int vm_delete(struct vm_realm *realm, vm_value base, vm_value key, int strict, vm_value *result);

/* Realm own-key enumeration appends on success and preserves the caller prefix on failure. */
int vm_own_property_keys(struct vm_realm *realm, struct vm_object *object, struct wb_vector *keys);

/* Prototype operations preserve Proxy internal methods and original requested-prototype ownership. */
int vm_get_prototype(struct vm_realm *realm, struct vm_object *object, vm_value *prototype);
int vm_set_prototype(struct vm_realm *realm, struct vm_object *object, struct vm_object *prototype, int *done);

/* Realm-aware extensibility operations preserve Proxy traps and pure native policy. */
int vm_is_extensible(struct vm_realm *realm, struct vm_object *object, int *extensible);
int vm_prevent_extensions(struct vm_realm *realm, struct vm_object *object, int *done);

/* Array identity unwraps native Proxy targets and rejects revoked records. */
int vm_is_array(struct vm_realm *realm, vm_value value, int *is_array);

int vm_has_property(struct vm_realm *realm, struct vm_object *object, vm_value key, int *found);
int vm_in(struct vm_realm *realm, vm_value key, vm_value object, vm_value *result);
int vm_instanceof(struct vm_realm *realm, vm_value value, vm_value constructor, vm_value *result);
int vm_ordinary_has_instance(struct vm_realm *realm, vm_value constructor, vm_value value, vm_value *result);
int vm_define_data(struct vm_realm *realm, vm_value object, vm_value key, vm_value value);
int vm_define_accessor(struct vm_realm *realm, vm_value object, vm_value key, vm_value function, int setter);
int vm_get_global(struct vm_realm *realm, vm_value key, int for_typeof, vm_value *result);
int vm_put_global(struct vm_realm *realm, vm_value key, vm_value value, int strict);
int vm_define_global_var(struct vm_realm *realm, vm_value key);
int vm_define_global_function(struct vm_realm *realm, vm_value key, vm_value function);
int vm_delete_global(struct vm_realm *realm, vm_value key, vm_value *result);
int vm_define_global_lexical(struct vm_realm *realm, vm_value key, int is_const);

/* Classes (class.c). */
int vm_class_setup(struct vm_realm *realm, vm_value constructor, vm_value parent, vm_value *prototype);
int vm_define_method(struct vm_realm *realm, vm_value home, vm_value key, vm_value function, uint32_t kind);
int vm_get_super(struct vm_realm *realm, vm_value home, vm_value key, vm_value this_value, vm_value *result);
int vm_throw_class_call(struct vm_realm *realm, struct vm_function *function);
int vm_private_get(struct vm_realm *realm, vm_value object, vm_value key, vm_value *result);
int vm_private_set(struct vm_realm *realm, vm_value object, vm_value key, vm_value value);
int vm_private_define(struct vm_realm *realm, vm_value object, vm_value key, vm_value value);
int vm_private_copy(struct vm_realm *realm, vm_value target, vm_value source, vm_value key);
int vm_private_in(struct vm_realm *realm, vm_value key, vm_value object, vm_value *result);

/* Promises (promise.c). */
int vm_value_is_promise(vm_value value);
int vm_promise_create(struct vm_realm *realm, struct vm_object *prototype, vm_value *promise);
int vm_promise_state(vm_value promise, vm_value *result);
int vm_promise_resolving_functions(struct vm_realm *realm, vm_value promise, vm_value *resolve, vm_value *reject);
int vm_promise_resolve(struct vm_realm *realm, vm_value promise, vm_value resolution);
int vm_promise_reject(struct vm_realm *realm, vm_value promise, vm_value reason);
int vm_promise_then(struct vm_realm *realm, vm_value promise, vm_value on_fulfilled, vm_value on_rejected, vm_value derived, vm_value resolve, vm_value reject);
int vm_promise_run_job(struct vm_realm *realm, vm_value job, vm_value argument);

/* Generators and async functions (generator.c). */
int vm_generator_call(struct vm_realm *realm, struct vm_function *function, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int vm_generator_resume(struct vm_realm *realm, vm_value generator, vm_value value, int how, vm_value *result, int *done, int *direct_result);
int vm_delegate_start(struct vm_realm *realm, vm_value iterable, vm_value *result);
int vm_delegate_step(struct vm_realm *realm, vm_value state, vm_value sent, int how, vm_value *result, int *outcome);
int vm_code_is_suspendable(const struct vm_function *function);

/* Spreading and destructuring (spread.c). */
int vm_iter_start(struct vm_realm *realm, vm_value value, vm_value *iterator);
int vm_iter_next(struct vm_realm *realm, vm_value iterator, vm_value *value, int *done);
int vm_iter_close(struct vm_realm *realm, vm_value iterator, int quiet);
vm_value vm_symbol_key(const struct vm_realm *realm, int which);
int vm_get_method(struct vm_realm *realm, vm_value value, vm_value key, vm_value *method);
int vm_iter_rest(struct vm_realm *realm, vm_value iterator, vm_value *array);
int vm_array_spread(struct vm_realm *realm, vm_value array, vm_value value);
int vm_copy_data_properties(struct vm_realm *realm, vm_value target, vm_value source);
int vm_object_rest(struct vm_realm *realm, vm_value source, vm_value excluded, vm_value *result);
int vm_call_array(struct vm_realm *realm, vm_value function, vm_value this_value, vm_value array, int construct, vm_value *result);
int vm_init_global_lexical(struct vm_realm *realm, vm_value key, vm_value value);
int vm_to_object(struct vm_realm *realm, vm_value value, vm_value *object);
int vm_to_property_descriptor(struct vm_realm *realm, vm_value value, struct vm_descriptor *descriptor);
int vm_from_property_descriptor(struct vm_realm *realm, const struct vm_descriptor *descriptor, vm_value *result);
void vm_complete_property_descriptor(struct vm_descriptor *descriptor);
int vm_get_own_property(struct vm_realm *realm, struct vm_object *object, vm_value key, struct vm_descriptor *descriptor, int *found);
int vm_get_own_descriptor(struct vm_object *object, vm_value key, struct vm_descriptor *descriptor);
int vm_define_own_property(struct vm_realm *realm, struct vm_object *object, vm_value key, const struct vm_descriptor *descriptor, int *done);
int vm_same_value(vm_value left, vm_value right);
int vm_for_in_start(struct vm_realm *realm, vm_value value, vm_value *iterator);
int vm_for_in_next(struct vm_realm *realm, vm_value iterator, vm_value *key, int *done);

/* The interpreter (interpreter.c). */
int vm_interpret(struct vm_realm *realm, struct vm_function *function, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int vm_interpret_construct(struct vm_realm *realm, struct vm_function *function, vm_value this_value, const vm_value *args, unsigned count, vm_value new_target, vm_value *result);

/* Makes an int32 value. */
static __inline vm_value
vm_value_int32(
	int32_t number)
{
	/* The tag, and the number's 32 bits. */
	return VM_VALUE_INT32_TAG | (uint64_t)(uint32_t)number;
}

/* Makes a double value (the NaNs become the canonical one). */
static __inline vm_value
vm_value_double(
	double number)
{
	uint64_t bits;

	/* The double's bits; a NaN of any payload becomes the canonical one. */
	memcpy(&bits, &number, sizeof(bits));
	if (number != number)
		bits = VM_VALUE_NAN_BITS;

	/* Shifted above the pointers. */
	return bits + VM_VALUE_DOUBLE_OFFSET;
}

/* Makes a number value: an int32 when the number is one (not -0), a double otherwise. */
static __inline vm_value
vm_value_number(
	double number)
{
	int32_t whole;
	vm_value result;

	/* A number out of int32's range, or with a fraction, stays a double. */
	if (!(number >= -2147483648.0 && number <= 2147483647.0)) {
		result = vm_value_double(number);
		return result;
	}

	/* Only in-range values may be converted to int32 without overflow. */
	whole = (int32_t)number;
	if ((double)whole != number) {
		result = vm_value_double(number);
		return result;
	}

	/* Zero keeps its sign as a double (-0 is not an int32). */
	if (whole == 0 && 1.0 / number < 0.0) {
		result = vm_value_double(number);
		return result;
	}

	/* A whole number in range is an int32. */
	result = vm_value_int32(whole);

	/* The boxed value retains this exact integer. */
	return result;
}

/* Makes a cell's value. */
static __inline vm_value
vm_value_cell(
	const void *cell)
{
	/* The pointer itself. */
	return (vm_value)(uintptr_t)cell;
}

/* Makes true or false. */
static __inline vm_value
vm_value_boolean(
	int truth)
{
	/* True for any nonzero truth. */
	if (truth)
		return VM_VALUE_TRUE;

	/* False otherwise. */
	return VM_VALUE_FALSE;
}

/* Tells whether a value is an int32. */
static __inline int
vm_value_is_int32(
	vm_value value)
{
	/* Every tag bit is set. */
	if ((value & VM_VALUE_INT32_TAG) == VM_VALUE_INT32_TAG)
		return 1;

	/* Some tag bit is clear. */
	return 0;
}

/* Tells whether a value is a number (an int32 or a double). */
static __inline int
vm_value_is_number(
	vm_value value)
{
	/* Some tag bit is set. */
	if ((value & VM_VALUE_INT32_TAG) != 0U)
		return 1;

	/* No tag bit: a cell or a constant. */
	return 0;
}

/* Tells whether a value is a double. */
static __inline int
vm_value_is_double(
	vm_value value)
{
	int number;
	int integer;

	/* A number that is not an int32. */
	number = vm_value_is_number(value);
	if (!number)
		return 0;

	/* An int32 is boxed separately from every double. */
	integer = vm_value_is_int32(value);
	if (integer)
		return 0;

	/* A double. */
	return 1;
}

/* Tells whether a value is a cell (not the empty value). */
static __inline int
vm_value_is_cell(
	vm_value value)
{
	/* No tag bit and not the other constants' bit. */
	if ((value & (VM_VALUE_INT32_TAG | VM_VALUE_OTHER_TAG)) != 0U)
		return 0;

	/* The empty value is zero, which is no cell. */
	if (value == VM_VALUE_EMPTY)
		return 0;

	/* A cell's pointer. */
	return 1;
}

/* Tells whether a value is true or false. */
static __inline int
vm_value_is_boolean(
	vm_value value)
{
	/* The two constants differ only in their lowest bit. */
	if ((value & ~1ULL) == VM_VALUE_FALSE)
		return 1;

	/* Anything else. */
	return 0;
}

/* Reports an int32 value's number. */
static __inline int32_t
vm_value_as_int32(
	vm_value value)
{
	/* The low 32 bits. */
	return (int32_t)(uint32_t)value;
}

/* Reports a double value's number. */
static __inline double
vm_value_as_double(
	vm_value value)
{
	uint64_t bits;
	double number;

	/* The bits before the shift. */
	bits = value - VM_VALUE_DOUBLE_OFFSET;
	memcpy(&number, &bits, sizeof(number));

	/* Reports the double. */
	return number;
}

/* Reports a number value (an int32 or a double) as a double. */
static __inline double
vm_value_as_number(
	vm_value value)
{
	double number;
	int integer;

	/* An int32's number. */
	integer = vm_value_is_int32(value);
	if (integer) {
		number = (double)vm_value_as_int32(value);
		return number;
	}

	/* A double's. */
	number = vm_value_as_double(value);

	/* The decoded number belongs to the caller. */
	return number;
}

/* Reports a cell value's cell. */
static __inline struct vm_cell *
vm_value_as_cell(
	vm_value value)
{
	/* The pointer. */
	return (struct vm_cell *)(uintptr_t)value;
}

#endif
