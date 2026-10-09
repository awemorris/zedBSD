/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws101-p002: the LOWERING of compute shaders (SPIR-V -> scalar IR), run on
 * a small interpreter.
 *
 * Each module of plan/ws101/tests/host/shaders/ (and the shared-memory
 * modules of the vkcs scenario, ws101-p006) is parsed with the i915
 * compiler's parser and its IR is run for every invocation of a dispatch,
 * against buffers the test fills.  The invocations of a group run one after
 * another up to a BARRIER, and only once every one of them has reached it
 * do they go on (a valid schedule); an invocation that ends while another
 * waits at a barrier is a failure.  Each group has its own shared memory.
 * The buffers are then compared with what C computes from the same inputs
 * on its own -- never with what the parser says it did.  A load or a store
 * past the end of a buffer by a channel the IR lets run is a failure: it is
 * what the predicate of a load or a store must prevent.
 *
 * Nothing in this file is evidence about GPU execution; the EU code is
 * checked by compute-dump.c and Mesa's disassembler.
 *
 *   compute-lower DIR      DIR holds add.spv, ids.spv, atomic.spv, length.spv, dynamic.spv and noct.spv,
 *                          and shared.spv, reduce.spv, scan.spv, oddbar.spv and atomsh.spv (ws101-p006)
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel's allocator, on the host. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

void
kern_free(
	void *pointer)
{
	free(pointer);
}

#include "../../../../src/drivers/gpu/compiler/spirv.c"

/* The most buffers one test binds, and the most loops one shader nests. */
#define LOWER_MAX_BUFFERS	8U
#define LOWER_MAX_LOOPS		16U

/* Where an invocation stands between the rounds of its group (ws101-p006). */
#define LOWER_RUNNING		0
#define LOWER_AT_BARRIER	1
#define LOWER_DONE		2

/* What shared memory holds before a group writes it. */
#define LOWER_SHARED_FILL	0xCDCDCDCDU

/* One invocation of a group: its values, its built-ins, where it is and its loops (ws101-p006). */
struct lower_thread {
	uint32_t *values;
	uint32_t system[DRV_GPU_IR_SYSTEM_COUNT];
	uint32_t index;
	uint32_t loops[LOWER_MAX_LOOPS];
	uint32_t loop_count;
	uint32_t steps;
	int state;
};

/*
 * One storage buffer of a test, bound at (set, binding): its words and its
 * size in bytes.  The system buffer (the group counts) is set
 * DRV_GPU_IR_SYSTEM_SET, binding 0.
 */
struct lower_buffer {
	uint32_t set;
	uint32_t binding;
	uint32_t *words;
	uint32_t bytes;
};

/* What one dispatch of a module runs with. */
struct lower_dispatch {
	const struct drv_gpu_shader_ir *ir;
	struct lower_buffer buffers[LOWER_MAX_BUFFERS];
	uint32_t buffer_count;
	uint8_t push[128];
	uint32_t groups[3];

	/* The buffer of each uniform of the IR, or NULL. */
	struct lower_buffer *bound[64];

	/* The shared memory of the group being run (ws101-p006). */
	uint32_t *shared;

	/* The first failure, for the report. */
	char failure[160];
};

static struct drv_gpu_shader_ir *lower_parse(const char *dir, const char *name);
static struct lower_buffer *lower_buffer_add(struct lower_dispatch *dispatch, uint32_t set, uint32_t binding, uint32_t words, uint32_t fill);
static int lower_bind(struct lower_dispatch *dispatch);
static int lower_run(struct lower_dispatch *dispatch);
static int lower_group(struct lower_dispatch *dispatch, struct lower_thread *threads, uint32_t count);
static int lower_invocation(struct lower_dispatch *dispatch, struct lower_thread *thread);
static uint32_t *lower_word(struct lower_dispatch *dispatch, uint32_t uniform, uint32_t byte);
static uint32_t *lower_shared_word(struct lower_dispatch *dispatch, uint32_t byte);
static int lower_atomic(struct lower_dispatch *dispatch, const struct drv_gpu_shader_ir_inst *inst, uint32_t *values);
static int lower_test_add(const char *dir);
static int lower_test_ids(const char *dir);
static int lower_test_atomic(const char *dir);
static int lower_test_length(const char *dir);
static int lower_test_dynamic(const char *dir);
static int lower_test_noct(const char *dir);
static int lower_test_shared(const char *dir);
static int lower_test_reduce(const char *dir);
static int lower_test_scan(const char *dir);
static int lower_test_oddbar(const char *dir);
static int lower_test_atomsh(const char *dir);
static int32_t lower_smod(int32_t left, int32_t right);

int
main(
	int argc,
	char **argv)
{
	int failures;

	/* The directory of the modules. */
	if (argc != 2) {
		fprintf(stderr, "usage: compute-lower DIR\n");
		return 2;
	}

	/* Runs every test, counting the failures. */
	failures = 0;
	failures += lower_test_add(argv[1]);
	failures += lower_test_ids(argv[1]);
	failures += lower_test_atomic(argv[1]);
	failures += lower_test_length(argv[1]);
	failures += lower_test_dynamic(argv[1]);
	failures += lower_test_noct(argv[1]);
	failures += lower_test_shared(argv[1]);
	failures += lower_test_reduce(argv[1]);
	failures += lower_test_scan(argv[1]);
	failures += lower_test_oddbar(argv[1]);
	failures += lower_test_atomsh(argv[1]);

	/* Reports the outcome. */
	if (failures != 0) {
		printf("lower: FAIL %d test(s)\n", failures);
		return 1;
	}
	printf("lower: every IR computes what C computes\n");
	return 0;
}

