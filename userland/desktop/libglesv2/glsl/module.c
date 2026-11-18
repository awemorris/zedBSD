/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's SPIR-V module builder: sections of words in the
 * order a module needs them, ids, and types and constants declared once
 * each (found again by their words).
 */

#include "emit.h"

#include <string.h>

/* SPIR-V's magic number, the version (1.0) and the generator (none registered). */
#define MODULE_MAGIC		0x07230203U
#define MODULE_VERSION		0x00010000U
#define MODULE_GENERATOR	0U

/* The length of the module header in words. */
#define MODULE_HEADER		5U

static void module_grow(struct glsl_module *module, struct glsl_words *words, size_t more);
static uint32_t module_hash(const uint32_t *words, size_t count);
static uint32_t module_find(struct glsl_module *module, uint32_t hash, const uint32_t *words, size_t count);
static void module_remember(struct glsl_module *module, uint32_t hash, size_t offset, uint32_t length, uint32_t id);
static void module_append(struct glsl_module *module, uint32_t *out, size_t *at, const struct glsl_words *words);

/*
 * Starts an empty module whose memory comes from an arena, with the
 * GLSL.std.450 import and the Shader capability.
 */
void
glsl_module_init(
	struct glsl_module *module,
	struct glsl_arena *arena)
{
	uint32_t operands[2];

	/* Nothing yet; ids start at 1. */
	memset(module, 0, sizeof(*module));
	module->arena = arena;
	module->next_id = 1U;

	/* The Shader capability. */
	operands[0] = SPV_CAPABILITY_SHADER;
	glsl_words_add(module, &module->capabilities, SPV_OP_CAPABILITY, operands, 1U);

	/* The extended instructions of GLSL.std.450. */
	module->std450 = glsl_module_id(module);
	operands[0] = module->std450;
	glsl_words_string(module, &module->imports, SPV_OP_EXT_INST_IMPORT, operands, 1U, "GLSL.std.450");
}

/*
 * Hands out the next id.
 */
uint32_t
glsl_module_id(
	struct glsl_module *module)
{
	uint32_t id;

	/* The id, and the next one after it. */
	id = module->next_id;
	module->next_id++;

	/* Succeeded: the id. */
	return id;
}

/*
 * Appends an instruction (its opcode and operands) to a section.
 */
void
glsl_words_add(
	struct glsl_module *module,
	struct glsl_words *words,
	uint32_t opcode,
	const uint32_t *operands,
	unsigned count)
{
	/* Room for the instruction. */
	module_grow(module, words, (size_t)count + 1U);

	/* The first word holds the length and the opcode, then the operands. */
	words->data[words->count] = ((uint32_t)(count + 1U) << 16) | opcode;
	if (count != 0U)
		memcpy(words->data + words->count + 1U, operands, count * sizeof(uint32_t));
	words->count += (size_t)count + 1U;
}

/*
 * Appends an instruction whose last operand is a string (padded with
 * zeros to whole words, always terminated).
 */
void
glsl_words_string(
	struct glsl_module *module,
	struct glsl_words *words,
	uint32_t opcode,
	const uint32_t *before,
	unsigned before_count,
	const char *text)
{
	uint32_t operands[GLSL_MAX_OPERANDS];
	size_t length;
	size_t string_words;

	/* The string's words, at least one zero byte after it. */
	length = strlen(text);
	string_words = length / 4U + 1U;
	if (before_count + string_words > GLSL_MAX_OPERANDS) {
		length = (GLSL_MAX_OPERANDS - before_count - 1U) * 4U;
		string_words = length / 4U + 1U;
	}

	/* The operands before the string, then the string. */
	memset(operands, 0, sizeof(operands));
	memcpy(operands, before, before_count * sizeof(uint32_t));
	memcpy(operands + before_count, text, length);

	/* The instruction. */
	glsl_words_add(module, words, opcode, operands, before_count + (unsigned)string_words);
}

/*
 * Declares a type or a constant (an instruction whose first operand, or
 * second for a constant, is its result id) and returns its id; the same
 * words declared before give the same id, unless `unique` asks for a
 * type of its own (a struct or an array that will be decorated).
 *
 * `operands` are the instruction's operands with 0 in the result id's
 * place; `count` includes that place.
 */
