/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of storage images to buffer accesses, included by spirv.c.
 *
 * A GPU whose shader instruction set has no image load, store or atomic
 * still reads and writes storage buffers.  This file rewrites a SPIR-V
 * module so that every storage image becomes two buffers of the same
 * descriptor set: its texels, as 32-bit words in a storage buffer at the
 * image's own binding, and the shape of the image view bound there, as a
 * uniform block of eight words at the binding with
 * DRV_GPU_SPIRV_IMAGE_INFO_BINDING set (spirv.h).  The rewritten module is
 * still SPIR-V, so every reader takes it -- the shared parser here and a
 * backend's own reader alike -- and nothing in it names a GPU.
 *
 * The shape words are, in order: the view's width, height, and number of
 * slices (a 3D image's depth, or the layers of an array or a cube, six to
 * a cube), the words from one row of texels to the next, and the words from
 * one slice to the next; the last three words are zero.  The texel buffer
 * starts at the first texel of the view's level and first layer, and the
 * texel at (x, y, z) starts at word z * slice + y * row + x * (texel bytes
 * / 4).  A 1D image's y is 0; the layer of an array is its z; a cube's face
 * is its z, and a cube array's layer * 6 + face.
 *
 * Each image instruction is replaced by one call of a function added at
 * the end of the module, so no block is split here: OpImageRead,
 * OpImageWrite, OpImageQuerySize, and the atomics on an OpImageTexelPointer
 * become calls whose arguments are the descriptor array's element (for an
 * array of images), the coordinate and the values.  The loads of the image
 * variables, the access chains to their elements and the texel pointers
 * leave no instruction.  A reader that does not inline calls inlines the
 * result with drv_gpu_spirv_inline() first.
 *
 * Each added function checks the coordinate against the view's extent.
 * A read outside it reads the texel at the origin and answers zeros; a
 * write and an atomic outside it do nothing, and the atomic answers zero.
 * That is what OpenGL ES 3.1 requires of an invalid texel and what Vulkan
 * requires with robustImageAccess; without it Vulkan leaves the access
 * undefined, so the stronger rule holds for both.
 *
 * The formats are the sixteen Vulkan requires storage images of.  Their
 * conversions follow Vulkan's texel input and output format conversions:
 * the normalized ones by GLSL.std.450's Pack and Unpack of 4x8, the half
 * floats by PackHalf2x16 and UnpackHalf2x16, the 32-bit ones bit for bit,
 * and the 8-bit and 16-bit integers by shifts, a write clamping the value
 * to the channel's range (Vulkan leaves an out-of-range integer undefined;
 * clamping is what Mesa's Intel lowering does).  Components the format
 * lacks read as (0, 0, 0, 1).  Atomics are only for the 32-bit integer
 * formats of one channel.
 *
 * What is refused (ENOTSUP, with the instruction as the diagnostic): an
 * image of a format outside the sixteen or of no format, a multisampled
 * one, an array of images of no fixed length, an image passed to or kept
 * in anything but the instructions above, a read or a write of something
 * other than a four-component texel, the Sample image operand, an atomic
 * on any other format, and a module that already names a binding with
 * DRV_GPU_SPIRV_IMAGE_INFO_BINDING set.
 *
 * Values referenced: Khronos SPIR-V 1.6 specification, sections 2.4
 * (logical layout of a module), 3.7 (Dim), 3.11 (Image Format), 3.20
 * (Decoration), 3.37 (instructions: OpTypeImage, OpImageRead,
 * OpImageWrite, OpImageQuerySize, OpImageTexelPointer and the atomics);
 * GLSL.std.450 1.0 (UMin 38, SClamp 45, PackSnorm4x8 54, PackUnorm4x8 55,
 * PackHalf2x16 58, UnpackHalf2x16 62, UnpackSnorm4x8 63, UnpackUnorm4x8
 * 64).
 *
 * Procedure referenced: Khronos Vulkan 1.3 specification, "Texel Input
 * Format Conversion", "Texel Output Format Conversion" and "Texel
 * Coordinate Validation"; Khronos OpenGL ES 3.1 specification, section
 * 8.22 (Texture Image Loads and Stores: an invalid texel reads zero, and a
 * store or an atomic to it has no effect).
 */

/* The opcodes, decorations and storage classes only the lowering reads. */
#define LOWER_OP_IMAGE_QUERY_SIZE 104U
#define LOWER_SC_IMAGE 11U
#define LOWER_IMAGE_OPERAND_SAMPLE 0x40U
#define LOWER_SCOPE_DEVICE 1U

/* The GLSL.std.450 instructions the lowering emits. */
#define LOWER_GLSL_UMIN 38U
#define LOWER_GLSL_SCLAMP 45U
#define LOWER_GLSL_PACK_SNORM_4X8 54U
#define LOWER_GLSL_PACK_UNORM_4X8 55U
#define LOWER_GLSL_PACK_HALF_2X16 58U
#define LOWER_GLSL_UNPACK_HALF_2X16 62U
#define LOWER_GLSL_UNPACK_SNORM_4X8 63U
#define LOWER_GLSL_UNPACK_UNORM_4X8 64U

/*
 * The words of the name SPV_KHR_storage_buffer_storage_class, which a
 * module before SPIR-V 1.3 declares to use the StorageBuffer class.
 */
#define LOWER_EXTENSION_WORDS 10U

/*
 * The bit of `declared_at` that marks an offset into the lowering's own
 * declarations rather than into the module.
 */
#define LOWER_DECLARED_NEW 0x80000000U

/* The SPIR-V version from which the entry point lists every global variable it uses. */
#define LOWER_VERSION_ALL_INTERFACE 0x00010400U

/* The SPIR-V version from which the StorageBuffer storage class needs no extension. */
#define LOWER_VERSION_STORAGE_BUFFER 0x00010300U

/* The most storage image variables and added functions one module may have. */
#define LOWER_MAX_IMAGES 32U
#define LOWER_MAX_HELPERS 96U

/* The words of a view's shape in the uniform block. */
#define LOWER_INFO_WORDS 8U
#define LOWER_INFO_WIDTH 0U
#define LOWER_INFO_HEIGHT 1U
#define LOWER_INFO_SLICES 2U
#define LOWER_INFO_ROW 3U
#define LOWER_INFO_SLICE 4U

/*
 * What an id of the module is to the lowering: nothing, a storage image
 * type, an array of them, a UniformConstant pointer to either, a storage
 * image variable, an element of an array of them (a pointer), an image
 * loaded from either, or a texel pointer.
 */
#define LOWER_ID_NONE 0U
#define LOWER_ID_IMAGE_TYPE 1U
#define LOWER_ID_IMAGE_ARRAY 2U
#define LOWER_ID_IMAGE_POINTER 3U
#define LOWER_ID_VARIABLE 4U
#define LOWER_ID_ELEMENT 5U
#define LOWER_ID_VALUE 6U
#define LOWER_ID_TEXEL 7U

/*
 * How a format's texels are converted: four normalized 8-bit channels,
 * four 8-bit or 16-bit integers, four half floats, or 32-bit channels
 * bit for bit.
 */
#define LOWER_PACK_UNORM8 1U
#define LOWER_PACK_SNORM8 2U
#define LOWER_PACK_UINT8 3U
#define LOWER_PACK_SINT8 4U
#define LOWER_PACK_UINT16 5U
#define LOWER_PACK_SINT16 6U
#define LOWER_PACK_HALF 7U
#define LOWER_PACK_WORDS 8U

/*
 * The kinds of number a channel is: a float, a signed or an unsigned
 * 32-bit integer.
 */
#define LOWER_NUMBER_FLOAT 0U
#define LOWER_NUMBER_INT 1U
#define LOWER_NUMBER_UINT 2U

/*
 * One storage image variable of the module and the buffers it becomes.
 *
 * The record lives as long as one lowering.  `variable` keeps its id and
 * becomes the texel buffer; the other ids are new.
 */
struct drv_gpu_image_lower_image {
	uint32_t variable;
	uint32_t info_variable;
	uint32_t set;
	uint32_t binding;

	/* The image type's Dim, Arrayed, Image Format and Sampled Type's number. */
	uint32_t dim;
	uint32_t arrayed;
	uint32_t format;
	uint32_t number;

	/* How the texels convert, how many channels and words a texel has. */
	uint32_t pack;
	uint32_t channels;
	uint32_t texel_words;

	/* The array's length constant (0 for a single image). */
	uint32_t length_id;

	/* The type of a texel word: an int for the 32-bit signed formats, else a uint. */
	uint32_t word_type;

	/* The pointers to one word of the texel buffer and of the uniform block. */
	uint32_t word_pointer;
	uint32_t info_word_pointer;
};

/*
 * One added function: the image, the instruction it stands for, and the
 * types (and for an atomic the scope and semantics ids) it was made for.
 */
struct drv_gpu_image_lower_helper {
	uint32_t image;
	uint32_t opcode;
	uint32_t result_type;
	uint32_t element_type;
	uint32_t coordinate_type;
	uint32_t value_type;
	uint32_t scope;
	uint32_t semantics;
	uint32_t semantics_unequal;
	uint32_t function;
};

/* The state of one module's lowering. */
struct drv_gpu_image_lower {
	/* The module's words, their count, its version and the next new id. */
	const uint32_t *code;
	uint32_t words;
	uint32_t version;
	uint32_t next_id;

	/*
	 * What each id is (LOWER_ID_*), the image record it belongs to, the
	 * element id of an element, value or texel pointer (0 for a single
	 * image), a texel pointer's coordinate, each id's result type, and
	 * where a type or constant id is declared (0 for none).
	 */
	uint8_t *kind;
	uint32_t *image_of;
	uint32_t *element_of;
	uint32_t *coordinate_of;
	uint32_t *type_of;
	uint32_t *declared_at;

	/* Each id's DescriptorSet and Binding, plus one (0 for none). */
	uint32_t *set_of;
	uint32_t *binding_of;

	/* The storage images and the added functions. */
	struct drv_gpu_image_lower_image images[LOWER_MAX_IMAGES];
	uint32_t image_count;
	struct drv_gpu_image_lower_helper helpers[LOWER_MAX_HELPERS];
	uint32_t helper_count;

	/*
	 * Where the module's sections are: past its last decoration (where the
	 * new ones go), its first type or global, and its first function.
	 */
	uint32_t annotations_end;
	uint32_t types_begin;
	uint32_t functions_begin;

	/* The GLSL.std.450 import, and whether it and the extension are new. */
	uint32_t glsl;
	int glsl_new;
	int extension_new;

	/* The new declarations, decorations, rewritten functions and added functions. */
	struct drv_gpu_spirv_words declarations;
	struct drv_gpu_spirv_words annotations;
	struct drv_gpu_spirv_words bodies;
	struct drv_gpu_spirv_words helper_words;

	/* An allocation failure on the way (ENOMEM), and the refused instruction. */
	int error;
	struct drv_gpu_compile_diagnostic diag;
};

/*
 * The name of the extension the StorageBuffer class needs before SPIR-V
 * 1.3, as the words of an OpExtension; constant for the life of the kernel.
 */
static const uint32_t drv_gpu_image_lower_extension[LOWER_EXTENSION_WORDS] = {
	0x5f565053U, 0x5f52484bU, 0x726f7473U, 0x5f656761U, 0x66667562U,
	0x735f7265U, 0x61726f74U, 0x635f6567U, 0x7373616cU, 0x00000000U
};