/* Parses DIR/NAME.spv as a compute shader; NULL on any failure. */
static struct drv_gpu_shader_ir *
lower_parse(
	const char *dir,
	const char *name)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *ir;
	char path[512];
	uint32_t *code;
	FILE *file;
	long size;
	int error;

	/* Reads the module. */
	snprintf(path, sizeof(path), "%s/%s.spv", dir, name);
	file = fopen(path, "rb");
	if (file == NULL) {
		printf("%s: FAIL cannot read %s\n", name, path);
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size + 4U);
	if (code == NULL || fread(code, 1U, (size_t)size, file) != (size_t)size) {
		fclose(file);
		printf("%s: FAIL cannot read %s\n", name, path);
		return NULL;
	}
	fclose(file);

	/* Parses it. */
	error = drv_gpu_shader_parse(code, (size_t)size / 4U, DRV_GPU_STAGE_COMPUTE, &ir, &diagnostic);
	free(code);
	if (error != 0) {
		printf("%s: FAIL refused by the parser: %d (%s)\n", name, error, diagnostic.reason != NULL ? diagnostic.reason : "?");
		return NULL;
	}

	/* Succeeded: the IR. */
	return ir;
}

/* Adds a buffer of `words` words, each `fill`, to a dispatch. */
static struct lower_buffer *
lower_buffer_add(
	struct lower_dispatch *dispatch,
	uint32_t set,
	uint32_t binding,
	uint32_t words,
	uint32_t fill)
{
	struct lower_buffer *buffer;
	uint32_t index;

	/* Takes the next buffer slot. */
	buffer = &dispatch->buffers[dispatch->buffer_count];
	dispatch->buffer_count++;

	/* Allocates and fills the words. */
	buffer->set = set;
	buffer->binding = binding;
	buffer->bytes = words * 4U;
	buffer->words = calloc(words + 1U, sizeof(uint32_t));
	for (index = 0U; index < words; index++)
		buffer->words[index] = fill;
	return buffer;
}

/* Binds each storage uniform of the IR to the buffer of its set and binding. Returns 0 when all are bound. */
static int
lower_bind(
	struct lower_dispatch *dispatch)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t index;
	uint32_t buffer;

	/* Every storage buffer the shader names needs a buffer of the test. */
	for (index = 0U; index < dispatch->ir->uniform_count; index++) {
		uniform = &dispatch->ir->uniforms[index];
		dispatch->bound[index] = NULL;
		if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE)
			continue;
		for (buffer = 0U; buffer < dispatch->buffer_count; buffer++) {
			if (dispatch->buffers[buffer].set == uniform->set && dispatch->buffers[buffer].binding == uniform->binding)
				dispatch->bound[index] = &dispatch->buffers[buffer];
		}
		if (dispatch->bound[index] == NULL) {
			snprintf(dispatch->failure, sizeof(dispatch->failure), "no buffer at set %u binding %u", uniform->set, uniform->binding);
			return 1;
		}
	}
	return 0;
}

/* Runs every group of the dispatch, one after another. Returns 0 when none fails. */
static int
lower_run(
	struct lower_dispatch *dispatch)
{
	struct lower_thread *threads;
	struct lower_buffer *counts;
	uint32_t *size;
	uint32_t gx;
	uint32_t gy;
	uint32_t gz;
	uint32_t linear;
	uint32_t invocations;
	uint32_t word;
	int error;

	/* The group counts the system buffer holds. */
	counts = lower_buffer_add(dispatch, DRV_GPU_IR_SYSTEM_SET, 0U, 3U, 0U);
	counts->words[0] = dispatch->groups[0];
	counts->words[1] = dispatch->groups[1];
	counts->words[2] = dispatch->groups[2];
	error = lower_bind(dispatch);
	if (error != 0)
		return error;

	/* One state per invocation of a group, and the group's shared memory. */
	size = ((struct drv_gpu_shader_ir *)dispatch->ir)->local_size;
	invocations = size[0] * size[1] * size[2];
	threads = calloc(invocations, sizeof(*threads));
	for (linear = 0U; linear < invocations; linear++)
		threads[linear].values = calloc(dispatch->ir->value_count + 1U, sizeof(uint32_t));
	dispatch->shared = calloc(dispatch->ir->shared_bytes / 4U + 1U, sizeof(uint32_t));

	/* Every group, its invocations in their linear order. */
	error = 0;
	for (gz = 0U; error == 0 && gz < dispatch->groups[2]; gz++) {
		for (gy = 0U; error == 0 && gy < dispatch->groups[1]; gy++) {
			for (gx = 0U; error == 0 && gx < dispatch->groups[0]; gx++) {
				for (word = 0U; word < dispatch->ir->shared_bytes / 4U; word++)
					dispatch->shared[word] = LOWER_SHARED_FILL;
				for (linear = 0U; linear < invocations; linear++) {
					threads[linear].system[DRV_GPU_IR_SYSTEM_LOCAL_ID_X] = linear % size[0];
					threads[linear].system[DRV_GPU_IR_SYSTEM_LOCAL_ID_Y] = (linear / size[0]) % size[1];
					threads[linear].system[DRV_GPU_IR_SYSTEM_LOCAL_ID_Z] = linear / (size[0] * size[1]);
					threads[linear].system[DRV_GPU_IR_SYSTEM_LOCAL_INDEX] = linear;
					threads[linear].system[DRV_GPU_IR_SYSTEM_GROUP_ID_X] = gx;
					threads[linear].system[DRV_GPU_IR_SYSTEM_GROUP_ID_Y] = gy;
					threads[linear].system[DRV_GPU_IR_SYSTEM_GROUP_ID_Z] = gz;
				}
				error = lower_group(dispatch, threads, invocations);
			}
		}
	}