uint32_t
glsl_module_declare(
	struct glsl_module *module,
	uint32_t opcode,
	const uint32_t *operands,
	unsigned count,
	int unique)
{
	uint32_t key[GLSL_MAX_OPERANDS + 1U];
	uint32_t words[GLSL_MAX_OPERANDS];
	uint32_t hash;
	uint32_t id;
	unsigned result_at;
	unsigned index;
	size_t offset;

	/* A constant's result id is its second operand, a type's its first. */
	result_at = 0U;
	if (opcode == SPV_OP_CONSTANT || opcode == SPV_OP_CONSTANT_COMPOSITE ||
	    opcode == SPV_OP_CONSTANT_TRUE || opcode == SPV_OP_CONSTANT_FALSE)
		result_at = 1U;

	/* The key: the opcode and the operands but the result id. */
	key[0] = opcode;
	for (index = 0U; index < count; index++) {
		key[index + 1U] = operands[index];
		if (index == result_at)
			key[index + 1U] = 0U;
	}

	/* The key's hash. */
	hash = module_hash(key, (size_t)count + 1U);

	/* The same declaration made before. */
	if (!unique) {
		id = module_find(module, hash, key, (size_t)count + 1U);
		if (id != 0U)
			return id;
	}

	/* A new declaration with a new id. */
	id = glsl_module_id(module);
	memcpy(words, operands, count * sizeof(uint32_t));
	words[result_at] = id;
	offset = module->globals.count;
	glsl_words_add(module, &module->globals, opcode, words, count);

	/* It is found again by its key (a unique one never is). */
	if (!unique)
		module_remember(module, hash, offset, count + 1U, id);

	/* Succeeded: the id. */
	return id;
}

/*
 * Returns a 32-bit scalar constant of a type (int, uint or float bits).
 */
uint32_t
glsl_module_constant(
	struct glsl_module *module,
	uint32_t type,
	uint32_t value)
{
	uint32_t operands[3];
	uint32_t id;

	/* OpConstant type id value. */
	operands[0] = type;
	operands[1] = 0U;
	operands[2] = value;
	id = glsl_module_declare(module, SPV_OP_CONSTANT, operands, 3U, 0);

	/* Succeeded: the constant. */
	return id;
}

/*
 * Returns a 64-bit scalar constant of a type (a double's bits): the low
 * word first, as SPIR-V orders a wide literal.
 */
uint32_t
glsl_module_constant64(
	struct glsl_module *module,
	uint32_t type,
	uint64_t value)
{
	uint32_t operands[4];
	uint32_t id;

	/* OpConstant type id low high. */
	operands[0] = type;
	operands[1] = 0U;
	operands[2] = (uint32_t)(value & 0xffffffffU);
	operands[3] = (uint32_t)(value >> 32);
	id = glsl_module_declare(module, SPV_OP_CONSTANT, operands, 4U, 0);

	/* Succeeded: the constant. */
	return id;
}

/*
 * Returns a composite constant of a type from constant parts.
 */
uint32_t
glsl_module_composite(
	struct glsl_module *module,
	uint32_t type,
	const uint32_t *parts,
	unsigned count)
{
	uint32_t operands[GLSL_MAX_OPERANDS];
	uint32_t id;

	/* OpConstantComposite type id parts... */
	operands[0] = type;
	operands[1] = 0U;
	memcpy(operands + 2, parts, count * sizeof(uint32_t));
	id = glsl_module_declare(module, SPV_OP_CONSTANT_COMPOSITE, operands, count + 2U, 0);

	/* Succeeded: the constant. */
	return id;
}

/*
 * Returns the bool constant true or false.
 */
uint32_t
glsl_module_bool(
	struct glsl_module *module,
	uint32_t type,
	int value)
{
	uint32_t operands[2];
	uint32_t opcode;
	uint32_t id;

	/* OpConstantTrue or OpConstantFalse. */
	operands[0] = type;
	operands[1] = 0U;
	opcode = SPV_OP_CONSTANT_FALSE;
	if (value)
		opcode = SPV_OP_CONSTANT_TRUE;
	id = glsl_module_declare(module, opcode, operands, 2U, 0);

	/* Succeeded: the constant. */
	return id;
}

