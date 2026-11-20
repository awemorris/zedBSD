/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of input attachments to sampled images, included by
 * spirv.c.
 *
 * A driver whose render pass runs each subpass as a render pass of its own
 * has the attachments a subpass reads in memory when the subpass starts,
 * so a fragment shader may read one as an image.  This file rewrites a
 * SPIR-V module so that every input attachment (an image of Dim
 * SubpassData) is a 2D sampled image (Sampled 1, of no format, its
 * multisampling kept), and every subpassLoad (OpImageRead of one) an
 * OpImageFetch at the fragment's pixel: the integer part of FragCoord's x
 * and y plus the read's coordinate (the offset of subpassLoad), at level 0
 * or at the read's sample.  The module's FragCoord input is used, or one
 * is declared and added to the interface of each fragment entry point.
 * The InputAttachmentIndex decorations and the InputAttachment capability
 * go.  The descriptor of the binding is then read as a sampled image's: the
 * image view, without a sampler.  The rewritten module is still SPIR-V, so
 * every reader takes it, and nothing in it names a GPU.
 *
 * XXX: a multiview subpass reads its view's layer of the attachment; here
 * the view is read as the 2D image it is, so only layer 0.
 *
 * What is refused (ENOTSUP, with the instruction as the diagnostic): a read
 * whose coordinate is not a vector of two 32-bit signed integers of the
 * module, a read with an image operand other than Sample and those with no
 * id (which go), and a module without room for the ids the rewrite adds.
 *
 * Values referenced: Khronos SPIR-V 1.6 specification, sections 2.3
 * (physical layout), 2.4 (logical layout of a module), 3.7 (Dim), 3.14
 * (Image Operands: Lod 0x2, Sample 0x40,
 * NonPrivateTexel 0x400, VolatileTexel 0x800, SignExtend 0x1000,
 * ZeroExtend 0x2000, Nontemporal 0x4000), 3.20 (Decoration:
 * InputAttachmentIndex 43, BuiltIn 11), 3.21 (BuiltIn: FragCoord 15), 3.31
 * (Capability: InputAttachment 40) and 3.37 (OpTypeImage 25, OpLoad 61,
 * OpImageRead 98, OpImageFetch 95, OpVectorShuffle 79, OpConvertFToS 110,
 * OpIAdd 128); the Vulkan specification, "Input Attachment Interface"
 * (subpassLoad reads the attachment at the fragment's framebuffer
 * coordinate).
 */

/* The opcodes, decorations, built-ins and capabilities the lowering reads and writes. */
#define INPUT_OP_CAPABILITY 17U
#define INPUT_OP_ENTRY_POINT 15U
#define INPUT_OP_TYPE_INT 21U
#define INPUT_OP_TYPE_FLOAT 22U
#define INPUT_OP_TYPE_VECTOR 23U
#define INPUT_OP_TYPE_IMAGE 25U
#define INPUT_OP_TYPE_POINTER 32U
#define INPUT_OP_CONSTANT 43U
#define INPUT_OP_CONSTANT_COMPOSITE 44U
#define INPUT_OP_CONSTANT_NULL 46U
#define INPUT_OP_SPEC_CONSTANT_COMPOSITE 51U
#define INPUT_OP_FUNCTION 54U
#define INPUT_OP_VARIABLE 59U
#define INPUT_OP_LOAD 61U
#define INPUT_OP_IMAGE_READ 98U
#define INPUT_OP_DECORATE 71U
#define INPUT_OP_VECTOR_SHUFFLE 79U
#define INPUT_OP_COMPOSITE_CONSTRUCT 80U
#define INPUT_OP_IMAGE_FETCH 95U
#define INPUT_OP_CONVERT_F_TO_S 110U
#define INPUT_OP_IADD 128U
#define INPUT_DIM_2D 1U
#define INPUT_DIM_SUBPASS_DATA 6U
#define INPUT_DECORATION_BUILT_IN 11U
#define INPUT_DECORATION_INDEX 43U
#define INPUT_BUILT_IN_FRAG_COORD 15U
#define INPUT_CAPABILITY 40U
#define INPUT_STORAGE_INPUT 1U
#define INPUT_EXECUTION_FRAGMENT 4U
#define INPUT_OPERAND_LOD 0x2U
#define INPUT_OPERAND_SAMPLE 0x40U

