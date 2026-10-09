/*
 * WS031 E-128: host tool -- runs the executor's own compiler (spirv.c -> compile.c -> eu.c) on a SPIR-V
 * module and writes the kernel it produces, for Mesa's gentool to disassemble / re-assemble.
 *   i915-vk-eudump vertex|fragment|geometry module.spv kernel.bin
 * A geometry shader is compiled after a vertex shader that writes exactly the locations it reads (ws075-p007a).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }

#include "../../../src/drivers/gpu/compiler/spirv.c"
#include "../../../src/drivers/gpu/i915/compiler/eu.c"
#include "../../../src/drivers/gpu/i915/compiler/compile.c"

int
main(int argc, char **argv)
{
	struct drv_gpu_shader_ir *ir;
	struct i915_shader_binary *binary;
	struct i915_shader_binary producer;
	enum drv_gpu_shader_stage stage;
	uint32_t index;
	struct drv_gpu_compile_diagnostic diag;
	uint32_t *words;
	long bytes;
	FILE *f;
	int error;

	if (argc != 4)
		return 2;
	f = fopen(argv[2], "rb");
	if (f == NULL)
		return 2;
	fseek(f, 0, SEEK_END);
	bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	words = malloc((size_t)bytes);
	if (fread(words, 1, (size_t)bytes, f) != (size_t)bytes)
		return 2;
	fclose(f);

	stage = DRV_GPU_STAGE_FRAGMENT;
	if (strcmp(argv[1], "vertex") == 0)
		stage = DRV_GPU_STAGE_VERTEX;
	if (strcmp(argv[1], "geometry") == 0)
		stage = DRV_GPU_STAGE_GEOMETRY;
	memset(&diag, 0, sizeof(diag));
	error = drv_gpu_shader_parse(words, (size_t)bytes / 4U, stage, &ir, &diag);
	if (error != 0) {
		fprintf(stderr, "parse: error %d (%s, opcode %u at word %u)\n", error,
			diag.reason != NULL ? diag.reason : "?", diag.opcode, diag.word_offset);
		return 1;
	}
	/* a geometry shader's producer writes the locations it reads, in ascending order (the parser lists them so) */
	memset(&producer, 0, sizeof(producer));
	producer.stage = DRV_GPU_STAGE_VERTEX;
	for (index = 0U; index < ir->input_count && index < I915_SHADER_MAX_INPUTS; index++)
		producer.varying_locations[producer.varying_count++] = ir->inputs[index].location;
	if (stage == DRV_GPU_STAGE_GEOMETRY)
		error = drv_i915_shader_compile_stage(ir, &producer, &binary);
	else
		error = drv_i915_shader_compile(ir, &binary);
	if (error != 0) {
		fprintf(stderr, "compile: error %d\n", error);
		return 1;
	}
	fprintf(stderr, "%s: %u IR instructions, %u bytes of kernel, grf_used %u\n", argv[1],
		ir->instruction_count, binary->code_bytes, binary->grf_used);
	f = fopen(argv[3], "wb");
	fwrite(binary->code, 1, binary->code_bytes, f);
	fclose(f);
	return 0;
}
