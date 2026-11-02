/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of the heap, shared by the files of vm/ and by nobody else.
 */

#ifndef KEILAND_BROWSER_VM_INTERNAL_H
#define KEILAND_BROWSER_VM_INTERNAL_H

#include "vm/vm.h"

/* How many steps of long native functions go between two looks at the request to end the task (a power of two, less one). */
#define VM_STEPS_MASK		0xFFFU

/* The size and alignment of a block of small cells. */
#define VM_BLOCK_SIZE		(64U * 1024U)

/* The alignment of every cell (the low bits of a boxed pointer are free). */
#define VM_CELL_ALIGN		16U

/* The most cells a block can hold (all of the smallest size). */
#define VM_BLOCK_CELLS_MAX	(VM_BLOCK_SIZE / VM_CELL_ALIGN)

/* How many size classes of small cells there are. */
#define VM_SIZE_CLASSES		29U

/* The largest small cell; anything larger gets an allocation of its own. */
#define VM_SMALL_MAX		4096U

/*
 * One 64 KiB block of cells of one size.
 *
 * The cells start at first_offset.  A cell is in use when its allocated
 * bit is set; the marked bits are only meaningful during a collection.
 * Free cells are chained through their first word in the heap's free list
 * for the block's size class.
 */
struct vm_block {
	struct vm_block *next;
	uint32_t cell_size;
	uint32_t cell_count;
	uint32_t first_offset;
	uint32_t used;
	uint32_t size_class;
	uint32_t reserved;
	uint8_t allocated[VM_BLOCK_CELLS_MAX / 8U];
	uint8_t marked[VM_BLOCK_CELLS_MAX / 8U];
};

/*
 * A cell too large for a block, with its own allocation.
 *
 * The header comes first and the cell follows at an aligned offset; the
 * large cells are kept sorted by address so a stack word can be looked up.
 */
struct vm_large {
	size_t size;
	int marked;
	int reserved;
};

/*
 * A tracer and the context it is called with.
 */
struct vm_tracer_entry {
	vm_tracer tracer;
	void *context;
};

/*
 * The atoms of a heap: an open-addressing table of interned strings.
 *
 * slots is a power of two long; an empty slot is NULL.  Atoms are never
 * removed, so no tombstones are needed.
 */
struct vm_atom_table {
	struct vm_string **slots;
	size_t capacity;
	size_t count;
};

/*
 * A garbage-collected heap.
 */
struct vm_heap {
	/* The blocks of small cells, and each size class's free cells. */
	struct vm_block *blocks;
	void *free_lists[VM_SIZE_CLASSES];

	/* The set of block addresses (open addressing, NULL is empty) for looking up stack words. */
	struct vm_block **block_set;
	size_t block_set_capacity;
	size_t block_count;

	/* The large cells' headers, sorted by address. */
	struct vm_large **large;
	size_t large_count;
	size_t large_capacity;

	/* The lowest and highest address of any cell, for rejecting stack words quickly. */
	uintptr_t lowest;
	uintptr_t highest;

	/* The byte counts that decide when to collect, and the limit on live bytes. */
	size_t live_bytes;
	size_t live_cells;
	size_t allocated_since;
	size_t threshold;
	size_t limit;

	/* The bottom of the C stack the collector scans up to. */
	const void *stack_base;

	/* The roots: slots holding a cell, and tracers of subsystems. */
	struct wb_vector roots;
	struct wb_vector tracers;

	/* The cells marked and not yet traced. */
	struct wb_vector mark_stack;

	/* Whether a collection is running (allocation is refused inside one). */
	int collecting;

	/* The counts for vm_heap_stats. */
	size_t collections;
	size_t freed_cells;

	/* The heap's atoms. */
	struct vm_atom_table atoms;

	/*
	 * The root of the shape tree (no properties), made by the first
	 * object and a registered root from then on, which keeps every shape
	 * made from it.
	 */
	struct vm_shape *root_shape;

	/* The heap's tasks: its own agent, and the agent its tasks run in (own_agent unless its user shares one). */
	struct vm_agent own_agent;
	struct vm_agent *agent;

	/* The steps of long native functions counted (vm_heap_count_step), the heap's thread's alone. */
	uint32_t steps;
};

/* The states of a generator (or of an async function's run). */
#define VM_GENERATOR_START		0U
#define VM_GENERATOR_SUSPENDED		1U
#define VM_GENERATOR_RUNNING		2U
#define VM_GENERATOR_DONE		3U

/* A managed Proxy keeps its target and handler alive until revocation clears both edges. */
struct vm_proxy {
	struct vm_object object;
	vm_value target;
	vm_value handler;
	/* Creation captures internal call/construct slots even after revocation clears object edges. */
	int callable;
	int constructor;
};