/* The image operands without an id that a read may have and a fetch does not need: NonPrivateTexel, VolatileTexel, SignExtend, ZeroExtend, Nontemporal. */
#define INPUT_OPERANDS_DROPPED 0x7C00U

/* The words one rewritten read adds: the four instructions before the fetch (20), and the fetch's Lod (2). */
#define INPUT_READ_EXTRA 22U

/* The most words the declarations of the rewrite add. */
#define INPUT_DECLARATION_WORDS 32U

/*
 * What the lowering knows of a module: the ids of the types and the
 * constant it uses (0 for one the module lacks), the FragCoord input, the
 * input attachment types, the type of each loaded or constant id, and
 * where the global declarations end.
 */
struct drv_gpu_input_lower {
	const uint32_t *code;
	uint32_t words;
	uint32_t next_id;
	uint8_t *subpass;
	uint32_t *type_of;
	uint32_t float_type;
	uint32_t int_type;
	uint32_t vec4_float;
	uint32_t vec2_float;
	uint32_t vec2_int;
	uint32_t input_pointer;
	uint32_t zero;
	uint32_t frag_coord;
	int frag_coord_new;
	uint32_t reads;
	uint32_t entry_points;
	uint32_t types_at;
	uint32_t functions_at;
	struct drv_gpu_compile_diagnostic diag;
};

static int drv_gpu_input_lower_scan(struct drv_gpu_input_lower *lower);
static void drv_gpu_input_lower_scan_one(struct drv_gpu_input_lower *lower, const uint32_t *word, uint32_t count, uint32_t at);
static int drv_gpu_input_lower_ids(struct drv_gpu_input_lower *lower);
static uint32_t drv_gpu_input_lower_new_id(struct drv_gpu_input_lower *lower);
static uint32_t drv_gpu_input_lower_declarations(struct drv_gpu_input_lower *lower, uint32_t *out);
static uint32_t drv_gpu_input_lower_read(struct drv_gpu_input_lower *lower, const uint32_t *word, uint32_t count, uint32_t *out);
static uint32_t drv_gpu_input_lower_entry_point(struct drv_gpu_input_lower *lower, const uint32_t *word, uint32_t count, uint32_t *out);
static int drv_gpu_input_lower_assemble(struct drv_gpu_input_lower *lower, uint32_t **out, size_t *out_count);
static int drv_gpu_input_lower_refuse(struct drv_gpu_input_lower *lower, uint32_t opcode, uint32_t at, const char *reason);

/*
 * Rewrites a SPIR-V module's input attachments as sampled images read at
 * the fragment's pixel.
 *
 * *out is NULL for a module without an input attachment (it is used as it
 * is), or the rewritten module, which the caller frees with kern_free().
 * Returns 0, ENOMEM, or ENOTSUP with the refused instruction in
 * *diagnostic; a module the parse refuses is left to it (0, *out NULL).
 */
int
drv_gpu_spirv_lower_input_attachments(
	const uint32_t *words,
	size_t word_count,
	uint32_t **out,
	size_t *out_count,
	struct drv_gpu_compile_diagnostic *diagnostic)
{
	struct drv_gpu_input_lower *lower;
	uint32_t index;
	int found;
	int error;

	/* Nothing is rewritten unless the module has an input attachment. */
	if (out == NULL || out_count == NULL)
		return EINVAL;
	*out = NULL;
	*out_count = 0U;
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A header the parse refuses is left to it. */
	if (words == NULL ||
	    word_count < SPIRV_HEADER_WORDS ||
	    word_count > UINT32_MAX / 2U ||
	    words[0] != SPIRV_MAGIC ||
	    words[3] == 0U ||
	    words[3] > SPIRV_MAX_BOUND)
		return 0;

