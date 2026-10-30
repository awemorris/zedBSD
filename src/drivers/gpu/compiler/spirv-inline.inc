/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inlining of function calls, included by spirv.c.
 *
 * The parser lowers one function, the entry point.  A parse with the
 * INLINE_CALLS feature of a module with more functions first rewrites the
 * module's words: every OpFunctionCall in the entry function is replaced by
 * the body of the function it calls (whose own calls were replaced first),
 * and the other functions are dropped.  The rest of the parse then reads
 * the rewritten words as it reads any module of one function.
 *
 * Each inlined body gets new ids for everything it defines (its labels and
 * results) past the module's bound, and its parameters become the call's
 * arguments.  Its Function-storage variables move to the entry function's
 * first block, where SPIR-V keeps every variable; one with an initializer
 * moves without it, and a store of the initializer takes its place.  A
 * function returns once, as its last instruction, and the call's result is
 * an OpCopyObject of what it returns.  A body of one block is spliced into
 * the calling block.  A body of several blocks splits the calling block: it
 * ends with a branch to the body's first block, and the instructions after
 * the call continue the body's last block, so the phis that named the
 * calling block as a parent name that last block instead.
 *
 * What is refused (ENOTSUP, with the call as the diagnostic): recursion, a
 * function that returns more than once or other than at its end, a
 * declaration without a body, and a body of several blocks called from a
 * loop's header or continue target (whose labels the loop's structure
 * names).  Decorations of the dropped functions' ids are not carried to the
 * new ids, but for NoContraction: the only one this parser interprets on a
 * function-local id, it is written again for each new id of a decorated
 * result.
 */

/* The opcodes only the inlining reads. */
#define OP_FUNCTION_PARAMETER 55U
#define OP_FUNCTION_CALL 57U
#define OP_COPY_MEMORY 63U
#define OP_COPY_MEMORY_SIZED 64U
#define OP_RETURN_VALUE 254U
#define OP_EMIT_STREAM_VERTEX 220U
#define OP_END_STREAM_PRIMITIVE 221U
#define OP_LIFETIME_START 256U
#define OP_LIFETIME_STOP 257U
#define OP_GROUP_NON_UNIFORM_BALLOT_BIT_COUNT 342U
#define OP_GROUP_NON_UNIFORM_IADD 349U
#define OP_GROUP_NON_UNIFORM_LOGICAL_XOR 362U
#define OP_TERMINATE_INVOCATION 4416U
#define OP_DEMOTE_TO_HELPER_INVOCATION 5380U
#define OP_DECORATE_ID 332U
#define OP_DECORATE_STRING 5632U

/* The most functions and parameters of one function the inlining takes. */
#define INLINE_MAX_FUNCTIONS 64U
#define INLINE_MAX_PARAMETERS 32U

/* Where an instruction's result id is: none, word 1 (a label), word 2. */
#define INLINE_RESULT_NONE 0U
#define INLINE_RESULT_LABEL 1U
#define INLINE_RESULT_TYPED 2U

/* A growing list of instruction words. */
struct drv_gpu_spirv_words {
	uint32_t *word;
	uint32_t count;
	uint32_t capacity;
};

/*
 * One function of the module: where its words are and, once expanded, its
 * variables and its blocks with every call inlined.
 */
struct drv_gpu_spirv_function {
	/* The function's id and the offsets of OpFunction, its first OpLabel and OpFunctionEnd. */
	uint32_t id;
	uint32_t begin;
	uint32_t body;
	uint32_t end;

	/* The ids of its parameters, in order. */
	uint32_t parameter[INLINE_MAX_PARAMETERS];
	uint32_t parameter_count;

	/* 0 not expanded, 1 being expanded (a call back to it is recursion), 2 expanded. */
	int state;

	/* The first block's variables (without initializers) and the blocks, OpLabel first. */
	struct drv_gpu_spirv_words variables;
	struct drv_gpu_spirv_words blocks;

	/* The id OpReturnValue returns (0 for OpReturn), the label of the last block, the blocks' count. */
	uint32_t result;
	uint32_t last_label;
	uint32_t labels;
};

/* The state of one module's inlining. */
struct drv_gpu_spirv_inliner {
	/* The module's words, their count and its id bound. */
	const uint32_t *code;
	uint32_t words;
	uint32_t bound;

	/* The next new id. */
	uint32_t next_id;

	/* The module's functions and the entry point's. */
	struct drv_gpu_spirv_function *functions;
	uint32_t function_count;
	uint32_t entry;

	/*
	 * An id's new id in the body being spliced: map[id], valid where
	 * stamp[id] is the splice's generation.
	 */
	uint32_t *map;
	uint32_t *stamp;
	uint32_t generation;

	/*
	 * Whether an id, of the module or new, is a result decorated
	 * NoContraction, and the offset just past the module's last
	 * decoration, where the new ids' decorations go.
	 */
	uint8_t *no_contraction;
	uint32_t annotations_end;

	/* The refused call, for the caller's diagnostic. */
	struct drv_gpu_compile_diagnostic diag;
};

