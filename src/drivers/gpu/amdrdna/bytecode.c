/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The runner of the video BIOS's command routines (amdrdna-bytecode.h).
 *
 * A routine starts with a header (its length, the dwords of workspace it
 * takes and the bytes of parameters it reads) and then its instructions.
 * An instruction is one opcode byte; most then take an attribute byte that
 * says where the source operand is (a register, a parameter, a workspace
 * dword, a scratch dword, a data table word, an immediate, or the PLL and
 * memory controller spaces, which this hardware does not have and which
 * read 0) and which bits of the destination and the source take part, and
 * the operands themselves.  A routine ends at its end instruction or at an
 * opcode the instruction set does not have.
 *
 * Every byte of the image is read through bounds checks: a routine that
 * reads past the image, runs more instructions than the runner allows or
 * calls deeper than it allows stops, and the run fails with the reason.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-bytecode.h"

/*
 * The image's marks and the places the runner follows: the option-ROM
 * mark, the vendor string at 0x30, the header's place at 0x48 and its
 * signature 4 bytes into it, the lists of routines and of data tables in
 * the header, the register programs' place in the data list, and each
 * list's entries after a 4-byte common header.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_BIOS_MAGIC 0xAA55, ATOM_ATI_MAGIC_PTR 0x30, ATOM_ATI_MAGIC
 * " 761295520", ATOM_ROM_TABLE_PTR 0x48, ATOM_ROM_MAGIC "ATOM",
 * ATOM_ROM_MAGIC_PTR 4, ATOM_ROM_CMD_PTR 0x1E, ATOM_ROM_DATA_PTR 0x20,
 * ATOM_DATA_IIO_PTR 0x32) and atom.c (the lists' entries at offset 4).
 */
#define BYTECODE_IMAGE_MARK		0xaa55U
#define BYTECODE_VENDOR_AT		0x30U
#define BYTECODE_VENDOR			" 761295520"
#define BYTECODE_VENDOR_BYTES		10U
#define BYTECODE_HEADER_POINTER		0x48U
#define BYTECODE_SIGNATURE_AT		4U
#define BYTECODE_SIGNATURE		"ATOM"
#define BYTECODE_SIGNATURE_BYTES	4U
#define BYTECODE_COMMAND_LIST_AT	0x1eU
#define BYTECODE_DATA_LIST_AT		0x20U
#define BYTECODE_IO_TABLE_AT		0x32U
#define BYTECODE_LIST_ENTRIES		4U

/*
 * A routine's header: its length, its workspace in dwords, its parameter
 * bytes (the low seven bits), and where its instructions start.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_CT_SIZE_PTR 0, ATOM_CT_WS_PTR 4, ATOM_CT_PS_PTR 5,
 * ATOM_CT_PS_MASK 0x7F, ATOM_CT_CODE_PTR 6).
 */
#define BYTECODE_ROUTINE_WORK_AT	4U
#define BYTECODE_ROUTINE_PARAMETERS_AT	5U
#define BYTECODE_ROUTINE_PARAMETERS	0x7fU
#define BYTECODE_ROUTINE_CODE_AT	6U

/*
 * The operand places (the low three bits of an attribute byte, and the
 * place an opcode's destination is in): a register, a parameter, a
 * workspace dword, a scratch dword, a data table dword, an immediate, the
 * PLL and the memory controller spaces.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_ARG_REG 0 .. ATOM_ARG_MC 7).
 */
#define PLACE_REGISTER			0U
#define PLACE_PARAMETER			1U
#define PLACE_WORK			2U
#define PLACE_SCRATCH			3U
#define PLACE_DATA			4U
#define PLACE_IMMEDIATE			5U
#define PLACE_PLL			6U
#define PLACE_MEMORY			7U

/*
 * The bits an operand covers (an attribute byte's bits 5:3 for the
 * source): the dword, the words at bits 0, 8 and 16, and the bytes at
 * bits 0, 8, 16 and 24; their masks and shifts; for each source field and
 * destination selector (bits 7:6) the field of the destination; and the
 * default destination field of an instruction that has no source.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_SRC_DWORD 0 .. ATOM_SRC_BYTE24 7) and atom.c (atom_arg_mask,
 * atom_arg_shift, atom_dst_to_src, atom_def_dst).
 */
#define FIELD_DWORD			0U
#define FIELD_WORD0			1U
#define FIELD_WORD8			2U
#define FIELD_WORD16			3U
#define FIELD_BYTE0			4U
#define FIELD_BYTE8			5U
#define FIELD_BYTE16			6U
#define FIELD_BYTE24			7U
#define FIELD_COUNT			8U

/*
 * Each field's mask and shift, the destination field of each source field
 * and selector, and each source field's default destination selector.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_arg_mask, atom_arg_shift, atom_dst_to_src, atom_def_dst).
 */
static const uint32_t field_masks[FIELD_COUNT] = {
	0xffffffffU, 0x0000ffffU, 0x00ffff00U, 0xffff0000U, 0x000000ffU, 0x0000ff00U, 0x00ff0000U, 0xff000000U
};
static const uint32_t field_shifts[FIELD_COUNT] = { 0U, 0U, 8U, 16U, 0U, 8U, 16U, 24U };
static const uint8_t destination_fields[FIELD_COUNT][4] = {
	{ 0U, 0U, 0U, 0U },
	{ 1U, 2U, 3U, 0U },
	{ 1U, 2U, 3U, 0U },
	{ 1U, 2U, 3U, 0U },
	{ 4U, 5U, 6U, 7U },
	{ 4U, 5U, 6U, 7U },
	{ 4U, 5U, 6U, 7U },
	{ 4U, 5U, 6U, 7U }
};
static const uint8_t default_destinations[FIELD_COUNT] = { 0U, 0U, 1U, 2U, 0U, 1U, 2U, 3U };

/*
 * The workspace dwords at 0x40 and up that are the runner's state: the
 * quotient (the product's low dword), the remainder (its high dword), the
 * data table base, the shift, one bit at the shift, every bit but it, the
 * scratch window, the register access attribute and the register block.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_WS_QUOTIENT 0x40 .. ATOM_WS_REGPTR 0x48).
 */
#define WORK_QUOTIENT			0x40U
#define WORK_REMAINDER			0x41U
#define WORK_DATA_BLOCK			0x42U
#define WORK_SHIFT			0x43U
#define WORK_ONE_BIT			0x44U
#define WORK_ALL_BUT_BIT		0x45U
#define WORK_SCRATCH_WINDOW		0x46U
#define WORK_ATTRIBUTES			0x47U
#define WORK_REGISTER_BLOCK		0x48U

/*
 * The register access kinds: plain (through the register space), PCI
 * configuration and system I/O (which the runner does not do), and a
 * register program (0x80 with the program's number).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_IO_MM 0, ATOM_IO_PCI 1, ATOM_IO_SYSIO 2, ATOM_IO_IIO 0x80).
 */
#define IO_PLAIN			0U
#define IO_PCI				1U
#define IO_SYSTEM			2U
#define IO_PROGRAM			0x80U
#define IO_PROGRAM_NUMBER		0x7fU