static int drv_gpu_image_lower_scan(struct drv_gpu_image_lower *lower);
static const uint32_t *drv_gpu_image_lower_declaration(const struct drv_gpu_image_lower *lower, uint32_t id);
static int drv_gpu_image_lower_has_extension(const struct drv_gpu_image_lower *lower);
static int drv_gpu_image_lower_variable(struct drv_gpu_image_lower *lower, const uint32_t *word, uint32_t offset);
static int drv_gpu_image_lower_format(struct drv_gpu_image_lower_image *image);
static void drv_gpu_image_lower_buffers(struct drv_gpu_image_lower *lower, struct drv_gpu_image_lower_image *image);
static int drv_gpu_image_lower_bodies(struct drv_gpu_image_lower *lower);
static int drv_gpu_image_lower_instruction(struct drv_gpu_image_lower *lower, const uint32_t *word, uint32_t count, uint32_t offset);
static int drv_gpu_image_lower_call(struct drv_gpu_image_lower *lower, const uint32_t *word, uint32_t count, uint32_t offset, uint32_t image_id);
static int drv_gpu_image_lower_atomic(struct drv_gpu_image_lower *lower, const uint32_t *word, uint32_t count, uint32_t offset);
static int drv_gpu_image_lower_uses(struct drv_gpu_image_lower *lower, const uint32_t *word, uint32_t count, uint32_t offset);
static uint32_t drv_gpu_image_lower_helper(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_helper *key);
static void drv_gpu_image_lower_emit_read(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_helper *helper, uint32_t element, uint32_t coordinate);
static void drv_gpu_image_lower_emit_write(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_helper *helper, uint32_t element, uint32_t coordinate, uint32_t texel);
static void drv_gpu_image_lower_emit_size(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_helper *helper, uint32_t element);
static void drv_gpu_image_lower_emit_atomic(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_helper *helper, uint32_t element, uint32_t coordinate, uint32_t value, uint32_t comparator);
static void drv_gpu_image_lower_address(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_image *image, uint32_t element, uint32_t coordinate, uint32_t coordinate_type, uint32_t *inside, uint32_t *index);
static uint32_t drv_gpu_image_lower_info(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_image *image, uint32_t element, uint32_t word);
static uint32_t drv_gpu_image_lower_texel_word(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_image *image, uint32_t element, uint32_t index, uint32_t word);
static void drv_gpu_image_lower_unpack(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_image *image, const uint32_t *words, uint32_t *channels);
static void drv_gpu_image_lower_pack(struct drv_gpu_image_lower *lower, const struct drv_gpu_image_lower_image *image, uint32_t texel, uint32_t texel_type, uint32_t *words);
static uint32_t drv_gpu_image_lower_bits(struct drv_gpu_image_lower *lower, uint32_t word, uint32_t shift, uint32_t width);
static uint32_t drv_gpu_image_lower_extend(struct drv_gpu_image_lower *lower, uint32_t word, uint32_t left, uint32_t right);
static uint32_t drv_gpu_image_lower_join(struct drv_gpu_image_lower *lower, const uint32_t *parts, uint32_t count, uint32_t shift);
static uint32_t drv_gpu_image_lower_cast(struct drv_gpu_image_lower *lower, uint32_t wanted, uint32_t id, uint32_t have);
static uint32_t drv_gpu_image_lower_component(struct drv_gpu_image_lower *lower, uint32_t vector, uint32_t vector_type, uint32_t index);
static uint32_t drv_gpu_image_lower_scalar(struct drv_gpu_image_lower *lower, uint32_t number);
static uint32_t drv_gpu_image_lower_number(const struct drv_gpu_image_lower *lower, uint32_t type);
static uint32_t drv_gpu_image_lower_vector_size(const struct drv_gpu_image_lower *lower, uint32_t type);
static uint32_t drv_gpu_image_lower_vector_of(const struct drv_gpu_image_lower *lower, uint32_t type);
static uint32_t drv_gpu_image_lower_type(struct drv_gpu_image_lower *lower, uint32_t opcode, const uint32_t *operands, uint32_t count);
static uint32_t drv_gpu_image_lower_constant(struct drv_gpu_image_lower *lower, uint32_t type, uint32_t value);
static uint32_t drv_gpu_image_lower_uint(struct drv_gpu_image_lower *lower, uint32_t value);
static uint32_t drv_gpu_image_lower_op(struct drv_gpu_image_lower *lower, uint32_t opcode, uint32_t type, const uint32_t *operands, uint32_t count);
static uint32_t drv_gpu_image_lower_op1(struct drv_gpu_image_lower *lower, uint32_t opcode, uint32_t type, uint32_t a);
static uint32_t drv_gpu_image_lower_op2(struct drv_gpu_image_lower *lower, uint32_t opcode, uint32_t type, uint32_t a, uint32_t b);
static uint32_t drv_gpu_image_lower_ext(struct drv_gpu_image_lower *lower, uint32_t type, uint32_t instruction, const uint32_t *operands, uint32_t count);
static void drv_gpu_image_lower_put(struct drv_gpu_image_lower *lower, struct drv_gpu_spirv_words *list, const uint32_t *word, uint32_t count);
static void drv_gpu_image_lower_decorate(struct drv_gpu_image_lower *lower, uint32_t id, uint32_t decoration, uint32_t value, int has_value);
static void drv_gpu_image_lower_member_offset(struct drv_gpu_image_lower *lower, uint32_t id, uint32_t member, uint32_t offset);
static int drv_gpu_image_lower_assemble(struct drv_gpu_image_lower *lower, uint32_t **out, size_t *out_count);
static int drv_gpu_image_lower_global(struct drv_gpu_image_lower *lower, struct drv_gpu_spirv_words *module, const uint32_t *word, uint32_t count);
static int drv_gpu_image_lower_dropped(const struct drv_gpu_image_lower *lower, uint32_t id);
static int drv_gpu_image_lower_refuse(struct drv_gpu_image_lower *lower, uint32_t opcode, uint32_t offset, const char *reason);
static void drv_gpu_image_lower_release(struct drv_gpu_image_lower *lower);

/*
 * Rewrites a module's storage images into storage buffers of their texels
 * and uniform blocks of their views' shapes.
 *
 * Returns 0 with the rewritten module in *out (allocated with kern_malloc;
 * the caller frees it with kern_free), or 0 with *out NULL for a module
 * without a storage image or a header the parse refuses itself; EINVAL for
 * a malformed module, ENOTSUP for one the lowering does not rewrite (with
 * the diagnostic), or ENOMEM.
 */
int
drv_gpu_spirv_lower_storage_images(
	const uint32_t *words,
	size_t word_count,
	uint32_t **out,
	size_t *out_count,
	struct drv_gpu_compile_diagnostic *diagnostic)
{
	struct drv_gpu_image_lower *lower;
	uint32_t index;
	int error;

	/* Nothing is rewritten unless the module has a storage image. */
	if (out == NULL || out_count == NULL)
		return EINVAL;
	*out = NULL;
	*out_count = 0U;
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A header the parse refuses is left to it. */
	if (words == NULL ||
	    word_count < SPIRV_HEADER_WORDS ||
	    word_count > UINT32_MAX ||
	    words[0] != SPIRV_MAGIC ||
	    words[3] == 0U ||
	    words[3] > SPIRV_MAX_BOUND)
		return 0;

	/* Allocates the lowering's state. */
	lower = kern_calloc(1U, sizeof(*lower));
	if (lower == NULL)
		return ENOMEM;

	/* Prepares the module's view and the tables by id. */
	lower->code = words;
	lower->words = (uint32_t)word_count;
	lower->version = words[1];
	lower->next_id = words[3];
	lower->kind = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->kind));
	lower->image_of = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->image_of));
	lower->element_of = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->element_of));
	lower->coordinate_of = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->coordinate_of));
	lower->type_of = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->type_of));
	lower->declared_at = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->declared_at));
	lower->set_of = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->set_of));
	lower->binding_of = kern_calloc(SPIRV_MAX_BOUND, sizeof(*lower->binding_of));
	if (lower->kind == NULL ||
	    lower->image_of == NULL ||
	    lower->element_of == NULL ||
	    lower->coordinate_of == NULL ||
	    lower->type_of == NULL ||
	    lower->declared_at == NULL ||
	    lower->set_of == NULL ||
	    lower->binding_of == NULL) {
		drv_gpu_image_lower_release(lower);
		return ENOMEM;
	}

	/* Finds the storage images and what the rewrite needs of the module. */
	error = drv_gpu_image_lower_scan(lower);

	/* A module without a storage image is parsed as it is. */
	if (error == 0 && lower->image_count == 0U) {
		drv_gpu_image_lower_release(lower);
		return 0;
	}

	/* Declares each image's buffers. */
	for (index = 0U; error == 0 && index < lower->image_count; index++) {
		drv_gpu_image_lower_buffers(lower, &lower->images[index]);
		error = lower->error;
	}

	/* Rewrites the functions, adding a function for each kind of image access. */
	if (error == 0)
		error = drv_gpu_image_lower_bodies(lower);

	/* Writes the rewritten module. */
	if (error == 0)
		error = drv_gpu_image_lower_assemble(lower, out, out_count);

	/* A refusal names the instruction for the caller. */
	if (error == ENOTSUP && diagnostic != NULL)
		*diagnostic = lower->diag;

	/* The tables are only needed while rewriting. */
	drv_gpu_image_lower_release(lower);

	/* A failure leaves nothing for the caller. */
	if (error != 0)
		return error;

	/* Succeeded: *out holds the rewritten module. */
	return 0;
}

/*
 * Reads the module's sections: the types, constants, decorations and
 * imports the rewrite reuses, where the sections end, and every storage
 * image variable.
 */
static int
drv_gpu_image_lower_scan(
	struct drv_gpu_image_lower *lower)
{
	const uint32_t *word;
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	uint32_t result;
	uint32_t pointer_type;
	uint32_t pointee;
	int error;

	/* Walks every instruction of the module. */
	offset = SPIRV_HEADER_WORDS;
	while (offset < lower->words) {
		word = lower->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;
		if (count == 0U || offset + count > lower->words)
			return EINVAL;

		/* The first function ends the module's global sections. */
		if (opcode == OP_FUNCTION && lower->functions_begin == 0U)
			lower->functions_begin = offset;

		/* Every typed instruction's result type, by its result id: a function's, a constant's or a variable's. */
		result = drv_gpu_spirv_inline_result(opcode);
		if (lower->functions_begin != 0U &&
		    count >= 3U &&
		    word[2] < SPIRV_MAX_BOUND &&
		    result == INLINE_RESULT_TYPED)
			lower->type_of[word[2]] = word[1];
		if (lower->functions_begin == 0U &&
		    count >= 3U &&
		    word[2] < SPIRV_MAX_BOUND &&
		    ((opcode >= OP_CONSTANT_TRUE && opcode <= 52U) || opcode == OP_VARIABLE || opcode == OP_UNDEF))
			lower->type_of[word[2]] = word[1];

		/* The global sections only: functions are rewritten later. */
		if (lower->functions_begin != 0U) {
			offset += count;
			continue;
		}

		/* The decorations' section ends after the last one (SPIR-V 1.6, 2.4). */
		if (opcode == OP_DECORATE ||
		    opcode == OP_MEMBER_DECORATE ||
		    opcode == OP_DECORATE_ID ||
		    opcode == OP_DECORATE_STRING)
			lower->annotations_end = offset + count;

		/* A type, a constant or a global variable starts the types' section. */
		if (lower->types_begin == 0U &&
		    ((opcode >= OP_TYPE_VOID && opcode <= OP_TYPE_FUNCTION) ||
		     (opcode >= OP_CONSTANT_TRUE && opcode <= 52U) ||
		     opcode == OP_VARIABLE ||
		     opcode == OP_UNDEF))
			lower->types_begin = offset;

		/* The descriptor set and binding of each decorated id. */
		if (opcode == OP_DECORATE && count >= 4U && word[1] < SPIRV_MAX_BOUND) {
			if (word[2] == DEC_DESCRIPTOR_SET)
				lower->set_of[word[1]] = word[3] + 1U;
			if (word[2] == DEC_BINDING) {
				/* A binding that already names an image's shape would collide with one. */
				if ((word[3] & DRV_GPU_SPIRV_IMAGE_INFO_BINDING) != 0U) {
					error = drv_gpu_image_lower_refuse(lower, opcode, offset, "a binding with the bit of a storage image's shape");
					return error;
				}
				lower->binding_of[word[1]] = word[3] + 1U;
			}
		}

		/* The GLSL.std.450 import the rewrite reuses. */
		if (opcode == OP_EXT_INST_IMPORT &&
		    count >= 5U &&
		    word[2] == 0x4c534c47U &&
		    word[3] == 0x6474732eU &&
		    word[4] == 0x3035342eU)
			lower->glsl = word[1];

		/* Types and constants are found again by their declarations. */
		if (opcode >= OP_TYPE_VOID && opcode <= OP_TYPE_FUNCTION && count >= 2U && word[1] < SPIRV_MAX_BOUND)
			lower->declared_at[word[1]] = offset;
		if (opcode == OP_CONSTANT && count >= 4U && word[2] < SPIRV_MAX_BOUND)
			lower->declared_at[word[2]] = offset;

		/* A storage image type of a Dim the lowering addresses. */
		if (opcode == OP_TYPE_IMAGE &&
		    count >= 9U &&
		    word[7] == 2U &&
		    (word[3] == DIM_1D || word[3] == DIM_2D || word[3] == DIM_3D || word[3] == DIM_CUBE))
			lower->kind[word[1]] = LOWER_ID_IMAGE_TYPE;

		/* An array of them, of a fixed or a run-time length. */
		if ((opcode == OP_TYPE_ARRAY || opcode == OP_TYPE_RUNTIME_ARRAY) &&
		    count >= 3U &&
		    word[2] < SPIRV_MAX_BOUND &&
		    lower->kind[word[2]] == LOWER_ID_IMAGE_TYPE)
			lower->kind[word[1]] = LOWER_ID_IMAGE_ARRAY;

		/* A UniformConstant pointer to either. */
		if (opcode == OP_TYPE_POINTER && count >= 4U && word[3] < SPIRV_MAX_BOUND && word[2] == SC_UNIFORM_CONSTANT) {
			pointee = word[3];
			if (lower->kind[pointee] == LOWER_ID_IMAGE_TYPE || lower->kind[pointee] == LOWER_ID_IMAGE_ARRAY)
				lower->kind[word[1]] = LOWER_ID_IMAGE_POINTER;
		}

		/* A variable of such a pointer is a storage image of the module. */
		if (opcode == OP_VARIABLE && count >= 4U && word[1] < SPIRV_MAX_BOUND) {
			pointer_type = word[1];
			if (lower->kind[pointer_type] == LOWER_ID_IMAGE_POINTER) {
				error = drv_gpu_image_lower_variable(lower, word, offset);
				if (error != 0)
					return error;
			}
		}

		/* Advances to the next instruction. */
		offset += count;
	}

	/* A module without a function, or without decorations or types, has its sections where they would be. */
	if (lower->functions_begin == 0U)
		lower->functions_begin = lower->words;
	if (lower->types_begin == 0U)
		lower->types_begin = lower->functions_begin;
	if (lower->annotations_end == 0U)
		lower->annotations_end = lower->types_begin;

	/* Succeeded: the module is read. */
	return 0;
}

