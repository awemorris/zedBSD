/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali Valhall shader compiler
 * (src/drivers/gpu/mali/compiler/).
 *
 * It reads the compiler's words back with a decoder of its own, whose
 * table comes from Mesa's ISA description alone
 * (tests/mali-compiler-isa-table.inc), so a word the compiler's encoder
 * gets wrong does not decode as what the compiler meant:
 *
 *   1. words the encoder builds are compared with Mesa's assembler test
 *      cases, and every case of that file (when its path is given)
 *      decodes as the instruction it names;
 *   2. every module given compiles, every word of the programs decodes,
 *      and a listing is written next to the results;
 *   3. each program runs on a one-lane model of the Valhall instructions
 *      (driven by the decoded words) and its outputs are compared with a
 *      reference interpreter of the IR for random inputs
 *      (tests/mali-compiler-model.c).
 *
 * usage: mali-compiler-host-test OUTDIR [--cases assembler-cases.txt]
 *        [--trials N] MODULE.(vert|frag|comp).spv...
 *
 * Nothing here is evidence about the GPU.
 */

#include "drivers/gpu/mali/compiler/shader.h"
#include "drivers/gpu/mali/compiler/va-private.h"

#include "drivers/gpu/compiler/spirv.h"

#include "tests/mali-compiler-model.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);

/* One instruction of the generated ISA table. */
struct isa_entry {
	const char *name;
	uint64_t mask;
	uint64_t value;
	uint32_t sources;
	uint8_t source_flags[4];
	uint16_t source_sizes[4];
	uint32_t dests;
	uint32_t staging_read;
	uint32_t staging_write;
	uint32_t message;
};

/* The table generated from Mesa 26.2.6 ISA.xml. */
static const struct isa_entry isa_table[] = {
#include "tests/mali-compiler-isa-table.inc"
};

/* The constant table of the ISA. */
static const uint32_t isa_immediates[32] = ISA_TABLE_IMMEDIATES;

/*
 * One word the encoder must build: the word of Mesa's assembler test
 * case, the bits of it the compiler leaves to a hint (the last-use bits
 * of register sources), and the word the encoder makes.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/test/assembler-cases.txt.
 */
struct encode_case {
	const char *text;
	uint64_t expected;
	uint64_t ignored;
	uint64_t built;
};

static uint64_t word_of(const char *hex);
static int decode_index(uint64_t word);
static int check_encoder(void);
static int check_cases_file(const char *path);
static int test_module(const char *outdir, const char *path, unsigned trials);
static int test_program(const char *outdir, const char *name, const uint32_t *words, size_t count, uint32_t stage, uint32_t flags, unsigned trials, int *has_ubo);
static int decode_program(FILE *listing, const char *what, const uint8_t *code, size_t length);
static void print_source(FILE *out, uint8_t byte, uint32_t page);
static uint32_t *read_file(const char *path, size_t *words);

void *
kern_malloc(
	size_t size)
{
	return malloc(size);
}

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

int
main(
	int argc,
	char **argv)
{
	const char *cases;
	unsigned trials;
	int failed;
	int index;

	if (argc < 2) {
		fprintf(stderr, "usage: %s OUTDIR [--cases FILE] [--trials N] MODULE.spv...\n", argv[0]);
		return 2;
	}
	cases = NULL;
	trials = 50U;
	index = 2;
	while (index + 1 < argc && strncmp(argv[index], "--", 2) == 0) {
		if (strcmp(argv[index], "--cases") == 0)
			cases = argv[index + 1];
		if (strcmp(argv[index], "--trials") == 0)
			trials = (unsigned)strtoul(argv[index + 1], NULL, 10);
		index += 2;
	}

	failed = check_encoder();
	if (cases != NULL)
		failed |= check_cases_file(cases);
	for (; index < argc; index++)
		failed |= test_module(argv[1], argv[index], trials);
	return failed;
}