	/* Allocates the lowering's state and its tables by id. */
	lower = kern_calloc(1U, sizeof(*lower));
	if (lower == NULL)
		return ENOMEM;
	lower->code = words;
	lower->words = (uint32_t)word_count;
	lower->next_id = words[3];
	lower->subpass = kern_calloc(words[3], sizeof(*lower->subpass));
	lower->type_of = kern_calloc(words[3], sizeof(*lower->type_of));
	error = 0;
	if (lower->subpass == NULL || lower->type_of == NULL)
		error = ENOMEM;

	/* Finds the input attachment types and what the rewrite needs. */
	if (error == 0)
		error = drv_gpu_input_lower_scan(lower);

	/* A module without one is used as it is. */
	found = 0;
	for (index = 0U; error == 0 && index < words[3] && !found; index++)
		found = lower->subpass[index] != 0U;

	/* Gives the ids the rewrite adds, then writes the rewritten module. */
	if (error == 0 && found)
		error = drv_gpu_input_lower_ids(lower);
	if (error == 0 && found)
		error = drv_gpu_input_lower_assemble(lower, out, out_count);

	/* A refusal names the instruction for the caller. */
	if (error == ENOTSUP && diagnostic != NULL)
		*diagnostic = lower->diag;

	/* The tables are only needed while rewriting. */
	kern_free(lower->type_of);
	kern_free(lower->subpass);
	kern_free(lower);
	if (error != 0)
		return error;

	/* Succeeded: *out is the rewritten module, or NULL. */
	return 0;
}

/*
 * Scans the module: the input attachment types, the 32-bit float and
 * signed integer types and their vectors, the FragCoord input, the zero
 * integer constant, the types of loaded and constant ids, the reads of
 * input attachments, the entry points, and where the types and the
 * functions start.  A malformed instruction leaves the module to the parse
 * (no input attachment is reported).
 */
static int
drv_gpu_input_lower_scan(
	struct drv_gpu_input_lower *lower)
{
	uint32_t at;
	uint32_t count;
	uint32_t index;

	/* Each instruction in turn. */
	lower->types_at = lower->words;
	lower->functions_at = lower->words;
	for (at = SPIRV_HEADER_WORDS; at < lower->words; at += count) {
		count = lower->code[at] >> 16;
		if (count == 0U || count > lower->words - at) {
			for (index = 0U; index < lower->next_id; index++)
				lower->subpass[index] = 0U;
			return 0;
		}
		drv_gpu_input_lower_scan_one(lower, &lower->code[at], count, at);
	}

	/* Succeeded: the module is known. */
	return 0;
}