	/* Gives the states back. */
	for (linear = 0U; linear < invocations; linear++)
		free(threads[linear].values);
	free(threads);
	free(dispatch->shared);
	dispatch->shared = NULL;
	return error;
}

/*
 * Runs one group: every invocation up to its next barrier or its end, in
 * rounds, until all have ended.  Returns 0, or 1 with the failure recorded
 * -- also when some invocations end while others wait at a barrier.
 */
static int
lower_group(
	struct lower_dispatch *dispatch,
	struct lower_thread *threads,
	uint32_t count)
{
	uint32_t linear;
	uint32_t waiting;
	uint32_t done;

	/* Every invocation starts at the top. */
	for (linear = 0U; linear < count; linear++) {
		threads[linear].index = 0U;
		threads[linear].loop_count = 0U;
		threads[linear].steps = 0U;
		threads[linear].state = LOWER_RUNNING;
	}

	/* Rounds until every invocation has ended. */
	for (;;) {
		waiting = 0U;
		done = 0U;
		for (linear = 0U; linear < count; linear++) {
			if (threads[linear].state == LOWER_RUNNING && lower_invocation(dispatch, &threads[linear]) != 0)
				return 1;
			if (threads[linear].state == LOWER_AT_BARRIER)
				waiting++;
			if (threads[linear].state == LOWER_DONE)
				done++;
		}
		if (done == count)
			return 0;
		if (waiting != count) {
			snprintf(dispatch->failure, sizeof(dispatch->failure), "%u invocations wait at a barrier, %u ended", waiting, done);
			return 1;
		}
		for (linear = 0U; linear < count; linear++)
			threads[linear].state = LOWER_RUNNING;
	}
}

/* Returns the word at byte `byte` of the buffer of uniform `uniform`, or NULL past its end. */
static uint32_t *
lower_word(
	struct lower_dispatch *dispatch,
	uint32_t uniform,
	uint32_t byte)
{
	struct lower_buffer *buffer;

	if (uniform >= dispatch->ir->uniform_count || dispatch->bound[uniform] == NULL)
		return NULL;
	buffer = dispatch->bound[uniform];
	if ((byte & 3U) != 0U || byte >= buffer->bytes)
		return NULL;
	return &buffer->words[byte / 4U];
}

/* Returns the word at byte `byte` of the group's shared memory, or NULL past its end (ws101-p006). */
static uint32_t *
lower_shared_word(
	struct lower_dispatch *dispatch,
	uint32_t byte)
{
	if ((byte & 3U) != 0U || byte >= dispatch->ir->shared_bytes)
		return NULL;
	return &dispatch->shared[byte / 4U];
}

/* Runs one ATOMIC for one invocation. Returns 0, or 1 for a word past the end. */
static int
lower_atomic(
	struct lower_dispatch *dispatch,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t *values)
{
	uint32_t operands;
	uint32_t *word;
	uint32_t old;
	uint32_t value;

	/* A predicated atomic whose channel is outside the block does nothing. */
	operands = 2U;
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
		operands = 3U;
	if (inst->component != 0U && values[inst->src[operands]] == 0U)
		return 0;

	/* The word must be inside the buffer or the shared memory. */
	if (inst->location == DRV_GPU_IR_LOCATION_SHARED) {
		word = lower_shared_word(dispatch, values[inst->src[0]]);
	} else {
		word = lower_word(dispatch, inst->location, values[inst->src[0]]);
	}
	if (word == NULL) {
		snprintf(dispatch->failure, sizeof(dispatch->failure), "atomic past the end at byte %u", values[inst->src[0]]);
		return 1;
	}

	/* The operation, in one step. */
	old = *word;
	value = values[inst->src[1]];
	switch (inst->immediate) {
	case DRV_GPU_IR_ATOMIC_ADD:
		*word = old + value;
		break;
	case DRV_GPU_IR_ATOMIC_SUB:
		*word = old - value;
		break;
	case DRV_GPU_IR_ATOMIC_AND:
		*word = old & value;
		break;
	case DRV_GPU_IR_ATOMIC_OR:
		*word = old | value;
		break;
	case DRV_GPU_IR_ATOMIC_XOR:
		*word = old ^ value;
		break;
	case DRV_GPU_IR_ATOMIC_XCHG:
		*word = value;
		break;
	case DRV_GPU_IR_ATOMIC_SMIN:
		if ((int32_t)value < (int32_t)old)
			*word = value;
		break;
	case DRV_GPU_IR_ATOMIC_SMAX:
		if ((int32_t)value > (int32_t)old)
			*word = value;
		break;
	case DRV_GPU_IR_ATOMIC_UMIN:
		if (value < old)
			*word = value;
		break;
	case DRV_GPU_IR_ATOMIC_UMAX:
		if (value > old)
			*word = value;
		break;
	case DRV_GPU_IR_ATOMIC_CMPXCHG:
		if (old == values[inst->src[2]])
			*word = value;
		break;
	default:
		snprintf(dispatch->failure, sizeof(dispatch->failure), "atomic operation %u", inst->immediate);
		return 1;
	}

	/* The old value is the result. */
	values[inst->dst] = old;
	return 0;
}