/* Reads eight bytes of hex text as a little-endian word. */
static uint64_t
word_of(
	const char *hex)
{
	uint64_t word;
	unsigned byte;
	unsigned index;

	word = 0U;
	for (index = 0U; index < 8U; index++) {
		if (sscanf(hex + index * 3U, "%2x", &byte) != 1)
			return 0U;
		word |= (uint64_t)byte << (8U * index);
	}
	return word;
}

/* Finds the most specific table entry a word matches, or -1. */
static int
decode_index(
	uint64_t word)
{
	unsigned best_bits;
	unsigned bits;
	unsigned index;
	int best;

	best = -1;
	best_bits = 0U;
	for (index = 0U; index < sizeof(isa_table) / sizeof(isa_table[0]); index++) {
		if ((word & isa_table[index].mask) != isa_table[index].value)
			continue;
		bits = (unsigned)__builtin_popcountll(isa_table[index].mask);
		if (best < 0 || bits > best_bits) {
			best = (int)index;
			best_bits = bits;
		}
	}
	return best;
}

/* Builds words with the encoder and compares them with Mesa's test cases. */
static int
check_encoder(void)
{
	struct encode_case cases[27];
	unsigned count;
	unsigned index;
	unsigned failed;
	int entry;

	count = 0U;
#define CASE(TEXT, IGNORED, BUILT) \
	do { \
		cases[count].text = TEXT; \
		cases[count].expected = word_of(TEXT); \
		cases[count].ignored = IGNORED; \
		cases[count].built = BUILT; \
		count++; \
	} while (0)

	CASE("02 00 00 00 00 c1 91 00    MOV.i32 r1, r2", 0U,
	     VA_OP_MOV_I32 | va_source_register(2U) | va_dest(1U));
	CASE("8a 00 00 00 00 c1 91 00    MOV.i32 r1, u5.w0", 0U,
	     VA_OP_MOV_I32 | va_source_uniform(10U) | va_dest(1U));
	CASE("01 02 00 00 10 c0 a4 00    FADD.f32 r0, r1, r2.neg", 0U,
	     VA_OP_FADD_F32 | va_source_register(1U) | va_field(va_source_register(2U), VA_SHIFT_SOURCE1) |
	     va_field(1U, VA_SHIFT_NEG_SOURCE1) | va_dest(0U));
	CASE("01 d0 00 00 00 c0 a4 00    FADD.f32 r0, r1, 0x3F800000", 0U,
	     VA_OP_FADD_F32 | va_source_register(1U) | va_field(va_source_immediate(VA_IMMEDIATE_FLOAT_ONE), VA_SHIFT_SOURCE1) | va_dest(0U));
	CASE("c0 00 00 00 00 f6 10 01    IADD_IMM.i32 r54, 0x0, #0x0", 0U,
	     VA_OP_IADD_IMM_I32 | va_source_immediate(VA_IMMEDIATE_ZERO) | va_dest(54U));
	CASE("42 ab 4b 00 00 c2 10 01    IADD_IMM.i32 r2, r2^, #0x4BAB", 0x40U,
	     VA_OP_IADD_IMM_I32 | va_source_register(2U) | va_field(0x4BABU, VA_SHIFT_SOURCE1) | va_dest(2U));
	CASE("3c d0 ea 00 02 bc 7d 68    ATEST.discard @r60, r60, 0x3F800000, atest_datum.w0", 0U,
	     VA_OP_ATEST | va_source_register(60U) | va_field(va_source_immediate(VA_IMMEDIATE_FLOAT_ONE), VA_SHIFT_SOURCE1) |
	     va_field(va_source_special(VA_SPECIAL_ATEST_DATUM, 0U), VA_SHIFT_SOURCE2) | va_staging(60U, 1U, VA_STAGING_WRITE) |
	     va_field(13U, VA_SHIFT_FLOW));
	CASE("f0 00 3c 33 04 40 7f 78    BLEND.slot0.v4.f16.end @r0:r1, blend_descriptor_0.w0, r60, target:0x0", 0U,
	     VA_OP_BLEND | va_source_special(VA_SPECIAL_BLEND_DESCRIPTOR_0, 0U) | va_field(va_source_register(60U), VA_SHIFT_SOURCE2) |
	     va_field(3U, VA_SHIFT_REGISTER_FORMAT) | va_field(3U, VA_SHIFT_VECTOR_SIZE) | va_staging(0U, 2U, VA_STAGING_READ) |
	     va_field(VA_FLOW_END, VA_SHIFT_FLOW));
	CASE("7b 0d 00 40 04 84 5e 08    LEA_BUF_IMM.slot1.wait0 @r4:r5, r59^, table:0xD, index:0x0", 0x40U,
	     VA_OP_LEA_BUF_IMM | va_source_register(59U) | va_field(VA_TABLE_61_FOLDED, VA_SHIFT_BUF_TABLE) |
	     va_field(1U, VA_SHIFT_SLOT) | va_staging(4U, 2U, VA_STAGING_WRITE) | va_field(1U, VA_SHIFT_FLOW));
	CASE("41 88 c0 00 04 c1 b2 00    FMA.f32 r1, r1^, u4.w0, 0x0.neg", 0x40U,
	     VA_OP_FMA_F32 | va_source_register(1U) | va_field(va_source_uniform(8U), VA_SHIFT_SOURCE1) |
	     va_field(va_source_immediate(VA_IMMEDIATE_ZERO), VA_SHIFT_SOURCE2) | va_field(1U, VA_SHIFT_NEG_SOURCE2) | va_dest(1U));
	CASE("44 00 00 39 08 48 61 78    STORE.i128.istream.slot0.end @r8:r9:r10:r11, [r4^:r5^], offset:0", 0x40U,
	     VA_OP_STORE_I128 | va_source_register(4U) | va_field(VA_MEMORY_ISTREAM, VA_SHIFT_MEMORY_ACCESS) |
	     va_staging(8U, 4U, VA_STAGING_READ) | va_field(VA_FLOW_END, VA_SHIFT_FLOW));
	CASE("80 00 00 18 82 82 60 00    LOAD.i32.unsigned.slot0 @r2, u0, offset:0", 0U,
	     VA_OP_LOAD_I32 | va_source_uniform(0U) | va_field(1U, VA_SHIFT_LOAD_UNSIGNED) | va_staging(2U, 1U, VA_STAGING_WRITE));
	CASE("80 00 00 28 f4 82 60 00    LOAD.i64.unsigned.slot0 @r2:r3, u0, offset:0", 0U,
	     VA_OP_LOAD_I64 | va_field(VA_LOAD_LANE_64_IDENTITY, VA_SHIFT_LOAD_LANE) | va_source_uniform(0U) |
	     va_field(1U, VA_SHIFT_LOAD_UNSIGNED) | va_staging(2U, 2U, VA_STAGING_WRITE));
	CASE("80 00 00 30 e6 82 60 00    LOAD.i96.unsigned.slot0 @r2:r3:r4, u0, offset:0", 0U,
	     VA_OP_LOAD_I96 | va_field(VA_LOAD_LANE_96_IDENTITY, VA_SHIFT_LOAD_LANE) | va_source_uniform(0U) |
	     va_field(1U, VA_SHIFT_LOAD_UNSIGNED) | va_staging(2U, 3U, VA_STAGING_WRITE));
	CASE("80 00 00 38 f8 84 60 00    LOAD.i128.unsigned.slot0 @r4:r5:r6:r7, u0, offset:0", 0U,
	     VA_OP_LOAD_I128 | va_field(VA_LOAD_LANE_128_IDENTITY, VA_SHIFT_LOAD_LANE) | va_source_uniform(0U) |
	     va_field(1U, VA_SHIFT_LOAD_UNSIGNED) | va_staging(4U, 4U, VA_STAGING_WRITE));
	CASE("3d 00 10 72 18 84 5c 00    LD_VAR_BUF_IMM.f32.slot1.v4.src_f32.center.retrieve @r4:r5:r6:r7, r61, index:0x10", 0U,
	     VA_OP_LD_VAR_BUF_IMM_F32 | va_source_register(61U) | va_field(0x10U, VA_SHIFT_VARYING_INDEX) |
	     va_field(VA_SOURCE_FORMAT_F32, VA_SHIFT_SOURCE_FORMAT) | va_field(3U, VA_SHIFT_VECTOR_SIZE) | va_field(1U, VA_SHIFT_SLOT) |
	     va_field(1U, VA_SHIFT_UPDATE) | va_staging(4U, 4U, VA_STAGING_WRITE));
	CASE("7c 7d 11 33 04 80 66 00    LD_ATTR_IMM.v4.f16.slot0 @r0:r1, r60^, r61^, index:0x1, table:0x1", 0x4040U,
	     VA_OP_LD_ATTR_IMM | va_source_register(60U) | va_field(va_source_register(61U), VA_SHIFT_SOURCE1) |
	     va_field(1U, VA_SHIFT_ATTR_TABLE) | va_field(1U, VA_SHIFT_ATTR_INDEX) | va_field(3U, VA_SHIFT_REGISTER_FORMAT) |
	     va_field(3U, VA_SHIFT_VECTOR_SIZE) | va_staging(0U, 2U, VA_STAGING_WRITE));
	CASE("80 00 c0 17 34 7c 25 01    TEX_FETCH.slot0.f.32.2d @r0:r1:r2:r3, @r60:r61, u0", 0U,
	     VA_OP_TEX_FETCH | va_source_uniform(0U) | va_field(0U, VA_SHIFT_STAGING_DEST) | va_field(VA_WRITE_MASK_RGBA, VA_SHIFT_TEX_WRITE_MASK) |
	     va_field(VA_REGISTER_TYPE_F, VA_SHIFT_TEX_REGISTER_TYPE) | va_field(VA_DIMENSION_2D, VA_SHIFT_TEX_DIMENSION) |
	     va_field(2U, VA_SHIFT_STAGING_COUNT) | va_field(3U, VA_SHIFT_STAGING_WRITE_COUNT) | va_field(60U, VA_SHIFT_DEST) |
	     va_field(1U, VA_SHIFT_TEX_REGISTER_WIDTH));
	CASE("41 03 00 00 00 c0 1f 50    BRANCHZ.reconverge r1^, offset:3", 0x40U,
	     VA_OP_BRANCHZ | va_source_register(1U) | va_field(3U, VA_SHIFT_SOURCE1) | va_dest(0U) | va_field(VA_FLOW_RECONVERGE, VA_SHIFT_FLOW));
	CASE("40 44 80 00 01 c0 b8 00    MUX.i32 r0, r0^, r4^, u0.w0", 0x4040U,
	     VA_OP_MUX_I32 | va_source_register(0U) | va_field(va_source_register(4U), VA_SHIFT_SOURCE1) |
	     va_field(va_source_uniform(0U), VA_SHIFT_SOURCE2) | va_field(VA_MUX_INT_ZERO, VA_SHIFT_MUX) | va_dest(0U));
	CASE("42 00 0d 80 40 c2 90 00    FROUND.f32.rtn r2, r2^.neg", 0x40U,
	     VA_OP_FROUND_F32 | va_source_register(2U) | va_field(1U, VA_SHIFT_NEG_SOURCE0) | va_field(VA_ROUND_RTN, VA_SHIFT_ROUND_MODE) | va_dest(2U));
	CASE("80 00 27 20 00 c2 a3 01    SHADDX.u64 [r2:r3], u0, [r0:r1].w0, shift:0x2", 0U,
	     VA_OP_SHADDX_U64 | va_source_uniform(0U) | va_field(va_source_register(0U), VA_SHIFT_SOURCE1) | va_field(2U, VA_SHIFT_SHADDX_SHIFT) |
	     va_field(VA_WIDEN64_W0, VA_SHIFT_WIDEN_SOURCE1) | va_dest(2U));
	CASE("77 c0 05 10 00 c2 a1 00    MKVEC.v2i16 r2, r55^.h1, 0x0.h0", 0x40U,
	     VA_OP_MKVEC_V2I16 | va_source_register(55U) | va_field(va_source_immediate(VA_IMMEDIATE_ZERO), VA_SHIFT_SOURCE1) |
	     va_field(1U, VA_SHIFT_LANE_SOURCE0) | va_dest(2U));
	CASE("77 00 15 10 00 c2 90 00    U16_TO_F32 r2, r55^.h1", 0x40U,
	     VA_OP_U16_TO_F32 | va_source_register(55U) | va_field(1U, VA_SHIFT_LANE_SOURCE0) | va_dest(2U));
	CASE("00 82 c0 80 03 c1 f0 00    ICMP_OR.u32.ne.m1 r1, r0, u1.w0, 0x0", 0U,
	     VA_OP_ICMP_OR_U32 | va_source_register(0U) | va_field(va_source_uniform(2U), VA_SHIFT_SOURCE1) |
	     va_field(va_source_immediate(VA_IMMEDIATE_ZERO), VA_SHIFT_SOURCE2) | va_field(VA_CONDITION_NE, VA_SHIFT_CONDITION) |
	     va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE) | va_dest(1U));
	CASE("4b c0 4c 10 01 cb b4 08    LSHIFT_AND.i32.wait0 r11, r11^, 0x0.b00, r12^", 0x404040U,
	     VA_OP_LSHIFT_AND_I32 | va_source_register(11U) | va_field(va_source_immediate(VA_IMMEDIATE_ZERO), VA_SHIFT_SOURCE1) |
	     va_field(va_source_register(12U), VA_SHIFT_SOURCE2) | va_field(VA_LANES_B00, VA_SHIFT_LANES) | va_dest(11U) | va_field(1U, VA_SHIFT_FLOW));
	CASE("00 00 00 00 00 c0 00 78    NOP.end", 0U,
	     VA_OP_NOP | va_dest(0U) | va_field(VA_FLOW_END, VA_SHIFT_FLOW));
