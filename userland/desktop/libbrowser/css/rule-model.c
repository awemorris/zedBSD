/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Owns ordered top-level CSS sources independently of flattened rendering rules. */

#include "css/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Bound source grammar nesting independently of native stack depth. */
#define MODEL_NESTING_MAX 128U
/* Retired immutable entries count too, so deletion cannot bypass retained source bounds. */
#define MODEL_ENTRIES_MAX 65536U

/* One immutable source remains available to stable rule handles until its model dies. */
struct model_source {
	struct wb_units units;
	uint32_t id;
	int type;
	int present;
	/* Rendering joins close omitted block braces or an import terminator without changing source. */
	unsigned closing;
	int semicolon;
};

/* Live order and retained sources are separately owned without native DOM or VM roots. */
struct css_rule_model {
	struct vm_heap *heap;
	struct wb_vector rules;
	struct wb_vector entries;
	uint32_t next_id;
};

static int model_parse(struct css_rule_model *model, const uint16_t *units, size_t length, int strict);
static int model_next(const struct css_token *tokens, size_t count, size_t start, size_t *end, int *type, unsigned *closing, int *semicolon);
static int model_at_type(const struct css_token *token);
static int model_scope_valid(struct vm_heap *heap, const struct css_token *tokens, size_t count, size_t start, const uint16_t *units, size_t length);
static int model_scope_selector(struct vm_heap *heap, const uint16_t *units, size_t length);
static int model_keep(struct css_rule_model *model, const uint16_t *units, size_t length, int type, unsigned closing, int semicolon);
static void model_source_destroy(struct model_source *source);

/*
 * Creates an ordered source list from a tolerant native stylesheet parse.
 */
int
css_rule_model_create(
	struct css_rule_model **model,
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length)
{
	struct css_rule_model *made;
	int status;

	/* Publish no partial model if construction fails. */
	*model = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Initialize both source archives before any parser can publish entries. */
	made->heap = heap;
	made->next_id = 1;
	wb_vector_init(&made->rules, sizeof(struct model_source *));
	wb_vector_init(&made->entries, sizeof(struct model_source *));

	/* Existing CSS tokenization and qualified-selector parsing decide source boundaries. */
	status = model_parse(made, units, length, 0);
	if (status != 0) {
		css_rule_model_destroy(made);
		return status;
	}

	/* Succeeded: the caller owns every immutable source and the current rule order. */
	*model = made;
	return 0;
}

/*
 * Releases all live and retired immutable sources without accessing native DOM cells.
 */
void
css_rule_model_destroy(
	struct css_rule_model *model)
{
	struct model_source *source;
	size_t index;

	/* Partial construction may not have allocated a model. */
	if (model == NULL)
		return;

	/* Retired handles and live order refer to this same uniquely owning archive. */
	for (index = 0; index < model->entries.count; index++) {
		source = *(struct model_source **)wb_vector_at(&model->entries, index);
		model_source_destroy(source);
	}

	/* Pointer vectors carry no independent source ownership. */
	wb_vector_release(&model->entries);
	wb_vector_release(&model->rules);
	free(model);

	/* Succeeded: no model-owned source storage remains. */
	return;
}

/*
 * Reports the number of current top-level rules, including unflattened recognized groups.
 */
size_t
css_rule_model_count(
	const struct css_rule_model *model)
{
	/* Succeeded: only current order contributes to the live length. */
	return model->rules.count;
}

/*
 * Reports the stable identity at an index, or zero when that index is absent.
 */
uint32_t
css_rule_model_id(
	const struct css_rule_model *model,
	size_t index)
{
	struct model_source *source;

	/* Zero is reserved for an absent indexed source. */
	if (index >= model->rules.count)
		return 0;
	source = *(struct model_source **)wb_vector_at(&model->rules, index);

	/* Succeeded: this identity survives insertion, reordering and later deletion. */
	return source->id;
}

/*
 * Reports how many closing braces the source of a rule omitted at its end.
 */
unsigned
css_rule_model_closing(
	const struct css_rule_model *model,
	uint32_t id)
{
	struct model_source *entry;
	size_t index;

	for (index = 0; index < model->entries.count; index++) {
		entry = *(struct model_source **)wb_vector_at(&model->entries, index);
		if (entry->id == id)
			return entry->closing;
	}
	return 0;
}