static int drv_gpu_spirv_inline(const uint32_t *words, size_t word_count, uint32_t **out, size_t *out_count, struct drv_gpu_compile_diagnostic *diagnostic);
static int drv_gpu_spirv_inline_scan(struct drv_gpu_spirv_inliner *inliner);
static int drv_gpu_spirv_inline_expand(struct drv_gpu_spirv_inliner *inliner, struct drv_gpu_spirv_function *function);
static int drv_gpu_spirv_inline_splice(struct drv_gpu_spirv_inliner *inliner, struct drv_gpu_spirv_function *caller, const struct drv_gpu_spirv_function *callee, const uint32_t *call, uint32_t count, uint32_t *current);
static int drv_gpu_spirv_inline_copy(struct drv_gpu_spirv_inliner *inliner, struct drv_gpu_spirv_words *destination, const uint32_t *word, uint32_t count);
static int drv_gpu_spirv_inline_define(struct drv_gpu_spirv_inliner *inliner, const struct drv_gpu_spirv_words *list);
static uint32_t drv_gpu_spirv_inline_id(const struct drv_gpu_spirv_inliner *inliner, uint32_t id);
static void drv_gpu_spirv_inline_fix_phis(struct drv_gpu_spirv_words *blocks, const struct drv_gpu_spirv_words *fixes);
static int drv_gpu_spirv_inline_assemble(struct drv_gpu_spirv_inliner *inliner, uint32_t **out, size_t *out_count);
static int drv_gpu_spirv_inline_decorate(const struct drv_gpu_spirv_inliner *inliner, struct drv_gpu_spirv_words *module);
static void drv_gpu_spirv_inline_mark_dropped(struct drv_gpu_spirv_inliner *inliner);
static int drv_gpu_spirv_inline_names_dropped(const struct drv_gpu_spirv_inliner *inliner, const uint32_t *word, uint32_t count);
static struct drv_gpu_spirv_function *drv_gpu_spirv_inline_find(struct drv_gpu_spirv_inliner *inliner, uint32_t id);
static int drv_gpu_spirv_inline_push(struct drv_gpu_spirv_words *list, const uint32_t *word, uint32_t count);
static uint32_t drv_gpu_spirv_inline_result(uint32_t opcode);
static int drv_gpu_spirv_inline_literal(uint32_t opcode, uint32_t index);
static int drv_gpu_spirv_inline_terminator(uint32_t opcode);
static int drv_gpu_spirv_inline_refuse(struct drv_gpu_spirv_inliner *inliner, uint32_t offset, const char *reason);
static void drv_gpu_spirv_inline_release(struct drv_gpu_spirv_inliner *inliner);

/*
 * Rewrites a module of several functions into one whose entry function has
 * every call inlined; *out is NULL (and nothing is allocated) for a module
 * of one function or a header the parse refuses itself.
 */
static int
drv_gpu_spirv_inline(
	const uint32_t *words,
	size_t word_count,
	uint32_t **out,
	size_t *out_count,
	struct drv_gpu_compile_diagnostic *diagnostic)
{
	int error;
	struct drv_gpu_spirv_inliner inliner;
	struct drv_gpu_spirv_function *entry;
	uint32_t offset;
	uint32_t count;
	uint32_t functions;

	/* Nothing is rewritten unless the module has several functions. */
	*out = NULL;
	*out_count = 0U;

	/* A header the parse refuses is left to it. */
	if (words == NULL ||
	    word_count < SPIRV_HEADER_WORDS ||
	    word_count > UINT32_MAX ||
	    words[0] != SPIRV_MAGIC ||
	    words[3] == 0U ||
	    words[3] > SPIRV_MAX_BOUND)
		return 0;

	/* Counts the functions; a malformed length is left to the parse. */
	functions = 0U;
	offset = SPIRV_HEADER_WORDS;
	while (offset < word_count) {
		count = words[offset] >> 16;
		if (count == 0U || offset + count > word_count)
			return 0;
		if ((words[offset] & 0xFFFFU) == OP_FUNCTION)
			functions++;
		offset += count;
	}

	/* A module of one function is parsed as it is. */
	if (functions < 2U)
		return 0;

	/* Prepares the inlining's state and the table of new ids. */
	kern_memset(&inliner, 0, sizeof(inliner));
	inliner.code = words;
	inliner.words = (uint32_t)word_count;
	inliner.bound = words[3];
	inliner.next_id = words[3];
	inliner.map = kern_calloc(SPIRV_MAX_BOUND, sizeof(*inliner.map));
	inliner.stamp = kern_calloc(SPIRV_MAX_BOUND, sizeof(*inliner.stamp));
	inliner.no_contraction = kern_calloc(SPIRV_MAX_BOUND, sizeof(*inliner.no_contraction));
	inliner.functions = kern_calloc(INLINE_MAX_FUNCTIONS, sizeof(*inliner.functions));
	if (inliner.map == NULL ||
	    inliner.stamp == NULL ||
	    inliner.no_contraction == NULL ||
	    inliner.functions == NULL) {
		drv_gpu_spirv_inline_release(&inliner);
		return ENOMEM;
	}