#undef CASE

	failed = 0U;
	for (index = 0U; index < count; index++) {
		entry = decode_index(cases[index].built);
		if ((cases[index].built & ~cases[index].ignored) != (cases[index].expected & ~cases[index].ignored)) {
			printf("FAIL encode %s: built %016llx\n", cases[index].text, (unsigned long long)cases[index].built);
			failed++;
		} else if (entry < 0 || strncmp(cases[index].text + 27, isa_table[entry].name, strlen(isa_table[entry].name)) != 0) {
			printf("FAIL encode %s: decodes as %s\n", cases[index].text, entry < 0 ? "nothing" : isa_table[entry].name);
			failed++;
		}
	}
	printf("%s encoder: %u known words, %u wrong\n", failed == 0U ? "PASS" : "FAIL", count, failed);
	return failed != 0U;
}

/* Decodes every case of Mesa's assembler test file as the instruction it names. */
static int
check_cases_file(
	const char *path)
{
	char line[512];
	char name[128];
	FILE *file;
	unsigned total;
	unsigned failed;
	size_t length;
	int entry;

	file = fopen(path, "r");
	if (file == NULL) {
		printf("FAIL cases: cannot open %s\n", path);
		return 1;
	}
	total = 0U;
	failed = 0U;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strlen(line) < 30U || sscanf(line + 27, "%127s", name) != 1)
			continue;
		total++;
		entry = decode_index(word_of(line));
		length = entry < 0 ? 0U : strlen(isa_table[entry].name);
		if (entry < 0 || strncmp(name, isa_table[entry].name, length) != 0 ||
		    (name[length] != '\0' && name[length] != '.')) {
			/* The decoder's table skips nothing; a name it lacks is reported. */
			printf("FAIL case %.23s %s: decodes as %s\n", line, name, entry < 0 ? "nothing" : isa_table[entry].name);
			failed++;
		}
	}
	fclose(file);
	printf("%s cases: %u words of the assembler cases, %u not decoded as named\n", failed == 0U ? "PASS" : "FAIL", total, failed);
	return failed != 0U;
}