/*
 * Finds immutable source by stable identity and reports whether it is still in the live list.
 */
int
css_rule_model_source(
	const struct css_rule_model *model,
	uint32_t id,
	const uint16_t **units,
	size_t *length,
	int *type,
	int *present)
{
	struct model_source *source;
	struct model_source *entry;
	size_t index;

	/* Missing identities publish no borrowed source pointers. */
	*units = NULL;
	*length = 0;
	*type = 0;
	*present = 0;

	/* Retired sources remain in the archive until every model handle is released. */
	source = NULL;
	for (index = 0; index < model->entries.count; index++) {
		entry = *(struct model_source **)wb_vector_at(&model->entries, index);
		if (entry->id == id) {
			source = entry;
			break;
		}
	}

	/* An unknown or zero identity has no source in this model. */
	if (source == NULL)
		return ENOENT;

	/* Succeeded: immutable storage is borrowed only for this model's lifetime. */
	*units = source->units.data;
	*length = source->units.length;
	*type = source->type;
	*present = source->present;
	return 0;
}

/*
 * Appends current source rules to a caller-owned buffer for ordinary cascade reparsing.
 */
int
css_rule_model_text(
	const struct css_rule_model *model,
	struct wb_units *units)
{
	struct model_source *source;
	size_t index;
	unsigned closing;
	int status;

	/* Line breaks separate rules without changing comments, strings or escaped source tokens. */
	for (index = 0; index < model->rules.count; index++) {
		source = *(struct model_source **)wb_vector_at(&model->rules, index);
		status = wb_units_append(units, source->units.data, source->units.length);
		if (status != 0)
			return status;

		/* Implicit end-of-input block closures must precede a newly inserted following rule. */
		for (closing = 0; closing < source->closing; closing++) {
			status = wb_units_append_code_point(units, '}');
			if (status != 0)
				return status;
		}

		/* A native EOF import is terminated before subsequent qualified-rule source. */
		if (source->semicolon) {
			status = wb_units_append_code_point(units, ';');
			if (status != 0)
				return status;
		}

		/* Separate complete source rules without joining their terminal tokens. */
		status = wb_units_append_code_point(units, '\n');
		if (status != 0)
			return status;
	}

	/* Succeeded: current native sources can be parsed by the ordinary stylesheet engine. */
	return 0;
}

/*
 * Inserts one valid ordinary style rule without changing current state on failure.
 */
int
css_rule_model_insert(
	struct css_rule_model *model,
	const uint16_t *units,
	size_t length,
	size_t index)
{
	struct css_rule_model *candidate;
	struct model_source *source;
	struct model_source **ordered;
	size_t count;
	int status;

	/* Index validation precedes any parse or source allocation. */
	count = model->rules.count;
	if (index > count)
		return ERANGE;

	/* Bound retained source storage independently of live rule count. */
	if (model->entries.count >= MODEL_ENTRIES_MAX || model->next_id == UINT32_MAX)
		return EOVERFLOW;

	/* Parse into independent owning storage before any mutation of the current list. */
	candidate = calloc(1, sizeof(*candidate));
	if (candidate == NULL)
		return ENOMEM;

	/* Initialize independent ownership before strict candidate parsing. */
	candidate->heap = model->heap;
	candidate->next_id = 1;
	wb_vector_init(&candidate->rules, sizeof(struct model_source *));
	wb_vector_init(&candidate->entries, sizeof(struct model_source *));

	/* Parse complete single-rule input before touching current model state. */
	status = model_parse(candidate, units, length, 1);
	if (status != 0) {
		css_rule_model_destroy(candidate);
		return status;
	}

	/* A single-rule parse rejects empty input and multiple top-level rules. */
	if (candidate->rules.count != 1U) {
		css_rule_model_destroy(candidate);
		return EINVAL;
	}

	/* At-rule placement and mutation need their own CSSOM design rather than unchecked insertion. */
	source = *(struct model_source **)wb_vector_at(&candidate->rules, 0);
	if (source->type == 3 || source->type == 10) {
		css_rule_model_destroy(candidate);
		return ENOTSUP;
	}

	/* Register owning storage first, rolling it back if the live vector cannot grow. */
	status = wb_vector_push(&model->entries, &source);
	if (status != 0) {
		css_rule_model_destroy(candidate);
		return status;
	}

	/* Live order grows only after owning archive publication succeeds. */
	status = wb_vector_push(&model->rules, &source);
	if (status != 0) {
		wb_vector_pop(&model->entries);
		css_rule_model_destroy(candidate);
		return status;
	}

	/* Publish order and stable identity only after every fallible allocation succeeded. */
	ordered = model->rules.items;
	memmove(ordered + index + 1U, ordered + index, (count - index) * sizeof(*ordered));
	ordered[index] = source;
	source->id = model->next_id;
	model->next_id++;
	wb_vector_pop(&candidate->entries);
	wb_vector_pop(&candidate->rules);
	css_rule_model_destroy(candidate);

	/* Succeeded: the current source model owns the inserted immutable rule. */
	return 0;
}