	/* Finds the functions and the entry point's. */
	error = drv_gpu_spirv_inline_scan(&inliner);

	/* Expands the entry function, and through it every function it calls. */
	if (error == 0) {
		entry = &inliner.functions[inliner.entry];
		error = drv_gpu_spirv_inline_expand(&inliner, entry);
	}

	/* Writes the module with the expanded entry function alone. */
	if (error == 0)
		error = drv_gpu_spirv_inline_assemble(&inliner, out, out_count);

	/* A refusal names the call for the caller. */
	if (error == ENOTSUP && diagnostic != NULL)
		*diagnostic = inliner.diag;

	/* The functions' lists and the id table are only needed while rewriting. */
	drv_gpu_spirv_inline_release(&inliner);

	/* A failure leaves nothing for the caller. */
	if (error != 0)
		return error;

	/* Succeeded: *out holds the rewritten module. */
	return 0;
}

/* Records each function's words and parameters, and which is the entry point. */
static int
drv_gpu_spirv_inline_scan(
	struct drv_gpu_spirv_inliner *inliner)
{
	const uint32_t *word;
	struct drv_gpu_spirv_function *function;
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	uint32_t entry_id;
	uint32_t index;
	int found;

	/* Walks the module, opening a record at each OpFunction. */
	function = NULL;
	entry_id = 0U;
	offset = SPIRV_HEADER_WORDS;
	while (offset < inliner->words) {
		word = inliner->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/* The first entry point names the function the parse lowers. */
		if (opcode == OP_ENTRY_POINT && entry_id == 0U && count > 2U)
			entry_id = word[2];

		/* A result decorated NoContraction keeps it under every new id. */
		if (opcode == OP_DECORATE &&
		    count >= 3U &&
		    word[1] < SPIRV_MAX_BOUND &&
		    word[2] == DEC_NO_CONTRACTION)
			inliner->no_contraction[word[1]] = 1U;

		/* The decorations' section ends after the last one (SPIR-V 1.6, 2.4). */
		if (function == NULL &&
		    (opcode == OP_DECORATE ||
		     opcode == OP_MEMBER_DECORATE ||
		     opcode == OP_DECORATE_ID ||
		     opcode == OP_DECORATE_STRING))
			inliner->annotations_end = offset + count;

		/* A function opens a record. */
		if (opcode == OP_FUNCTION) {
			if (function != NULL || count < 5U)
				return EINVAL;
			if (inliner->function_count >= INLINE_MAX_FUNCTIONS) {
				/* Records the refusal in the caller's diagnostic. */
				return drv_gpu_spirv_inline_refuse(inliner, offset, "more functions than the inlining takes");
			}
			function = &inliner->functions[inliner->function_count];
			inliner->function_count++;
			function->id = word[2];
			function->begin = offset;
		} else if (opcode == OP_FUNCTION_PARAMETER) {
			/* A parameter belongs to the open function, before its first block. */
			if (function == NULL || function->body != 0U || count < 3U)
				return EINVAL;
			if (function->parameter_count >= INLINE_MAX_PARAMETERS) {
				/* Records the refusal in the caller's diagnostic. */
				return drv_gpu_spirv_inline_refuse(inliner, offset, "more parameters than the inlining takes");
			}
			function->parameter[function->parameter_count] = word[2];
			function->parameter_count++;
		} else if (opcode == OP_LABEL && function != NULL && function->body == 0U) {
			/* The first label starts the body. */
			function->body = offset;
		} else if (opcode == OP_FUNCTION_END) {
			/* The end closes the record; a function without a body is a declaration. */
			if (function == NULL)
				return EINVAL;
			if (function->body == 0U) {
				/* Records the refusal in the caller's diagnostic. */
				return drv_gpu_spirv_inline_refuse(inliner, offset, "a function declaration without a body");
			}
			function->end = offset;
			function = NULL;
		}

		/* Advances to the next instruction. */
		offset += count;
	}

	/* Every function must be closed. */
	if (function != NULL)
		return EINVAL;

	/* Finds the entry point's record. */
	found = 0;
	for (index = 0U; index < inliner->function_count; index++) {
		if (inliner->functions[index].id == entry_id) {
			inliner->entry = index;
			found = 1;
			break;
		}
	}

	/* A module without its entry function is malformed. */
	if (found == 0)
		return EINVAL;

	/* Succeeded: every function is recorded. */
	return 0;
}

/*
 * Expands one function: its variables and blocks, with each call replaced
 * by the called function's expanded body.
 */
static int
drv_gpu_spirv_inline_expand(
	struct drv_gpu_spirv_inliner *inliner,
	struct drv_gpu_spirv_function *function)
{
	int error;
	const uint32_t *word;
	struct drv_gpu_spirv_function *callee;
	struct drv_gpu_spirv_words loop_blocks;
	struct drv_gpu_spirv_words fixes;
	uint32_t store[3];
	uint32_t pair[2];
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	uint32_t original;
	uint32_t current;
	uint32_t index;
	int is_entry;
	int in_loop_block;

