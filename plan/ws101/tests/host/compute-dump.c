/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws101-p002: compiles one compute SPIR-V module with the i915 executor's
 * compiler on the host, checks the kernel and writes its EU code for Mesa's
 * disassembler (plan/ws101/tests/host/run.sh).
 *
 * The checks, each printed as a line the script reads:
 *   - the scoreboard is sound (plan/ws075/tests/guard/scoreboard-check.h);
 *   - every SEND's descriptor fields agree: a data-port atomic asks for the
 *     old value (message control bit 5) exactly when it has a reply
 *     register, and its message is two address registers long (A64) or one
 *     at binding table entry 254 in SIMD8 form (shared memory, ws101-p006);
 *   - a memory fence is a SIMD1 NoMask message with a header, the commit
 *     bit and one reply register, and a workgroup barrier is the gateway's
 *     barrier message outside the mask followed by sync.bar (ws101-p006);
 *   - the kernel ends with the thread's end sent to the thread spawner;
 *   - the binary describes the workgroup and the push data.
 *
 *   compute-dump FILE.spv OUT.bin      compiles, checks and writes the kernel
 *   compute-dump -refuse FILE.spv      expects the module to be refused
 */

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
#include "../../../../src/drivers/gpu/i915/compiler/eu.c"
#include "../../../../src/drivers/gpu/i915/compiler/compile.c"
#include "../../../ws075/tests/guard/scoreboard-check.h"

/* The Gen12 SEND opcode, and the shared functions a compute kernel sends to. */
#define DUMP_OP_SEND		0x31U
#define DUMP_SFID_DC1		12U
#define DUMP_SFID_TS		7U

/* The data-port message types of the A64 and the surface untyped atomics. */
#define DUMP_A64_ATOMIC		0x12U
#define DUMP_UNTYPED_ATOMIC	0x02U

/* The gateway, the data cache's fence and the sync opcode (ws101-p006). */
#define DUMP_SFID_GATEWAY	3U
#define DUMP_SFID_DC		10U
#define DUMP_MEMORY_FENCE	7U
#define DUMP_OP_SYNC		0x01U
#define DUMP_SYNC_BAR		0x0eU

static uint32_t *dump_read(const char *path, size_t *words);
static uint32_t dump_descriptor(const uint32_t *inst);
static int dump_check_sends(const char *name, const uint32_t *code, unsigned count);