/*
 * Replaces one indexed rule with a newly parsed single rule, giving it a new stable identity.
 */
int
css_rule_model_replace(
	struct css_rule_model *model,
	const uint16_t *units,
	size_t length,
	size_t index)
{
	struct model_source **ordered;
	struct model_source *old;
	size_t count;
	int status;

	/* A missing index changes nothing. */
	count = model->rules.count;
	if (index >= count)
		return ERANGE;

	/* The new source is inserted after the old one, then the old one retires. */
	status = css_rule_model_insert(model, units, length, index);
	if (status != 0)
		return status;
	ordered = model->rules.items;
	old = ordered[index + 1U];
	old->present = 0;
	memmove(ordered + index + 1U, ordered + index + 2U, (count - index - 1U) * sizeof(*ordered));
	wb_vector_pop(&model->rules);

	/* Succeeded: the list keeps its length and the new rule sits at the index. */
	return 0;
}

/*
 * Removes one indexed rule while keeping its immutable source available to existing handles.
 */
int
css_rule_model_delete(
	struct css_rule_model *model,
	size_t index)
{
	struct model_source **ordered;
	struct model_source *source;
	size_t count;

	/* Invalid deletion leaves both live order and retained sources unchanged. */
	count = model->rules.count;
	if (index >= count)
		return ERANGE;
	ordered = model->rules.items;
	source = ordered[index];

	/* Retired membership changes without replacing the immutable source or its stable identity. */
	source->present = 0;
	memmove(ordered + index, ordered + index + 1U, (count - index - 1U) * sizeof(*ordered));
	wb_vector_pop(&model->rules);

	/* Succeeded: the list shrank while existing source handles remain valid. */
	return 0;
}

/* Parses rule boundaries with original source spans and existing qualified-rule validation. */
static int
model_parse(
	struct css_rule_model *model,
	const uint16_t *units,
	size_t length,
	int strict)
{
	struct wb_arena arena;
	struct css_token *tokens;
	struct css_sheet *parsed;
	size_t count;
	size_t index;
	size_t end;
	size_t start_offset;
	size_t end_offset;
	size_t qualified;
	int type;
	unsigned closing;
	int semicolon;
	int status;
	int valid;

	/* Original token extents avoid independently scanning comments, escapes and strings. */
	wb_arena_init(&arena, 0);
	status = css_tokenize(&arena, units, length, &tokens, &count);
	if (status != 0) {
		wb_arena_release(&arena);
		return status;
	}

	/* Iterate top-level rules without flattening recognized grouping rules. */
	index = 0;
	while (index < count && tokens[index].type != CSS_TOKEN_EOF) {
		/* CSS whitespace never creates a top-level rule. */
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* HTML comment markers are sheet-only syntax, not valid single-rule input. */
		if (tokens[index].type == CSS_TOKEN_CDO || tokens[index].type == CSS_TOKEN_CDC) {
			if (strict) {
				wb_arena_release(&arena);
				return EINVAL;
			}

			/* Tolerant sheet parsing discards this HTML comment marker. */
			index++;
			continue;
		}

		/* Boundaries refer to native tokenizer offsets in the original UTF16 input. */
		status = model_next(tokens, count, index, &end, &type, &closing, &semicolon);
		if (status != 0) {
			if (status == EOVERFLOW || strict) {
				wb_arena_release(&arena);
				return status;
			}

			/* A malformed trailing qualified rule contributes no sheet rule. */
			break;
		}

		/* Use half-open original source extents supplied by the native tokens. */
		start_offset = tokens[index].source_start;
		end_offset = tokens[end - 1U].source_end;
		if (end_offset > length || start_offset > end_offset) {
			wb_arena_release(&arena);
			return EINVAL;
		}

		/* Ordinary qualified-rule validity comes from the same parser used by rendering. */
		valid = 0;
		if (type == 1) {
			parsed = NULL;
			status = css_sheet_create(&parsed, model->heap, units + start_offset, end_offset - start_offset);
			if (status == ENOMEM) {
				wb_arena_release(&arena);
				return status;
			}

			/* A supported ordinary selector must survive the actual rendering parser. */
			if (status == 0) {
				qualified = css_sheet_rule_count(parsed);
				if (qualified != 0)
					valid = 1;
			}

			/* Only copied source text survives temporary qualified-rule validation. */
			css_sheet_destroy(parsed);
		} else if (type == 102) {
			/* A scope rule's prelude must be its optional root and limit selector lists. */
			valid = model_scope_valid(model->heap, tokens, count, index, units, length);
		} else if (type != 0) {
			/* Recognized groups remain top-level sources even when their condition is false. */
			valid = 1;
		}

		/* Strict insertion cannot silently discard an invalid selector or unsupported source. */
		if (!valid && strict) {
			wb_arena_release(&arena);
			return EINVAL;
		}

		/* Tolerant sheets retain only valid ordinary or recognized grouping sources. */
		if (valid) {
			status = model_keep(model, units + start_offset, end_offset - start_offset, type, closing, semicolon);
			if (status != 0) {
				wb_arena_release(&arena);
				return status;
			}
		}

		/* The next token belongs to the next source rule or to trailing whitespace. */
		index = end;
	}

	/* Token arenas own no source pointers once immutable entries have copied their input. */
	wb_arena_release(&arena);

	/* Succeeded: the model owns each accepted top-level source. */
	return 0;
}