/* Records what one instruction of the module tells the lowering (drv_gpu_input_lower_scan). */
static void
drv_gpu_input_lower_scan_one(
	struct drv_gpu_input_lower *lower,
	const uint32_t *word,
	uint32_t count,
	uint32_t at)
{
	uint32_t opcode;
	uint32_t bound;

	opcode = word[0] & 0xFFFFU;
	bound = lower->next_id;

	/* Where the types (after the annotations) and the functions start. */
	if (lower->types_at == lower->words &&
	    ((opcode >= 19U && opcode <= 39U) ||
	     (opcode >= 41U && opcode <= 52U) ||
	     opcode == INPUT_OP_VARIABLE))
		lower->types_at = at;
	if (lower->functions_at == lower->words && opcode == INPUT_OP_FUNCTION)
		lower->functions_at = at;

	/* The types the rewrite reads. */
	if (opcode == INPUT_OP_TYPE_IMAGE && count >= 9U && word[1] < bound && word[3] == INPUT_DIM_SUBPASS_DATA)
		lower->subpass[word[1]] = 1U;
	if (opcode == INPUT_OP_TYPE_FLOAT && count >= 3U && word[2] == 32U && lower->float_type == 0U)
		lower->float_type = word[1];
	if (opcode == INPUT_OP_TYPE_INT && count >= 4U && word[2] == 32U && word[3] == 1U && lower->int_type == 0U)
		lower->int_type = word[1];
	if (opcode == INPUT_OP_TYPE_VECTOR && count >= 4U && word[2] != 0U) {
		if (word[2] == lower->float_type && word[3] == 4U && lower->vec4_float == 0U)
			lower->vec4_float = word[1];
		if (word[2] == lower->float_type && word[3] == 2U && lower->vec2_float == 0U)
			lower->vec2_float = word[1];
		if (word[2] == lower->int_type && word[3] == 2U && lower->vec2_int == 0U)
			lower->vec2_int = word[1];
	}
	if (opcode == INPUT_OP_TYPE_POINTER && count >= 4U && word[2] == INPUT_STORAGE_INPUT &&
	    word[3] != 0U && word[3] == lower->vec4_float && lower->input_pointer == 0U)
		lower->input_pointer = word[1];

	/* The zero of the integer type. */
	if (opcode == INPUT_OP_CONSTANT && count == 4U && word[1] != 0U && word[1] == lower->int_type && word[3] == 0U &&
	    lower->zero == 0U)
		lower->zero = word[2];

	/* The FragCoord input. */
	if (opcode == INPUT_OP_DECORATE && count >= 4U && word[2] == INPUT_DECORATION_BUILT_IN &&
	    word[3] == INPUT_BUILT_IN_FRAG_COORD)
		lower->frag_coord = word[1];

	/* The type of each id a read names: a loaded image, a constant or a built coordinate. */
	if ((opcode == INPUT_OP_LOAD ||
	     opcode == INPUT_OP_CONSTANT_COMPOSITE ||
	     opcode == INPUT_OP_CONSTANT_NULL ||
	     opcode == INPUT_OP_SPEC_CONSTANT_COMPOSITE ||
	     opcode == INPUT_OP_COMPOSITE_CONSTRUCT ||
	     opcode == INPUT_OP_VARIABLE) &&
	    count >= 3U && word[2] < bound)
		lower->type_of[word[2]] = word[1];

	/* The reads of input attachments, and the entry points. */
	if (opcode == INPUT_OP_IMAGE_READ && count >= 5U && word[3] < bound && lower->type_of[word[3]] < bound &&
	    lower->subpass[lower->type_of[word[3]]] != 0U)
		lower->reads++;
	if (opcode == INPUT_OP_ENTRY_POINT)
		lower->entry_points++;
}

/*
 * Gives the ids the rewrite adds: the types, the zero and the FragCoord
 * input the module lacks.  Returns 0, or ENOTSUP when the bound has no
 * room.
 */
static int
drv_gpu_input_lower_ids(
	struct drv_gpu_input_lower *lower)
{
	/* A module without a read needs nothing more than its types rewritten. */
	if (lower->reads == 0U)
		return 0;

	/* The coordinate's integer type is the module's (a subpassLoad's coordinate is one). */
	if (lower->int_type == 0U || lower->vec2_int == 0U)
		return drv_gpu_input_lower_refuse(lower, INPUT_OP_IMAGE_READ, 0U, "input attachment read without a vector of two 32-bit signed integers");

	/* The float types, the zero and the input the fetch's coordinate needs. */
	if (lower->float_type == 0U)
		lower->float_type = drv_gpu_input_lower_new_id(lower);
	if (lower->vec4_float == 0U)
		lower->vec4_float = drv_gpu_input_lower_new_id(lower);
	if (lower->vec2_float == 0U)
		lower->vec2_float = drv_gpu_input_lower_new_id(lower);
	if (lower->zero == 0U)
		lower->zero = drv_gpu_input_lower_new_id(lower);
	if (lower->frag_coord == 0U) {
		if (lower->input_pointer == 0U)
			lower->input_pointer = drv_gpu_input_lower_new_id(lower);
		lower->frag_coord = drv_gpu_input_lower_new_id(lower);
		lower->frag_coord_new = 1;
	}