/*
 * The run of a generator or an async function between its steps: the
 * function, the this value, new.target and the argument count of its
 * frame, and the frame's registers while it is suspended.
 *
 * A generator object (VM_KIND_GENERATOR) holds one as its internal value;
 * an async function's is held by the reaction that resumes it and by its
 * frame while it runs.  registers is malloc'd with register_count values
 * (the code's) when the run first suspends and freed when the run ends;
 * pc is where the run goes on, and value_register and how_register are
 * the suspend instruction's registers that take what the resumption
 * brings.  promise is an async function's promise (undefined for a
 * generator).
 */
struct vm_generator {
	struct vm_cell cell;
	struct vm_function *function;
	vm_value this_value;
	vm_value new_target;
	vm_value argument_count;
	vm_value promise;
	vm_value *registers;
	uint32_t register_count;
	uint32_t state;
	uint32_t pc;
	uint32_t value_register;
	uint32_t how_register;
	uint32_t direct_result; /* The suspension already supplied an iterator result, so the caller must preserve its identity. */
};

/*
 * A promise: an object with its state (VM_PROMISE_*), its value or reason
 * once settled, and the reactions waiting for it while it is pending, in
 * the order they came.  handled says a reaction was ever added (an
 * unhandled rejection is reported); resolved says the promise's own
 * resolution (vm_promise_resolve) has been decided, which a second one
 * does not change.
 */
struct vm_promise {
	struct vm_object object;
	uint32_t state;
	uint32_t handled;
	uint32_t resolved;
	uint32_t reserved;
	vm_value result;
	struct vm_promise_job *first;
	struct vm_promise_job *last;
};

/* The kinds of promise job. */
#define VM_JOB_REACTION			0U
#define VM_JOB_THENABLE			1U
#define VM_JOB_AWAIT			2U

/*
 * A job a promise queues, in a cell so the realm's queue holds it like a
 * callback.
 *
 * A reaction (VM_JOB_REACTION) is what then added: its two handlers and
 * the derived promise it settles (derived; resolve and reject are that
 * promise's resolving functions when a constructor other than Promise
 * made it, the empty value when the promise is settled directly, and
 * derived is undefined when nothing is settled).  An await
 * (VM_JOB_AWAIT) resumes generator with the settled value.  Both wait in
 * their promise's list (next) and are queued once it settles, with
 * rejected saying which way.  A thenable job (VM_JOB_THENABLE) resolves
 * derived by calling then (on_fulfilled) on the thenable (its argument).
 */
struct vm_promise_job {
	struct vm_cell cell;
	struct vm_promise_job *next;
	uint32_t kind;
	uint32_t rejected;
	vm_value on_fulfilled;
	vm_value on_rejected;
	vm_value derived;
	vm_value resolve;
	vm_value reject;
	struct vm_generator *generator;
};

/*
 * Reports VM_TERMINATED when another thread asked the task that runs to
 * end, 0 otherwise.  The engine calls it where a script can run for long
 * (a loop's head, a function's entry), so the common case is one relaxed
 * load of a word nobody writes.
 */
static __inline int
vm_termination_poll(
	struct vm_heap *heap)
{
	struct vm_agent *agent;
	uint32_t asked;
	uint32_t task;
	int passed;

	/* Nobody asked: the task goes on. */
	agent = heap->agent;
	asked = __atomic_load_n(&agent->terminate, __ATOMIC_RELAXED);
	if (asked == 0U)
		return 0;

	/* A task with a deadline ends at it. */
	if (agent->deadline_ns != 0U) {
		passed = vm_agent_deadline_passed(agent);
		if (passed)
			return VM_TERMINATED;
	}

	/* A request for another task (one that ended already) does not end this one. */
	task = __atomic_load_n(&agent->task, __ATOMIC_RELAXED);
	if (asked != task)
		return 0;

	/* The task that runs is asked to end. */
	return VM_TERMINATED;
}

/* The interpreter's runs of generators (interpreter.c). */
int vm_interpret_start(struct vm_realm *realm, struct vm_function *function, vm_value this_value, const vm_value *args, unsigned count, struct vm_generator *generator, vm_value *result, int *suspended);
int vm_interpret_resume(struct vm_realm *realm, struct vm_generator *generator, vm_value value, int how, vm_value *result, int *suspended);

/* Generators (generator.c). */
extern const struct vm_cell_type vm_generator_type;
int vm_generator_continue(struct vm_realm *realm, struct vm_generator *generator, vm_value value, int how);

/* Promises (promise.c). */
int vm_promise_await(struct vm_realm *realm, vm_value value, struct vm_generator *generator);
void vm_promise_trace_rejections(struct vm_heap *heap, struct vm_realm *realm);

/* The atom table (atom.c). */
void vm_atom_table_trace(struct vm_heap *heap);
void vm_atom_table_release(struct vm_heap *heap);

/* Strings (string.c). */
struct vm_string *vm_string_alloc(struct vm_heap *heap, size_t length, int wide);
unsigned char *vm_string_latin1_mutable(struct vm_string *string);
uint16_t *vm_string_units_mutable(struct vm_string *string);

#endif