	/* An expanded function is reused; one being expanded is called recursively. */
	if (function->state == 2)
		return 0;
	if (function->state == 1)
		return drv_gpu_spirv_inline_refuse(inliner, function->begin, "a recursive function call");
	function->state = 1;
	is_entry = function == &inliner->functions[inliner->entry];

	/*
	 * The labels of the loops' headers and continue targets, which a body
	 * of several blocks may not split.
	 */
	kern_memset(&loop_blocks, 0, sizeof(loop_blocks));
	kern_memset(&fixes, 0, sizeof(fixes));
	error = 0;
	original = 0U;
	offset = function->body;
	while (offset < function->end && error == 0) {
		word = inliner->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;
		if (opcode == OP_LABEL && count >= 2U)
			original = word[1];
		if (opcode == OP_LOOP_MERGE && count >= 3U) {
			pair[0] = original;
			pair[1] = word[2];
			error = drv_gpu_spirv_inline_push(&loop_blocks, pair, 2U);
		}
		offset += count;
	}

	/*
	 * Copies the body, moving the first block's variables out, splicing
	 * each call and taking the return.
	 */
	original = 0U;
	current = 0U;
	offset = function->body;
	while (offset < function->end && error == 0) {
		word = inliner->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/* A label opens a block of the same name. */
		if (opcode == OP_LABEL) {
			if (count < 2U) {
				error = EINVAL;
				break;
			}
			original = word[1];
			current = word[1];
			function->labels++;
			error = drv_gpu_spirv_inline_push(&function->blocks, word, count);
			offset += count;
			continue;
		}

		/* The first block's variables move out; an initializer becomes a store. */
		if (opcode == OP_VARIABLE && function->labels == 1U && count >= 4U) {
			error = drv_gpu_spirv_inline_push(&function->variables, word, 4U);
			if (error == 0 && count > 4U) {
				store[0] = (3U << 16) | OP_STORE;
				store[1] = word[2];
				store[2] = word[4];
				function->variables.word[function->variables.count - 4U] = (4U << 16) | OP_VARIABLE;
				error = drv_gpu_spirv_inline_push(&function->blocks, store, 3U);
			}
			offset += count;
			continue;
		}

		/* A call takes the called function's expanded body. */
		if (opcode == OP_FUNCTION_CALL) {
			if (count < 4U) {
				error = EINVAL;
				break;
			}
			callee = drv_gpu_spirv_inline_find(inliner, word[3]);
			if (callee == NULL) {
				error = EINVAL;
				break;
			}

			/* Expands the called function first (refusing recursion). */
			error = drv_gpu_spirv_inline_expand(inliner, callee);
			if (error != 0)
				break;

			/* A body of several blocks may not split a block a loop's structure names. */
			in_loop_block = 0;
			for (index = 0U; index < loop_blocks.count; index++) {
				if (loop_blocks.word[index] == original)
					in_loop_block = 1;
			}
			if (callee->labels > 1U && in_loop_block != 0) {
				/* Records the refusal in the caller's diagnostic. */
				error = drv_gpu_spirv_inline_refuse(inliner, offset, "a call of several blocks in a loop's header or continue target");
				break;
			}

			/* Splices the body in place of the call. */
			error = drv_gpu_spirv_inline_splice(inliner, function, callee, word, count, &current);
			offset += count;
			continue;
		}

		/*
		 * A called function returns once, at its end; the entry function's
		 * returns stay.
		 */
		if ((opcode == OP_RETURN || opcode == OP_RETURN_VALUE) && is_entry == 0) {
			if (offset + count != function->end) {
				/* Records the refusal in the caller's diagnostic. */
				error = drv_gpu_spirv_inline_refuse(inliner, offset, "a function that returns before its end");
				break;
			}
			if (opcode == OP_RETURN_VALUE && count >= 2U)
				function->result = word[1];
			offset += count;
			continue;
		}

		/* A block a call split ends under its last part's label; its successors' phis follow. */
		if (drv_gpu_spirv_inline_terminator(opcode) != 0 && current != original) {
			pair[0] = original;
			pair[1] = current;
			error = drv_gpu_spirv_inline_push(&fixes, pair, 2U);
			if (error != 0)
				break;
		}

		/* Any other instruction is copied. */
		error = drv_gpu_spirv_inline_push(&function->blocks, word, count);
		offset += count;
	}

	/* The phis naming a split block name its last part. */
	if (error == 0)
		drv_gpu_spirv_inline_fix_phis(&function->blocks, &fixes);

	/* Releases the walk's lists. */
	if (loop_blocks.word != NULL)
		kern_free(loop_blocks.word);
	if (fixes.word != NULL)
		kern_free(fixes.word);

	/* A failure leaves the function unexpanded. */
	if (error != 0)
		return error;

	/* Succeeded: the function is expanded and its last block named. */
	function->last_label = current;
	function->state = 2;
	return 0;
}