/* Finds one top-level block or semicolon boundary without interpreting token contents again. */
static int
model_next(
	const struct css_token *tokens,
	size_t count,
	size_t start,
	size_t *end,
	int *type,
	unsigned *closing,
	int *semicolon)
{
	size_t index;
	unsigned grouping;
	unsigned curly;
	int at_rule;
	int token;

	/* Qualified rules require a block; at-rules also allow a terminating semicolon. */
	*closing = 0;
	*semicolon = 0;
	at_rule = 0;
	*type = 1;
	if (tokens[start].type == CSS_TOKEN_AT_KEYWORD) {
		at_rule = 1;
		*type = model_at_type(&tokens[start]);
	}

	/* Prelude component depth is independent of the later curly block depth. */
	grouping = 0;

	/* Prelude functions and square brackets protect punctuation from becoming a rule boundary. */
	for (index = start; index < count; index++) {
		token = tokens[index].type;
		if (token == CSS_TOKEN_EOF) {
			/* A qualified rule still needs an opening block, even at end of input. */
			if (!at_rule)
				return EINVAL;

			/* Only a recognized import permits its semicolon to be omitted at EOF. */
			if (*type == 3) {
				*semicolon = 1;
			} else {
				*type = 0;
			}

			/* Succeeded: this at-rule source ended at the tokenizer's original EOF extent. */
			*end = index + 1U;
			return 0;
		}

		/* Component-value groups keep rule punctuation nested until their closing delimiter. */
		if (token == CSS_TOKEN_FUNCTION ||
		    token == CSS_TOKEN_OPEN_PAREN ||
		    token == CSS_TOKEN_OPEN_SQUARE) {
			grouping++;
			if (grouping > MODEL_NESTING_MAX)
				return EOVERFLOW;
		} else if (token == CSS_TOKEN_CLOSE_PAREN || token == CSS_TOKEN_CLOSE_SQUARE) {
			if (grouping != 0)
				grouping--;
		}

		/* Punctuation nested inside a component-value group cannot close the top-level rule. */
		if (grouping != 0)
			continue;
		if (at_rule && token == CSS_TOKEN_SEMICOLON) {
			/* Grouping and font-face rules cannot replace their required block with a semicolon. */
			if (*type == 4 ||
			    *type == 5 ||
			    *type == 12 ||
			    *type == 6 ||
			    *type == 7 ||
			    *type == 11 ||
			    *type == 14 ||
			    (*type >= 101 && *type <= 107))
				*type = 0;

			/* Succeeded: this complete semicolon source ends before the following rule. */
			*end = index + 1U;
			return 0;
		}

		/* This top-level opening brace starts the rule's body. */
		if (token == CSS_TOKEN_OPEN_CURLY) {
			/* Import syntax cannot use a block instead of its URL and terminating semicolon. */
			if (*type == 3)
				*type = 0;
			break;
		}
	}

	/* No qualified rule exists without an opening block. */
	if (index >= count)
		return EINVAL;

	/* Nested braces are native punctuation tokens; strings and escapes cannot imitate them. */
	curly = 1;
	index++;
	for (;
	     index < count;
	     index++) {
		token = tokens[index].type;
		if (token == CSS_TOKEN_EOF) {
			/* Record implicit block closures separately from the immutable original source. */
			*closing = curly;
			*end = index + 1U;
			return 0;
		}

		/* Nested curly blocks must close before the outer source rule can end. */
		if (token == CSS_TOKEN_OPEN_CURLY) {
			curly++;
			if (curly > MODEL_NESTING_MAX)
				return EOVERFLOW;
		} else if (token == CSS_TOKEN_CLOSE_CURLY) {
			curly--;
			if (curly == 0)
				break;
		}
	}

	/* A tokenizer result without a closing block or EOF is not a complete source stream. */
	if (index >= count)
		return EINVAL;

	/* Succeeded: the matched outer closing brace completes this immutable source span. */
	*end = index + 1U;
	return 0;
}