/*
 * Runs one invocation from where it stands to its next barrier (past which
 * it then stands, LOWER_AT_BARRIER) or its end (LOWER_DONE).  Returns 0, or
 * 1 with the failure recorded.
 */
static int
lower_invocation(
	struct lower_dispatch *dispatch,
	struct lower_thread *thread)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	const uint32_t *system;
	uint32_t *values;
	uint32_t *loops;
	uint32_t loop_count;
	uint32_t index;
	uint32_t *word;
	uint32_t a;
	uint32_t b;
	uint32_t steps;

	ir = dispatch->ir;
	values = thread->values;
	system = thread->system;
	loops = thread->loops;
	loop_count = thread->loop_count;
	steps = thread->steps;
	for (index = thread->index; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		a = values[inst->src[0] < ir->value_count ? inst->src[0] : 0U];
		b = values[inst->src[1] < ir->value_count ? inst->src[1] : 0U];
		if (++steps > 10000000U) {
			snprintf(dispatch->failure, sizeof(dispatch->failure), "runaway loop");
			return 1;
		}
		switch (inst->op) {
		case DRV_GPU_IR_NOP:
		case DRV_GPU_IR_SKIP_BEGIN:
		case DRV_GPU_IR_SKIP_END:
			break;
		case DRV_GPU_IR_CONST:
		case DRV_GPU_IR_ICONST:
		case DRV_GPU_IR_BOOL:
			values[inst->dst] = inst->immediate;
			break;
		case DRV_GPU_IR_LOAD_PUSH:
			memcpy(&values[inst->dst], dispatch->push + inst->immediate, 4U);
			break;
		case DRV_GPU_IR_LOAD_SYSTEM:
			values[inst->dst] = system[inst->component];
			break;
		case DRV_GPU_IR_LOAD_STORAGE:
			if (inst->component != 0U && b == 0U)
				break;
			word = lower_word(dispatch, inst->location, a);
			if (word == NULL) {
				snprintf(dispatch->failure, sizeof(dispatch->failure), "load past the end at byte %u", a);
				return 1;
			}
			values[inst->dst] = *word;
			break;
		case DRV_GPU_IR_STORE_STORAGE:
			if (inst->component != 0U && values[inst->src[2]] == 0U)
				break;
			word = lower_word(dispatch, inst->location, a);
			if (word == NULL) {
				snprintf(dispatch->failure, sizeof(dispatch->failure), "store past the end at byte %u", a);
				return 1;
			}
			*word = b;
			break;
		case DRV_GPU_IR_STORAGE_SIZE:
			values[inst->dst] = dispatch->bound[inst->location]->bytes;
			break;
		case DRV_GPU_IR_LOAD_SHARED:
			if (inst->component != 0U && b == 0U)
				break;
			word = lower_shared_word(dispatch, a);
			if (word == NULL) {
				snprintf(dispatch->failure, sizeof(dispatch->failure), "shared load past the end at byte %u", a);
				return 1;
			}
			values[inst->dst] = *word;
			break;
		case DRV_GPU_IR_STORE_SHARED:
			if (inst->component != 0U && values[inst->src[2]] == 0U)
				break;
			word = lower_shared_word(dispatch, a);
			if (word == NULL) {
				snprintf(dispatch->failure, sizeof(dispatch->failure), "shared store past the end at byte %u", a);
				return 1;
			}
			*word = b;
			break;
		case DRV_GPU_IR_FENCE:
			break;
		case DRV_GPU_IR_BARRIER:
			thread->index = index + 1U;
			thread->loop_count = loop_count;
			thread->steps = steps;
			thread->state = LOWER_AT_BARRIER;
			return 0;
		case DRV_GPU_IR_ATOMIC:
			if (lower_atomic(dispatch, inst, values) != 0)
				return 1;
			break;
		case DRV_GPU_IR_IADD:
			values[inst->dst] = a + b;
			break;
		case DRV_GPU_IR_ISUB:
			values[inst->dst] = a - b;
			break;
		case DRV_GPU_IR_IMUL:
			values[inst->dst] = a * b;
			break;
		case DRV_GPU_IR_INEG:
			values[inst->dst] = 0U - a;
			break;
		case DRV_GPU_IR_UDIV:
			values[inst->dst] = b != 0U ? a / b : 0xFFFFFFFFU;
			break;
		case DRV_GPU_IR_UMOD:
			values[inst->dst] = b != 0U ? a % b : 0U;
			break;
		case DRV_GPU_IR_IDIV:
			values[inst->dst] = (b != 0U && !(a == 0x80000000U && b == 0xFFFFFFFFU)) ? (uint32_t)((int32_t)a / (int32_t)b) : 0U;
			break;
		case DRV_GPU_IR_IREM:
			values[inst->dst] = (b != 0U && !(a == 0x80000000U && b == 0xFFFFFFFFU)) ? (uint32_t)((int32_t)a % (int32_t)b) : 0U;
			break;
		case DRV_GPU_IR_IAND:
		case DRV_GPU_IR_AND:
			values[inst->dst] = a & b;
			break;
		case DRV_GPU_IR_IOR:
		case DRV_GPU_IR_OR:
			values[inst->dst] = a | b;
			break;
		case DRV_GPU_IR_IXOR:
			values[inst->dst] = a ^ b;
			break;
		case DRV_GPU_IR_INOT:
		case DRV_GPU_IR_NOT:
			values[inst->dst] = ~a;
			break;
		case DRV_GPU_IR_SHL:
			values[inst->dst] = a << (b & 31U);
			break;
		case DRV_GPU_IR_SHR:
			values[inst->dst] = a >> (b & 31U);
			break;
		case DRV_GPU_IR_ASR:
			values[inst->dst] = (uint32_t)((int32_t)a >> (b & 31U));
			break;
		case DRV_GPU_IR_ILT:
			values[inst->dst] = (int32_t)a < (int32_t)b ? 0xFFFFFFFFU : 0U;
			break;
		case DRV_GPU_IR_IGE:
			values[inst->dst] = (int32_t)a >= (int32_t)b ? 0xFFFFFFFFU : 0U;
			break;
		case DRV_GPU_IR_ULT:
			values[inst->dst] = a < b ? 0xFFFFFFFFU : 0U;
			break;
		case DRV_GPU_IR_UGE:
			values[inst->dst] = a >= b ? 0xFFFFFFFFU : 0U;
			break;
		case DRV_GPU_IR_IEQ:
			values[inst->dst] = a == b ? 0xFFFFFFFFU : 0U;
			break;
		case DRV_GPU_IR_INE:
			values[inst->dst] = a != b ? 0xFFFFFFFFU : 0U;
			break;
		case DRV_GPU_IR_SELECT:
			values[inst->dst] = a != 0U ? b : values[inst->src[2]];
			break;
		case DRV_GPU_IR_MOVE:
			values[inst->dst] = a;
			break;
		case DRV_GPU_IR_LOOP_BEGIN:
			if (loop_count >= LOWER_MAX_LOOPS) {
				snprintf(dispatch->failure, sizeof(dispatch->failure), "loops nested too deep");
				return 1;
			}
			loops[loop_count] = index;
			loop_count++;
			break;
		case DRV_GPU_IR_LOOP_END:
			if (loop_count == 0U) {
				snprintf(dispatch->failure, sizeof(dispatch->failure), "LOOP_END without LOOP_BEGIN");
				return 1;
			}
			if (a != 0U) {
				index = loops[loop_count - 1U];
			} else {
				loop_count--;
			}
			break;
		default:
			snprintf(dispatch->failure, sizeof(dispatch->failure), "IR operation %u not interpreted", (unsigned)inst->op);
			return 1;
		}
	}
	thread->state = LOWER_DONE;
	return 0;
}