/* Records one storage image variable: its image type, its descriptor and its format. */
static int
drv_gpu_image_lower_variable(
	struct drv_gpu_image_lower *lower,
	const uint32_t *word,
	uint32_t offset)
{
	struct drv_gpu_image_lower_image *image;
	const uint32_t *pointer;
	const uint32_t *type;
	const uint32_t *array;
	uint32_t pointee;
	uint32_t id;
	int error;

	/* Refuses more images than the lowering keeps. */
	if (lower->image_count >= LOWER_MAX_IMAGES) {
		error = drv_gpu_image_lower_refuse(lower, OP_VARIABLE, offset, "more storage images than the lowering takes");
		return error;
	}

	/* The variable's pointer type, and the image type or the array it points to. */
	id = word[2];
	pointer = drv_gpu_image_lower_declaration(lower, word[1]);
	pointee = pointer[3];
	type = drv_gpu_image_lower_declaration(lower, pointee);
	array = NULL;
	if (lower->kind[pointee] == LOWER_ID_IMAGE_ARRAY) {
		array = type;
		type = drv_gpu_image_lower_declaration(lower, array[2]);
	}

	/* A type the module does not declare is malformed. */
	if (type == NULL)
		return EINVAL;

	/* Refuses an array of no fixed length (descriptor indexing). */
	if (array != NULL && (array[0] & 0xFFFFU) != OP_TYPE_ARRAY) {
		error = drv_gpu_image_lower_refuse(lower, OP_VARIABLE, offset, "a storage image array of a run-time length");
		return error;
	}

	/* Refuses a variable outside UniformConstant, and one without its descriptor. */
	if (word[3] != SC_UNIFORM_CONSTANT || lower->set_of[id] == 0U || lower->binding_of[id] == 0U) {
		error = drv_gpu_image_lower_refuse(lower, OP_VARIABLE, offset, "a storage image without a descriptor set and binding");
		return error;
	}

	/* Refuses a multisampled storage image (shaderStorageImageMultisample is not offered). */
	if (type[6] != 0U) {
		error = drv_gpu_image_lower_refuse(lower, OP_VARIABLE, offset, "a multisampled storage image");
		return error;
	}

	/* Records the image. */
	image = &lower->images[lower->image_count];
	kern_memset(image, 0, sizeof(*image));
	image->variable = id;
	image->set = lower->set_of[id] - 1U;
	image->binding = lower->binding_of[id] - 1U;
	image->dim = type[3];
	image->arrayed = type[5];
	image->format = type[8];
	image->number = drv_gpu_image_lower_number(lower, type[2]);
	if (array != NULL)
		image->length_id = array[3];

	/* Refuses a format outside the sixteen, and a Sampled Type that does not match it. */
	error = drv_gpu_image_lower_format(image);
	if (error != 0) {
		error = drv_gpu_image_lower_refuse(lower, OP_VARIABLE, offset, "a storage image of a format the lowering does not convert");
		return error;
	}

	/* The variable is the image's from now on. */
	lower->kind[id] = LOWER_ID_VARIABLE;
	lower->image_of[id] = lower->image_count;
	lower->image_count++;

	/* Succeeded: the image is recorded. */
	return 0;
}

/*
 * Chooses how an image's texels convert from its Image Format (SPIR-V
 * 1.6, 3.11), and checks its Sampled Type against it; EINVAL for another.
 */
static int
drv_gpu_image_lower_format(
	struct drv_gpu_image_lower_image *image)
{
	uint32_t number;

	/* Each of the sixteen formats: its conversion, channels, words and channels' number. */
	image->channels = 4U;
	image->texel_words = 1U;
	switch (image->format) {
	case 1U:
		/* Rgba32f. */
		image->pack = LOWER_PACK_WORDS;
		image->texel_words = 4U;
		number = LOWER_NUMBER_FLOAT;
		break;
	case 2U:
		/* Rgba16f. */
		image->pack = LOWER_PACK_HALF;
		image->texel_words = 2U;
		number = LOWER_NUMBER_FLOAT;
		break;
	case 3U:
		/* R32f. */
		image->pack = LOWER_PACK_WORDS;
		image->channels = 1U;
		number = LOWER_NUMBER_FLOAT;
		break;
	case 4U:
		/* Rgba8. */
		image->pack = LOWER_PACK_UNORM8;
		number = LOWER_NUMBER_FLOAT;
		break;
	case 5U:
		/* Rgba8Snorm. */
		image->pack = LOWER_PACK_SNORM8;
		number = LOWER_NUMBER_FLOAT;
		break;
	case 6U:
		/* Rg32f. */
		image->pack = LOWER_PACK_WORDS;
		image->channels = 2U;
		image->texel_words = 2U;
		number = LOWER_NUMBER_FLOAT;
		break;
	case 21U:
		/* Rgba32i. */
		image->pack = LOWER_PACK_WORDS;
		image->texel_words = 4U;
		number = LOWER_NUMBER_INT;
		break;
	case 22U:
		/* Rgba16i. */
		image->pack = LOWER_PACK_SINT16;
		image->texel_words = 2U;
		number = LOWER_NUMBER_INT;
		break;
	case 23U:
		/* Rgba8i. */
		image->pack = LOWER_PACK_SINT8;
		number = LOWER_NUMBER_INT;
		break;
	case 24U:
		/* R32i. */
		image->pack = LOWER_PACK_WORDS;
		image->channels = 1U;
		number = LOWER_NUMBER_INT;
		break;
	case 25U:
		/* Rg32i. */
		image->pack = LOWER_PACK_WORDS;
		image->channels = 2U;
		image->texel_words = 2U;
		number = LOWER_NUMBER_INT;
		break;
	case 30U:
		/* Rgba32ui. */
		image->pack = LOWER_PACK_WORDS;
		image->texel_words = 4U;
		number = LOWER_NUMBER_UINT;
		break;
	case 31U:
		/* Rgba16ui. */
		image->pack = LOWER_PACK_UINT16;
		image->texel_words = 2U;
		number = LOWER_NUMBER_UINT;
		break;
	case 32U:
		/* Rgba8ui. */
		image->pack = LOWER_PACK_UINT8;
		number = LOWER_NUMBER_UINT;
		break;
	case 33U:
		/* R32ui. */
		image->pack = LOWER_PACK_WORDS;
		image->channels = 1U;
		number = LOWER_NUMBER_UINT;
		break;
	case 35U:
		/* Rg32ui. */
		image->pack = LOWER_PACK_WORDS;
		image->channels = 2U;
		image->texel_words = 2U;
		number = LOWER_NUMBER_UINT;
		break;
	default:
		return EINVAL;
	}

	/* A float format needs a float Sampled Type, an integer one an integer type. */
	if (number == LOWER_NUMBER_FLOAT && image->number != LOWER_NUMBER_FLOAT)
		return EINVAL;
	if (number != LOWER_NUMBER_FLOAT && image->number == LOWER_NUMBER_FLOAT)
		return EINVAL;

	/* The format decides the integers' signedness, whatever the Sampled Type's. */
	image->number = number;

	/* Succeeded: the conversion is chosen. */
	return 0;
}

/*
 * Declares an image's two buffers: the storage buffer of its texel words
 * (the variable keeps its id) and the uniform block of its view's shape,
 * with their decorations.
 */
static void
drv_gpu_image_lower_buffers(
	struct drv_gpu_image_lower *lower,
	struct drv_gpu_image_lower_image *image)
{
	uint32_t operands[LOWER_INFO_WORDS + 2U];
	uint32_t uint_type;
	uint32_t runtime;
	uint32_t texel_struct;
	uint32_t texel_type;
	uint32_t texel_pointer;
	uint32_t info_struct;
	uint32_t info_type;
	uint32_t info_pointer;
	uint32_t variable[4];
	uint32_t index;

	/* The word type: an int for a signed 32-bit format, so its atomics are signed. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	image->word_type = uint_type;
	if (image->pack == LOWER_PACK_WORDS && image->number == LOWER_NUMBER_INT)
		image->word_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_INT);

	/* The texels: a block of a run-time array of words, 4 bytes apart. */
	runtime = lower->next_id;
	lower->next_id++;
	operands[0] = (OP_TYPE_RUNTIME_ARRAY) | (3U << 16);
	operands[1] = runtime;
	operands[2] = image->word_type;
	drv_gpu_image_lower_put(lower, &lower->declarations, operands, 3U);
	drv_gpu_image_lower_decorate(lower, runtime, DEC_ARRAY_STRIDE, 4U, 1);
	texel_struct = lower->next_id;
	lower->next_id++;
	operands[0] = (OP_TYPE_STRUCT) | (3U << 16);
	operands[1] = texel_struct;
	operands[2] = runtime;
	drv_gpu_image_lower_put(lower, &lower->declarations, operands, 3U);
	drv_gpu_image_lower_decorate(lower, texel_struct, DEC_BLOCK, 0U, 0);
	drv_gpu_image_lower_member_offset(lower, texel_struct, 0U, 0U);

	/* The view's shape: a block of eight words at offsets 0 to 28. */
	info_struct = lower->next_id;
	lower->next_id++;
	operands[0] = (OP_TYPE_STRUCT) | ((2U + LOWER_INFO_WORDS) << 16);
	operands[1] = info_struct;
	for (index = 0U; index < LOWER_INFO_WORDS; index++)
		operands[2U + index] = uint_type;
	drv_gpu_image_lower_put(lower, &lower->declarations, operands, 2U + LOWER_INFO_WORDS);
	drv_gpu_image_lower_decorate(lower, info_struct, DEC_BLOCK, 0U, 0);
	for (index = 0U; index < LOWER_INFO_WORDS; index++)
		drv_gpu_image_lower_member_offset(lower, info_struct, index, index * 4U);

	/* An array of images is an array of each block, of the same length. */
	texel_type = texel_struct;
	info_type = info_struct;
	if (image->length_id != 0U) {
		texel_type = lower->next_id;
		lower->next_id++;
		operands[0] = (OP_TYPE_ARRAY) | (4U << 16);
		operands[1] = texel_type;
		operands[2] = texel_struct;
		operands[3] = image->length_id;
		drv_gpu_image_lower_put(lower, &lower->declarations, operands, 4U);
		info_type = lower->next_id;
		lower->next_id++;
		operands[1] = info_type;
		operands[2] = info_struct;
		drv_gpu_image_lower_put(lower, &lower->declarations, operands, 4U);
	}

	/* The pointers to the blocks and to one word of each. */
	operands[0] = SC_STORAGE_BUFFER;
	operands[1] = texel_type;
	texel_pointer = drv_gpu_image_lower_type(lower, OP_TYPE_POINTER, operands, 2U);
	operands[1] = image->word_type;
	image->word_pointer = drv_gpu_image_lower_type(lower, OP_TYPE_POINTER, operands, 2U);
	operands[0] = SC_UNIFORM;
	operands[1] = info_type;
	info_pointer = drv_gpu_image_lower_type(lower, OP_TYPE_POINTER, operands, 2U);
	operands[1] = uint_type;
	image->info_word_pointer = drv_gpu_image_lower_type(lower, OP_TYPE_POINTER, operands, 2U);

	/* The texel buffer keeps the image variable's id, set and binding. */
	variable[0] = (OP_VARIABLE) | (4U << 16);
	variable[1] = texel_pointer;
	variable[2] = image->variable;
	variable[3] = SC_STORAGE_BUFFER;
	drv_gpu_image_lower_put(lower, &lower->declarations, variable, 4U);

	/* The shape's block is a new variable of the same set, at the binding with the shape's bit. */
	image->info_variable = lower->next_id;
	lower->next_id++;
	variable[1] = info_pointer;
	variable[2] = image->info_variable;
	variable[3] = SC_UNIFORM;
	drv_gpu_image_lower_put(lower, &lower->declarations, variable, 4U);
	drv_gpu_image_lower_decorate(lower, image->info_variable, DEC_DESCRIPTOR_SET, image->set, 1);
	drv_gpu_image_lower_decorate(lower, image->info_variable, DEC_BINDING, image->binding | DRV_GPU_SPIRV_IMAGE_INFO_BINDING, 1);
}

/*
 * Rewrites every function of the module into the bodies list: the image
 * accesses become calls, the rest is copied.
 */
static int
drv_gpu_image_lower_bodies(
	struct drv_gpu_image_lower *lower)
{
	const uint32_t *word;
	uint32_t offset;
	uint32_t count;
	int error;

	/* Walks every instruction from the first function on. */
	offset = lower->functions_begin;
	while (offset < lower->words) {
		word = lower->code + offset;
		count = word[0] >> 16;

		/* Rewrites or copies the instruction. */
		error = drv_gpu_image_lower_instruction(lower, word, count, offset);
		if (error != 0)
			return error;

		/* Refuses a module the added functions ran out of memory for. */
		if (lower->error != 0)
			return lower->error;

		/* Advances to the next instruction. */
		offset += count;
	}

	/* Succeeded: the functions are rewritten. */
	return 0;
}