int
main(
	int argc,
	char **argv)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *ir;
	struct i915_shader_binary *binary;
	const char *path;
	uint32_t *code;
	uint32_t *stripped;
	size_t words;
	unsigned count;
	unsigned index;
	unsigned kept;
	unsigned block;
	int refuse;
	int fault;
	int error;
	FILE *file;

	/* The mode, the module and the output. */
	refuse = 0;
	if (argc == 3 && strcmp(argv[1], "-refuse") == 0) {
		refuse = 1;
		path = argv[2];
	} else if (argc == 3) {
		path = argv[1];
	} else {
		fprintf(stderr, "usage: compute-dump FILE.spv OUT.bin | compute-dump -refuse FILE.spv\n");
		return 2;
	}

	/* Reads the module. */
	code = dump_read(path, &words);
	if (code == NULL) {
		fprintf(stderr, "%s: cannot be read\n", path);
		return 1;
	}

	/* Parses it as a compute shader. */
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_COMPUTE, &ir, &diagnostic);

	/* A module that must be refused passes when the parser or the compiler refuses it. */
	if (refuse != 0) {
		if (error != 0) {
			printf("%s: refused as expected: %d (%s)\n", path, error, diagnostic.reason != NULL ? diagnostic.reason : "?");
			return 0;
		}
		error = drv_i915_shader_compile(ir, &binary);
		if (error != 0) {
			printf("%s: refused as expected by the compiler: %d\n", path, error);
			return 0;
		}
		printf("%s: FAIL accepted, but must be refused\n", path);
		return 1;
	}

	/* Any other module must parse and compile. */
	if (error != 0) {
		printf("%s: FAIL refused by the parser: %d (%s, opcode %u)\n", path, error,
		       diagnostic.reason != NULL ? diagnostic.reason : "?", diagnostic.opcode);
		return 1;
	}
	if (ir->stage != DRV_GPU_STAGE_COMPUTE) {
		printf("%s: FAIL not a compute shader\n", path);
		return 1;
	}
	error = drv_i915_shader_compile(ir, &binary);
	if (error != 0) {
		printf("%s: FAIL refused by the compiler: %d\n", path, error);
		return 1;
	}

	/* The scoreboard: sound as made. */
	count = binary->code_bytes / 16U;
	fault = sbc_check(binary->code, count);
	if (fault >= 0) {
		printf("%s: FAIL scoreboard fault at instruction %d\n", path, fault);
		return 1;
	}

	/* Without its waits the kernel should be faulty when it waits at all. */
	stripped = calloc(count + 1U, 16U);
	kept = 0U;
	for (index = 0U; index < count; index++) {
		if (sbc_field(binary->code + index * 4U, 6U, 0U) == 1U)
			continue;
		memcpy(stripped + kept * 4U, binary->code + index * 4U, 16U);
		kept++;
	}
	fault = sbc_check(stripped, kept);
	printf("%s: scoreboard sound; %u sync.nop; without them %s\n", path, count - kept, fault >= 0 ? "a fault is found" : "no fault");
	free(stripped);

	/* The messages' descriptors and the thread's end. */
	if (dump_check_sends(path, binary->code, count) != 0)
		return 1;

	/* What the dispatch programs around the kernel. */
	printf("%s: local size %u %u %u, push %u regs (constants %u bytes), cross-thread %u, per-thread %u, scratch %u, grf %u, shared %u, barrier %u\n",
	       path,
	       binary->local_size[0], binary->local_size[1], binary->local_size[2],
	       binary->push_regs, binary->push_constant_bytes,
	       binary->cross_thread_regs, binary->per_thread_regs,
	       binary->scratch_bytes, binary->grf_used,
	       binary->shared_bytes, binary->uses_barrier);
	for (block = 0U; block < binary->block_count; block++) {
		printf("%s: block %u set %u binding %u address %u push offset %u bytes %u\n",
		       path, block,
		       binary->blocks[block].set, binary->blocks[block].binding,
		       binary->blocks[block].address, binary->blocks[block].push_offset,
		       binary->blocks[block].bytes);
	}
	if (binary->per_thread_regs != I915_SHADER_PER_THREAD_REGS ||
	    binary->cross_thread_regs != binary->push_regs ||
	    binary->dispatch_grf_start != 1U) {
		printf("%s: FAIL the binary does not describe the compute payload\n", path);
		return 1;
	}

	/* Writes the kernel's code. */
	file = fopen(argv[2], "wb");
	if (file == NULL)
		return 1;
	fwrite(binary->code, 1U, binary->code_bytes, file);
	fclose(file);
	printf("%s: %u instructions\n", path, count);
	return 0;
}

/* Reads a SPIR-V module into words; NULL when it cannot be read. */
static uint32_t *
dump_read(
	const char *path,
	size_t *words)
{
	uint32_t *code;
	FILE *file;
	long size;
	size_t got;

	/* Opens the module and measures it. */
	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);

	/* Reads it whole. */
	code = malloc((size_t)size + 4U);
	if (code == NULL) {
		fclose(file);
		return NULL;
	}
	got = fread(code, 1U, (size_t)size, file);
	fclose(file);
	if (got != (size_t)size) {
		free(code);
		return NULL;
	}

	/* Succeeded: the module's words. */
	*words = (size_t)size / 4U;
	return code;
}

/* Gathers a SEND's message descriptor from the fields eu.c scatters it into. */
static uint32_t
dump_descriptor(
	const uint32_t *inst)
{
	uint32_t descriptor;

	descriptor = 0U;
	descriptor |= sbc_field(inst, 123U, 122U) << 30;
	descriptor |= sbc_field(inst, 71U, 67U) << 25;
	descriptor |= sbc_field(inst, 55U, 51U) << 20;
	descriptor |= sbc_field(inst, 121U, 113U) << 11;
	descriptor |= sbc_field(inst, 91U, 81U);
	return descriptor;
}

/*
 * Checks every SEND of a kernel: an atomic's reply bit agrees with its reply
 * length and an A64 atomic has two address registers; the last instruction
 * is the thread's end to the thread spawner.  Returns 0 when all agree.
 */