/*
 * The register programs' instructions (nothing, the start of a program,
 * read, write, clear bits, set bits, move bits of the index, of the
 * attribute word or of the data, end) and each one's length in bytes.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_IIO_NOP 0 .. ATOM_IIO_END 9) and atom.c (atom_iio_len).
 */
#define PROGRAM_NOTHING			0U
#define PROGRAM_START			1U
#define PROGRAM_READ			2U
#define PROGRAM_WRITE			3U
#define PROGRAM_CLEAR			4U
#define PROGRAM_SET			5U
#define PROGRAM_MOVE_INDEX		6U
#define PROGRAM_MOVE_ATTRIBUTES		7U
#define PROGRAM_MOVE_DATA		8U
#define PROGRAM_END			9U
#define PROGRAM_OPCODES			10U

static const uint8_t program_lengths[PROGRAM_OPCODES] = { 1U, 2U, 3U, 3U, 3U, 3U, 4U, 4U, 4U, 3U };

/*
 * A branch table's entry mark and its end mark.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.h
 * (ATOM_CASE_MAGIC 0x63, ATOM_CASE_END 0x5A5A).
 */
#define BRANCH_ENTRY			0x63U
#define BRANCH_END			0x5a5aU

/*
 * The value an operand read of a place that does not exist gives (the
 * reference's placeholder).
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_get_src_int, atom_iio_execute: the value before a read).
 */
#define PLACEHOLDER			0xcdcdcdcdU

/*
 * What an instruction does.  Each opcode is one of these with the place
 * of its destination (an operand place), its condition (a branch) or its
 * unit (a wait).
 */
enum operation {
	OPERATION_NONE,
	OPERATION_COPY,
	OPERATION_AND,
	OPERATION_OR,
	OPERATION_SHIFT_LEFT_BY,
	OPERATION_SHIFT_RIGHT_BY,
	OPERATION_MULTIPLY,
	OPERATION_DIVIDE,
	OPERATION_ADD,
	OPERATION_SUBTRACT,
	OPERATION_SELECT_PLAIN_OR_PROGRAM,
	OPERATION_SELECT_PCI,
	OPERATION_SELECT_SYSTEM,
	OPERATION_SET_REGISTER_BLOCK,
	OPERATION_SET_SCRATCH_WINDOW,
	OPERATION_COMPARE,
	OPERATION_BRANCH_TABLE,
	OPERATION_GO,
	OPERATION_TEST_BITS,
	OPERATION_WAIT,
	OPERATION_CALL,
	OPERATION_ZERO,
	OPERATION_NOTHING,
	OPERATION_END,
	OPERATION_KEEP_AND_SET,
	OPERATION_SKIP_BYTE,
	OPERATION_MISSING,
	OPERATION_SET_DATA_BLOCK,
	OPERATION_EXCLUSIVE_OR,
	OPERATION_SHIFT_LEFT,
	OPERATION_SHIFT_RIGHT,
	OPERATION_SKIP_DATA,
	OPERATION_MULTIPLY_WIDE,
	OPERATION_DIVIDE_WIDE
};

/* The conditions of a branch: above, above or equal, always, below, below or equal, equal, not equal. */
#define WHEN_ABOVE			0U
#define WHEN_ABOVE_OR_EQUAL		1U
#define WHEN_ALWAYS			2U
#define WHEN_BELOW			3U
#define WHEN_BELOW_OR_EQUAL		4U
#define WHEN_EQUAL			5U
#define WHEN_NOT_EQUAL			6U

/* The units of a wait: microseconds and milliseconds. */
#define UNIT_MICROSECONDS		0U
#define UNIT_MILLISECONDS		1U

/* One opcode: what it does and its place, condition or unit. */
struct opcode {
	uint8_t operation;
	uint8_t detail;
};

/*
 * The opcodes 0 to 126 (0 and 127 up end a routine).  Opcodes 1 to 54 run
 * in groups of six destinations (register, parameter, workspace, scratch,
 * PLL, memory controller) for the copy, and, or, the two shifts by an
 * immediate count, multiply, divide, add and subtract; then the register
 * access selections, the register block and scratch window, the six
 * comparisons, the branch table, the seven branches, the six bit tests,
 * the two waits, the call, an instruction the runner does not have, the
 * six zeroings, nothing, the end, the six keep-and-set masks, the
 * progress code, a beep, two more the runner does not have, the data
 * block, the six exclusive ors, the six left and six right shifts by an
 * operand, the debug code, the data skip, and the wide multiply and divide
 * of a parameter or a workspace dword.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (opcode_table) and atom.h (ATOM_OP_CNT 127, ATOM_OP_EOT 91).
 */
#define OPCODES				127U
#define GROUP(operation) \
	{ operation, PLACE_REGISTER }, { operation, PLACE_PARAMETER }, { operation, PLACE_WORK }, \
	{ operation, PLACE_SCRATCH }, { operation, PLACE_PLL }, { operation, PLACE_MEMORY }

static const struct opcode opcodes[OPCODES] = {
	{ OPERATION_NONE, 0U },
	GROUP(OPERATION_COPY),
	GROUP(OPERATION_AND),
	GROUP(OPERATION_OR),
	GROUP(OPERATION_SHIFT_LEFT_BY),
	GROUP(OPERATION_SHIFT_RIGHT_BY),
	GROUP(OPERATION_MULTIPLY),
	GROUP(OPERATION_DIVIDE),
	GROUP(OPERATION_ADD),
	GROUP(OPERATION_SUBTRACT),
	{ OPERATION_SELECT_PLAIN_OR_PROGRAM, 0U },
	{ OPERATION_SELECT_PCI, 0U },
	{ OPERATION_SELECT_SYSTEM, 0U },
	{ OPERATION_SET_REGISTER_BLOCK, 0U },
	{ OPERATION_SET_SCRATCH_WINDOW, 0U },
	GROUP(OPERATION_COMPARE),
	{ OPERATION_BRANCH_TABLE, 0U },
	{ OPERATION_GO, WHEN_ALWAYS },
	{ OPERATION_GO, WHEN_EQUAL },
	{ OPERATION_GO, WHEN_BELOW },
	{ OPERATION_GO, WHEN_ABOVE },
	{ OPERATION_GO, WHEN_BELOW_OR_EQUAL },
	{ OPERATION_GO, WHEN_ABOVE_OR_EQUAL },
	{ OPERATION_GO, WHEN_NOT_EQUAL },
	GROUP(OPERATION_TEST_BITS),
	{ OPERATION_WAIT, UNIT_MILLISECONDS },
	{ OPERATION_WAIT, UNIT_MICROSECONDS },
	{ OPERATION_CALL, 0U },
	{ OPERATION_MISSING, 0U },
	GROUP(OPERATION_ZERO),
	{ OPERATION_NOTHING, 0U },
	{ OPERATION_END, 0U },
	GROUP(OPERATION_KEEP_AND_SET),
	{ OPERATION_SKIP_BYTE, 0U },
	{ OPERATION_NOTHING, 0U },
	{ OPERATION_MISSING, 0U },
	{ OPERATION_MISSING, 0U },
	{ OPERATION_SET_DATA_BLOCK, 0U },
	GROUP(OPERATION_EXCLUSIVE_OR),
	GROUP(OPERATION_SHIFT_LEFT),
	GROUP(OPERATION_SHIFT_RIGHT),
	{ OPERATION_SKIP_BYTE, 0U },
	{ OPERATION_SKIP_DATA, 0U },
	{ OPERATION_MULTIPLY_WIDE, PLACE_PARAMETER },
	{ OPERATION_MULTIPLY_WIDE, PLACE_WORK },
	{ OPERATION_DIVIDE_WIDE, PLACE_PARAMETER },
	{ OPERATION_DIVIDE_WIDE, PLACE_WORK }
};