/* Rewrites one instruction of a function body. */
static int
drv_gpu_image_lower_instruction(
	struct drv_gpu_image_lower *lower,
	const uint32_t *word,
	uint32_t count,
	uint32_t offset)
{
	uint32_t opcode;
	uint32_t base;
	uint32_t result;
	int error;

	/* Dispatches by the instruction. */
	opcode = word[0] & 0xFFFFU;
	switch (opcode) {
	case OP_LOAD:
	case OP_COPY_OBJECT:
		/* A load of an image variable or element is the image itself: it leaves no instruction. */
		if (count < 4U || word[3] >= SPIRV_MAX_BOUND || word[2] >= SPIRV_MAX_BOUND)
			return EINVAL;
		base = word[3];
		result = word[2];
		if (lower->kind[base] == LOWER_ID_VARIABLE || lower->kind[base] == LOWER_ID_ELEMENT) {
			if (opcode == OP_COPY_OBJECT) {
				/* Records the refusal in the caller's diagnostic. */
				return drv_gpu_image_lower_refuse(lower, opcode, offset, "a copy of a storage image's pointer");
			}
			lower->kind[result] = LOWER_ID_VALUE;
			lower->image_of[result] = lower->image_of[base];
			lower->element_of[result] = lower->element_of[base];
			return 0;
		}
		if (lower->kind[base] == LOWER_ID_VALUE) {
			lower->kind[result] = LOWER_ID_VALUE;
			lower->image_of[result] = lower->image_of[base];
			lower->element_of[result] = lower->element_of[base];
			return 0;
		}
		break;
	case OP_ACCESS_CHAIN:
		/* An element of an array of images: the element's index is kept for the calls. */
		if (count < 4U || word[3] >= SPIRV_MAX_BOUND || word[2] >= SPIRV_MAX_BOUND)
			return EINVAL;
		base = word[3];
		if (lower->kind[base] == LOWER_ID_VARIABLE) {
			if (count != 5U || lower->images[lower->image_of[base]].length_id == 0U) {
				/* Records the refusal in the caller's diagnostic. */
				return drv_gpu_image_lower_refuse(lower, opcode, offset, "an access chain into a storage image that is not one element");
			}
			result = word[2];
			lower->kind[result] = LOWER_ID_ELEMENT;
			lower->image_of[result] = lower->image_of[base];
			lower->element_of[result] = word[4];
			return 0;
		}
		break;
	case OP_IMAGE_TEXEL_POINTER:
		/* A texel pointer leaves no instruction: its atomics become calls with its coordinate. */
		if (count < 6U || word[3] >= SPIRV_MAX_BOUND || word[2] >= SPIRV_MAX_BOUND)
			return EINVAL;
		base = word[3];
		if (lower->kind[base] == LOWER_ID_VARIABLE || lower->kind[base] == LOWER_ID_ELEMENT) {
			result = word[2];
			lower->kind[result] = LOWER_ID_TEXEL;
			lower->image_of[result] = lower->image_of[base];
			lower->element_of[result] = lower->element_of[base];
			lower->coordinate_of[result] = word[4];
			return 0;
		}
		break;
	case OP_IMAGE_READ:
	case LOWER_OP_IMAGE_QUERY_SIZE:
		/* A read or a size of a lowered image is a call. */
		if (count < 4U || word[3] >= SPIRV_MAX_BOUND)
			return EINVAL;
		if (lower->kind[word[3]] == LOWER_ID_VALUE) {
			error = drv_gpu_image_lower_call(lower, word, count, offset, word[3]);
			return error;
		}
		break;
	case OP_IMAGE_WRITE:
		/* A write to a lowered image is a call. */
		if (count < 4U || word[1] >= SPIRV_MAX_BOUND)
			return EINVAL;
		if (lower->kind[word[1]] == LOWER_ID_VALUE) {
			error = drv_gpu_image_lower_call(lower, word, count, offset, word[1]);
			return error;
		}
		break;
	case OP_ATOMIC_LOAD:
	case OP_ATOMIC_EXCHANGE:
	case OP_ATOMIC_COMPARE_EXCHANGE:
	case OP_ATOMIC_I_INCREMENT:
	case OP_ATOMIC_I_DECREMENT:
	case OP_ATOMIC_I_ADD:
	case OP_ATOMIC_I_SUB:
	case OP_ATOMIC_S_MIN:
	case OP_ATOMIC_U_MIN:
	case OP_ATOMIC_S_MAX:
	case OP_ATOMIC_U_MAX:
	case OP_ATOMIC_AND:
	case OP_ATOMIC_OR:
	case OP_ATOMIC_XOR:
		/* An atomic on a texel pointer is a call. */
		if (count < 4U || word[3] >= SPIRV_MAX_BOUND)
			return EINVAL;
		if (lower->kind[word[3]] == LOWER_ID_TEXEL) {
			error = drv_gpu_image_lower_atomic(lower, word, count, offset);
			return error;
		}
		break;
	case OP_ATOMIC_STORE:
		/* An atomic store to a texel pointer is a call. */
		if (count < 2U || word[1] >= SPIRV_MAX_BOUND)
			return EINVAL;
		if (lower->kind[word[1]] == LOWER_ID_TEXEL) {
			error = drv_gpu_image_lower_atomic(lower, word, count, offset);
			return error;
		}
		break;
	case OP_FUNCTION_PARAMETER:
		/* A function taking an image is not rewritten. */
		if (count < 3U || word[1] >= SPIRV_MAX_BOUND)
			return EINVAL;
		if (lower->kind[word[1]] != LOWER_ID_NONE) {
			/* Records the refusal in the caller's diagnostic. */
			return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image passed to a function");
		}
		break;
	default:
		break;
	}

	/* Any other instruction must not name a lowered image. */
	error = drv_gpu_image_lower_uses(lower, word, count, offset);
	if (error != 0)
		return error;

	/* Copies the instruction as it is. */
	drv_gpu_image_lower_put(lower, &lower->bodies, word, count);

	/* Succeeded: the instruction is copied. */
	return 0;
}

/*
 * Replaces OpImageRead, OpImageWrite or OpImageQuerySize of a lowered
 * image with a call of the function made for it.
 */
static int
drv_gpu_image_lower_call(
	struct drv_gpu_image_lower *lower,
	const uint32_t *word,
	uint32_t count,
	uint32_t offset,
	uint32_t image_id)
{
	struct drv_gpu_image_lower_helper key;
	const struct drv_gpu_image_lower_image *image;
	uint32_t call[8];
	uint32_t opcode;
	uint32_t element;
	uint32_t function;
	uint32_t length;
	uint32_t vector;
	uint32_t number;

	/* The image, its element and the instruction. */
	opcode = word[0] & 0xFFFFU;
	image = &lower->images[lower->image_of[image_id]];
	element = lower->element_of[image_id];
	kern_memset(&key, 0, sizeof(key));
	key.image = lower->image_of[image_id];
	key.opcode = opcode;
	if (element != 0U)
		key.element_type = lower->type_of[element];

	/* Each instruction's types; a read and a write move four channels of the image's kind of number. */
	if (opcode == OP_IMAGE_READ) {
		if (count < 5U)
			return EINVAL;
		if (count > 5U && (word[5] & LOWER_IMAGE_OPERAND_SAMPLE) != 0U) {
			/* Records the refusal in the caller's diagnostic. */
			return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image read of a sample");
		}
		key.result_type = word[1];
		key.coordinate_type = lower->type_of[word[4]];
		vector = drv_gpu_image_lower_vector_size(lower, key.result_type);
		number = drv_gpu_image_lower_number(lower, drv_gpu_image_lower_vector_of(lower, key.result_type));
	} else if (opcode == OP_IMAGE_WRITE) {
		if (count < 4U)
			return EINVAL;
		if (count > 4U && (word[4] & LOWER_IMAGE_OPERAND_SAMPLE) != 0U) {
			/* Records the refusal in the caller's diagnostic. */
			return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image write of a sample");
		}
		key.result_type = drv_gpu_image_lower_type(lower, OP_TYPE_VOID, NULL, 0U);
		key.coordinate_type = lower->type_of[word[2]];
		key.value_type = lower->type_of[word[3]];
		vector = drv_gpu_image_lower_vector_size(lower, key.value_type);
		number = drv_gpu_image_lower_number(lower, drv_gpu_image_lower_vector_of(lower, key.value_type));
	} else {
		key.result_type = word[1];
		number = drv_gpu_image_lower_number(lower, drv_gpu_image_lower_vector_of(lower, key.result_type));
		if (number == LOWER_NUMBER_FLOAT) {
			/* Records the refusal in the caller's diagnostic. */
			return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image size that is not integers");
		}
		vector = 4U;
		number = image->number;
	}

	/* Refuses a texel that is not four channels of the image's kind of number. */
	if (vector != 4U) {
		/* Records the refusal in the caller's diagnostic. */
		return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image texel that is not four channels");
	}
	if ((number == LOWER_NUMBER_FLOAT && image->number != LOWER_NUMBER_FLOAT) ||
	    (number != LOWER_NUMBER_FLOAT && image->number == LOWER_NUMBER_FLOAT)) {
		/* Records the refusal in the caller's diagnostic. */
		return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image texel that is not its format's kind of number");
	}

	/* Finds or makes the function. */
	function = drv_gpu_image_lower_helper(lower, &key);
	if (function == 0U)
		return lower->error;

	/* The call: [the element], then the coordinate and a write's texel. */
	length = 4U;
	call[1] = key.result_type;
	call[3] = function;
	if (opcode == OP_IMAGE_WRITE) {
		call[2] = lower->next_id;
		lower->next_id++;
	} else {
		call[2] = word[2];
	}
	if (element != 0U) {
		call[length] = element;
		length++;
	}
	if (opcode == OP_IMAGE_READ) {
		call[length] = word[4];
		length++;
	} else if (opcode == OP_IMAGE_WRITE) {
		call[length] = word[2];
		length++;
		call[length] = word[3];
		length++;
	}
	call[0] = (OP_FUNCTION_CALL) | (length << 16);
	drv_gpu_image_lower_put(lower, &lower->bodies, call, length);

	/* Succeeded: the access is a call. */
	return 0;
}

/* Replaces an atomic on a texel pointer with a call of the function made for it. */
static int
drv_gpu_image_lower_atomic(
	struct drv_gpu_image_lower *lower,
	const uint32_t *word,
	uint32_t count,
	uint32_t offset)
{
	struct drv_gpu_image_lower_helper key;
	const struct drv_gpu_image_lower_image *image;
	uint32_t call[9];
	uint32_t opcode;
	uint32_t pointer;
	uint32_t element;
	uint32_t function;
	uint32_t length;
	uint32_t value;
	uint32_t comparator;

	/* The texel pointer and its image. */
	opcode = word[0] & 0xFFFFU;
	pointer = word[3];
	if (opcode == OP_ATOMIC_STORE)
		pointer = word[1];
	image = &lower->images[lower->image_of[pointer]];
	element = lower->element_of[pointer];

	/* Refuses an atomic on a format other than the 32-bit integers of one channel. */
	if (image->pack != LOWER_PACK_WORDS || image->channels != 1U || image->number == LOWER_NUMBER_FLOAT) {
		/* Records the refusal in the caller's diagnostic. */
		return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image atomic on a format without atomics");
	}

	/* The instruction's operands: the scope, the semantics and the values. */
	kern_memset(&key, 0, sizeof(key));
	key.image = lower->image_of[pointer];
	key.opcode = opcode;
	if (element != 0U)
		key.element_type = lower->type_of[element];
	key.coordinate_type = lower->type_of[lower->coordinate_of[pointer]];
	value = 0U;
	comparator = 0U;
	if (opcode == OP_ATOMIC_STORE) {
		if (count < 5U)
			return EINVAL;
		key.result_type = drv_gpu_image_lower_type(lower, OP_TYPE_VOID, NULL, 0U);
		key.scope = word[2];
		key.semantics = word[3];
		value = word[4];
		key.value_type = lower->type_of[value];
	} else if (opcode == OP_ATOMIC_COMPARE_EXCHANGE) {
		if (count < 9U)
			return EINVAL;
		key.result_type = word[1];
		key.scope = word[4];
		key.semantics = word[5];
		key.semantics_unequal = word[6];
		value = word[7];
		comparator = word[8];
		key.value_type = lower->type_of[value];
	} else if (opcode == OP_ATOMIC_LOAD || opcode == OP_ATOMIC_I_INCREMENT || opcode == OP_ATOMIC_I_DECREMENT) {
		if (count < 6U)
			return EINVAL;
		key.result_type = word[1];
		key.scope = word[4];
		key.semantics = word[5];
	} else {
		if (count < 7U)
			return EINVAL;
		key.result_type = word[1];
		key.scope = word[4];
		key.semantics = word[5];
		value = word[6];
		key.value_type = lower->type_of[value];
	}

	/* Refuses a result or a value of another type than the texel word. */
	if ((opcode != OP_ATOMIC_STORE && key.result_type != image->word_type) ||
	    (value != 0U && key.value_type != image->word_type)) {
		/* Records the refusal in the caller's diagnostic. */
		return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image atomic of another type than its texel");
	}

	/* Finds or makes the function. */
	function = drv_gpu_image_lower_helper(lower, &key);
	if (function == 0U)
		return lower->error;

	/* The call: [the element], the coordinate, [the value, [the comparator]]. */
	length = 4U;
	call[1] = key.result_type;
	call[3] = function;
	if (opcode == OP_ATOMIC_STORE) {
		call[2] = lower->next_id;
		lower->next_id++;
	} else {
		call[2] = word[2];
	}
	if (element != 0U) {
		call[length] = element;
		length++;
	}
	call[length] = lower->coordinate_of[pointer];
	length++;
	if (value != 0U) {
		call[length] = value;
		length++;
	}
	if (comparator != 0U) {
		call[length] = comparator;
		length++;
	}
	call[0] = (OP_FUNCTION_CALL) | (length << 16);
	drv_gpu_image_lower_put(lower, &lower->bodies, call, length);

	/* Succeeded: the atomic is a call. */
	return 0;
}

/* Refuses an instruction that names a lowered image other than as the lowering rewrites it. */
static int
drv_gpu_image_lower_uses(
	struct drv_gpu_image_lower *lower,
	const uint32_t *word,
	uint32_t count,
	uint32_t offset)
{
	uint32_t opcode;
	uint32_t result;
	uint32_t first;
	uint32_t index;
	uint32_t id;
	int literal;

	/* The ids from the first operand after the result, by the instruction's grammar. */
	opcode = word[0] & 0xFFFFU;
	result = drv_gpu_spirv_inline_result(opcode);
	first = 1U;
	if (result == INLINE_RESULT_TYPED)
		first = 3U;
	if (result == INLINE_RESULT_LABEL)
		first = 2U;
	if (opcode == OP_FUNCTION)
		first = 5U;

	/* Checks each id operand. */
	for (index = first; index < count; index++) {
		literal = drv_gpu_spirv_inline_literal(opcode, index);
		if (literal != 0)
			continue;
		id = word[index];
		if (id >= SPIRV_MAX_BOUND)
			continue;
		if (lower->kind[id] == LOWER_ID_VARIABLE ||
		    lower->kind[id] == LOWER_ID_ELEMENT ||
		    lower->kind[id] == LOWER_ID_VALUE ||
		    lower->kind[id] == LOWER_ID_TEXEL) {
			/* Records the refusal in the caller's diagnostic. */
			return drv_gpu_image_lower_refuse(lower, opcode, offset, "a storage image used by an instruction the lowering does not rewrite");
		}
	}

	/* Succeeded: no lowered image is named. */
	return 0;
}

/*
 * Finds the added function of a key, or makes it: the function, its
 * parameters, and the body for the instruction.  Returns its id, or 0
 * after an allocation failure or with too many functions.
 */