/* Prints a source byte. */
static void
print_source(
	FILE *out,
	uint8_t byte,
	uint32_t page)
{
	if (byte < 0x80U) {
		fprintf(out, "r%u", byte & 0x3FU);
	} else if (byte < 0xC0U) {
		fprintf(out, "u%u.w%u", page * 32U + ((byte >> 1) & 0x1FU), byte & 1U);
	} else if (byte < 0xE0U) {
		fprintf(out, "#%08x", isa_immediates[byte & 0x1FU]);
	} else {
		fprintf(out, "special%u.%u.w%u", page, (byte >> 1) & 0xFU, byte & 1U);
	}
}

/* Decodes a program, writing its listing; reports the words that do not decode. */
static int
decode_program(
	FILE *listing,
	const char *what,
	const uint8_t *code,
	size_t length)
{
	const struct isa_entry *entry;
	uint64_t word;
	unsigned failed;
	unsigned flow;
	unsigned source;
	size_t index;
	size_t last;
	int found;

	/* The program ends at its last nonzero word (the rest is padding). */
	last = 0U;
	for (index = 0U; index + 8U <= length; index += 8U) {
		memcpy(&word, code + index, 8U);
		if (word != 0U)
			last = index + 8U;
	}
	failed = 0U;
	fprintf(listing, "%s:\n", what);
	for (index = 0U; index < last; index += 8U) {
		memcpy(&word, code + index, 8U);
		found = decode_index(word);
		flow = (unsigned)((word >> 59) & 0xFU);
		if (found < 0 || flow == 11U || flow == 14U) {
			fprintf(listing, "  %04zx: %016llx ???\n", index, (unsigned long long)word);
			failed++;
			continue;
		}
		entry = &isa_table[found];
		fprintf(listing, "  %04zx: %016llx %-20s flow %2u", index, (unsigned long long)word, entry->name, flow);
		if (entry->dests != 0U)
			fprintf(listing, " r%u", (unsigned)((word >> 40) & 0x3FU));
		if (entry->staging_read != 0U || entry->staging_write != 0U)
			fprintf(listing, " @r%u x%u", (unsigned)((word >> 40) & 0x3FU), (unsigned)((word >> 33) & 7U));
		for (source = 0U; source < entry->sources && source < 3U; source++) {
			fputc(' ', listing);
			print_source(listing, (uint8_t)(word >> (8U * source)), (unsigned)((word >> 57) & 3U));
		}
		fputc('\n', listing);
	}
	return (int)failed;
}