/*
 * Splices a called function's expanded body in place of a call: its ids
 * renamed, its parameters the call's arguments, its variables moved to the
 * caller's, and the call's result a copy of what it returns.
 */
static int
drv_gpu_spirv_inline_splice(
	struct drv_gpu_spirv_inliner *inliner,
	struct drv_gpu_spirv_function *caller,
	const struct drv_gpu_spirv_function *callee,
	const uint32_t *call,
	uint32_t count,
	uint32_t *current)
{
	int error;
	uint32_t branch[2];
	uint32_t copy[4];
	uint32_t index;
	uint32_t first_count;

	/* The call passes one argument for each parameter. */
	if (count - 4U != callee->parameter_count)
		return EINVAL;

	/* A new generation of the id table: the parameters are the arguments. */
	inliner->generation++;
	for (index = 0U; index < callee->parameter_count; index++) {
		if (callee->parameter[index] >= SPIRV_MAX_BOUND)
			return EINVAL;
		inliner->map[callee->parameter[index]] = call[4U + index];
		inliner->stamp[callee->parameter[index]] = inliner->generation;
	}

	/* Every id the body defines takes a new one. */
	error = drv_gpu_spirv_inline_define(inliner, &callee->variables);
	if (error == 0)
		error = drv_gpu_spirv_inline_define(inliner, &callee->blocks);
	if (error != 0)
		return error;

	/* The variables join the caller's. */
	error = drv_gpu_spirv_inline_copy(inliner, &caller->variables, callee->variables.word, callee->variables.count);
	if (error != 0)
		return error;

	/*
	 * A body of one block continues the calling block; a body of several
	 * is branched to, and its last block continues after the call.
	 */
	first_count = callee->blocks.word[0] >> 16;
	if (callee->labels == 1U) {
		error = drv_gpu_spirv_inline_copy(inliner, &caller->blocks, callee->blocks.word + first_count, callee->blocks.count - first_count);
	} else {
		branch[0] = (2U << 16) | OP_BRANCH;
		branch[1] = drv_gpu_spirv_inline_id(inliner, callee->blocks.word[1]);
		error = drv_gpu_spirv_inline_push(&caller->blocks, branch, 2U);
		if (error == 0)
			error = drv_gpu_spirv_inline_copy(inliner, &caller->blocks, callee->blocks.word, callee->blocks.count);
		*current = drv_gpu_spirv_inline_id(inliner, callee->last_label);
	}
	if (error != 0)
		return error;

	/* The call's result is what the body returns. */
	if (callee->result != 0U) {
		copy[0] = (4U << 16) | OP_COPY_OBJECT;
		copy[1] = call[1];
		copy[2] = call[2];
		copy[3] = drv_gpu_spirv_inline_id(inliner, callee->result);
		error = drv_gpu_spirv_inline_push(&caller->blocks, copy, 4U);
		if (error != 0)
			return error;
	}

	/* Succeeded: the call is replaced by the body. */
	return 0;
}

/* Gives each label and result a list of instructions defines a new id. */
static int
drv_gpu_spirv_inline_define(
	struct drv_gpu_spirv_inliner *inliner,
	const struct drv_gpu_spirv_words *list)
{
	uint32_t offset;
	uint32_t count;
	uint32_t place;
	uint32_t id;

	/* Walks the list's instructions. */
	offset = 0U;
	while (offset < list->count) {
		count = list->word[offset] >> 16;
		place = drv_gpu_spirv_inline_result(list->word[offset] & 0xFFFFU);
		if (place != INLINE_RESULT_NONE && place < count) {
			id = list->word[offset + place];
			if (id >= SPIRV_MAX_BOUND)
				return EINVAL;
			if (inliner->next_id >= SPIRV_MAX_BOUND) {
				/* Records the refusal in the caller's diagnostic. */
				return drv_gpu_spirv_inline_refuse(inliner, 0U, "more ids than a module may have after inlining");
			}
			inliner->map[id] = inliner->next_id;
			inliner->stamp[id] = inliner->generation;
			inliner->no_contraction[inliner->next_id] = inliner->no_contraction[id];
			inliner->next_id++;
		}
		offset += count;
	}

	/* Succeeded: every definition has its new id. */
	return 0;
}

/* Appends instructions with each id the splice renames replaced. */
static int
drv_gpu_spirv_inline_copy(
	struct drv_gpu_spirv_inliner *inliner,
	struct drv_gpu_spirv_words *destination,
	const uint32_t *word,
	uint32_t count)
{
	int error;
	uint32_t offset;
	uint32_t length;
	uint32_t opcode;
	uint32_t index;
	uint32_t first;
	int literal;

	/* Copies the instructions as they are. */
	first = destination->count;
	error = drv_gpu_spirv_inline_push(destination, word, count);
	if (error != 0)
		return error;

	/* Renames each id operand of each copied instruction. */
	offset = first;
	while (offset < destination->count) {
		length = destination->word[offset] >> 16;
		opcode = destination->word[offset] & 0xFFFFU;
		for (index = 1U; index < length; index++) {
			literal = drv_gpu_spirv_inline_literal(opcode, index);
			if (literal == 0)
				destination->word[offset + index] = drv_gpu_spirv_inline_id(inliner, destination->word[offset + index]);
		}
		offset += length;
	}

	/* Succeeded: the instructions are copied and renamed. */
	return 0;
}