static uint32_t
drv_gpu_image_lower_helper(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_helper *key)
{
	struct drv_gpu_image_lower_helper *helper;
	uint32_t function_type[8];
	uint32_t instruction[6];
	uint32_t parameter_types[4];
	uint32_t parameters[4];
	uint32_t parameter_count;
	uint32_t index;
	uint32_t type_id;
	uint32_t element;
	uint32_t coordinate;
	uint32_t value;
	uint32_t comparator;

	/* Reuses a function made for the same key. */
	for (index = 0U; index < lower->helper_count; index++) {
		helper = &lower->helpers[index];
		if (helper->image == key->image &&
		    helper->opcode == key->opcode &&
		    helper->result_type == key->result_type &&
		    helper->element_type == key->element_type &&
		    helper->coordinate_type == key->coordinate_type &&
		    helper->value_type == key->value_type &&
		    helper->scope == key->scope &&
		    helper->semantics == key->semantics &&
		    helper->semantics_unequal == key->semantics_unequal)
			return helper->function;
	}

	/* Refuses more functions than the lowering keeps. */
	if (lower->helper_count >= LOWER_MAX_HELPERS) {
		lower->error = drv_gpu_image_lower_refuse(lower, key->opcode, 0U, "more storage image accesses than the lowering takes");
		return 0U;
	}

	/* Records the key. */
	helper = &lower->helpers[lower->helper_count];
	*helper = *key;
	helper->function = lower->next_id;
	lower->next_id++;
	lower->helper_count++;

	/* The parameters' types: [the element], the coordinate, [the value, [the comparator]]. */
	parameter_count = 0U;
	if (key->element_type != 0U) {
		parameter_types[parameter_count] = key->element_type;
		parameter_count++;
	}
	if (key->opcode != LOWER_OP_IMAGE_QUERY_SIZE) {
		parameter_types[parameter_count] = key->coordinate_type;
		parameter_count++;
	}
	if (key->value_type != 0U) {
		parameter_types[parameter_count] = key->value_type;
		parameter_count++;
	}
	if (key->opcode == OP_ATOMIC_COMPARE_EXCHANGE) {
		parameter_types[parameter_count] = key->value_type;
		parameter_count++;
	}

	/* The function's type. */
	function_type[0] = key->result_type;
	for (index = 0U; index < parameter_count; index++)
		function_type[1U + index] = parameter_types[index];
	type_id = drv_gpu_image_lower_type(lower, OP_TYPE_FUNCTION, function_type, 1U + parameter_count);

	/* OpFunction and its parameters. */
	instruction[0] = (OP_FUNCTION) | (5U << 16);
	instruction[1] = key->result_type;
	instruction[2] = helper->function;
	instruction[3] = 0U;
	instruction[4] = type_id;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 5U);
	for (index = 0U; index < parameter_count; index++) {
		parameters[index] = lower->next_id;
		lower->next_id++;
		instruction[0] = (OP_FUNCTION_PARAMETER) | (3U << 16);
		instruction[1] = parameter_types[index];
		instruction[2] = parameters[index];
		drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 3U);
	}

	/* The parameters by their roles. */
	index = 0U;
	element = 0U;
	coordinate = 0U;
	value = 0U;
	comparator = 0U;
	if (key->element_type != 0U) {
		element = parameters[index];
		index++;
	}
	if (key->opcode != LOWER_OP_IMAGE_QUERY_SIZE) {
		coordinate = parameters[index];
		index++;
	}
	if (key->value_type != 0U) {
		value = parameters[index];
		index++;
	}
	if (key->opcode == OP_ATOMIC_COMPARE_EXCHANGE)
		comparator = parameters[index];

	/* The body for the instruction. */
	if (key->opcode == OP_IMAGE_READ) {
		drv_gpu_image_lower_emit_read(lower, helper, element, coordinate);
	} else if (key->opcode == OP_IMAGE_WRITE) {
		drv_gpu_image_lower_emit_write(lower, helper, element, coordinate, value);
	} else if (key->opcode == LOWER_OP_IMAGE_QUERY_SIZE) {
		drv_gpu_image_lower_emit_size(lower, helper, element);
	} else {
		drv_gpu_image_lower_emit_atomic(lower, helper, element, coordinate, value, comparator);
	}

	/* The function's end. */
	instruction[0] = (OP_FUNCTION_END) | (1U << 16);
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 1U);

	/* Refuses after an allocation failure. */
	if (lower->error != 0)
		return 0U;

	/* Succeeded: the function's id. */
	return helper->function;
}

/*
 * Emits the body of a read: the texel's words at the coordinate (at the
 * origin when it is outside the view), converted to four channels and
 * zeroed when it was outside.
 */
static void
drv_gpu_image_lower_emit_read(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_helper *helper,
	uint32_t element,
	uint32_t coordinate)
{
	const struct drv_gpu_image_lower_image *image;
	uint32_t instruction[4];
	uint32_t words[4];
	uint32_t channels[4];
	uint32_t operands[3];
	uint32_t uint_type;
	uint32_t channel_type;
	uint32_t result_channel;
	uint32_t zero;
	uint32_t inside;
	uint32_t index;
	uint32_t safe;
	uint32_t result;
	uint32_t word;

	/* The block's label, the coordinate's place and whether it is inside the view. */
	image = &lower->images[helper->image];
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = lower->next_id;
	lower->next_id++;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
	drv_gpu_image_lower_address(lower, image, element, coordinate, helper->coordinate_type, &inside, &index);

	/* Reads the texel at the origin when the coordinate is outside. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	operands[0] = inside;
	operands[1] = index;
	operands[2] = drv_gpu_image_lower_uint(lower, 0U);
	safe = drv_gpu_image_lower_op(lower, OP_SELECT, uint_type, operands, 3U);
	for (word = 0U; word < image->texel_words; word++)
		words[word] = drv_gpu_image_lower_texel_word(lower, image, element, safe, word);

	/* Converts the words to the channels, in the conversion's kind of number. */
	drv_gpu_image_lower_unpack(lower, image, words, channels);

	/* Each channel as the result's kind, zero where the coordinate was outside. */
	result_channel = drv_gpu_image_lower_vector_of(lower, helper->result_type);
	channel_type = drv_gpu_image_lower_scalar(lower, image->number);
	zero = drv_gpu_image_lower_constant(lower, result_channel, 0U);
	for (word = 0U; word < 4U; word++) {
		channels[word] = drv_gpu_image_lower_cast(lower, result_channel, channels[word], channel_type);
		operands[0] = inside;
		operands[1] = channels[word];
		operands[2] = zero;
		channels[word] = drv_gpu_image_lower_op(lower, OP_SELECT, result_channel, operands, 3U);
	}

	/* The texel, returned. */
	result = drv_gpu_image_lower_op(lower, OP_COMPOSITE_CONSTRUCT, helper->result_type, channels, 4U);
	instruction[0] = (OP_RETURN_VALUE) | (2U << 16);
	instruction[1] = result;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
}

/*
 * Emits the body of a write: the texel converted to the format's words,
 * stored only when the coordinate is inside the view.
 */
static void
drv_gpu_image_lower_emit_write(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_helper *helper,
	uint32_t element,
	uint32_t coordinate,
	uint32_t texel)
{
	const struct drv_gpu_image_lower_image *image;
	uint32_t instruction[4];
	uint32_t words[4];
	uint32_t pointers[4];
	uint32_t inside;
	uint32_t index;
	uint32_t then_label;
	uint32_t merge_label;
	uint32_t word;
	uint32_t place;

	/* The entry block: the place, whether it is inside, the words and their pointers. */
	image = &lower->images[helper->image];
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = lower->next_id;
	lower->next_id++;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
	drv_gpu_image_lower_address(lower, image, element, coordinate, helper->coordinate_type, &inside, &index);
	drv_gpu_image_lower_pack(lower, image, texel, helper->value_type, words);
	for (word = 0U; word < image->texel_words; word++) {
		place = index;
		if (word != 0U)
			place = drv_gpu_image_lower_op2(lower, OP_IADD, drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT), index, drv_gpu_image_lower_uint(lower, word));
		pointers[word] = drv_gpu_image_lower_texel_word(lower, image, element, place, 0xFFFFFFFFU);
	}

	/* Stores only where the coordinate is inside: a selection of one arm. */
	then_label = lower->next_id;
	lower->next_id++;
	merge_label = lower->next_id;
	lower->next_id++;
	instruction[0] = (OP_SELECTION_MERGE) | (3U << 16);
	instruction[1] = merge_label;
	instruction[2] = 0U;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 3U);
	instruction[0] = (OP_BRANCH_CONDITIONAL) | (4U << 16);
	instruction[1] = inside;
	instruction[2] = then_label;
	instruction[3] = merge_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 4U);

	/* The arm: one store a word. */
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = then_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
	for (word = 0U; word < image->texel_words; word++) {
		instruction[0] = (OP_STORE) | (3U << 16);
		instruction[1] = pointers[word];
		instruction[2] = words[word];
		drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 3U);
	}
	instruction[0] = (OP_BRANCH) | (2U << 16);
	instruction[1] = merge_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);

	/* The merge returns. */
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = merge_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
	instruction[0] = (OP_RETURN) | (1U << 16);
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 1U);
}

/*
 * Emits the body of a size: the view's width, height and slices as the
 * image's Dim and Arrayed give them, a cube array's layers being its slices
 * by six.
 */
static void
drv_gpu_image_lower_emit_size(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_helper *helper,
	uint32_t element)
{
	const struct drv_gpu_image_lower_image *image;
	uint32_t instruction[4];
	uint32_t parts[3];
	uint32_t uint_type;
	uint32_t channel;
	uint32_t size;
	uint32_t count;
	uint32_t index;
	uint32_t result;

	/* The block's label. */
	image = &lower->images[helper->image];
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = lower->next_id;
	lower->next_id++;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);

	/* The extent's parts by the image's shape. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	count = 0U;
	parts[count] = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_WIDTH);
	count++;
	if (image->dim != DIM_1D) {
		parts[count] = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_HEIGHT);
		count++;
	}
	if (image->dim == DIM_3D || (image->arrayed != 0U && image->dim != DIM_CUBE)) {
		parts[count] = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_SLICES);
		count++;
	} else if (image->dim == DIM_CUBE && image->arrayed != 0U) {
		parts[count] = drv_gpu_image_lower_op2(lower, OP_UDIV, uint_type, drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_SLICES), drv_gpu_image_lower_uint(lower, 6U));
		count++;
	}

	/* The result: a scalar of the first part, or a vector of as many parts as it has. */
	channel = drv_gpu_image_lower_vector_of(lower, helper->result_type);
	size = drv_gpu_image_lower_vector_size(lower, helper->result_type);
	if (size > count)
		size = count;
	for (index = 0U; index < size; index++)
		parts[index] = drv_gpu_image_lower_cast(lower, channel, parts[index], uint_type);
	result = parts[0];
	if (channel != helper->result_type)
		result = drv_gpu_image_lower_op(lower, OP_COMPOSITE_CONSTRUCT, helper->result_type, parts, size);

	/* Returns it. */
	instruction[0] = (OP_RETURN_VALUE) | (2U << 16);
	instruction[1] = result;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
}

/*
 * Emits the body of an atomic: the instruction on the texel's word, run
 * only when the coordinate is inside the view; the result is zero when it
 * is outside.
 */
static void
drv_gpu_image_lower_emit_atomic(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_helper *helper,
	uint32_t element,
	uint32_t coordinate,
	uint32_t value,
	uint32_t comparator)
{
	const struct drv_gpu_image_lower_image *image;
	uint32_t instruction[10];
	uint32_t inside;
	uint32_t index;
	uint32_t pointer;
	uint32_t entry_label;
	uint32_t then_label;
	uint32_t merge_label;
	uint32_t old;
	uint32_t result;
	uint32_t length;

	/* The entry block: the place, whether it is inside, and the word's pointer. */
	image = &lower->images[helper->image];
	entry_label = lower->next_id;
	lower->next_id++;
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = entry_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
	drv_gpu_image_lower_address(lower, image, element, coordinate, helper->coordinate_type, &inside, &index);
	pointer = drv_gpu_image_lower_texel_word(lower, image, element, index, 0xFFFFFFFFU);

	/* Runs the atomic only where the coordinate is inside: a selection of one arm. */
	then_label = lower->next_id;
	lower->next_id++;
	merge_label = lower->next_id;
	lower->next_id++;
	instruction[0] = (OP_SELECTION_MERGE) | (3U << 16);
	instruction[1] = merge_label;
	instruction[2] = 0U;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 3U);
	instruction[0] = (OP_BRANCH_CONDITIONAL) | (4U << 16);
	instruction[1] = inside;
	instruction[2] = then_label;
	instruction[3] = merge_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 4U);

	/* The arm: the atomic on the word, with the instruction's own scope and semantics. */
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = then_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
	old = 0U;
	if (helper->opcode == OP_ATOMIC_STORE) {
		instruction[0] = (OP_ATOMIC_STORE) | (5U << 16);
		instruction[1] = pointer;
		instruction[2] = helper->scope;
		instruction[3] = helper->semantics;
		instruction[4] = value;
		drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 5U);
	} else {
		old = lower->next_id;
		lower->next_id++;
		instruction[1] = helper->result_type;
		instruction[2] = old;
		instruction[3] = pointer;
		instruction[4] = helper->scope;
		instruction[5] = helper->semantics;
		length = 6U;
		if (helper->opcode == OP_ATOMIC_COMPARE_EXCHANGE) {
			instruction[6] = helper->semantics_unequal;
			instruction[7] = value;
			instruction[8] = comparator;
			length = 9U;
		} else if (value != 0U) {
			instruction[6] = value;
			length = 7U;
		}
		instruction[0] = helper->opcode | (length << 16);
		drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, length);
	}
	instruction[0] = (OP_BRANCH) | (2U << 16);
	instruction[1] = merge_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);

	/* The merge: the old word from the arm, zero from the entry; or nothing for a store. */
	instruction[0] = (OP_LABEL) | (2U << 16);
	instruction[1] = merge_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
	if (helper->opcode == OP_ATOMIC_STORE) {
		instruction[0] = (OP_RETURN) | (1U << 16);
		drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 1U);
		return;
	}
	result = lower->next_id;
	lower->next_id++;
	instruction[0] = (OP_PHI) | (7U << 16);
	instruction[1] = helper->result_type;
	instruction[2] = result;
	instruction[3] = old;
	instruction[4] = then_label;
	instruction[5] = drv_gpu_image_lower_constant(lower, helper->result_type, 0U);
	instruction[6] = entry_label;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 7U);
	instruction[0] = (OP_RETURN_VALUE) | (2U << 16);
	instruction[1] = result;
	drv_gpu_image_lower_put(lower, &lower->helper_words, instruction, 2U);
}