/* The data block selector that names the running routine itself, and the one that names none. */
#define DATA_BLOCK_NONE			0U
#define DATA_BLOCK_ROUTINE		255U

/* The deepest call, and the instructions one run takes at most by default. */
#define BYTECODE_DEPTH			16U
#define BYTECODE_STEPS			(1ULL << 24)

/* The microseconds of a millisecond. */
#define BYTECODE_US_PER_MS		1000U

/*
 * One routine being run: where its code starts (branch targets are
 * relative to it), its parameters (the caller's array from the routine's
 * first one) and its workspace, each with its dwords, and how many of the
 * parameters are its own (a routine it calls takes the ones after them).
 */
struct frame {
	uint32_t start;
	uint32_t *parameters;
	uint32_t parameter_words;
	uint32_t *work;
	uint32_t work_words;
	uint32_t own_parameters;
};

static int bytecode_routine(struct amdrdna_bytecode *runner, unsigned routine, uint32_t *parameters, uint32_t parameter_words);
static bool bytecode_step(struct amdrdna_bytecode *runner, struct frame *frame, uint32_t *at);
static void bytecode_arithmetic(struct amdrdna_bytecode *runner, struct frame *frame, const struct opcode *opcode, uint32_t *at);
static void bytecode_compare(struct amdrdna_bytecode *runner, struct frame *frame, const struct opcode *opcode, uint32_t *at);
static void bytecode_shift(struct amdrdna_bytecode *runner, struct frame *frame, const struct opcode *opcode, uint32_t *at);
static void bytecode_branch_table(struct amdrdna_bytecode *runner, struct frame *frame, uint32_t *at);
static void bytecode_go(struct amdrdna_bytecode *runner, struct frame *frame, unsigned condition, uint32_t *at);
static void bytecode_call(struct amdrdna_bytecode *runner, struct frame *frame, uint32_t *at);
static void bytecode_select(struct amdrdna_bytecode *runner, unsigned kind, uint32_t *at);
static void bytecode_data_block(struct amdrdna_bytecode *runner, struct frame *frame, uint32_t *at);
static uint32_t operand_read(struct amdrdna_bytecode *runner, struct frame *frame, unsigned place, unsigned field, uint32_t *at, uint32_t *whole);
static void operand_skip(struct amdrdna_bytecode *runner, unsigned place, unsigned field, uint32_t *at);
static uint32_t operand_immediate(struct amdrdna_bytecode *runner, unsigned field, uint32_t *at);
static void operand_write(struct amdrdna_bytecode *runner, struct frame *frame, unsigned place, unsigned field, uint32_t *at, uint32_t value, uint32_t whole);
static unsigned destination_field(uint8_t attributes);
static uint32_t register_read(struct amdrdna_bytecode *runner, uint32_t index);
static void register_write(struct amdrdna_bytecode *runner, uint32_t index, uint32_t value);
static uint32_t program_run(struct amdrdna_bytecode *runner, uint32_t start, uint32_t index, uint32_t data);
static uint32_t work_read(struct amdrdna_bytecode *runner, struct frame *frame, uint32_t index);
static void work_write(struct amdrdna_bytecode *runner, struct frame *frame, uint32_t index, uint32_t value);
static bool scratch_place(const struct amdrdna_bytecode *runner, uint32_t index, uint32_t *word);
static uint32_t image_byte(struct amdrdna_bytecode *runner, uint32_t at);
static uint32_t image_word(struct amdrdna_bytecode *runner, uint32_t at);
static uint32_t image_dword(struct amdrdna_bytecode *runner, uint32_t at);
static void bytecode_fail(struct amdrdna_bytecode *runner, const char *reason, uint32_t at);
static void index_programs(struct amdrdna_bytecode *runner);
static bool image_matches(const uint8_t *image, uint32_t length, uint32_t at, const char *text, uint32_t bytes);

/*
 * Opens a runner over an image: checks its marks, finds its lists and
 * indexes its register programs.  access is copied; scratch (scratch_bytes
 * bytes, NULL for none) stays the caller's.
 *
 * Returns 0, or EINVAL for an image without the marks or with its lists
 * outside it.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (amdgpu_atom_parse: the marks, the lists, atom_index_iio).
 */
int
amdrdna_bytecode_open(
	struct amdrdna_bytecode *runner,
	const uint8_t *image,
	uint32_t length,
	const struct amdrdna_bytecode_access *access,
	uint32_t *scratch,
	uint32_t scratch_bytes)
{
	uint32_t header;
	bool matches;

	/* Empty, with the image, the access and the scratch. */
	kern_memset(runner, 0, sizeof(*runner));
	runner->image = image;
	runner->length = length;
	runner->access = *access;
	runner->scratch = scratch;
	runner->scratch_bytes = scratch_bytes;
	runner->max_steps = BYTECODE_STEPS;

	/* The option-ROM mark and the vendor string. */
	if (image_word(runner, 0U) != BYTECODE_IMAGE_MARK)
		return EINVAL;
	matches = image_matches(image, length, BYTECODE_VENDOR_AT, BYTECODE_VENDOR, BYTECODE_VENDOR_BYTES);
	if (!matches)
		return EINVAL;

	/* The header and its signature. */
	header = image_word(runner, BYTECODE_HEADER_POINTER);
	matches = image_matches(image, length, header + BYTECODE_SIGNATURE_AT, BYTECODE_SIGNATURE, BYTECODE_SIGNATURE_BYTES);
	if (!matches)
		return EINVAL;

	/* The two lists, inside the image. */
	runner->command_list = (uint16_t)image_word(runner, header + BYTECODE_COMMAND_LIST_AT);
	runner->data_list = (uint16_t)image_word(runner, header + BYTECODE_DATA_LIST_AT);
	if (runner->failure != NULL)
		return EINVAL;
	if (runner->command_list == 0U || runner->data_list == 0U)
		return EINVAL;
	if ((uint32_t)runner->command_list + BYTECODE_LIST_ENTRIES > length || (uint32_t)runner->data_list + BYTECODE_LIST_ENTRIES > length)
		return EINVAL;

	/* The register programs. */
	index_programs(runner);
	if (runner->failure != NULL)
		return EINVAL;

	/* Succeeded: the runner can run the image's routines. */
	return 0;
}