/* Returns an id's new id in the splice, or the id itself (a module-level one). */
static uint32_t
drv_gpu_spirv_inline_id(
	const struct drv_gpu_spirv_inliner *inliner,
	uint32_t id)
{
	/* An id the splice did not rename keeps its number. */
	if (id >= SPIRV_MAX_BOUND || inliner->stamp[id] != inliner->generation)
		return id;

	/* Succeeded: the id is renamed. */
	return inliner->map[id];
}

/* Makes the phis that name a split block as a parent name its last part. */
static void
drv_gpu_spirv_inline_fix_phis(
	struct drv_gpu_spirv_words *blocks,
	const struct drv_gpu_spirv_words *fixes)
{
	uint32_t offset;
	uint32_t count;
	uint32_t index;
	uint32_t fix;

	/* Without a split block no phi changes. */
	if (fixes->count == 0U)
		return;

	/* Walks the phis' parents (words 4, 6, ...). */
	offset = 0U;
	while (offset < blocks->count) {
		count = blocks->word[offset] >> 16;
		if ((blocks->word[offset] & 0xFFFFU) == OP_PHI) {
			for (index = 4U; index < count; index += 2U) {
				for (fix = 0U; fix + 1U < fixes->count; fix += 2U) {
					if (blocks->word[offset + index] == fixes->word[fix]) {
						blocks->word[offset + index] = fixes->word[fix + 1U];
						break;
					}
				}
			}
		}
		offset += count;
	}

	/* Succeeded: every phi names the blocks that branch to it. */
	return;
}

/*
 * Writes the rewritten module: the module-level instructions as they are
 * (but the names and decorations of the dropped functions' ids), the entry
 * function with its variables first, and no other function.
 */
static int
drv_gpu_spirv_inline_assemble(
	struct drv_gpu_spirv_inliner *inliner,
	uint32_t **out,
	size_t *out_count)
{
	int error;
	const struct drv_gpu_spirv_function *entry;
	const struct drv_gpu_spirv_function *skipped;
	struct drv_gpu_spirv_words module;
	uint32_t offset;
	uint32_t count;
	uint32_t index;
	uint32_t first_count;

	/* The header, with the bound past the new ids. */
	kern_memset(&module, 0, sizeof(module));
	entry = &inliner->functions[inliner->entry];
	error = drv_gpu_spirv_inline_push(&module, inliner->code, SPIRV_HEADER_WORDS);
	if (error == 0)
		module.word[3] = inliner->next_id;

	/* Marks the ids only the dropped functions define. */
	drv_gpu_spirv_inline_mark_dropped(inliner);

	/* Copies the module, the functions replaced by the expanded entry function. */
	offset = SPIRV_HEADER_WORDS;
	while (offset < inliner->words && error == 0) {
		count = inliner->code[offset] >> 16;

		/* After the last decoration, the NoContraction of each new id of a decorated result. */
		if (offset == inliner->annotations_end) {
			error = drv_gpu_spirv_inline_decorate(inliner, &module);
			if (error != 0)
				break;
		}

		/* A function's words are skipped, the entry function's rewritten. */
		skipped = NULL;
		for (index = 0U; index < inliner->function_count; index++) {
			if (inliner->functions[index].begin == offset)
				skipped = &inliner->functions[index];
		}
		if (skipped == NULL) {
			if (!drv_gpu_spirv_inline_names_dropped(inliner, inliner->code + offset, count))
				error = drv_gpu_spirv_inline_push(&module, inliner->code + offset, count);
			offset += count;
			continue;
		}

		if (skipped == entry) {
			/* OpFunction, the first label, the variables, the rest of the blocks, OpFunctionEnd. */
			first_count = entry->blocks.word[0] >> 16;
			error = drv_gpu_spirv_inline_push(&module, inliner->code + entry->begin, entry->body - entry->begin);
			if (error == 0)
				error = drv_gpu_spirv_inline_push(&module, entry->blocks.word, first_count);
			if (error == 0)
				error = drv_gpu_spirv_inline_push(&module, entry->variables.word, entry->variables.count);
			if (error == 0)
				error = drv_gpu_spirv_inline_push(&module, entry->blocks.word + first_count, entry->blocks.count - first_count);
			if (error == 0)
				error = drv_gpu_spirv_inline_push(&module, inliner->code + entry->end, inliner->code[entry->end] >> 16);
		}
		offset = skipped->end + (inliner->code[skipped->end] >> 16);
	}

	/* A failure releases the partial module. */
	if (error != 0) {
		if (module.word != NULL)
			kern_free(module.word);
		return error;
	}

	/* Succeeded: the caller owns the rewritten module. */
	*out = module.word;
	*out_count = module.count;
	return 0;
}