/* SPIR-V's OpSMod: the remainder with the divisor's sign. */
static int32_t
lower_smod(
	int32_t left,
	int32_t right)
{
	int32_t remainder;

	remainder = left % right;
	if (remainder != 0 && ((remainder < 0) != (right < 0)))
		remainder += right;
	return remainder;
}

/* add.comp: c[i] = a[i] + b[i] for i < n, the rest untouched; a and b are only n words long. */
static int
lower_test_add(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *a;
	struct lower_buffer *b;
	struct lower_buffer *c;
	uint32_t n;
	uint32_t i;
	uint32_t expected;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "add");
	if (dispatch.ir == NULL)
		return 1;
	n = 1000U;
	a = lower_buffer_add(&dispatch, 0U, 0U, n, 0U);
	b = lower_buffer_add(&dispatch, 0U, 1U, n, 0U);
	c = lower_buffer_add(&dispatch, 0U, 2U, 1024U, 0xDEADBEEFU);
	for (i = 0U; i < n; i++) {
		a->words[i] = i * 3U + 1U;
		b->words[i] = i * 7U;
	}
	memcpy(dispatch.push, &n, 4U);
	dispatch.groups[0] = 16U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("add: FAIL %s\n", dispatch.failure);
		return 1;
	}
	for (i = 0U; i < 1024U; i++) {
		expected = 0xDEADBEEFU;
		if (i < n)
			expected = i * 3U + 1U + i * 7U;
		if (c->words[i] != expected) {
			printf("add: FAIL c[%u] = 0x%08x, expected 0x%08x\n", i, c->words[i], expected);
			return 1;
		}
	}
	printf("add: 1024 invocations, c[i] = a[i] + b[i] for the 1000 in range, the 24 others untouched and read nothing\n");
	return 0;
}

/* ids.comp: the five built-ins of every invocation of a (3, 2, 2) dispatch of (4, 2, 3) groups. */
static int
lower_test_ids(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *o;
	uint32_t expected[13];
	uint32_t gx, gy, gz, lx, ly, lz;
	uint32_t x, y, z;
	uint32_t linear;
	uint32_t k;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "ids");
	if (dispatch.ir == NULL)
		return 1;
	o = lower_buffer_add(&dispatch, 0U, 0U, 288U * 13U, 0xDEADBEEFU);
	dispatch.groups[0] = 3U;
	dispatch.groups[1] = 2U;
	dispatch.groups[2] = 2U;
	if (lower_run(&dispatch) != 0) {
		printf("ids: FAIL %s\n", dispatch.failure);
		return 1;
	}
	for (gz = 0U; gz < 2U; gz++)
	for (gy = 0U; gy < 2U; gy++)
	for (gx = 0U; gx < 3U; gx++)
	for (lz = 0U; lz < 3U; lz++)
	for (ly = 0U; ly < 2U; ly++)
	for (lx = 0U; lx < 4U; lx++) {
		x = gx * 4U + lx;
		y = gy * 2U + ly;
		z = gz * 3U + lz;
		linear = x + 12U * (y + 4U * z);
		expected[0] = lx;
		expected[1] = ly;
		expected[2] = lz;
		expected[3] = lx + 4U * ly + 8U * lz;
		expected[4] = gx;
		expected[5] = gy;
		expected[6] = gz;
		expected[7] = 3U;
		expected[8] = 2U;
		expected[9] = 2U;
		expected[10] = x;
		expected[11] = y;
		expected[12] = z;
		for (k = 0U; k < 13U; k++) {
			if (o->words[linear * 13U + k] != expected[k]) {
				printf("ids: FAIL invocation (%u %u %u) word %u = %u, expected %u\n", x, y, z, k, o->words[linear * 13U + k], expected[k]);
				return 1;
			}
		}
	}
	printf("ids: 288 invocations, local id, local index, group id, group counts and global id all as C computes\n");
	return 0;
}