	/* Each read's four results come after these. */
	if (lower->next_id > SPIRV_MAX_BOUND - 4U * lower->reads)
		return drv_gpu_input_lower_refuse(lower, INPUT_OP_IMAGE_READ, 0U, "no room for the ids of the input attachment reads");

	/* Succeeded: the ids are given. */
	return 0;
}

/* Gives a fresh id (the bound grows by one); 0 when the bound is reached. */
static uint32_t
drv_gpu_input_lower_new_id(
	struct drv_gpu_input_lower *lower)
{
	uint32_t id;

	/* No more than the parse takes. */
	if (lower->next_id >= SPIRV_MAX_BOUND)
		return 0U;

	/* Succeeded: the next id. */
	id = lower->next_id;
	lower->next_id++;
	return id;
}

/*
 * Writes the declarations the rewrite adds before the module's functions:
 * the float types, the zero, and the FragCoord input with its pointer
 * type.  Returns the words written.
 */
static uint32_t
drv_gpu_input_lower_declarations(
	struct drv_gpu_input_lower *lower,
	uint32_t *out)
{
	uint32_t used;

	used = 0U;

	/* A module without a read needs nothing declared. */
	if (lower->reads == 0U)
		return 0U;

	/* The float types the module lacks: the ids above its bound are the rewrite's. */
	if (lower->float_type >= lower->code[3]) {
		out[used++] = (3U << 16) | INPUT_OP_TYPE_FLOAT;
		out[used++] = lower->float_type;
		out[used++] = 32U;
	}
	if (lower->vec4_float >= lower->code[3]) {
		out[used++] = (4U << 16) | INPUT_OP_TYPE_VECTOR;
		out[used++] = lower->vec4_float;
		out[used++] = lower->float_type;
		out[used++] = 4U;
	}
	if (lower->vec2_float >= lower->code[3]) {
		out[used++] = (4U << 16) | INPUT_OP_TYPE_VECTOR;
		out[used++] = lower->vec2_float;
		out[used++] = lower->float_type;
		out[used++] = 2U;
	}

	/* The zero of the integer type. */
	if (lower->zero >= lower->code[3]) {
		out[used++] = (4U << 16) | INPUT_OP_CONSTANT;
		out[used++] = lower->int_type;
		out[used++] = lower->zero;
		out[used++] = 0U;
	}

	/* The FragCoord input and its pointer type. */
	if (lower->input_pointer >= lower->code[3]) {
		out[used++] = (4U << 16) | INPUT_OP_TYPE_POINTER;
		out[used++] = lower->input_pointer;
		out[used++] = INPUT_STORAGE_INPUT;
		out[used++] = lower->vec4_float;
	}
	if (lower->frag_coord_new) {
		out[used++] = (4U << 16) | INPUT_OP_VARIABLE;
		out[used++] = lower->input_pointer;
		out[used++] = lower->frag_coord;
		out[used++] = INPUT_STORAGE_INPUT;
	}

	/* Succeeded: the words written. */
	return used;
}

/*
 * Writes one OpImageRead: a read of an input attachment as the fetch at
 * the fragment's pixel plus its coordinate, any other as it is.  Returns
 * the words written, or 0 for a coordinate that is not a vector of two
 * signed integers or an operand that is not lowered (refused by the
 * caller).
 */