/* Compiles, decodes and runs one module. */
static int
test_module(
	const char *outdir,
	const char *path,
	unsigned trials)
{
	const char *name;
	uint32_t *words;
	uint32_t stage;
	size_t count;
	size_t at;
	int has_ubo;
	int failed;

	name = strrchr(path, '/');
	name = name == NULL ? path : name + 1;
	words = read_file(path, &count);
	if (words == NULL) {
		printf("FAIL %s: cannot read\n", name);
		return 1;
	}

	/* The stage is the execution model of the module's first entry point. */
	stage = MALI_SHADER_COMPUTE;
	for (at = 5U; at < count && (words[at] >> 16) != 0U; at += words[at] >> 16) {
		if ((words[at] & 0xFFFFU) == 15U && at + 1U < count) {
			if (words[at + 1U] == 0U)
				stage = MALI_SHADER_VERTEX;
			else if (words[at + 1U] == 4U)
				stage = MALI_SHADER_FRAGMENT;
			break;
		}
	}

	/* 2 and 3 as the driver compiles it, then with the vector uniform buffer loads for a module that reads a uniform buffer. */
	has_ubo = 0;
	failed = test_program(outdir, name, words, count, stage, 0U, trials, &has_ubo);
	if (failed == 0 && has_ubo != 0)
		failed = test_program(outdir, name, words, count, stage, MALI_SHADER_OPTION_VECTOR_UBO, trials, &has_ubo);
	free(words);
	return failed;
}