/* atomic.comp: every atomic of 200 of 256 invocations, and each old value of count handed out once. */
static int
lower_test_atomic(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *v;
	struct lower_buffer *h;
	struct lower_buffer *old;
	uint32_t bins[16];
	uint32_t seen[200];
	uint32_t total, umin, umax, band, bor, bxor, x, n, i;
	int32_t smin, smax;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "atomic");
	if (dispatch.ir == NULL)
		return 1;
	n = 200U;
	v = lower_buffer_add(&dispatch, 0U, 0U, n, 0U);
	h = lower_buffer_add(&dispatch, 0U, 1U, 27U, 0U);
	old = lower_buffer_add(&dispatch, 0U, 2U, 256U, 0xDEADBEEFU);
	for (i = 0U; i < n; i++)
		v->words[i] = (i * 37U + 11U) % 1000U;
	h->words[17] = 0x7FFFFFFFU;
	h->words[18] = 0x80000000U;
	h->words[19] = 0xFFFFFFFFU;
	h->words[21] = 0xFFFFFFFFU;
	memcpy(dispatch.push, &n, 4U);
	dispatch.groups[0] = 4U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("atomic: FAIL %s\n", dispatch.failure);
		return 1;
	}

	/* What C computes. */
	memset(bins, 0, sizeof(bins));
	total = 0U;
	smin = INT32_MAX;
	smax = INT32_MIN;
	umin = 0xFFFFFFFFU;
	umax = 0U;
	band = 0xFFFFFFFFU;
	bor = 0U;
	bxor = 0U;
	for (i = 0U; i < n; i++) {
		x = v->words[i];
		bins[x % 16U]++;
		total += x;
		if ((int32_t)x - 1000 < smin)
			smin = (int32_t)x - 1000;
		if ((int32_t)x - 1000 > smax)
			smax = (int32_t)x - 1000;
		if (x < umin)
			umin = x;
		if (x > umax)
			umax = x;
		band &= x | 0xFFFF0000U;
		bor |= x;
		bxor ^= x;
	}
	for (i = 0U; i < 16U; i++) {
		if (h->words[i] != bins[i]) {
			printf("atomic: FAIL bin %u = %u, expected %u\n", i, h->words[i], bins[i]);
			return 1;
		}
	}
	if (h->words[16] != total || (int32_t)h->words[17] != smin || (int32_t)h->words[18] != smax ||
	    h->words[19] != umin || h->words[20] != umax || h->words[21] != band || h->words[22] != bor ||
	    h->words[23] != bxor || h->words[24] != 7U || h->words[25] != 1U || h->words[26] != n) {
		printf("atomic: FAIL the scalars: total %u/%u smin %d/%d smax %d/%d umin %u/%u umax %u/%u and %08x/%08x or %08x/%08x xor %08x/%08x swap %u cas %u count %u\n",
		       h->words[16], total, (int32_t)h->words[17], smin, (int32_t)h->words[18], smax, h->words[19], umin,
		       h->words[20], umax, h->words[21], band, h->words[22], bor, h->words[23], bxor,
		       h->words[24], h->words[25], h->words[26]);
		return 1;
	}

	/* Each old value of count is handed to exactly one invocation in range; the others store nothing. */
	memset(seen, 0, sizeof(seen));
	for (i = 0U; i < 256U; i++) {
		if (i >= n) {
			if (old->words[i] != 0xDEADBEEFU) {
				printf("atomic: FAIL old[%u] written by an invocation out of range\n", i);
				return 1;
			}
			continue;
		}
		if (old->words[i] >= n || seen[old->words[i]] != 0U) {
			printf("atomic: FAIL old[%u] = %u is not a fresh count\n", i, old->words[i]);
			return 1;
		}
		seen[old->words[i]] = 1U;
	}
	printf("atomic: the histogram, add, smin, smax, umin, umax, and, or, xor, exchange, compare-exchange and the returned old values as C computes; 56 invocations out of range change nothing\n");
	return 0;
}

/* length.comp: .length() of a run-time array of words after a header, and of 12-byte structures. */
static int
lower_test_length(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *o;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "length");
	if (dispatch.ir == NULL)
		return 1;
	(void)lower_buffer_add(&dispatch, 0U, 0U, 11U, 0U);
	(void)lower_buffer_add(&dispatch, 0U, 1U, 17U, 0U);
	o = lower_buffer_add(&dispatch, 0U, 2U, 2U, 0U);
	dispatch.groups[0] = 1U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("length: FAIL %s\n", dispatch.failure);
		return 1;
	}
	if (o->words[0] != 10U || o->words[1] != 5U) {
		printf("length: FAIL %u and %u, expected 10 and 5\n", o->words[0], o->words[1]);
		return 1;
	}
	printf("length: 44 bytes after a 4-byte header hold 10 words; 68 bytes hold 5 12-byte structures\n");
	return 0;
}