/* Whether a text is the selector list of a scope rule's root or limit. */
static int
model_scope_selector(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length)
{
	struct css_query *query;
	struct wb_units relative;
	size_t index;
	int status;

	/* Pseudo-elements cannot be scoped. */
	for (index = 0; index + 1U < length; index++) {
		if (units[index] == ':' && units[index + 1U] == ':')
			return 0;
	}
	/* A relative selector (a leading combinator) is read with the implied nesting selector before it. */
	query = NULL;
	for (index = 0; index < length && (units[index] == ' ' || units[index] == '\t' || units[index] == '\n'); index++)
		;
	if (index < length && (units[index] == '>' || units[index] == '+' || units[index] == '~')) {
		wb_units_init(&relative);
		status = wb_units_append_code_point(&relative, '&');
		if (status == 0)
			status = wb_units_append(&relative, units + index, length - index);
		if (status == 0)
			status = css_query_parse(heap, relative.data, relative.length, &query);
		wb_units_release(&relative);
	} else {
		status = css_query_parse(heap, units, length, &query);
	}
	if (status != 0)
		return 0;
	css_query_destroy(query);
	return 1;
}

/* Reads one parenthesized selector list at *at, leaving *at after its closing parenthesis. */
static int
model_scope_paren(
	struct vm_heap *heap,
	const struct css_token *tokens,
	size_t count,
	size_t *at,
	const uint16_t *units,
	size_t length)
{
	size_t index;
	size_t open;
	unsigned depth;
	int token;

	open = *at;
	depth = 0;
	for (index = open; index < count; index++) {
		token = tokens[index].type;
		if (token == CSS_TOKEN_FUNCTION || token == CSS_TOKEN_OPEN_PAREN || token == CSS_TOKEN_OPEN_SQUARE) {
			depth++;
		} else if (token == CSS_TOKEN_CLOSE_PAREN || token == CSS_TOKEN_CLOSE_SQUARE) {
			if (depth == 0)
				return 0;
			depth--;
			if (depth == 0)
				break;
		} else if (token == CSS_TOKEN_EOF || (token == CSS_TOKEN_OPEN_CURLY && depth == 0)) {
			return 0;
		}
	}
	if (index >= count || tokens[index].type != CSS_TOKEN_CLOSE_PAREN)
		return 0;
	if (tokens[open].source_end > tokens[index].source_start || tokens[index].source_start > length)
		return 0;
	if (!model_scope_selector(heap, units + tokens[open].source_end, tokens[index].source_start - tokens[open].source_end))
		return 0;
	*at = index + 1U;
	return 1;
}