static uint32_t
drv_gpu_input_lower_read(
	struct drv_gpu_input_lower *lower,
	const uint32_t *word,
	uint32_t count,
	uint32_t *out)
{
	uint32_t loaded;
	uint32_t pair;
	uint32_t pixel;
	uint32_t coordinate;
	uint32_t operands;
	uint32_t fetch_at;
	uint32_t index;
	uint32_t used;

	/* Any other read is written as it is. */
	if (lower->type_of[word[3]] >= lower->code[3] || lower->subpass[lower->type_of[word[3]]] == 0U) {
		for (index = 0U; index < count; index++)
			out[index] = word[index];
		return count;
	}

	/* The coordinate is the offset of subpassLoad, a vector of two signed integers. */
	if (word[4] >= lower->code[3] || lower->type_of[word[4]] != lower->vec2_int)
		return 0U;

	/*
	 * Of the read's operands, the Sample (whose id comes first) is kept;
	 * the sign and memory operands, which have no id, go (a fetch reads
	 * the texel as its type says); any other is refused.
	 */
	operands = 0U;
	if (count > 5U)
		operands = word[5];
	if ((operands & ~(INPUT_OPERAND_SAMPLE | INPUT_OPERANDS_DROPPED)) != 0U)
		return 0U;
	if ((operands & INPUT_OPERAND_SAMPLE) != 0U && count < 7U)
		return 0U;

	/* FragCoord's x and y as integers, plus the offset. */
	loaded = lower->next_id++;
	pair = lower->next_id++;
	pixel = lower->next_id++;
	coordinate = lower->next_id++;
	used = 0U;
	out[used++] = (4U << 16) | INPUT_OP_LOAD;
	out[used++] = lower->vec4_float;
	out[used++] = loaded;
	out[used++] = lower->frag_coord;
	out[used++] = (7U << 16) | INPUT_OP_VECTOR_SHUFFLE;
	out[used++] = lower->vec2_float;
	out[used++] = pair;
	out[used++] = loaded;
	out[used++] = loaded;
	out[used++] = 0U;
	out[used++] = 1U;
	out[used++] = (4U << 16) | INPUT_OP_CONVERT_F_TO_S;
	out[used++] = lower->vec2_int;
	out[used++] = pixel;
	out[used++] = pair;
	out[used++] = (5U << 16) | INPUT_OP_IADD;
	out[used++] = lower->vec2_int;
	out[used++] = coordinate;
	out[used++] = pixel;
	out[used++] = word[4];

	/* The fetch: the read's operands (a multisampled one's Sample), or level 0. */
	fetch_at = used;
	out[used++] = INPUT_OP_IMAGE_FETCH;
	out[used++] = word[1];
	out[used++] = word[2];
	out[used++] = word[3];
	out[used++] = coordinate;
	if ((operands & INPUT_OPERAND_SAMPLE) != 0U) {
		out[used++] = INPUT_OPERAND_SAMPLE;
		out[used++] = word[6];
	} else {
		out[used++] = INPUT_OPERAND_LOD;
		out[used++] = lower->zero;
	}
	out[fetch_at] |= (used - fetch_at) << 16;

	/* Succeeded: the words written. */
	return used;
}

/*
 * Writes one OpEntryPoint, with a new FragCoord input added to the
 * interface of a fragment one.  Returns the words written.
 */
static uint32_t
drv_gpu_input_lower_entry_point(
	struct drv_gpu_input_lower *lower,
	const uint32_t *word,
	uint32_t count,
	uint32_t *out)
{
	uint32_t index;

	/* The entry point as it is. */
	for (index = 0U; index < count; index++)
		out[index] = word[index];

	/* A fragment one names the new input too. */
	if (!lower->frag_coord_new || count < 3U || word[1] != INPUT_EXECUTION_FRAGMENT)
		return count;
	out[count] = lower->frag_coord;
	out[0] = ((count + 1U) << 16) | INPUT_OP_ENTRY_POINT;

	/* Succeeded: one word more. */
	return count + 1U;
}

/*
 * Writes the rewritten module: the input attachment types as 2D sampled
 * images, the decorations and the capability of input attachments left
 * out, the FragCoord decoration and the declarations added, and each read
 * as a fetch.  Returns 0, ENOMEM, or ENOTSUP for a read refused.
 */