/*
 * Runs routine (its number in the image's list) with parameters, which it
 * reads and writes (parameter_words dwords).  The data block, the register
 * block, the scratch window, the register access and the product start
 * anew; the counts are of this run.
 *
 * Returns 0; ENOENT for a routine the image lacks; EINVAL when the routine
 * read past the image, ran too many instructions, called too deep or
 * called a routine that failed (runner->failure says which); ENOMEM when
 * a workspace could not be had.
 *
 * The order of the operand reads and writes is the firmware interpreter's,
 * so that a routine reads and writes the registers in the same order and
 * with the same values; its quirks that change what is written (an operand
 * read of an unknown place gives 0xcdcdcdcd, a read-modify-write of the
 * bits outside a narrow destination) are kept.  Where it reads past its
 * arrays the runner gives the placeholder value instead.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (the operand reads and writes, each instruction's semantics, the
 * register programs, amdgpu_atom_execute_table and its _locked form, the
 * register program index) and atom.h.
 */
int
amdrdna_bytecode_run(

	struct amdrdna_bytecode *runner,
	unsigned routine,
	uint32_t *parameters,
	uint32_t parameter_words)
{
	int error;

	/* The state a run starts with. */
	runner->data_block = 0U;
	runner->register_block = 0U;
	runner->scratch_window = 0U;
	runner->io_mode = IO_PLAIN;
	runner->product_low = 0U;
	runner->product_high = 0U;

	/* The counts start over. */
	runner->steps = 0U;
	runner->reads = 0U;
	runner->writes = 0U;
	runner->waited_us = 0U;
	runner->depth = 0U;
	runner->deepest = 0U;
	runner->missing = 0U;
	runner->failure = NULL;
	runner->failed_at = 0U;

	/* The routine and the ones it calls. */
	error = bytecode_routine(runner, routine, parameters, parameter_words);
	if (error != 0)
		return error;

	/* Succeeded: the routine ran to its end. */
	return 0;
}

/* Reports whether the image has a routine of that number: an entry of its list that is not 0. */
bool
amdrdna_bytecode_has(
	const struct amdrdna_bytecode *runner,
	unsigned routine)
{
	uint32_t list_bytes;
	uint32_t at;
	uint32_t start;

	/* The list's entry, inside the list (its first word is its size) and the image. */
	list_bytes = (uint32_t)runner->image[runner->command_list] | ((uint32_t)runner->image[runner->command_list + 1U] << 8);
	at = (uint32_t)runner->command_list + BYTECODE_LIST_ENTRIES + 2U * routine;
	if (routine > 0xffU || at + 2U > runner->length)
		return false;
	if (at + 2U > (uint32_t)runner->command_list + list_bytes)
		return false;

	/* A routine is there when its entry is not 0. */
	start = (uint32_t)runner->image[at] | ((uint32_t)runner->image[at + 1U] << 8);
	if (start == 0U)
		return false;

	/* Succeeded: the image has the routine. */
	return true;
}

/*
 * Runs one routine (and, through its calls, the ones it calls): its
 * header, a workspace of its own, then its instructions until its end.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (amdgpu_atom_execute_table_locked).
 */
static int
bytecode_routine(
	struct amdrdna_bytecode *runner,
	unsigned routine,
	uint32_t *parameters,
	uint32_t parameter_words)
{
	struct frame frame;
	uint32_t base;
	uint32_t at;
	bool present;
	bool going;

	/* The routine's place; one the image lacks is not run. */
	present = amdrdna_bytecode_has(runner, routine);
	if (!present)
		return ENOENT;
	base = image_word(runner, (uint32_t)runner->command_list + BYTECODE_LIST_ENTRIES + 2U * routine);
	if (runner->failure != NULL)
		return EINVAL;
	if (base == 0U)
		return ENOENT;

	/* A call deeper than the runner allows stops the run. */
	if (runner->depth >= BYTECODE_DEPTH) {
		bytecode_fail(runner, "calls too deep", base);
		return EINVAL;
	}

	/* The frame: the code, the parameters, the workspace dwords the header asks for. */
	kern_memset(&frame, 0, sizeof(frame));
	frame.start = base;
	frame.parameters = parameters;
	frame.parameter_words = parameter_words;
	frame.own_parameters = (image_byte(runner, base + BYTECODE_ROUTINE_PARAMETERS_AT) & BYTECODE_ROUTINE_PARAMETERS) / 4U;
	frame.work_words = image_byte(runner, base + BYTECODE_ROUTINE_WORK_AT);
	if (runner->failure != NULL)
		return EINVAL;
	if (frame.work_words != 0U) {
		frame.work = kern_calloc(frame.work_words, sizeof(uint32_t));
		if (frame.work == NULL)
			return ENOMEM;
	}

	/* One level deeper. */
	runner->depth++;
	if (runner->depth > runner->deepest)
		runner->deepest = runner->depth;

	/* The instructions, until the end or a failure. */
	at = base + BYTECODE_ROUTINE_CODE_AT;
	going = true;
	while (going && runner->failure == NULL)
		going = bytecode_step(runner, &frame, &at);

	/* Back to the caller's level, the workspace given back. */
	runner->depth--;
	if (frame.work != NULL)
		kern_free(frame.work);

	/* Reports a routine that stopped. */
	if (runner->failure != NULL)
		return EINVAL;

	/* Succeeded: the routine ran to its end. */
	return 0;
}

/*
 * Runs the instruction at *at and moves *at past it (or to a branch's
 * target).  Returns false at the routine's end.
 */
static bool
bytecode_step(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	uint32_t *at)
{
	const struct opcode *opcode;
	uint32_t attributes;
	uint32_t code;
	uint32_t count;

	/* One more instruction; a run past the allowed count stops. */
	runner->steps++;
	if (runner->steps > runner->max_steps) {
		bytecode_fail(runner, "too many instructions", *at);
		return false;
	}

	/* The opcode; 0 and those past the instruction set end the routine. */
	code = image_byte(runner, *at);
	if (runner->failure != NULL)
		return false;
	(*at)++;
	if (code == 0U || code >= OPCODES)
		return false;
	opcode = &opcodes[code];

	/* What the opcode does. */
	switch (opcode->operation) {
	case OPERATION_COPY:
	case OPERATION_AND:
	case OPERATION_OR:
	case OPERATION_ADD:
	case OPERATION_SUBTRACT:
	case OPERATION_EXCLUSIVE_OR:
	case OPERATION_KEEP_AND_SET:
	case OPERATION_ZERO:
		bytecode_arithmetic(runner, frame, opcode, at);
		break;
	case OPERATION_MULTIPLY:
	case OPERATION_DIVIDE:
	case OPERATION_MULTIPLY_WIDE:
	case OPERATION_DIVIDE_WIDE:
	case OPERATION_COMPARE:
	case OPERATION_TEST_BITS:
		bytecode_compare(runner, frame, opcode, at);
		break;
	case OPERATION_SHIFT_LEFT_BY:
	case OPERATION_SHIFT_RIGHT_BY:
	case OPERATION_SHIFT_LEFT:
	case OPERATION_SHIFT_RIGHT:
		bytecode_shift(runner, frame, opcode, at);
		break;
	case OPERATION_SELECT_PLAIN_OR_PROGRAM:
	case OPERATION_SELECT_PCI:
	case OPERATION_SELECT_SYSTEM:
		bytecode_select(runner, opcode->operation, at);
		break;
	case OPERATION_SET_REGISTER_BLOCK:
		runner->register_block = (uint16_t)image_word(runner, *at);
		*at += 2U;
		break;
	case OPERATION_SET_SCRATCH_WINDOW:
		/* The window from a source operand after its attribute byte. */
		attributes = image_byte(runner, *at);
		*at += 1U;
		runner->scratch_window = operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		break;
	case OPERATION_BRANCH_TABLE:
		bytecode_branch_table(runner, frame, at);
		break;
	case OPERATION_GO:
		bytecode_go(runner, frame, opcode->detail, at);
		break;
	case OPERATION_WAIT:
		count = image_byte(runner, *at);
		*at += 1U;
		if (opcode->detail == UNIT_MILLISECONDS)
			count *= BYTECODE_US_PER_MS;
		runner->waited_us += count;
		runner->access.wait(runner->access.owner, count);
		break;
	case OPERATION_CALL:
		bytecode_call(runner, frame, at);
		break;
	case OPERATION_SET_DATA_BLOCK:
		bytecode_data_block(runner, frame, at);
		break;
	case OPERATION_SKIP_BYTE:
		*at += 1U;
		break;
	case OPERATION_SKIP_DATA:
		count = image_word(runner, *at);
		*at += count + 2U;
		break;
	case OPERATION_MISSING:
		runner->missing++;
		break;
	case OPERATION_END:
		return false;
	default:
		break;
	}

	/* Succeeded: the routine goes on. */
	return true;
}