/* Checks "[ ( <selector-list> ) ]? [ to ( <selector-list> ) ]?" before the block of a scope rule. */
static int
model_scope_valid(
	struct vm_heap *heap,
	const struct css_token *tokens,
	size_t count,
	size_t start,
	const uint16_t *units,
	size_t length)
{
	size_t index;

	index = start + 1U;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index < count && tokens[index].type == CSS_TOKEN_OPEN_PAREN) {
		if (!model_scope_paren(heap, tokens, count, &index, units, length))
			return 0;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
	}
	if (index < count && tokens[index].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[index], "to")) {
		index++;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (index >= count || tokens[index].type != CSS_TOKEN_OPEN_PAREN)
			return 0;
		if (!model_scope_paren(heap, tokens, count, &index, units, length))
			return 0;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
	}
	return index < count && tokens[index].type == CSS_TOKEN_OPEN_CURLY;
}

/* Classifies supported top-level at-rule sources without claiming nested CSSOM interfaces. */
static int
model_at_type(
	const struct css_token *token)
{
	int same;

	/* Imported sources remain distinct top-level rules rather than flattened imported content. */
	same = css_ident_equal(token, "import");
	if (same)
		return 3;
	same = css_ident_equal(token, "media");
	if (same)
		return 4;
	same = css_ident_equal(token, "font-face");
	if (same)
		return 5;
	same = css_ident_equal(token, "supports");
	if (same)
		return 12;
	same = css_ident_equal(token, "page");
	if (same)
		return 6;
	same = css_ident_equal(token, "keyframes") || css_ident_equal(token, "-webkit-keyframes");
	if (same)
		return 7;
	same = css_ident_equal(token, "namespace");
	if (same)
		return 10;
	same = css_ident_equal(token, "counter-style");
	if (same)
		return 11;
	same = css_ident_equal(token, "font-feature-values");
	if (same)
		return 14;
	same = css_ident_equal(token, "layer");
	if (same)
		return 100;
	same = css_ident_equal(token, "container");
	if (same)
		return 101;
	same = css_ident_equal(token, "scope");
	if (same)
		return 102;
	same = css_ident_equal(token, "starting-style");
	if (same)
		return 103;
	same = css_ident_equal(token, "position-try");
	if (same)
		return 104;
	same = css_ident_equal(token, "property");
	if (same)
		return 105;
	same = css_ident_equal(token, "view-transition");
	if (same)
		return 106;
	same = css_ident_equal(token, "font-palette-values");
	if (same)
		return 107;

	/* Succeeded: unsupported at-rule sources contribute no native model entry. */
	return 0;
}

/* Copies one immutable rule before publishing archive ownership and live order. */
static int
model_keep(
	struct css_rule_model *model,
	const uint16_t *units,
	size_t length,
	int type,
	unsigned closing,
	int semicolon)
{
	struct model_source *source;
	int status;

	/* Retired sources cannot evade this retained-entry bound. */
	if (model->entries.count >= MODEL_ENTRIES_MAX || model->next_id == UINT32_MAX)
		return EOVERFLOW;

	/* Allocate one immutable source before initializing its borrowed metadata. */
	source = calloc(1, sizeof(*source));
	if (source == NULL)
		return ENOMEM;

	/* Initialize native copied-text ownership before archive publication. */
	wb_units_init(&source->units);
	source->type = type;
	source->present = 1;
	source->closing = closing;
	source->semicolon = semicolon;

	/* Source copying never retains the tokenizer's arena or the caller's input buffer. */
	status = wb_units_append(&source->units, units, length);
	if (status != 0) {
		model_source_destroy(source);
		return status;
	}

	/* Publish ownership before adding the borrowed pointer to live order. */
	status = wb_vector_push(&model->entries, &source);
	if (status != 0) {
		model_source_destroy(source);
		return status;
	}

	/* Live order grows only after owning archive publication succeeds. */
	status = wb_vector_push(&model->rules, &source);
	if (status != 0) {
		wb_vector_pop(&model->entries);
		model_source_destroy(source);
		return status;
	}

	/* Identity allocation occurs only after every fallible ownership publication succeeded. */
	source->id = model->next_id;
	model->next_id++;

	/* Succeeded: this model owns an immutable live source. */
	return 0;
}

/* Releases immutable C source storage without inspecting any heap cell. */
static void
model_source_destroy(
	struct model_source *source)
{
	/* Each archive entry owns only its copied UTF16 buffer. */
	wb_units_release(&source->units);
	free(source);

	/* Succeeded: no source-owned storage remains. */
	return;
}