/* dynamic.comp: t[x][y] = m[y][x] * 3 + 1, two dynamic indices on each side. */
static int
lower_test_dynamic(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *m;
	struct lower_buffer *t;
	uint32_t x;
	uint32_t y;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "dynamic");
	if (dispatch.ir == NULL)
		return 1;
	m = lower_buffer_add(&dispatch, 0U, 0U, 32U, 0U);
	t = lower_buffer_add(&dispatch, 0U, 1U, 32U, 0U);
	for (y = 0U; y < 4U; y++) {
		for (x = 0U; x < 8U; x++)
			m->words[y * 8U + x] = y * 8U + x + 100U;
	}
	dispatch.groups[0] = 1U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("dynamic: FAIL %s\n", dispatch.failure);
		return 1;
	}
	for (y = 0U; y < 4U; y++) {
		for (x = 0U; x < 8U; x++) {
			if (t->words[x * 4U + y] != (y * 8U + x + 100U) * 3U + 1U) {
				printf("dynamic: FAIL t[%u][%u] = %u\n", x, y, t->words[x * 4U + y]);
				return 1;
			}
		}
	}
	printf("dynamic: a 4x8 matrix transposed through two dynamic indices on each side\n");
	return 0;
}

/* noct.comp: the shape of Noct's OpenGL ES kernels (lane guard, raw words, signed division, a result word). */
static int
lower_test_noct(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *in;
	struct lower_buffer *out;
	struct lower_buffer *scalar;
	struct lower_buffer *result;
	uint32_t trip, i, v4, v6, v9, v10, v11, v13, sum;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "noct");
	if (dispatch.ir == NULL)
		return 1;
	trip = 300U;
	in = lower_buffer_add(&dispatch, 0U, 0U, trip, 0U);
	out = lower_buffer_add(&dispatch, 0U, 1U, 320U, 0xDEADBEEFU);
	scalar = lower_buffer_add(&dispatch, 0U, 2U, 3U, 0U);
	result = lower_buffer_add(&dispatch, 0U, 3U, 1U, 0U);
	for (i = 0U; i < trip; i++)
		in->words[i] = i * 2654435761U;
	scalar->words[0] = 7U;
	scalar->words[1] = 0U;
	scalar->words[2] = trip;
	dispatch.groups[0] = 5U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("noct: FAIL %s\n", dispatch.failure);
		return 1;
	}
	sum = 0U;
	for (i = 0U; i < 320U; i++) {
		if (i >= trip) {
			if (out->words[i] != 0xDEADBEEFU) {
				printf("noct: FAIL lane %u past the trip wrote\n", i);
				return 1;
			}
			continue;
		}
		v4 = in->words[i] * 0x0019660DU;
		v6 = v4 + 0x3C6EF35FU;
		v9 = v6 ^ (v6 >> 13);
		v10 = (uint32_t)((int32_t)v9 / 7);
		v11 = (uint32_t)lower_smod((int32_t)v9, 7);
		v13 = (int32_t)v10 < (int32_t)v11 ? v10 : v11;
		sum += v11;
		if (out->words[i] != v13) {
			printf("noct: FAIL lane %u = %u, expected %u\n", i, out->words[i], v13);
			return 1;
		}
	}
	if (result->words[0] != sum) {
		printf("noct: FAIL result %u, expected %u\n", result->words[0], sum);
		return 1;
	}
	printf("noct: 300 lanes of the Noct kernel's shape and the result word's sum as C computes; 20 lanes past the trip change nothing\n");
	return 0;
}

/* shared.comp (ws101-p006): a 64 x 64 matrix transposed through 8 x 8 tiles of shared memory, a barrier between. */
static int
lower_test_shared(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *m;
	struct lower_buffer *t;
	uint32_t r;
	uint32_t c;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "shared");
	if (dispatch.ir == NULL)
		return 1;
	m = lower_buffer_add(&dispatch, 0U, 0U, 4096U, 0U);
	t = lower_buffer_add(&dispatch, 0U, 1U, 4096U, 0xDEADBEEFU);
	for (r = 0U; r < 4096U; r++)
		m->words[r] = r * 2654435761U;
	dispatch.groups[0] = 8U;
	dispatch.groups[1] = 8U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("shared: FAIL %s\n", dispatch.failure);
		return 1;
	}
	for (r = 0U; r < 64U; r++) {
		for (c = 0U; c < 64U; c++) {
			if (t->words[r * 64U + c] != m->words[c * 64U + r]) {
				printf("shared: FAIL t[%u][%u] = 0x%08x, expected 0x%08x\n", r, c, t->words[r * 64U + c], m->words[c * 64U + r]);
				return 1;
			}
		}
	}
	printf("shared: a 64 x 64 matrix transposed through 64 groups' 8 x 8 tiles of shared memory (two dynamic indices, a barrier)\n");
	return 0;
}

/* reduce.comp (ws101-p006): each group's 128 words summed by a tree in shared memory, a barrier after each halving. */
static int
lower_test_reduce(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *v;
	struct lower_buffer *sums;
	uint32_t group;
	uint32_t i;
	uint32_t sum;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "reduce");
	if (dispatch.ir == NULL)
		return 1;
	v = lower_buffer_add(&dispatch, 0U, 0U, 512U, 0U);
	sums = lower_buffer_add(&dispatch, 0U, 1U, 5U, 0xDEADBEEFU);
	for (i = 0U; i < 512U; i++)
		v->words[i] = i * 40503U + 7U;
	dispatch.groups[0] = 4U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("reduce: FAIL %s\n", dispatch.failure);
		return 1;
	}
	for (group = 0U; group < 4U; group++) {
		sum = 0U;
		for (i = 0U; i < 128U; i++)
			sum += v->words[group * 128U + i];
		if (sums->words[group] != sum) {
			printf("reduce: FAIL group %u sum %u, expected %u\n", group, sums->words[group], sum);
			return 1;
		}
	}
	if (sums->words[4] != 0xDEADBEEFU) {
		printf("reduce: FAIL a word past the groups written\n");
		return 1;
	}
	printf("reduce: 4 groups of 128 summed by a tree in shared memory, 8 barriers, 7 of them in a loop\n");
	return 0;
}