/*
 * Runs an instruction that combines its destination with its source and
 * writes the result back: the copy, and, or, add, subtract, exclusive or,
 * keep-and-set (the destination's bits under an immediate mask, then the
 * source's) and zero (no source).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_move, atom_op_and, atom_op_or, atom_op_add, atom_op_sub,
 * atom_op_xor, atom_op_mask, atom_op_clear: the destination read for the
 * bits around it, then the source, then the write).
 */
static void
bytecode_arithmetic(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	const struct opcode *opcode,
	uint32_t *at)
{
	uint8_t attributes;
	uint32_t destination_at;
	uint32_t destination;
	uint32_t source;
	uint32_t whole;
	uint32_t mask;
	unsigned field;

	/* The attribute byte; zero takes its destination's default field. */
	attributes = (uint8_t)image_byte(runner, *at);
	*at += 1U;
	if (opcode->operation == OPERATION_ZERO) {
		attributes &= 0x38U;
		attributes |= (uint8_t)(default_destinations[attributes >> 3] << 6);
	}
	destination_at = *at;
	field = destination_field(attributes);

	/* The destination: a whole-dword copy does not read it; the others read it and the bits around it. */
	whole = PLACEHOLDER;
	destination = 0U;
	if (opcode->operation == OPERATION_COPY && ((attributes >> 3) & 7U) == FIELD_DWORD)
		operand_skip(runner, opcode->detail, field, at);
	else
		destination = operand_read(runner, frame, opcode->detail, field, at, &whole);

	/* The operation with its source (a zeroing has none). */
	switch (opcode->operation) {
	case OPERATION_COPY:
		destination = operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		break;
	case OPERATION_AND:
		destination &= operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		break;
	case OPERATION_OR:
		destination |= operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		break;
	case OPERATION_ADD:
		destination += operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		break;
	case OPERATION_SUBTRACT:
		destination -= operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		break;
	case OPERATION_EXCLUSIVE_OR:
		destination ^= operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		break;
	case OPERATION_KEEP_AND_SET:
		mask = operand_immediate(runner, (attributes >> 3) & 7U, at);
		source = operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);
		destination = (destination & mask) | source;
		break;
	default:
		destination = 0U;
		break;
	}

	/* The result into the destination's field. */
	operand_write(runner, frame, opcode->detail, field, &destination_at, destination, whole);
}

/*
 * Runs an instruction that reads two operands and keeps a result in the
 * runner's state rather than in an operand: the comparison and the bit
 * test (the flags), the multiplications and divisions (the product
 * words).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_compare, atom_op_test, atom_op_mul, atom_op_div,
 * atom_op_mul32, atom_op_div32).
 */
static void
bytecode_compare(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	const struct opcode *opcode,
	uint32_t *at)
{
	uint8_t attributes;
	uint32_t first;
	uint32_t second;
	uint64_t wide;

	/* The attribute byte, the first operand (the destination's place), the second. */
	attributes = (uint8_t)image_byte(runner, *at);
	*at += 1U;
	first = operand_read(runner, frame, opcode->detail, destination_field(attributes), at, NULL);
	second = operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);

	/* What is kept. */
	switch (opcode->operation) {
	case OPERATION_COMPARE:
		/* Equal, and above (unsigned). */
		runner->equal = false;
		if (first == second)
			runner->equal = true;
		runner->above = false;
		if (first > second)
			runner->above = true;
		break;
	case OPERATION_TEST_BITS:
		/* Equal when no bit is common; above is left as it is. */
		runner->equal = false;
		if ((first & second) == 0U)
			runner->equal = true;
		break;
	case OPERATION_MULTIPLY:
		runner->product_low = first * second;
		break;
	case OPERATION_MULTIPLY_WIDE:
		wide = (uint64_t)first * (uint64_t)second;
		runner->product_low = (uint32_t)wide;
		runner->product_high = (uint32_t)(wide >> 32);
		break;
	case OPERATION_DIVIDE:
		/* The quotient and the remainder; a division by 0 gives two zeros. */
		if (second == 0U) {
			runner->product_low = 0U;
			runner->product_high = 0U;
			break;
		}
		runner->product_low = first / second;
		runner->product_high = first % second;
		break;
	case OPERATION_DIVIDE_WIDE:
		/* The 64-bit dividend of the remainder above the operand; a division by 0 gives two zeros. */
		if (second == 0U) {
			runner->product_low = 0U;
			runner->product_high = 0U;
			break;
		}
		wide = ((uint64_t)runner->product_high << 32) | first;
		wide /= second;
		runner->product_low = (uint32_t)wide;
		runner->product_high = (uint32_t)(wide >> 32);
		break;
	default:
		break;
	}
}

/*
 * Runs a shift: by an immediate byte count in the destination's default
 * field, or by a source operand over the destination's whole dword, the
 * result cut to the destination's field.  A count of 32 or more gives 0.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_shift_left, atom_op_shift_right, atom_op_shl, atom_op_shr).
 */