/*
 * Compiles a module with the option flags, writes its listing (NAME.txt,
 * or NAME.vector-ubo.txt with the vector uniform buffer loads), decodes
 * it and runs it on the model against the IR's reference; says whether
 * the program binds a uniform buffer.
 */
static int
test_program(
	const char *outdir,
	const char *name,
	const uint32_t *words,
	size_t count,
	uint32_t stage,
	uint32_t flags,
	unsigned trials,
	int *has_ubo)
{
	struct mali_shader_diagnostic diagnostic;
	struct mali_shader_options options;
	struct mali_shader_program *program;
	char listing_path[1024];
	const char *variant;
	uint32_t index;
	FILE *listing;
	int failed;
	int error;

	/* 2: compiles and decodes. */
	variant = "";
	if ((flags & MALI_SHADER_OPTION_VECTOR_UBO) != 0U)
		variant = " vector-ubo";
	options.flags = flags;
	program = NULL;
	error = mali_shader_compile(words, count, stage, flags != 0U ? &options : NULL, &program, &diagnostic);
	if (error != 0) {
		printf("FAIL %s%s: compile error %d at %u: %s\n", name, variant, error, diagnostic.instruction,
		       diagnostic.reason != NULL ? diagnostic.reason : "");
		return 1;
	}
	for (index = 0U; index < program->buffer_count; index++) {
		if (program->buffers[index].kind == MALI_SHADER_BUFFER_UNIFORM)
			*has_ubo = 1;
	}
	if (flags == 0U)
		snprintf(listing_path, sizeof(listing_path), "%s/%s.txt", outdir, name);
	else
		snprintf(listing_path, sizeof(listing_path), "%s/%s.vector-ubo.txt", outdir, name);
	listing = fopen(listing_path, "w");
	if (listing == NULL)
		listing = stdout;
	failed = 0;
	if (program->varying_offset != 0U) {
		failed += decode_program(listing, "position", program->code, program->varying_offset);
		failed += decode_program(listing, "varying", program->code + program->varying_offset, program->length - program->varying_offset);
	} else {
		failed += decode_program(listing, "program", program->code, program->length);
	}
	fprintf(listing, "registers %u preload %04x fau %u textures %u buffers %u varyings %08x (%u bytes) colors %02x\n",
		program->work_registers, program->preload, program->fau_words, program->texture_count, program->buffer_count,
		program->varying_mask, program->varying_bytes, program->color_mask);
	if (program->tls_bytes != 0U || program->image_count != 0U)
		fprintf(listing, "thread local storage %u bytes, storage images %u\n", program->tls_bytes, program->image_count);
	if (listing != stdout)
		fclose(listing);
	if (failed != 0) {
		printf("FAIL %s%s: %d words do not decode (%s)\n", name, variant, failed, listing_path);
		mali_shader_program_free(program);
		return 1;
	}

	/* 3: runs on the model against the IR's reference. */
	error = mali_model_check(words, count, stage, program, trials, name);
	mali_shader_program_free(program);
	if (error != 0) {
		printf("FAIL %s%s: the model disagrees (above)\n", name, variant);
		return 1;
	}
	printf("PASS %s%s\n", name, variant);
	return 0;
}

/* Reads a SPIR-V file. */
static uint32_t *
read_file(
	const char *path,
	size_t *words)
{
	uint32_t *buffer;
	FILE *file;
	long size;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	buffer = malloc((size_t)size + 4U);
	if (buffer == NULL || fread(buffer, 1U, (size_t)size, file) != (size_t)size) {
		fclose(file);
		free(buffer);
		return NULL;
	}
	fclose(file);
	*words = (size_t)size / 4U;
	return buffer;
}