/*
 * Decorates an id.
 */
void
glsl_module_decorate(
	struct glsl_module *module,
	uint32_t target,
	uint32_t decoration,
	const uint32_t *operands,
	unsigned count)
{
	uint32_t words[8];

	/* OpDecorate target decoration operands... */
	words[0] = target;
	words[1] = decoration;
	if (count > 6U)
		count = 6U;
	if (count != 0U)
		memcpy(words + 2, operands, count * sizeof(uint32_t));
	glsl_words_add(module, &module->decorations, SPV_OP_DECORATE, words, count + 2U);
}

/*
 * Decorates a member of a struct (with one operand, or none when the
 * decoration has none: pass SPV's "no operand" as ~0).
 */
void
glsl_module_member_decorate(
	struct glsl_module *module,
	uint32_t target,
	uint32_t member,
	uint32_t decoration,
	uint32_t operand)
{
	uint32_t words[4];
	unsigned count;

	/* OpMemberDecorate target member decoration [operand]. */
	words[0] = target;
	words[1] = member;
	words[2] = decoration;
	words[3] = operand;
	count = 4U;
	if (operand == 0xffffffffU)
		count = 3U;
	glsl_words_add(module, &module->decorations, SPV_OP_MEMBER_DECORATE, words, count);
}

/*
 * Names an id (libGLESv2's reflection finds interface variables by name).
 */
void
glsl_module_name(
	struct glsl_module *module,
	uint32_t target,
	const char *name)
{
	/* OpName target "name". */
	glsl_words_string(module, &module->names, SPV_OP_NAME, &target, 1U, name);
}

/*
 * Names a member of a struct.
 */
void
glsl_module_member_name(
	struct glsl_module *module,
	uint32_t target,
	uint32_t member,
	const char *name)
{
	uint32_t before[2];

	/* OpMemberName target member "name". */
	before[0] = target;
	before[1] = member;
	glsl_words_string(module, &module->names, SPV_OP_MEMBER_NAME, before, 2U, name);
}

/*
 * Joins the sections into one module (in the arena) and returns it and
 * its length in words.  The entry function's variables go at the start
 * of its first block, which the body section begins with.
 */
uint32_t *
glsl_module_finish(
	struct glsl_module *module,
	size_t *words)
{
	struct glsl_words memory_model;
	uint32_t operands[2];
	uint32_t *out;
	size_t total;
	size_t at;
	size_t first_block;

	/* The memory model: Logical GLSL450. */
	memset(&memory_model, 0, sizeof(memory_model));
	operands[0] = 0U;
	operands[1] = 1U;
	glsl_words_add(module, &memory_model, SPV_OP_MEMORY_MODEL, operands, 2U);

	/* The body starts with OpFunction (5 words) and OpLabel (2 words); the variables follow them. */
	first_block = 7U;
	if (module->body.count < first_block)
		first_block = module->body.count;

	/* The whole module's length. */
	total = MODULE_HEADER + module->capabilities.count + module->imports.count + memory_model.count +
		module->entry_points.count + module->modes.count + module->names.count + module->decorations.count +
		module->globals.count + module->variables.count + module->body.count;
	out = glsl_alloc(module->arena, total * sizeof(uint32_t));

	/* The header. */
	out[0] = MODULE_MAGIC;
	out[1] = MODULE_VERSION;
	out[2] = MODULE_GENERATOR;
	out[3] = module->next_id;
	out[4] = 0U;
	at = MODULE_HEADER;

	/* The sections in SPIR-V's order. */
	module_append(module, out, &at, &module->capabilities);
	module_append(module, out, &at, &module->imports);
	module_append(module, out, &at, &memory_model);
	module_append(module, out, &at, &module->entry_points);
	module_append(module, out, &at, &module->modes);
	module_append(module, out, &at, &module->names);
	module_append(module, out, &at, &module->decorations);
	module_append(module, out, &at, &module->globals);

	/* The function: its first instructions, the variables, then the rest. */
	memcpy(out + at, module->body.data, first_block * sizeof(uint32_t));
	at += first_block;
	module_append(module, out, &at, &module->variables);
	memcpy(out + at, module->body.data + first_block, (module->body.count - first_block) * sizeof(uint32_t));
	at += module->body.count - first_block;

	/* Succeeded: the module. */
	*words = at;
	return out;
}