/*
 * Emits the texel's place: the coordinate's x, y and z as unsigned
 * integers, whether each is inside the view's extent, and the word index
 * of the texel's first word.
 */
static void
drv_gpu_image_lower_address(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_image *image,
	uint32_t element,
	uint32_t coordinate,
	uint32_t coordinate_type,
	uint32_t *inside,
	uint32_t *index)
{
	uint32_t component[3];
	uint32_t place[3];
	uint32_t bool_type;
	uint32_t uint_type;
	uint32_t channel;
	uint32_t count;
	uint32_t taken;
	uint32_t width;
	uint32_t height;
	uint32_t slices;
	uint32_t row;
	uint32_t slice;
	uint32_t within;
	uint32_t offset;

	/* The coordinate's components the image takes, as unsigned integers. */
	bool_type = drv_gpu_image_lower_type(lower, OP_TYPE_BOOL, NULL, 0U);
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	channel = drv_gpu_image_lower_vector_of(lower, coordinate_type);
	count = drv_gpu_image_lower_vector_size(lower, coordinate_type);
	if (count > 3U)
		count = 3U;
	for (taken = 0U; taken < 3U; taken++) {
		component[taken] = drv_gpu_image_lower_uint(lower, 0U);
		if (taken >= count)
			continue;
		component[taken] = drv_gpu_image_lower_component(lower, coordinate, coordinate_type, taken);
		component[taken] = drv_gpu_image_lower_cast(lower, uint_type, component[taken], channel);
	}

	/* The x, y and z: a 1D image has no y, the layer of a 1D array is its z. */
	place[0] = component[0];
	place[1] = component[1];
	place[2] = component[2];
	if (image->dim == DIM_1D) {
		place[1] = drv_gpu_image_lower_uint(lower, 0U);
		place[2] = drv_gpu_image_lower_uint(lower, 0U);
		if (image->arrayed != 0U)
			place[2] = component[1];
	} else if (image->dim == DIM_2D && image->arrayed == 0U) {
		place[2] = drv_gpu_image_lower_uint(lower, 0U);
	}

	/* The view's shape. */
	width = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_WIDTH);
	height = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_HEIGHT);
	slices = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_SLICES);
	row = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_ROW);
	slice = drv_gpu_image_lower_info(lower, image, element, LOWER_INFO_SLICE);

	/* Inside when x, y and z are below the extent (a negative one is a large unsigned). */
	within = drv_gpu_image_lower_op2(lower, OP_ULESS_THAN, bool_type, place[0], width);
	*inside = drv_gpu_image_lower_op2(lower, OP_LOGICAL_AND, bool_type, within, drv_gpu_image_lower_op2(lower, OP_ULESS_THAN, bool_type, place[1], height));
	*inside = drv_gpu_image_lower_op2(lower, OP_LOGICAL_AND, bool_type, *inside, drv_gpu_image_lower_op2(lower, OP_ULESS_THAN, bool_type, place[2], slices));

	/* The first word: z slices, y rows and x texels in. */
	offset = drv_gpu_image_lower_op2(lower, OP_IMUL, uint_type, place[0], drv_gpu_image_lower_uint(lower, image->texel_words));
	offset = drv_gpu_image_lower_op2(lower, OP_IADD, uint_type, offset, drv_gpu_image_lower_op2(lower, OP_IMUL, uint_type, place[1], row));
	*index = drv_gpu_image_lower_op2(lower, OP_IADD, uint_type, offset, drv_gpu_image_lower_op2(lower, OP_IMUL, uint_type, place[2], slice));
}

/* Emits a load of one word of the view's shape. */
static uint32_t
drv_gpu_image_lower_info(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_image *image,
	uint32_t element,
	uint32_t word)
{
	uint32_t chain[3];
	uint32_t count;
	uint32_t pointer;
	uint32_t value;

	/* The chain: [the element], the member. */
	count = 0U;
	chain[count] = image->info_variable;
	count++;
	if (element != 0U) {
		chain[count] = element;
		count++;
	}
	chain[count] = drv_gpu_image_lower_constant(lower, drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_INT), word);
	count++;
	pointer = drv_gpu_image_lower_op(lower, OP_ACCESS_CHAIN, image->info_word_pointer, chain, count);

	/* The word. */
	value = drv_gpu_image_lower_op1(lower, OP_LOAD, drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT), pointer);

	/* Succeeded: the word's id. */
	return value;
}

/*
 * Emits a pointer to the texel word `index` (+ `word`) and, unless `word`
 * is all ones, a load of it as an unsigned integer; with all ones the
 * pointer itself is returned.
 */
static uint32_t
drv_gpu_image_lower_texel_word(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_image *image,
	uint32_t element,
	uint32_t index,
	uint32_t word)
{
	uint32_t chain[4];
	uint32_t count;
	uint32_t uint_type;
	uint32_t place;
	uint32_t pointer;
	uint32_t value;

	/* The word's place. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	place = index;
	if (word != 0U && word != 0xFFFFFFFFU)
		place = drv_gpu_image_lower_op2(lower, OP_IADD, uint_type, index, drv_gpu_image_lower_uint(lower, word));

	/* The chain: [the element], the array member, the word. */
	count = 0U;
	chain[count] = image->variable;
	count++;
	if (element != 0U) {
		chain[count] = element;
		count++;
	}
	chain[count] = drv_gpu_image_lower_constant(lower, drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_INT), 0U);
	count++;
	chain[count] = place;
	count++;
	pointer = drv_gpu_image_lower_op(lower, OP_ACCESS_CHAIN, image->word_pointer, chain, count);
	if (word == 0xFFFFFFFFU)
		return pointer;

	/* The word, as an unsigned integer. */
	value = drv_gpu_image_lower_op1(lower, OP_LOAD, image->word_type, pointer);
	value = drv_gpu_image_lower_cast(lower, uint_type, value, image->word_type);

	/* Succeeded: the word's id. */
	return value;
}

/*
 * Emits the conversion of a texel's words (unsigned integers) to its four
 * channels, of the format's kind of number; the channels the format lacks
 * are (0, 0, 0, 1).
 */
static void
drv_gpu_image_lower_unpack(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_image *image,
	const uint32_t *words,
	uint32_t *channels)
{
	uint32_t uint_type;
	uint32_t float_type;
	uint32_t channel_type;
	uint32_t vector_type;
	uint32_t operands[2];
	uint32_t vector;
	uint32_t index;
	uint32_t instruction;

	/* The types. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	float_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_FLOAT);
	channel_type = drv_gpu_image_lower_scalar(lower, image->number);

	/* The channels by the conversion. */
	switch (image->pack) {
	case LOWER_PACK_UNORM8:
	case LOWER_PACK_SNORM8:
		/* Four normalized bytes: GLSL.std.450's unpack. */
		operands[0] = float_type;
		operands[1] = 4U;
		vector_type = drv_gpu_image_lower_type(lower, OP_TYPE_VECTOR, operands, 2U);
		instruction = LOWER_GLSL_UNPACK_UNORM_4X8;
		if (image->pack == LOWER_PACK_SNORM8)
			instruction = LOWER_GLSL_UNPACK_SNORM_4X8;
		vector = drv_gpu_image_lower_ext(lower, vector_type, instruction, words, 1U);
		for (index = 0U; index < 4U; index++)
			channels[index] = drv_gpu_image_lower_component(lower, vector, vector_type, index);
		return;
	case LOWER_PACK_HALF:
		/* Two words of two half floats. */
		operands[0] = float_type;
		operands[1] = 2U;
		vector_type = drv_gpu_image_lower_type(lower, OP_TYPE_VECTOR, operands, 2U);
		for (index = 0U; index < 2U; index++) {
			vector = drv_gpu_image_lower_ext(lower, vector_type, LOWER_GLSL_UNPACK_HALF_2X16, &words[index], 1U);
			channels[index * 2U] = drv_gpu_image_lower_component(lower, vector, vector_type, 0U);
			channels[index * 2U + 1U] = drv_gpu_image_lower_component(lower, vector, vector_type, 1U);
		}
		return;
	case LOWER_PACK_UINT8:
		/* Four bytes, each zero-extended. */
		for (index = 0U; index < 4U; index++)
			channels[index] = drv_gpu_image_lower_bits(lower, words[0], index * 8U, 8U);
		return;
	case LOWER_PACK_SINT8:
		/* Four bytes, each sign-extended: shifted to the top, then down arithmetically. */
		for (index = 0U; index < 4U; index++) {
			channels[index] = drv_gpu_image_lower_extend(lower, words[0], 24U - index * 8U, 24U);
			channels[index] = drv_gpu_image_lower_cast(lower, channel_type, channels[index], uint_type);
		}
		return;
	case LOWER_PACK_UINT16:
		/* Two halves a word, each zero-extended. */
		for (index = 0U; index < 4U; index++)
			channels[index] = drv_gpu_image_lower_bits(lower, words[index / 2U], (index % 2U) * 16U, 16U);
		return;
	case LOWER_PACK_SINT16:
		/* Two halves a word, each sign-extended. */
		for (index = 0U; index < 4U; index++) {
			channels[index] = drv_gpu_image_lower_extend(lower, words[index / 2U], 16U - (index % 2U) * 16U, 16U);
			channels[index] = drv_gpu_image_lower_cast(lower, channel_type, channels[index], uint_type);
		}
		return;
	default:
		break;
	}

	/* 32-bit channels bit for bit; the ones the format lacks are 0, 0 and 1. */
	for (index = 0U; index < 4U; index++) {
		if (index < image->channels) {
			channels[index] = drv_gpu_image_lower_cast(lower, channel_type, words[index], uint_type);
		} else if (index < 3U) {
			channels[index] = drv_gpu_image_lower_constant(lower, channel_type, 0U);
		} else if (image->number == LOWER_NUMBER_FLOAT) {
			channels[index] = drv_gpu_image_lower_constant(lower, channel_type, 0x3F800000U);
		} else {
			channels[index] = drv_gpu_image_lower_constant(lower, channel_type, 1U);
		}
	}
}

/*
 * Emits the conversion of a four-channel texel to the format's words
 * (unsigned integers, or the word type for a 32-bit integer format).
 */
static void
drv_gpu_image_lower_pack(
	struct drv_gpu_image_lower *lower,
	const struct drv_gpu_image_lower_image *image,
	uint32_t texel,
	uint32_t texel_type,
	uint32_t *words)
{
	uint32_t channels[4];
	uint32_t parts[4];
	uint32_t operands[3];
	uint32_t uint_type;
	uint32_t int_type;
	uint32_t float_type;
	uint32_t channel;
	uint32_t vector_type;
	uint32_t vector;
	uint32_t instruction;
	uint32_t index;

	/* The texel's channels as the format's kind of number. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	int_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_INT);
	float_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_FLOAT);
	channel = drv_gpu_image_lower_vector_of(lower, texel_type);
	for (index = 0U; index < 4U; index++) {
		channels[index] = drv_gpu_image_lower_component(lower, texel, texel_type, index);
		channels[index] = drv_gpu_image_lower_cast(lower, drv_gpu_image_lower_scalar(lower, image->number), channels[index], channel);
	}

	/* The words by the conversion. */
	switch (image->pack) {
	case LOWER_PACK_UNORM8:
	case LOWER_PACK_SNORM8:
		/* Four normalized bytes: GLSL.std.450's pack of the float texel. */
		operands[0] = float_type;
		operands[1] = 4U;
		vector_type = drv_gpu_image_lower_type(lower, OP_TYPE_VECTOR, operands, 2U);
		vector = drv_gpu_image_lower_op(lower, OP_COMPOSITE_CONSTRUCT, vector_type, channels, 4U);
		instruction = LOWER_GLSL_PACK_UNORM_4X8;
		if (image->pack == LOWER_PACK_SNORM8)
			instruction = LOWER_GLSL_PACK_SNORM_4X8;
		words[0] = drv_gpu_image_lower_ext(lower, uint_type, instruction, &vector, 1U);
		return;
	case LOWER_PACK_HALF:
		/* Two words of two half floats. */
		operands[0] = float_type;
		operands[1] = 2U;
		vector_type = drv_gpu_image_lower_type(lower, OP_TYPE_VECTOR, operands, 2U);
		for (index = 0U; index < 2U; index++) {
			vector = drv_gpu_image_lower_op(lower, OP_COMPOSITE_CONSTRUCT, vector_type, &channels[index * 2U], 2U);
			words[index] = drv_gpu_image_lower_ext(lower, uint_type, LOWER_GLSL_PACK_HALF_2X16, &vector, 1U);
		}
		return;
	case LOWER_PACK_UINT8:
	case LOWER_PACK_UINT16:
		/* Unsigned channels clamped to the largest the channel holds, then joined. */
		for (index = 0U; index < 4U; index++) {
			operands[0] = channels[index];
			operands[1] = drv_gpu_image_lower_uint(lower, 0xFFU);
			if (image->pack == LOWER_PACK_UINT16)
				operands[1] = drv_gpu_image_lower_uint(lower, 0xFFFFU);
			parts[index] = drv_gpu_image_lower_ext(lower, uint_type, LOWER_GLSL_UMIN, operands, 2U);
		}
		break;
	case LOWER_PACK_SINT8:
	case LOWER_PACK_SINT16:
		/* Signed channels clamped to the channel's range, then their low bits joined. */
		for (index = 0U; index < 4U; index++) {
			operands[0] = channels[index];
			operands[1] = drv_gpu_image_lower_constant(lower, int_type, 0xFFFFFF80U);
			operands[2] = drv_gpu_image_lower_constant(lower, int_type, 0x7FU);
			if (image->pack == LOWER_PACK_SINT16) {
				operands[1] = drv_gpu_image_lower_constant(lower, int_type, 0xFFFF8000U);
				operands[2] = drv_gpu_image_lower_constant(lower, int_type, 0x7FFFU);
			}
			parts[index] = drv_gpu_image_lower_ext(lower, int_type, LOWER_GLSL_SCLAMP, operands, 3U);
			parts[index] = drv_gpu_image_lower_cast(lower, uint_type, parts[index], int_type);
			operands[0] = 0xFFU;
			if (image->pack == LOWER_PACK_SINT16)
				operands[0] = 0xFFFFU;
			parts[index] = drv_gpu_image_lower_op2(lower, OP_BITWISE_AND, uint_type, parts[index], drv_gpu_image_lower_uint(lower, operands[0]));
		}
		break;
	default:
		/* 32-bit channels bit for bit, as the word type. */
		for (index = 0U; index < image->texel_words; index++)
			words[index] = drv_gpu_image_lower_cast(lower, image->word_type, channels[index], drv_gpu_image_lower_scalar(lower, image->number));
		return;
	}

	/* The 8-bit channels make one word, the 16-bit ones two. */
	if (image->pack == LOWER_PACK_UINT8 || image->pack == LOWER_PACK_SINT8) {
		words[0] = drv_gpu_image_lower_join(lower, parts, 4U, 8U);
		return;
	}
	words[0] = drv_gpu_image_lower_join(lower, parts, 2U, 16U);
	words[1] = drv_gpu_image_lower_join(lower, &parts[2], 2U, 16U);
}