static void
bytecode_shift(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	const struct opcode *opcode,
	uint32_t *at)
{
	uint8_t attributes;
	uint32_t destination_at;
	uint32_t destination;
	uint32_t whole;
	uint32_t count;
	unsigned field;
	bool by_operand;

	/* The attribute byte; a shift by an immediate takes its default field. */
	attributes = (uint8_t)image_byte(runner, *at);
	*at += 1U;
	by_operand = false;
	if (opcode->operation == OPERATION_SHIFT_LEFT || opcode->operation == OPERATION_SHIFT_RIGHT)
		by_operand = true;
	if (!by_operand) {
		attributes &= 0x38U;
		attributes |= (uint8_t)(default_destinations[attributes >> 3] << 6);
	}
	destination_at = *at;
	field = destination_field(attributes);

	/* The destination, then the count (an immediate byte, or the source operand's low byte). */
	whole = 0U;
	destination = operand_read(runner, frame, opcode->detail, field, at, &whole);
	if (by_operand) {
		destination = whole;
		count = operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL) & 0xffU;
	} else {
		count = operand_immediate(runner, FIELD_BYTE0, at);
	}

	/* The shift; a count past the dword clears it. */
	if (count >= 32U) {
		destination = 0U;
	} else if (opcode->operation == OPERATION_SHIFT_LEFT_BY || opcode->operation == OPERATION_SHIFT_LEFT) {
		destination <<= count;
	} else {
		destination >>= count;
	}

	/* A shift by an operand is cut to the destination's field. */
	if (by_operand) {
		destination &= field_masks[field];
		destination >>= field_shifts[field];
	}

	/* The result into the destination's field. */
	operand_write(runner, frame, opcode->detail, field, &destination_at, destination, whole);
}

/*
 * Runs a branch table: a source operand, then entries of an immediate
 * value (in the source's width) and a target until the end mark; the
 * first entry equal to the source branches.  A malformed entry stops the
 * table where it is (the next instruction is read from there, as the
 * reference does).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_switch).
 */
static void
bytecode_branch_table(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	uint32_t *at)
{
	uint8_t attributes;
	uint32_t source;
	uint32_t value;
	uint32_t target;

	/* The attribute byte and the source. */
	attributes = (uint8_t)image_byte(runner, *at);
	*at += 1U;
	source = operand_read(runner, frame, attributes & 7U, (attributes >> 3) & 7U, at, NULL);

	/* Each entry until the end mark. */
	while (runner->failure == NULL && image_word(runner, *at) != BRANCH_END) {
		/* An entry without its mark ends the table where it is. */
		if (image_byte(runner, *at) != BRANCH_ENTRY)
			return;
		*at += 1U;

		/* The value and the target; an equal value branches. */
		value = operand_read(runner, frame, PLACE_IMMEDIATE, (attributes >> 3) & 7U, at, NULL);
		target = image_word(runner, *at);
		if (value == source) {
			*at = frame->start + target;
			return;
		}
		*at += 2U;
	}

	/* No entry matched: past the end mark. */
	*at += 2U;
}

/*
 * Runs a branch: its target, relative to the routine's start, taken when
 * the last comparison's flags meet the condition.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_jump; its time limit on a loop is the runner's instruction
 * limit here).
 */
static void
bytecode_go(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	unsigned condition,
	uint32_t *at)
{
	uint32_t target;
	bool taken;

	/* The target. */
	target = image_word(runner, *at);
	*at += 2U;

	/* Whether the flags meet the condition. */
	taken = false;
	switch (condition) {
	case WHEN_ABOVE:
		taken = runner->above;
		break;
	case WHEN_ABOVE_OR_EQUAL:
		taken = runner->above || runner->equal;
		break;
	case WHEN_ALWAYS:
		taken = true;
		break;
	case WHEN_BELOW:
		taken = !(runner->above || runner->equal);
		break;
	case WHEN_BELOW_OR_EQUAL:
		taken = !runner->above;
		break;
	case WHEN_EQUAL:
		taken = runner->equal;
		break;
	case WHEN_NOT_EQUAL:
		taken = !runner->equal;
		break;
	default:
		break;
	}

	/* A taken branch moves to its target. */
	if (taken)
		*at = frame->start + target;
}

/*
 * Runs a call: the routine of the byte's number, when the image has it,
 * with the parameters after the caller's own.  A routine that fails stops
 * the run.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_calltable).
 */
static void
bytecode_call(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	uint32_t *at)
{
	uint32_t *parameters;
	uint32_t parameter_words;
	unsigned routine;
	bool present;
	int error;

	/* The routine's number; one the image lacks is not called. */
	routine = image_byte(runner, *at);
	*at += 1U;
	present = amdrdna_bytecode_has(runner, routine);
	if (!present)
		return;

	/* The parameters after the caller's own (none when the caller's are all its own). */
	parameters = frame->parameters;
	parameter_words = 0U;
	if (parameters != NULL && frame->own_parameters < frame->parameter_words) {
		parameters += frame->own_parameters;
		parameter_words = frame->parameter_words - frame->own_parameters;
	}

	/* The routine; its failure is the run's. */
	error = bytecode_routine(runner, routine, parameters, parameter_words);
	if (error != 0 && runner->failure == NULL)
		bytecode_fail(runner, "a called routine failed", *at);
}

/*
 * Selects the register access: plain or a register program (a 16-bit
 * port, 0 for plain), PCI configuration or system I/O (each with a byte
 * the runner skips).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_setport).
 */
static void
bytecode_select(
	struct amdrdna_bytecode *runner,
	unsigned kind,
	uint32_t *at)
{
	uint32_t port;

	/* Plain or a register program by its port; PCI or system I/O. */
	if (kind == OPERATION_SELECT_PLAIN_OR_PROGRAM) {
		port = image_word(runner, *at);
		*at += 2U;
		runner->io_mode = IO_PLAIN;
		if (port != 0U)
			runner->io_mode = IO_PROGRAM | port;
		return;
	}
	*at += 1U;
	runner->io_mode = IO_SYSTEM;
	if (kind == OPERATION_SELECT_PCI)
		runner->io_mode = IO_PCI;
}

/*
 * Sets the data block the data operands are relative to: none (0), the
 * running routine (255), or a data table of the image's list.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_op_setdatablock).
 */
static void
bytecode_data_block(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	uint32_t *at)
{
	uint32_t selector;

	/* The selector byte. */
	selector = image_byte(runner, *at);
	*at += 1U;

	/* None, the routine itself, or a table. */
	if (selector == DATA_BLOCK_NONE) {
		runner->data_block = 0U;
		return;
	}
	if (selector == DATA_BLOCK_ROUTINE) {
		runner->data_block = (uint16_t)frame->start;
		return;
	}
	runner->data_block = (uint16_t)image_word(runner, (uint32_t)runner->data_list + BYTECODE_LIST_ENTRIES + 2U * selector);
}

/*
 * Reads an operand of a place in a field: its whole dword goes to *whole
 * (when given) and the field's bits, shifted down, are the result; an
 * immediate is read in the field's width and is the result as it is.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_get_src_int).
 */
