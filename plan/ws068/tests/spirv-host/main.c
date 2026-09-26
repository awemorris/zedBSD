/*
 * Host test of libGLESv2's SPIR-V side (ws068-p008): reflects glslc's output and rewrites the vertex
 * shader's gl_Position; spirv-val checks the result (run.sh).
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include "../../../../userland/base/libglesv2/gles.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t *load(const char *path, size_t *words)
{
	FILE *file;
	long size;
	uint32_t *code;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size);
	if (fread(code, 1U, (size_t)size, file) != (size_t)size)
		return NULL;
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

int main(int argc, char **argv)
{
	struct gles_spirv spirv;
	char log[256];
	uint32_t *code;
	uint32_t *patched;
	size_t words;
	size_t patched_words;
	unsigned index;
	FILE *out;
	int status;

	code = load(argv[1], &words);
	if (code == NULL || argc < 3)
		return 2;
	status = gles_spirv_reflect(code, words, &spirv, log, sizeof(log));
	if (status != 0) {
		printf("reflect failed: %s", log);
		return 1;
	}
	printf("model=%u block=%d binding=%u\n", spirv.model, spirv.has_block, spirv.block_binding);
	for (index = 0U; index < spirv.input_count; index++)
		printf("in %s location=%u type=0x%x components=%u\n", spirv.inputs[index].name, spirv.inputs[index].location,
		       spirv.inputs[index].type, spirv.inputs[index].components);
	for (index = 0U; index < spirv.output_count; index++)
		printf("out %s location=%u\n", spirv.outputs[index].name, spirv.outputs[index].location);
	for (index = 0U; index < spirv.uniform_count; index++)
		printf("uniform %s type=0x%x size=%d offset=%u stride=%u mstride=%u sampler=%d binding=%u\n",
		       spirv.uniforms[index].name, spirv.uniforms[index].type, spirv.uniforms[index].size,
		       spirv.uniforms[index].offset, spirv.uniforms[index].array_stride, spirv.uniforms[index].matrix_stride,
		       spirv.uniforms[index].sampler, spirv.uniforms[index].binding);
	gles_spirv_free(&spirv);
	patched = gles_spirv_position(code, words, &patched_words);
	if (patched == NULL) {
		printf("position failed\n");
		return 1;
	}
	out = fopen(argv[2], "wb");
	fwrite(patched, 4U, patched_words, out);
	fclose(out);
	printf("patched words=%zu (was %zu)\n", patched_words, words);
	return 0;
}