/* Emits the field of `width` bits at `shift` of a word, on unsigned integers. */
static uint32_t
drv_gpu_image_lower_bits(
	struct drv_gpu_image_lower *lower,
	uint32_t word,
	uint32_t shift,
	uint32_t width)
{
	uint32_t uint_type;
	uint32_t value;

	/* Shifts the field down, unless it is at the bottom. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	value = word;
	if (shift != 0U)
		value = drv_gpu_image_lower_op2(lower, OP_SHIFT_RIGHT_LOGICAL, uint_type, value, drv_gpu_image_lower_uint(lower, shift));

	/* Keeps the field's bits, unless nothing is above it. */
	if (shift + width < 32U)
		value = drv_gpu_image_lower_op2(lower, OP_BITWISE_AND, uint_type, value, drv_gpu_image_lower_uint(lower, (1U << width) - 1U));

	/* Succeeded: the field. */
	return value;
}

/* Emits (word << left) >> right, the right shift arithmetic, on unsigned integers. */
static uint32_t
drv_gpu_image_lower_extend(
	struct drv_gpu_image_lower *lower,
	uint32_t word,
	uint32_t left,
	uint32_t right)
{
	uint32_t uint_type;
	uint32_t value;

	/* Moves the field's top bit to bit 31, then shifts it down with its sign. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	value = word;
	if (left != 0U)
		value = drv_gpu_image_lower_op2(lower, OP_SHIFT_LEFT_LOGICAL, uint_type, value, drv_gpu_image_lower_uint(lower, left));
	value = drv_gpu_image_lower_op2(lower, OP_SHIFT_RIGHT_ARITHMETIC, uint_type, value, drv_gpu_image_lower_uint(lower, right));

	/* Succeeded: the field, sign-extended. */
	return value;
}

/* Emits parts[0] | parts[1] << shift | parts[2] << 2 * shift ... on unsigned integers. */
static uint32_t
drv_gpu_image_lower_join(
	struct drv_gpu_image_lower *lower,
	const uint32_t *parts,
	uint32_t count,
	uint32_t shift)
{
	uint32_t uint_type;
	uint32_t value;
	uint32_t moved;
	uint32_t index;

	/* Ors each part in at its place. */
	uint_type = drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT);
	value = parts[0];
	for (index = 1U; index < count; index++) {
		moved = drv_gpu_image_lower_op2(lower, OP_SHIFT_LEFT_LOGICAL, uint_type, parts[index], drv_gpu_image_lower_uint(lower, index * shift));
		value = drv_gpu_image_lower_op2(lower, OP_BITWISE_OR, uint_type, value, moved);
	}

	/* Succeeded: the word. */
	return value;
}

/* Emits a bitcast of id from type `have` to `wanted`, or nothing when they are the same. */
static uint32_t
drv_gpu_image_lower_cast(
	struct drv_gpu_image_lower *lower,
	uint32_t wanted,
	uint32_t id,
	uint32_t have)
{
	uint32_t value;

	/* The same type needs no instruction. */
	if (wanted == have)
		return id;

	/* The bits as the wanted type. */
	value = drv_gpu_image_lower_op1(lower, OP_BITCAST, wanted, id);

	/* Succeeded: the cast's id. */
	return value;
}

/* Emits one component of a vector, or the scalar itself for a scalar type. */
static uint32_t
drv_gpu_image_lower_component(
	struct drv_gpu_image_lower *lower,
	uint32_t vector,
	uint32_t vector_type,
	uint32_t index)
{
	uint32_t operands[4];
	uint32_t channel;
	uint32_t value;

	/* A scalar is its own only component. */
	channel = drv_gpu_image_lower_vector_of(lower, vector_type);
	if (channel == vector_type)
		return vector;

	/* The component. */
	operands[0] = (OP_COMPOSITE_EXTRACT) | (5U << 16);
	operands[1] = channel;
	operands[2] = lower->next_id;
	operands[3] = vector;
	lower->next_id++;
	value = operands[2];
	drv_gpu_image_lower_put(lower, &lower->helper_words, operands, 4U);
	drv_gpu_image_lower_put(lower, &lower->helper_words, &index, 1U);

	/* Succeeded: the component's id. */
	return value;
}

/* Finds or declares the 32-bit scalar type of a kind of number (LOWER_NUMBER_*). */
static uint32_t
drv_gpu_image_lower_scalar(
	struct drv_gpu_image_lower *lower,
	uint32_t number)
{
	uint32_t operands[2];
	uint32_t type;

	/* A float is OpTypeFloat 32. */
	operands[0] = 32U;
	if (number == LOWER_NUMBER_FLOAT) {
		type = drv_gpu_image_lower_type(lower, OP_TYPE_FLOAT, operands, 1U);
		return type;
	}

	/* An integer is OpTypeInt 32 with its signedness. */
	operands[1] = 0U;
	if (number == LOWER_NUMBER_INT)
		operands[1] = 1U;
	type = drv_gpu_image_lower_type(lower, OP_TYPE_INT, operands, 2U);

	/* Succeeded: the type's id. */
	return type;
}

/* Reports the kind of number (LOWER_NUMBER_*) of a 32-bit scalar type of the module. */
static uint32_t
drv_gpu_image_lower_number(
	const struct drv_gpu_image_lower *lower,
	uint32_t type)
{
	const uint32_t *word;

	/* A type nobody declares is no number the lowering knows. */
	word = drv_gpu_image_lower_declaration(lower, type);
	if (word == NULL)
		return LOWER_NUMBER_FLOAT;

	/* An integer by its signedness; anything else counts as a float. */
	if ((word[0] & 0xFFFFU) == OP_TYPE_INT && word[3] != 0U)
		return LOWER_NUMBER_INT;
	if ((word[0] & 0xFFFFU) == OP_TYPE_INT)
		return LOWER_NUMBER_UINT;

	/* Succeeded: a float. */
	return LOWER_NUMBER_FLOAT;
}

/* Reports a type's components: a vector's count, 1 for anything else. */
static uint32_t
drv_gpu_image_lower_vector_size(
	const struct drv_gpu_image_lower *lower,
	uint32_t type)
{
	const uint32_t *word;

	/* A type nobody declares is one component. */
	word = drv_gpu_image_lower_declaration(lower, type);
	if (word == NULL)
		return 1U;

	/* A vector's count. */
	if ((word[0] & 0xFFFFU) == OP_TYPE_VECTOR)
		return word[3];

	/* Succeeded: one component. */
	return 1U;
}

/* Reports a vector type's component type, or the type itself for a scalar. */
static uint32_t
drv_gpu_image_lower_vector_of(
	const struct drv_gpu_image_lower *lower,
	uint32_t type)
{
	const uint32_t *word;

	/* A type nobody declares is its own component. */
	word = drv_gpu_image_lower_declaration(lower, type);
	if (word == NULL)
		return type;

	/* A vector's component type. */
	if ((word[0] & 0xFFFFU) == OP_TYPE_VECTOR)
		return word[2];

	/* Succeeded: a scalar. */
	return type;
}

/*
 * Finds a type the module or the lowering declares with these operands
 * after its result id, or declares it; returns its id.
 */
static uint32_t
drv_gpu_image_lower_type(
	struct drv_gpu_image_lower *lower,
	uint32_t opcode,
	const uint32_t *operands,
	uint32_t count)
{
	const uint32_t *word;
	uint32_t instruction[16];
	uint32_t offset;
	uint32_t length;
	uint32_t index;
	uint32_t id;
	int dropped;
	int same;

	/* Looks among the module's types. */
	offset = lower->types_begin;
	while (offset < lower->functions_begin) {
		word = lower->code + offset;
		length = word[0] >> 16;
		dropped = drv_gpu_image_lower_dropped(lower, word[1]);
		if ((word[0] & 0xFFFFU) == opcode && length == count + 2U && dropped == 0) {
			same = 1;
			for (index = 0U; index < count; index++) {
				if (word[2U + index] != operands[index])
					same = 0;
			}
			if (same != 0)
				return word[1];
		}
		offset += length;
	}

	/* Looks among the lowering's own declarations. */
	offset = 0U;
	while (offset < lower->declarations.count) {
		word = lower->declarations.word + offset;
		length = word[0] >> 16;
		if ((word[0] & 0xFFFFU) == opcode && length == count + 2U) {
			same = 1;
			for (index = 0U; index < count; index++) {
				if (word[2U + index] != operands[index])
					same = 0;
			}
			if (same != 0)
				return word[1];
		}
		offset += length;
	}

	/* Declares it, recording where so that its components are found again. */
	id = lower->next_id;
	lower->next_id++;
	if (id < SPIRV_MAX_BOUND)
		lower->declared_at[id] = LOWER_DECLARED_NEW | lower->declarations.count;
	instruction[0] = opcode | ((count + 2U) << 16);
	instruction[1] = id;
	for (index = 0U; index < count && index < 14U; index++)
		instruction[2U + index] = operands[index];
	drv_gpu_image_lower_put(lower, &lower->declarations, instruction, count + 2U);

	/* Succeeded: the type's id. */
	return id;
}

/* Finds or declares a 32-bit OpConstant of a type; returns its id. */
static uint32_t
drv_gpu_image_lower_constant(
	struct drv_gpu_image_lower *lower,
	uint32_t type,
	uint32_t value)
{
	const uint32_t *word;
	uint32_t instruction[4];
	uint32_t offset;
	uint32_t length;
	uint32_t id;

	/* Looks among the module's constants. */
	offset = lower->types_begin;
	while (offset < lower->functions_begin) {
		word = lower->code + offset;
		length = word[0] >> 16;
		if ((word[0] & 0xFFFFU) == OP_CONSTANT && length == 4U && word[1] == type && word[3] == value)
			return word[2];
		offset += length;
	}

	/* Looks among the lowering's own. */
	offset = 0U;
	while (offset < lower->declarations.count) {
		word = lower->declarations.word + offset;
		length = word[0] >> 16;
		if ((word[0] & 0xFFFFU) == OP_CONSTANT && length == 4U && word[1] == type && word[3] == value)
			return word[2];
		offset += length;
	}

	/* Declares it. */
	id = lower->next_id;
	lower->next_id++;
	instruction[0] = (OP_CONSTANT) | (4U << 16);
	instruction[1] = type;
	instruction[2] = id;
	instruction[3] = value;
	drv_gpu_image_lower_put(lower, &lower->declarations, instruction, 4U);

	/* Succeeded: the constant's id. */
	return id;
}

/* Finds or declares a 32-bit unsigned integer constant. */
static uint32_t
drv_gpu_image_lower_uint(
	struct drv_gpu_image_lower *lower,
	uint32_t value)
{
	uint32_t id;

	/* The constant of the unsigned type. */
	id = drv_gpu_image_lower_constant(lower, drv_gpu_image_lower_scalar(lower, LOWER_NUMBER_UINT), value);

	/* Succeeded: the constant's id. */
	return id;
}

/* Emits one instruction of a type, a new result and operands into the added functions; returns the result. */
static uint32_t
drv_gpu_image_lower_op(
	struct drv_gpu_image_lower *lower,
	uint32_t opcode,
	uint32_t type,
	const uint32_t *operands,
	uint32_t count)
{
	uint32_t head[3];
	uint32_t result;

	/* The opcode, the type and the result. */
	result = lower->next_id;
	lower->next_id++;
	head[0] = opcode | ((count + 3U) << 16);
	head[1] = type;
	head[2] = result;
	drv_gpu_image_lower_put(lower, &lower->helper_words, head, 3U);

	/* The operands. */
	drv_gpu_image_lower_put(lower, &lower->helper_words, operands, count);

	/* Succeeded: the result's id. */
	return result;
}

/* Emits an instruction of one operand. */
static uint32_t
drv_gpu_image_lower_op1(
	struct drv_gpu_image_lower *lower,
	uint32_t opcode,
	uint32_t type,
	uint32_t a)
{
	uint32_t result;

	/* The instruction. */
	result = drv_gpu_image_lower_op(lower, opcode, type, &a, 1U);

	/* Succeeded: the result's id. */
	return result;
}

/* Emits an instruction of two operands. */
static uint32_t
drv_gpu_image_lower_op2(
	struct drv_gpu_image_lower *lower,
	uint32_t opcode,
	uint32_t type,
	uint32_t a,
	uint32_t b)
{
	uint32_t operands[2];
	uint32_t result;

	/* The instruction. */
	operands[0] = a;
	operands[1] = b;
	result = drv_gpu_image_lower_op(lower, opcode, type, operands, 2U);

	/* Succeeded: the result's id. */
	return result;
}