static uint32_t
operand_read(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	unsigned place,
	unsigned field,
	uint32_t *at,
	uint32_t *whole)
{
	uint32_t index;
	uint32_t word;
	uint32_t value;
	bool inside;

	/* What the place holds. */
	value = PLACEHOLDER;
	switch (place) {
	case PLACE_REGISTER:
		index = image_word(runner, *at) + runner->register_block;
		*at += 2U;
		value = register_read(runner, index);
		break;
	case PLACE_PARAMETER:
		index = image_byte(runner, *at);
		*at += 1U;
		if (frame->parameters != NULL && index < frame->parameter_words)
			value = frame->parameters[index];
		break;
	case PLACE_WORK:
		index = image_byte(runner, *at);
		*at += 1U;
		value = work_read(runner, frame, index);
		break;
	case PLACE_DATA:
		index = image_word(runner, *at);
		*at += 2U;
		value = image_dword(runner, index + runner->data_block);
		break;
	case PLACE_SCRATCH:
		index = image_byte(runner, *at);
		*at += 1U;
		inside = scratch_place(runner, index, &word);
		value = 0U;
		if (inside)
			value = runner->scratch[word];
		break;
	case PLACE_IMMEDIATE:
		value = operand_immediate(runner, field, at);
		return value;
	case PLACE_PLL:
	case PLACE_MEMORY:
		*at += 1U;
		value = 0U;
		break;
	default:
		break;
	}

	/* The whole dword, then the field's bits. */
	if (whole != NULL)
		*whole = value;
	value &= field_masks[field];
	value >>= field_shifts[field];

	/* Succeeded: the operand's value. */
	return value;
}

/* Moves past an operand of a place in a field without reading it. */
static void
operand_skip(
	struct amdrdna_bytecode *runner,
	unsigned place,
	unsigned field,
	uint32_t *at)
{
	/* Two bytes for a register or a data offset, one for the others, the field's width for an immediate. */
	switch (place) {
	case PLACE_REGISTER:
	case PLACE_DATA:
		*at += 2U;
		break;
	case PLACE_IMMEDIATE:
		(void)operand_immediate(runner, field, at);
		break;
	default:
		*at += 1U;
		break;
	}
}

/* Reads an immediate in a field's width: four bytes, two (a word field) or one (a byte field). */
static uint32_t
operand_immediate(
	struct amdrdna_bytecode *runner,
	unsigned field,
	uint32_t *at)
{
	uint32_t value;

	/* The width the field names. */
	if (field == FIELD_DWORD) {
		value = image_dword(runner, *at);
		*at += 4U;
	} else if (field <= FIELD_WORD16) {
		value = image_word(runner, *at);
		*at += 2U;
	} else {
		value = image_byte(runner, *at);
		*at += 1U;
	}

	/* Succeeded: the immediate. */
	return value;
}

/*
 * Writes value into a field of an operand of a place: the field's bits of
 * value over the other bits of whole (what the destination held, or the
 * placeholder of a copy that did not read it).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_put_dst).
 */
static void
operand_write(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	unsigned place,
	unsigned field,
	uint32_t *at,
	uint32_t value,
	uint32_t whole)
{
	uint32_t index;
	uint32_t word;
	bool inside;

	/* The field's bits over the rest of the dword. */
	value <<= field_shifts[field];
	value &= field_masks[field];
	value |= whole & ~field_masks[field];

	/* Into the place. */
	switch (place) {
	case PLACE_REGISTER:
		index = image_word(runner, *at) + runner->register_block;
		*at += 2U;
		register_write(runner, index, value);
		break;
	case PLACE_PARAMETER:
		index = image_byte(runner, *at);
		*at += 1U;
		if (frame->parameters != NULL && index < frame->parameter_words)
			frame->parameters[index] = value;
		break;
	case PLACE_WORK:
		index = image_byte(runner, *at);
		*at += 1U;
		work_write(runner, frame, index, value);
		break;
	case PLACE_SCRATCH:
		index = image_byte(runner, *at);
		*at += 1U;
		inside = scratch_place(runner, index, &word);
		if (inside)
			runner->scratch[word] = value;
		break;
	default:
		*at += 1U;
		break;
	}
}

/* Reports the field of an instruction's destination: its source field and its destination selector (bits 7:6). */
static unsigned
destination_field(
	uint8_t attributes)
{
	/* The table of the two. */
	return destination_fields[(attributes >> 3) & 7U][(attributes >> 6) & 3U];
}

/*
 * Reads a register through the selected access: plain, or a register
 * program; PCI configuration and system I/O read 0.
 */