/* scan.comp (ws101-p006): each group's 64 words' inclusive prefix sums, a loop of six steps with two barriers each. */
static int
lower_test_scan(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *v;
	struct lower_buffer *prefix;
	uint32_t i;
	uint32_t sum;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "scan");
	if (dispatch.ir == NULL)
		return 1;
	v = lower_buffer_add(&dispatch, 0U, 0U, 192U, 0U);
	prefix = lower_buffer_add(&dispatch, 0U, 1U, 192U, 0xDEADBEEFU);
	for (i = 0U; i < 192U; i++)
		v->words[i] = (i * 2246822519U) >> 20;
	dispatch.groups[0] = 3U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("scan: FAIL %s\n", dispatch.failure);
		return 1;
	}
	sum = 0U;
	for (i = 0U; i < 192U; i++) {
		if (i % 64U == 0U)
			sum = 0U;
		sum += v->words[i];
		if (prefix->words[i] != sum) {
			printf("scan: FAIL prefix[%u] = %u, expected %u\n", i, prefix->words[i], sum);
			return 1;
		}
	}
	printf("scan: 3 groups' prefix sums of 64 words, a loop of six steps with two barriers in each\n");
	return 0;
}

/* oddbar.comp (ws101-p006): a 5 x 3 group's words mirrored through shared memory across a barrier. */
static int
lower_test_oddbar(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *v;
	struct lower_buffer *o;
	uint32_t group;
	uint32_t l;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "oddbar");
	if (dispatch.ir == NULL)
		return 1;
	v = lower_buffer_add(&dispatch, 0U, 0U, 45U, 0U);
	o = lower_buffer_add(&dispatch, 0U, 1U, 46U, 0xDEADBEEFU);
	for (l = 0U; l < 45U; l++)
		v->words[l] = l * 1000003U;
	dispatch.groups[0] = 3U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("oddbar: FAIL %s\n", dispatch.failure);
		return 1;
	}
	for (group = 0U; group < 3U; group++) {
		for (l = 0U; l < 15U; l++) {
			if (o->words[group * 15U + l] != v->words[group * 15U + 14U - l] * 3U + l) {
				printf("oddbar: FAIL group %u word %u = %u\n", group, l, o->words[group * 15U + l]);
				return 1;
			}
		}
	}
	if (o->words[45] != 0xDEADBEEFU) {
		printf("oddbar: FAIL a word past the groups written\n");
		return 1;
	}
	printf("oddbar: 3 groups of 5 x 3 mirrored through shared memory across a barrier\n");
	return 0;
}

/* atomsh.comp (ws101-p006): shared-memory atomics of 4 groups of 64 between barriers. */
static int
lower_test_atomsh(
	const char *dir)
{
	struct lower_dispatch dispatch;
	struct lower_buffer *v;
	struct lower_buffer *results;
	struct lower_buffer *old;
	uint32_t expected[20];
	uint32_t seen[64];
	uint32_t group;
	uint32_t i;
	uint32_t x;

	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.ir = lower_parse(dir, "atomsh");
	if (dispatch.ir == NULL)
		return 1;
	v = lower_buffer_add(&dispatch, 0U, 0U, 256U, 0U);
	results = lower_buffer_add(&dispatch, 0U, 1U, 80U, 0xDEADBEEFU);
	old = lower_buffer_add(&dispatch, 0U, 2U, 256U, 0xDEADBEEFU);
	for (i = 0U; i < 256U; i++)
		v->words[i] = (i * 2654435761U) >> 22;
	dispatch.groups[0] = 4U;
	dispatch.groups[1] = 1U;
	dispatch.groups[2] = 1U;
	if (lower_run(&dispatch) != 0) {
		printf("atomsh: FAIL %s\n", dispatch.failure);
		return 1;
	}
	for (group = 0U; group < 4U; group++) {
		memset(expected, 0, sizeof(expected));
		for (i = 0U; i < 64U; i++) {
			x = v->words[group * 64U + i];
			expected[x % 16U]++;
			expected[16] += x;
			if (x > expected[17])
				expected[17] = x;
		}
		expected[19] = 64U;
		for (i = 0U; i < 20U; i++) {
			if (i == 18U)
				continue;
			if (results->words[group * 20U + i] != expected[i]) {
				printf("atomsh: FAIL group %u word %u = %u, expected %u\n", group, i, results->words[group * 20U + i], expected[i]);
				return 1;
			}
		}
		if (results->words[group * 20U + 18U] == 0U || results->words[group * 20U + 18U] > 64U) {
			printf("atomsh: FAIL group %u: the compare-exchange left %u\n", group, results->words[group * 20U + 18U]);
			return 1;
		}
		memset(seen, 0, sizeof(seen));
		for (i = 0U; i < 64U; i++) {
			x = old->words[group * 64U + i];
			if (x >= 64U || seen[x] != 0U) {
				printf("atomsh: FAIL group %u invocation %u: old value %u is not a fresh count\n", group, i, x);
				return 1;
			}
			seen[x] = 1U;
		}
	}
	printf("atomsh: 4 groups' shared histogram, sum, maximum, compare-exchange and counter between barriers\n");
	return 0;
}