/* Appends an OpDecorate NoContraction for each new id of a decorated result. */
static int
drv_gpu_spirv_inline_decorate(
	const struct drv_gpu_spirv_inliner *inliner,
	struct drv_gpu_spirv_words *module)
{
	uint32_t decoration[3];
	uint32_t id;
	int error;

	/* The new ids are those from the module's bound on. */
	for (id = inliner->bound; id < inliner->next_id; id++) {
		if (inliner->no_contraction[id] == 0U)
			continue;
		decoration[0] = (3U << 16) | OP_DECORATE;
		decoration[1] = id;
		decoration[2] = DEC_NO_CONTRACTION;
		error = drv_gpu_spirv_inline_push(module, decoration, 3U);
		if (error != 0)
			return error;
	}

	/* Succeeded: every new decorated result is decorated. */
	return 0;
}

/*
 * Marks in the id table, under a new generation, every id the functions
 * other than the entry define: the function, its parameters, its labels
 * and its results.  None of them is in the rewritten module.
 */
static void
drv_gpu_spirv_inline_mark_dropped(
	struct drv_gpu_spirv_inliner *inliner)
{
	const struct drv_gpu_spirv_function *function;
	uint32_t index;
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	uint32_t place;
	uint32_t id;

	/* A new generation, so no splice's mark is read as dropped. */
	inliner->generation++;
	for (index = 0U; index < inliner->function_count; index++) {
		if (index == inliner->entry)
			continue;
		function = &inliner->functions[index];

		/* Each instruction from OpFunction to OpFunctionEnd. */
		offset = function->begin;
		while (offset < function->end) {
			count = inliner->code[offset] >> 16;
			opcode = inliner->code[offset] & 0xFFFFU;
			place = drv_gpu_spirv_inline_result(opcode);
			id = 0U;
			if (place == INLINE_RESULT_LABEL && count > 1U)
				id = inliner->code[offset + 1U];
			if (place == INLINE_RESULT_TYPED && count > 2U)
				id = inliner->code[offset + 2U];
			if (id != 0U && id < SPIRV_MAX_BOUND)
				inliner->stamp[id] = inliner->generation;
			offset += count;
		}
	}

	/* Succeeded: the dropped ids carry the generation. */
	return;
}

/*
 * Returns whether a module-level instruction is a debug name or a
 * decoration of an id the dropped functions define (left out of the
 * rewritten module, where the id no longer exists).
 */
static int
drv_gpu_spirv_inline_names_dropped(
	const struct drv_gpu_spirv_inliner *inliner,
	const uint32_t *word,
	uint32_t count)
{
	uint32_t opcode;
	uint32_t id;

	/* Only the names and decorations of one id. */
	opcode = word[0] & 0xFFFFU;
	if (opcode != OP_NAME &&
	    opcode != OP_DECORATE &&
	    opcode != OP_DECORATE_ID &&
	    opcode != OP_DECORATE_STRING)
		return 0;
	if (count < 2U)
		return 0;

	/* The target id, marked or not. */
	id = word[1];
	if (id >= SPIRV_MAX_BOUND || inliner->stamp[id] != inliner->generation)
		return 0;

	/* The target is a dropped function's id. */
	return 1;
}

/* Returns the record of the function with an id, or NULL. */
static struct drv_gpu_spirv_function *
drv_gpu_spirv_inline_find(
	struct drv_gpu_spirv_inliner *inliner,
	uint32_t id)
{
	uint32_t index;

	/* Looks the id up among the module's functions. */
	for (index = 0U; index < inliner->function_count; index++) {
		if (inliner->functions[index].id == id)
			return &inliner->functions[index];
	}

	/* No function has the id. */
	return NULL;
}

/* Appends words to a list, growing it. */
static int
drv_gpu_spirv_inline_push(
	struct drv_gpu_spirv_words *list,
	const uint32_t *word,
	uint32_t count)
{
	uint32_t *grown;
	uint32_t capacity;

	/* Grows the list to hold the words (at least doubling). */
	if (list->count + count > list->capacity) {
		capacity = list->capacity * 2U + count + 64U;
		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		if (list->word != NULL) {
			kern_memcpy(grown, list->word, list->count * sizeof(*grown));
			kern_free(list->word);
		}
		list->word = grown;
		list->capacity = capacity;
	}

	/* Appends the words. */
	if (count != 0U)
		kern_memcpy(list->word + list->count, word, count * sizeof(*word));
	list->count += count;

	/* Succeeded: the words are in the list. */
	return 0;
}