static uint32_t
register_read(
	struct amdrdna_bytecode *runner,
	uint32_t index)
{
	uint32_t program;
	uint32_t value;

	/* The plain register space. */
	if (runner->io_mode == IO_PLAIN) {
		runner->reads++;
		value = runner->access.read(runner->access.owner, index);
		return value;
	}

	/* A register program, when the image has the selected one. */
	if ((runner->io_mode & IO_PROGRAM) == 0U)
		return 0U;
	program = runner->io_programs[runner->io_mode & IO_PROGRAM_NUMBER];
	if (program == 0U)
		return 0U;
	value = program_run(runner, program, index, 0U);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a register through the selected access: plain (index 0, the
 * indirect window's address, in bytes), or a register program; PCI
 * configuration and system I/O write nothing.
 */
static void
register_write(
	struct amdrdna_bytecode *runner,
	uint32_t index,
	uint32_t value)
{
	uint32_t program;

	/* The plain register space; the window's address is given in dwords. */
	if (runner->io_mode == IO_PLAIN) {
		runner->writes++;
		if (index == 0U)
			value <<= 2;
		runner->access.write(runner->access.owner, index, value);
		return;
	}

	/* A register program, when the image has the selected one. */
	if ((runner->io_mode & IO_PROGRAM) == 0U)
		return;
	program = runner->io_programs[runner->io_mode & IO_PROGRAM_NUMBER];
	if (program == 0U)
		return;
	(void)program_run(runner, program, index, value);
}

/*
 * Runs a register program from start: a working dword built from bits of
 * the index, the data and the attribute word, read from and written to
 * registers.  Returns the working dword at the program's end (0 for a
 * program with an instruction it does not have).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_iio_execute).
 */
static uint32_t
program_run(
	struct amdrdna_bytecode *runner,
	uint32_t start,
	uint32_t index,
	uint32_t data)
{
	uint32_t working;
	uint32_t at;
	uint32_t code;
	uint32_t bits;
	uint32_t from;
	uint32_t to;
	uint32_t mask;
	uint32_t source;

	/* Each instruction until the end. */
	working = PLACEHOLDER;
	at = start;
	while (runner->failure == NULL) {
		code = image_byte(runner, at);

		/* The instruction. */
		switch (code) {
		case PROGRAM_NOTHING:
			at += 1U;
			break;
		case PROGRAM_READ:
			runner->reads++;
			working = runner->access.read(runner->access.owner, image_word(runner, at + 1U));
			at += 3U;
			break;
		case PROGRAM_WRITE:
			runner->writes++;
			runner->access.write(runner->access.owner, image_word(runner, at + 1U), working);
			at += 3U;
			break;
		case PROGRAM_CLEAR:
		case PROGRAM_SET:
			/* A run of bits at a place cleared or set. */
			bits = image_byte(runner, at + 1U);
			to = image_byte(runner, at + 2U);
			mask = 0U;
			if (bits != 0U && bits <= 32U && to < 32U)
				mask = (0xffffffffU >> (32U - bits)) << to;
			if (code == PROGRAM_CLEAR)
				working &= ~mask;
			else
				working |= mask;
			at += 3U;
			break;
		case PROGRAM_MOVE_INDEX:
		case PROGRAM_MOVE_ATTRIBUTES:
		case PROGRAM_MOVE_DATA:
			/* A run of bits of the index, the attribute word or the data moved into place. */
			bits = image_byte(runner, at + 1U);
			from = image_byte(runner, at + 2U);
			to = image_byte(runner, at + 3U);
			source = index;
			if (code == PROGRAM_MOVE_ATTRIBUTES)
				source = runner->io_attributes;
			else if (code == PROGRAM_MOVE_DATA)
				source = data;
			mask = 0U;
			if (bits != 0U && bits <= 32U)
				mask = 0xffffffffU >> (32U - bits);
			if (to < 32U)
				working &= ~(mask << to);
			if (from < 32U && to < 32U)
				working |= ((source >> from) & mask) << to;
			at += 4U;
			break;
		case PROGRAM_END:
			return working;
		default:
			return 0U;
		}
	}

	/* A program that read past the image gives 0. */
	return 0U;
}

/* Reads a workspace dword or one of the runner's state dwords (0x40 up); one past the workspace is the placeholder. */
static uint32_t
work_read(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	uint32_t index)
{
	/* The state dwords. */
	switch (index) {
	case WORK_QUOTIENT:
		return runner->product_low;
	case WORK_REMAINDER:
		return runner->product_high;
	case WORK_DATA_BLOCK:
		return runner->data_block;
	case WORK_SHIFT:
		return runner->shift;
	case WORK_ONE_BIT:
		return (uint32_t)1U << (runner->shift & 31U);
	case WORK_ALL_BUT_BIT:
		return ~((uint32_t)1U << (runner->shift & 31U));
	case WORK_SCRATCH_WINDOW:
		return runner->scratch_window;
	case WORK_ATTRIBUTES:
		return runner->io_attributes;
	case WORK_REGISTER_BLOCK:
		return runner->register_block;
	default:
		break;
	}

	/* The routine's own workspace. */
	if (index < frame->work_words)
		return frame->work[index];

	/* Succeeded: no such dword. */
	return PLACEHOLDER;
}

/* Writes a workspace dword or one of the runner's state dwords (0x40 up; the two masks are read-only). */
static void
work_write(
	struct amdrdna_bytecode *runner,
	struct frame *frame,
	uint32_t index,
	uint32_t value)
{
	/* The state dwords. */
	switch (index) {
	case WORK_QUOTIENT:
		runner->product_low = value;
		return;
	case WORK_REMAINDER:
		runner->product_high = value;
		return;
	case WORK_DATA_BLOCK:
		runner->data_block = (uint16_t)value;
		return;
	case WORK_SHIFT:
		runner->shift = (uint8_t)value;
		return;
	case WORK_ONE_BIT:
	case WORK_ALL_BUT_BIT:
		return;
	case WORK_SCRATCH_WINDOW:
		runner->scratch_window = value;
		return;
	case WORK_ATTRIBUTES:
		runner->io_attributes = (uint16_t)value;
		return;
	case WORK_REGISTER_BLOCK:
		runner->register_block = (uint16_t)value;
		return;
	default:
		break;
	}

	/* The routine's own workspace. */
	if (index < frame->work_words)
		frame->work[index] = value;
}

/* Reports the scratch dword index of the window's dword index; false when it lies outside the scratch area. */
static bool
scratch_place(
	const struct amdrdna_bytecode *runner,
	uint32_t index,
	uint32_t *word)
{
	uint64_t byte;

	/* A runner without scratch has none. */
	if (runner->scratch == NULL)
		return false;

	/* The byte of the window and the index, inside the area. */
	byte = (uint64_t)runner->scratch_window + (uint64_t)index * 4U;
	if (byte + 4U > runner->scratch_bytes)
		return false;
	*word = (uint32_t)(runner->scratch_window / 4U) + index;

	/* Succeeded: the dword is inside. */
	return true;
}

/* Reads a byte of the image; one past it fails the run and reads 0. */
static uint32_t
image_byte(
	struct amdrdna_bytecode *runner,
	uint32_t at)
{
	/* Inside the image. */
	if (at >= runner->length) {
		bytecode_fail(runner, "read past the image", at);
		return 0U;
	}

	/* Succeeded: the byte. */
	return runner->image[at];
}

/* Reads a little-endian word of the image. */
static uint32_t
image_word(
	struct amdrdna_bytecode *runner,
	uint32_t at)
{
	uint32_t low;
	uint32_t high;

	/* Its two bytes. */
	low = image_byte(runner, at);
	high = image_byte(runner, at + 1U);

	/* Succeeded: the word. */
	return low | (high << 8);
}

/* Reads a little-endian dword of the image. */
static uint32_t
image_dword(
	struct amdrdna_bytecode *runner,
	uint32_t at)
{
	uint32_t low;
	uint32_t high;

	/* Its two words. */
	low = image_word(runner, at);
	high = image_word(runner, at + 2U);

	/* Succeeded: the dword. */
	return low | (high << 16);
}

/* Stops the run with a reason, at an image offset; the first reason is kept. */
static void
bytecode_fail(
	struct amdrdna_bytecode *runner,
	const char *reason,
	uint32_t at)
{
	/* The first failure. */
	if (runner->failure != NULL)
		return;
	runner->failure = reason;
	runner->failed_at = at;
}

/*
 * Indexes the image's register programs: from the data list's register
 * program table (after its 4-byte header), each program's start mark and
 * number, then its instructions to its end.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/atom.c
 * (atom_index_iio).
 */
static void
index_programs(
	struct amdrdna_bytecode *runner)
{
	uint32_t table;
	uint32_t at;
	uint32_t code;

	/* The table, when the image has one. */
	table = image_word(runner, (uint32_t)runner->data_list + BYTECODE_IO_TABLE_AT);
	if (table == 0U || runner->failure != NULL)
		return;
	at = table + BYTECODE_LIST_ENTRIES;

	/* Each program: its number, its start, its instructions to its end mark (three bytes). */
	while (runner->failure == NULL && image_byte(runner, at) == PROGRAM_START) {
		runner->io_programs[image_byte(runner, at + 1U)] = (uint16_t)(at + 2U);
		at += 2U;
		for (code = image_byte(runner, at);
		     runner->failure == NULL && code != PROGRAM_END;
		     code = image_byte(runner, at)) {
			/* An instruction the programs do not have ends the index. */
			if (code >= PROGRAM_OPCODES) {
				bytecode_fail(runner, "a register program has an unknown instruction", at);
				return;
			}
			at += program_lengths[code];
		}
		at += program_lengths[PROGRAM_END];
	}
}

/* Reports whether the image holds text's bytes at an offset. */
static bool
image_matches(
	const uint8_t *image,
	uint32_t length,
	uint32_t at,
	const char *text,
	uint32_t bytes)
{
	uint32_t index;

	/* Inside the image. */
	if (at > length || bytes > length - at)
		return false;

	/* Each byte. */
	for (index = 0U; index < bytes; index++) {
		if (image[at + index] != (uint8_t)text[index])
			return false;
	}

	/* Succeeded: the bytes are there. */
	return true;
}