static int
drv_gpu_input_lower_assemble(
	struct drv_gpu_input_lower *lower,
	uint32_t **out,
	size_t *out_count)
{
	const uint32_t *word;
	uint32_t *module;
	uint32_t capacity;
	uint32_t written;
	uint32_t used;
	uint32_t opcode;
	uint32_t count;
	uint32_t at;
	uint32_t index;

	/* Room for the module, what each read and entry point adds, and the declarations. */
	capacity = lower->words + lower->reads * INPUT_READ_EXTRA + lower->entry_points + INPUT_DECLARATION_WORDS;
	module = kern_calloc(capacity, sizeof(*module));
	if (module == NULL)
		return ENOMEM;

	/* The header, with the bound the rewrite reaches (each read's four ids included). */
	for (index = 0U; index < SPIRV_HEADER_WORDS; index++)
		module[index] = lower->code[index];
	module[3] = lower->next_id + 4U * lower->reads;
	written = SPIRV_HEADER_WORDS;

	/* Each instruction, rewritten where the lowering says. */
	for (at = SPIRV_HEADER_WORDS; at < lower->words; at += count) {
		word = &lower->code[at];
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/* The FragCoord decoration before the types, and the declarations before the functions. */
		if (at == lower->types_at && lower->frag_coord_new) {
			module[written++] = (4U << 16) | INPUT_OP_DECORATE;
			module[written++] = lower->frag_coord;
			module[written++] = INPUT_DECORATION_BUILT_IN;
			module[written++] = INPUT_BUILT_IN_FRAG_COORD;
		}
		if (at == lower->functions_at)
			written += drv_gpu_input_lower_declarations(lower, &module[written]);

		/* The capability and the index decorations of input attachments go. */
		if (opcode == INPUT_OP_CAPABILITY && count == 2U && word[1] == INPUT_CAPABILITY)
			continue;
		if (opcode == INPUT_OP_DECORATE && count >= 3U && word[2] == INPUT_DECORATION_INDEX)
			continue;

		/* An input attachment type is a 2D sampled image of no format. */
		if (opcode == INPUT_OP_TYPE_IMAGE && count >= 9U && word[1] < lower->code[3] && lower->subpass[word[1]] != 0U) {
			for (index = 0U; index < count; index++)
				module[written + index] = word[index];
			module[written + 3U] = INPUT_DIM_2D;
			module[written + 7U] = 1U;
			module[written + 8U] = 0U;
			written += count;
			continue;
		}

		/* A read of one is the fetch at the pixel. */
		if (opcode == INPUT_OP_IMAGE_READ && count >= 5U && word[3] < lower->code[3]) {
			used = drv_gpu_input_lower_read(lower, word, count, &module[written]);
			if (used == 0U) {
				kern_free(module);
				return drv_gpu_input_lower_refuse(lower, opcode, at, "input attachment read at a coordinate that is not two signed integers, or with an operand that is not lowered");
			}
			written += used;
			continue;
		}

		/* A fragment entry point names the new input. */
		if (opcode == INPUT_OP_ENTRY_POINT) {
			written += drv_gpu_input_lower_entry_point(lower, word, count, &module[written]);
			continue;
		}

		/* Anything else as it is. */
		for (index = 0U; index < count; index++)
			module[written + index] = word[index];
		written += count;
	}

	/* A module with no function still gets its declarations. */
	if (lower->functions_at == lower->words)
		written += drv_gpu_input_lower_declarations(lower, &module[written]);

	/* Succeeded: the rewritten module is the caller's. */
	*out = module;
	*out_count = written;
	return 0;
}

/* Records a refused instruction for the caller's diagnostic and returns ENOTSUP. */
static int
drv_gpu_input_lower_refuse(
	struct drv_gpu_input_lower *lower,
	uint32_t opcode,
	uint32_t at,
	const char *reason)
{
	/* The instruction and the reason. */
	lower->diag.opcode = opcode;
	lower->diag.word_offset = at;
	lower->diag.reason = reason;

	/* Refused. */
	return ENOTSUP;
}