/* Emits a GLSL.std.450 instruction, importing the set when the module does not. */
static uint32_t
drv_gpu_image_lower_ext(
	struct drv_gpu_image_lower *lower,
	uint32_t type,
	uint32_t instruction,
	const uint32_t *operands,
	uint32_t count)
{
	uint32_t all[5];
	uint32_t index;
	uint32_t result;

	/* The import, new when the module has none. */
	if (lower->glsl == 0U) {
		lower->glsl = lower->next_id;
		lower->next_id++;
		lower->glsl_new = 1;
	}

	/* The set, the instruction and its operands. */
	all[0] = lower->glsl;
	all[1] = instruction;
	for (index = 0U; index < count && index < 3U; index++)
		all[2U + index] = operands[index];
	result = drv_gpu_image_lower_op(lower, OP_EXT_INST, type, all, 2U + count);

	/* Succeeded: the result's id. */
	return result;
}

/* Appends words to a list, recording an allocation failure. */
static void
drv_gpu_image_lower_put(
	struct drv_gpu_image_lower *lower,
	struct drv_gpu_spirv_words *list,
	const uint32_t *word,
	uint32_t count)
{
	int error;

	/* Nothing more after a failure. */
	if (lower->error != 0)
		return;

	/* Appends them. */
	error = drv_gpu_spirv_inline_push(list, word, count);
	if (error != 0)
		lower->error = error;
}

/* Emits an OpDecorate of a new id, with one literal or none. */
static void
drv_gpu_image_lower_decorate(
	struct drv_gpu_image_lower *lower,
	uint32_t id,
	uint32_t decoration,
	uint32_t value,
	int has_value)
{
	uint32_t instruction[4];

	/* The decoration, with its literal when it has one. */
	instruction[0] = (OP_DECORATE) | (3U << 16);
	instruction[1] = id;
	instruction[2] = decoration;
	instruction[3] = value;
	if (has_value != 0)
		instruction[0] = (OP_DECORATE) | (4U << 16);
	drv_gpu_image_lower_put(lower, &lower->annotations, instruction, instruction[0] >> 16);
}

/* Emits the Offset of a new struct's member. */
static void
drv_gpu_image_lower_member_offset(
	struct drv_gpu_image_lower *lower,
	uint32_t id,
	uint32_t member,
	uint32_t offset)
{
	uint32_t instruction[5];

	/* OpMemberDecorate Offset. */
	instruction[0] = (OP_MEMBER_DECORATE) | (5U << 16);
	instruction[1] = id;
	instruction[2] = member;
	instruction[3] = DEC_OFFSET;
	instruction[4] = offset;
	drv_gpu_image_lower_put(lower, &lower->annotations, instruction, 5U);
}

/*
 * Writes the rewritten module: the header with the new bound, the global
 * sections with the new extension, import, decorations and declarations
 * and without the image types, then the rewritten and the added functions.
 */
static int
drv_gpu_image_lower_assemble(
	struct drv_gpu_image_lower *lower,
	uint32_t **out,
	size_t *out_count)
{
	struct drv_gpu_spirv_words module;
	const uint32_t *word;
	uint32_t header[SPIRV_HEADER_WORDS];
	uint32_t extension[1U + LOWER_EXTENSION_WORDS];
	uint32_t import[6];
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	uint32_t index;
	int extension_done;
	int import_done;
	int declared;
	int error;

	/* The extension the StorageBuffer class needs before SPIR-V 1.3, unless declared. */
	lower->extension_new = 0;
	if (lower->version < LOWER_VERSION_STORAGE_BUFFER)
		lower->extension_new = 1;
	declared = drv_gpu_image_lower_has_extension(lower);
	if (declared != 0)
		lower->extension_new = 0;

	/* The header, with the bound past every new id. */
	kern_memset(&module, 0, sizeof(module));
	header[0] = lower->code[0];
	header[1] = lower->code[1];
	header[2] = lower->code[2];
	header[3] = lower->next_id;
	header[4] = lower->code[4];
	error = drv_gpu_spirv_inline_push(&module, header, SPIRV_HEADER_WORDS);

	/* The global sections, with what the lowering adds placed where SPIR-V's layout wants it. */
	extension_done = 0;
	import_done = 0;
	offset = SPIRV_HEADER_WORDS;
	while (error == 0 && offset < lower->functions_begin) {
		word = lower->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/* The extension before the first import or the memory model. */
		if (extension_done == 0 && (opcode == OP_EXT_INST_IMPORT || opcode == OP_MEMORY_MODEL)) {
			extension_done = 1;
			if (lower->extension_new != 0) {
				/* OpExtension SPV_KHR_storage_buffer_storage_class. */
				extension[0] = (OP_EXTENSION) | ((1U + LOWER_EXTENSION_WORDS) << 16);
				for (index = 0U; index < LOWER_EXTENSION_WORDS; index++)
					extension[1U + index] = drv_gpu_image_lower_extension[index];
				error = drv_gpu_spirv_inline_push(&module, extension, 1U + LOWER_EXTENSION_WORDS);
			}
		}

		/* The import before the memory model. */
		if (error == 0 && import_done == 0 && opcode == OP_MEMORY_MODEL) {
			import_done = 1;
			if (lower->glsl_new != 0) {
				/* OpExtInstImport GLSL.std.450. */
				import[0] = (OP_EXT_INST_IMPORT) | (6U << 16);
				import[1] = lower->glsl;
				import[2] = 0x4c534c47U;
				import[3] = 0x6474732eU;
				import[4] = 0x3035342eU;
				import[5] = 0U;
				error = drv_gpu_spirv_inline_push(&module, import, 6U);
			}
		}

		/* The new decorations where the module's end (or where its types begin). */
		if (error == 0 && offset == lower->annotations_end)
			error = drv_gpu_spirv_inline_push(&module, lower->annotations.word, lower->annotations.count);

		/* The new declarations before the first function. */
		if (error == 0)
			error = drv_gpu_image_lower_global(lower, &module, word, count);

		/* Advances to the next instruction. */
		offset += count;
	}

	/* A module whose sections end with its globals gets what comes after them. */
	if (error == 0 && lower->annotations_end >= lower->functions_begin)
		error = drv_gpu_spirv_inline_push(&module, lower->annotations.word, lower->annotations.count);
	if (error == 0)
		error = drv_gpu_spirv_inline_push(&module, lower->declarations.word, lower->declarations.count);

	/* The functions, rewritten, then the added ones. */
	if (error == 0)
		error = drv_gpu_spirv_inline_push(&module, lower->bodies.word, lower->bodies.count);
	if (error == 0)
		error = drv_gpu_spirv_inline_push(&module, lower->helper_words.word, lower->helper_words.count);

	/* Refuses a module the memory did not hold. */
	if (error != 0) {
		if (module.word != NULL)
			kern_free(module.word);
		return error;
	}

	/* Succeeded: the module is the caller's. */
	*out = module.word;
	*out_count = module.count;
	return 0;
}

/*
 * Copies one global instruction to the module: the entry points with the
 * shapes' blocks in their interface from SPIR-V 1.4, the names and
 * decorations without the dropped ids and an image variable's access
 * decorations, and the types without the image types and variables.
 */
static int
drv_gpu_image_lower_global(
	struct drv_gpu_image_lower *lower,
	struct drv_gpu_spirv_words *module,
	const uint32_t *word,
	uint32_t count)
{
	uint32_t opcode;
	uint32_t entry[64];
	uint32_t index;
	uint32_t length;
	int dropped;
	int error;

	/* By the instruction, and whether it names (as its first operand) a type that goes. */
	opcode = word[0] & 0xFFFFU;
	dropped = 0;
	if (count >= 2U)
		dropped = drv_gpu_image_lower_dropped(lower, word[1]);
	switch (opcode) {
	case OP_ENTRY_POINT:
		/* From SPIR-V 1.4 an entry point lists every global it uses: the shapes' blocks join. */
		if (lower->version < LOWER_VERSION_ALL_INTERFACE || count + lower->image_count > 64U)
			break;
		for (index = 0U; index < count; index++)
			entry[index] = word[index];
		length = count;
		for (index = 0U; index < lower->image_count; index++) {
			entry[length] = lower->images[index].info_variable;
			length++;
		}
		entry[0] = (OP_ENTRY_POINT) | (length << 16);
		error = drv_gpu_spirv_inline_push(module, entry, length);
		if (error != 0)
			return error;

		/* Succeeded: the entry point is in the module. */
		return 0;
	case OP_NAME:
	case OP_MEMBER_NAME:
		/* A name of a dropped type goes with it. */
		if (dropped != 0)
			return 0;
		break;
	case OP_DECORATE:
	case OP_MEMBER_DECORATE:
		/* A decoration of a dropped type goes; an image variable keeps only its set and binding. */
		if (count >= 3U && dropped != 0)
			return 0;
		if (opcode == OP_DECORATE &&
		    count >= 3U &&
		    word[1] < SPIRV_MAX_BOUND &&
		    lower->kind[word[1]] == LOWER_ID_VARIABLE &&
		    word[2] != DEC_DESCRIPTOR_SET &&
		    word[2] != DEC_BINDING)
			return 0;
		break;
	case OP_VARIABLE:
		/* An image variable is declared again, as its texel buffer, after the new types. */
		if (count >= 3U && word[2] < SPIRV_MAX_BOUND && lower->kind[word[2]] == LOWER_ID_VARIABLE)
			return 0;
		break;
	default:
		/* An image type, an array of them or a pointer to either goes. */
		if (opcode >= OP_TYPE_VOID && opcode <= OP_TYPE_FUNCTION && dropped != 0)
			return 0;
		break;
	}

	/* Copies the instruction as it is. */
	error = drv_gpu_spirv_inline_push(module, word, count);
	if (error != 0)
		return error;

	/* Succeeded: the instruction is in the module. */
	return 0;
}

/* Returns whether an id is a type the rewritten module no longer declares. */
static int
drv_gpu_image_lower_dropped(
	const struct drv_gpu_image_lower *lower,
	uint32_t id)
{
	/* An id past the table is the module's own. */
	if (id >= SPIRV_MAX_BOUND)
		return 0;

	/* The image types, their arrays and the pointers to them. */
	if (lower->kind[id] == LOWER_ID_IMAGE_TYPE)
		return 1;
	if (lower->kind[id] == LOWER_ID_IMAGE_ARRAY)
		return 1;
	if (lower->kind[id] == LOWER_ID_IMAGE_POINTER)
		return 1;

	/* Succeeded: the id stays. */
	return 0;
}

/*
 * Returns the words of the instruction that declares a type or a constant,
 * in the module or among the lowering's own declarations; NULL for an id
 * nobody declares.
 */
static const uint32_t *
drv_gpu_image_lower_declaration(
	const struct drv_gpu_image_lower *lower,
	uint32_t id)
{
	uint32_t at;

	/* An id past the table, or one never declared, has no declaration. */
	if (id >= SPIRV_MAX_BOUND || lower->declared_at[id] == 0U)
		return NULL;

	/* A new one is among the lowering's declarations. */
	at = lower->declared_at[id];
	if ((at & LOWER_DECLARED_NEW) != 0U)
		return lower->declarations.word + (at & ~LOWER_DECLARED_NEW);

	/* Succeeded: the module's own. */
	return lower->code + at;
}

/* Returns whether the module already declares SPV_KHR_storage_buffer_storage_class. */
static int
drv_gpu_image_lower_has_extension(
	const struct drv_gpu_image_lower *lower)
{
	const uint32_t *word;
	uint32_t offset;
	uint32_t count;
	uint32_t index;
	int same;

	/* Compares each OpExtension's name with the extension's, word for word. */
	offset = SPIRV_HEADER_WORDS;
	while (offset < lower->types_begin) {
		word = lower->code + offset;
		count = word[0] >> 16;
		if ((word[0] & 0xFFFFU) == OP_EXTENSION && count == 1U + LOWER_EXTENSION_WORDS) {
			same = 1;
			for (index = 0U; index < LOWER_EXTENSION_WORDS; index++) {
				if (word[1U + index] != drv_gpu_image_lower_extension[index])
					same = 0;
			}
			if (same != 0)
				return 1;
		}
		offset += count;
	}

	/* The module does not declare it. */
	return 0;
}

/* Records an instruction the lowering refuses. */
static int
drv_gpu_image_lower_refuse(
	struct drv_gpu_image_lower *lower,
	uint32_t opcode,
	uint32_t offset,
	const char *reason)
{
	/* Names the refused instruction for the caller's diagnostic. */
	lower->diag.opcode = opcode;
	lower->diag.word_offset = offset;
	lower->diag.reason = reason;

	/* Reports valid SPIR-V the lowering does not rewrite. */
	return ENOTSUP;
}

/* Releases the lowering's lists and tables. */
static void
drv_gpu_image_lower_release(
	struct drv_gpu_image_lower *lower)
{
	/* The lists. */
	if (lower->declarations.word != NULL)
		kern_free(lower->declarations.word);
	if (lower->annotations.word != NULL)
		kern_free(lower->annotations.word);
	if (lower->bodies.word != NULL)
		kern_free(lower->bodies.word);
	if (lower->helper_words.word != NULL)
		kern_free(lower->helper_words.word);

	/* The tables by id. */
	if (lower->kind != NULL)
		kern_free(lower->kind);
	if (lower->image_of != NULL)
		kern_free(lower->image_of);
	if (lower->element_of != NULL)
		kern_free(lower->element_of);
	if (lower->coordinate_of != NULL)
		kern_free(lower->coordinate_of);
	if (lower->type_of != NULL)
		kern_free(lower->type_of);
	if (lower->declared_at != NULL)
		kern_free(lower->declared_at);
	if (lower->set_of != NULL)
		kern_free(lower->set_of);
	if (lower->binding_of != NULL)
		kern_free(lower->binding_of);

	/* The state itself. */
	kern_free(lower);
}