static int
dump_check_sends(
	const char *name,
	const uint32_t *code,
	unsigned count)
{
	const uint32_t *inst;
	uint32_t descriptor;
	unsigned index;
	unsigned sfid;
	unsigned type;
	unsigned control;
	unsigned rlen;
	unsigned mlen;
	unsigned atomics;
	unsigned sends;
	unsigned fences;
	unsigned barriers;
	unsigned next;

	/* Walks every SEND. */
	atomics = 0U;
	sends = 0U;
	fences = 0U;
	barriers = 0U;
	for (index = 0U; index < count; index++) {
		inst = code + index * 4U;
		if (sbc_field(inst, 6U, 0U) != DUMP_OP_SEND)
			continue;
		sends++;
		sfid = sbc_field(inst, 95U, 92U);
		descriptor = dump_descriptor(inst);
		type = (descriptor >> 14) & 0x1fU;
		control = (descriptor >> 8) & 0x3fU;
		rlen = (descriptor >> 20) & 0x1fU;
		mlen = (descriptor >> 25) & 0xfU;

		/* A memory fence: SIMD1 outside the mask, a header, commit, one reply register (ws101-p006). */
		if (sfid == DUMP_SFID_DC && type == DUMP_MEMORY_FENCE) {
			fences++;
			if (sbc_field(inst, 18U, 16U) != 0U || sbc_field(inst, 31U, 31U) != 1U ||
			    (descriptor & (1U << 19)) == 0U || (control & 0x20U) == 0U || rlen != 1U || mlen != 1U ||
			    ((descriptor & 0xffU) != 0U && (descriptor & 0xffU) != 254U)) {
				printf("%s: FAIL fence at %u: descriptor 0x%08x\n", name, index, descriptor);
				return 1;
			}
			continue;
		}

		/* A workgroup barrier: the gateway's message outside the mask, then sync.bar before anything else runs (ws101-p006). */
		if (sfid == DUMP_SFID_GATEWAY) {
			barriers++;
			if (descriptor != 0x02000004U || sbc_field(inst, 31U, 31U) != 1U) {
				printf("%s: FAIL barrier message at %u: descriptor 0x%08x\n", name, index, descriptor);
				return 1;
			}
			next = index + 1U;
			while (next < count && sbc_field(code + next * 4U, 6U, 0U) == DUMP_OP_SYNC &&
			       sbc_field(code + next * 4U, 95U, 92U) == 0U)
				next++;
			if (next >= count || sbc_field(code + next * 4U, 6U, 0U) != DUMP_OP_SYNC ||
			    sbc_field(code + next * 4U, 95U, 92U) != DUMP_SYNC_BAR) {
				printf("%s: FAIL barrier message at %u is not followed by sync.bar\n", name, index);
				return 1;
			}
			continue;
		}
		if (sfid != DUMP_SFID_DC1)
			continue;
		if (type != DUMP_A64_ATOMIC && type != DUMP_UNTYPED_ATOMIC)
			continue;

		/* An atomic asks for the old value exactly when it has a reply register. */
		atomics++;
		if (((control >> 5) & 1U) != (rlen != 0U ? 1U : 0U)) {
			printf("%s: FAIL atomic at %u: reply bit %u but reply length %u (descriptor 0x%08x)\n",
			       name, index, (control >> 5) & 1U, rlen, descriptor);
			return 1;
		}
		if (type == DUMP_A64_ATOMIC && mlen != 2U) {
			printf("%s: FAIL A64 atomic at %u with %u address registers\n", name, index, mlen);
			return 1;
		}
		if (type == DUMP_UNTYPED_ATOMIC && (mlen != 1U || (control & 0x10U) == 0U || (descriptor & 0xffU) != 254U)) {
			printf("%s: FAIL shared-memory atomic at %u: descriptor 0x%08x\n", name, index, descriptor);
			return 1;
		}
	}

	/* The kernel ends by sending the header to the thread spawner. */
	inst = code + (count - 1U) * 4U;
	if (count == 0U ||
	    sbc_field(inst, 6U, 0U) != DUMP_OP_SEND ||
	    sbc_field(inst, 95U, 92U) != DUMP_SFID_TS ||
	    dump_descriptor(inst) != 0x02000000U) {
		printf("%s: FAIL the kernel does not end with the thread spawner message\n", name);
		return 1;
	}

	/* Succeeded: every message agrees. */
	printf("%s: %u sends, %u atomics, %u fences, %u barriers, descriptors agree, ends at the thread spawner\n", name, sends, atomics, fences, barriers);
	return 0;
}