/* Grows a section to hold some more words. */
static void
module_grow(
	struct glsl_module *module,
	struct glsl_words *words,
	size_t more)
{
	uint32_t *grown;
	size_t capacity;

	/* Enough room already. */
	if (words->count + more <= words->capacity)
		return;

	/* Double until it fits (the old array stays in the arena unused). */
	capacity = words->capacity * 2U;
	if (capacity < 256U)
		capacity = 256U;
	while (capacity < words->count + more)
		capacity *= 2U;
	grown = glsl_alloc(module->arena, capacity * sizeof(uint32_t));
	if (words->count != 0U)
		memcpy(grown, words->data, words->count * sizeof(uint32_t));
	words->data = grown;
	words->capacity = capacity;
}

/* Hashes words (FNV-1a over their bytes' values). */
static uint32_t
module_hash(
	const uint32_t *words,
	size_t count)
{
	uint32_t hash;
	size_t index;

	/* Each word mixed in. */
	hash = 2166136261U;
	for (index = 0U; index < count; index++) {
		hash ^= words[index];
		hash *= 16777619U;
	}

	/* Succeeded: the hash. */
	return hash;
}

/* Finds a declaration by its key (opcode and operands without the result id); 0 when there is none. */
static uint32_t
module_find(
	struct glsl_module *module,
	uint32_t hash,
	const uint32_t *words,
	size_t count)
{
	const struct glsl_module_entry *entry;
	const uint32_t *declared;
	unsigned index;
	unsigned operand;
	unsigned result_at;
	int same;

	/* The entries of the same hash and length. */
	for (index = 0U; index < module->entry_count; index++) {
		entry = &module->entries[index];
		if (entry->hash != hash || entry->length != count)
			continue;

		/* The declared words (the first holds the length and the opcode). */
		declared = module->globals.data + entry->offset;
		if ((declared[0] & 0xffffU) != words[0])
			continue;
		result_at = 1U;
		if (words[0] == SPV_OP_CONSTANT || words[0] == SPV_OP_CONSTANT_COMPOSITE ||
		    words[0] == SPV_OP_CONSTANT_TRUE || words[0] == SPV_OP_CONSTANT_FALSE)
			result_at = 2U;

		/* Every operand but the result id. */
		same = 1;
		for (operand = 1U; operand < count && same; operand++) {
			if (operand != result_at && declared[operand] != words[operand])
				same = 0;
		}

		/* The same declaration. */
		if (same)
			return entry->id;
	}

	/* Not declared. */
	return 0U;
}

/* Remembers a declaration so it is found again. */
static void
module_remember(
	struct glsl_module *module,
	uint32_t hash,
	size_t offset,
	uint32_t length,
	uint32_t id)
{
	struct glsl_module_entry *grown;
	unsigned capacity;

	/* Room for one more entry. */
	if (module->entry_count == module->entry_capacity) {
		capacity = module->entry_capacity * 2U;
		if (capacity < 64U)
			capacity = 64U;
		grown = glsl_alloc(module->arena, capacity * sizeof(*grown));
		if (module->entry_count != 0U)
			memcpy(grown, module->entries, module->entry_count * sizeof(*grown));
		module->entries = grown;
		module->entry_capacity = capacity;
	}

	/* The entry. */
	module->entries[module->entry_count].hash = hash;
	module->entries[module->entry_count].offset = offset;
	module->entries[module->entry_count].length = length;
	module->entries[module->entry_count].id = id;
	module->entry_count++;
}

/* Copies a section to the output. */
static void
module_append(
	struct glsl_module *module,
	uint32_t *out,
	size_t *at,
	const struct glsl_words *words)
{
	/* The words, if any. */
	(void)module;
	if (words->count == 0U)
		return;
	memcpy(out + *at, words->data, words->count * sizeof(uint32_t));
	*at += words->count;
}