/* Returns where a function-body instruction's result id is (SPIR-V's grammar). */
static uint32_t
drv_gpu_spirv_inline_result(
	uint32_t opcode)
{
	/* The instructions of a function body without a result, a label, and the rest with a type and a result. */
	switch (opcode) {
	case OP_NOP:
	case OP_LINE:
	case OP_NO_LINE:
	case OP_STORE:
	case OP_COPY_MEMORY:
	case OP_COPY_MEMORY_SIZED:
	case OP_IMAGE_WRITE:
	case OP_EMIT_VERTEX:
	case OP_END_PRIMITIVE:
	case OP_EMIT_STREAM_VERTEX:
	case OP_END_STREAM_PRIMITIVE:
	case OP_CONTROL_BARRIER:
	case OP_MEMORY_BARRIER:
	case OP_ATOMIC_STORE:
	case OP_LOOP_MERGE:
	case OP_SELECTION_MERGE:
	case OP_BRANCH:
	case OP_BRANCH_CONDITIONAL:
	case OP_SWITCH:
	case OP_KILL:
	case OP_RETURN:
	case OP_RETURN_VALUE:
	case OP_UNREACHABLE:
	case OP_LIFETIME_START:
	case OP_LIFETIME_STOP:
	case OP_TERMINATE_INVOCATION:
	case OP_DEMOTE_TO_HELPER_INVOCATION:
		return INLINE_RESULT_NONE;
	case OP_LABEL:
		return INLINE_RESULT_LABEL;
	default:
		return INLINE_RESULT_TYPED;
	}
}

/*
 * Returns whether word `index` of a function-body instruction is a literal
 * rather than an id (SPIR-V's grammar).  A scope or a memory operand's id
 * among literals is a module-level constant, which the inlining never
 * renames, so it may be read as a literal.
 */
static int
drv_gpu_spirv_inline_literal(
	uint32_t opcode,
	uint32_t index)
{
	/* The literal operands, by instruction. */
	switch (opcode) {
	case OP_LINE:
		return index >= 2U;
	case OP_EXT_INST:
		return index == 4U;
	case OP_VARIABLE:
		return index == 3U;
	case OP_LOAD:
	case OP_COPY_MEMORY_SIZED:
		return index >= 4U;
	case OP_STORE:
	case OP_COPY_MEMORY:
		return index >= 3U;
	case OP_VECTOR_SHUFFLE:
	case OP_COMPOSITE_INSERT:
		return index >= 5U;
	case OP_COMPOSITE_EXTRACT:
		return index >= 4U;
	case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD:
	case OP_IMAGE_FETCH:
	case OP_IMAGE_READ:
		return index == 5U;
	case OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD:
	case OP_IMAGE_GATHER:
	case OP_IMAGE_DREF_GATHER:
		return index == 6U;
	case OP_IMAGE_WRITE:
		return index == 4U;
	case OP_LOOP_MERGE:
		return index >= 3U;
	case OP_SELECTION_MERGE:
		return index >= 2U;
	case OP_BRANCH_CONDITIONAL:
		return index >= 4U;
	case OP_SWITCH:
		return index >= 3U && ((index - 3U) & 1U) == 0U;
	case OP_GROUP_NON_UNIFORM_BALLOT_BIT_COUNT:
		return index == 4U;
	default:
		break;
	}

	/* The group arithmetic's operation. */
	if (opcode >= OP_GROUP_NON_UNIFORM_IADD && opcode <= OP_GROUP_NON_UNIFORM_LOGICAL_XOR)
		return index == 4U;

	/* Every other operand is an id. */
	return 0;
}

/* Returns whether an instruction ends a block. */
static int
drv_gpu_spirv_inline_terminator(
	uint32_t opcode)
{
	/* The block terminators of a function body. */
	switch (opcode) {
	case OP_BRANCH:
	case OP_BRANCH_CONDITIONAL:
	case OP_SWITCH:
	case OP_KILL:
	case OP_RETURN:
	case OP_RETURN_VALUE:
	case OP_UNREACHABLE:
	case OP_TERMINATE_INVOCATION:
		return 1;
	default:
		return 0;
	}
}

/* Records a call the inlining refuses. */
static int
drv_gpu_spirv_inline_refuse(
	struct drv_gpu_spirv_inliner *inliner,
	uint32_t offset,
	const char *reason)
{
	/* Names the refused instruction for the caller's diagnostic. */
	inliner->diag.opcode = OP_FUNCTION_CALL;
	inliner->diag.word_offset = offset;
	inliner->diag.reason = reason;

	/* Reports valid SPIR-V the inlining does not rewrite. */
	return ENOTSUP;
}

/* Releases the inlining's lists and tables. */
static void
drv_gpu_spirv_inline_release(
	struct drv_gpu_spirv_inliner *inliner)
{
	uint32_t index;

	/* Releases each function's lists. */
	if (inliner->functions != NULL) {
		for (index = 0U; index < inliner->function_count; index++) {
			if (inliner->functions[index].variables.word != NULL)
				kern_free(inliner->functions[index].variables.word);
			if (inliner->functions[index].blocks.word != NULL)
				kern_free(inliner->functions[index].blocks.word);
		}
		kern_free(inliner->functions);
	}

	/* Releases the id table. */
	if (inliner->map != NULL)
		kern_free(inliner->map);
	if (inliner->stamp != NULL)
		kern_free(inliner->stamp);
	if (inliner->no_contraction != NULL)
		kern_free(inliner->no_contraction);

	/* Succeeded: nothing of the inlining is left. */
	return;
}
