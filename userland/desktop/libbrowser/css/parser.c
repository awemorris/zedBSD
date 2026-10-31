/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CSS parser: style sheets into style rules (selectors and
 * declarations), and style attributes into declarations.
 *
 * The URLs of the @import rules before the style rules are kept for the
 * page to fetch (ws074-p068); other at-rules are skipped whole in this
 * pass.  A rule whose selector this pass cannot read is dropped, as the
 * standard drops a rule with an invalid selector.  The parsed sheet's
 * selectors are filed in its rule index (index.c).
 */

#include "css/internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most declarations one declaration may expand into (a shorthand's longhands). */
#define PARSER_EXPANSION_MAX	16U

/*
 * A run of tokens: where it starts and how many there are.
 */
struct token_range {
	const struct css_token *tokens;
	size_t count;
};

/*
 * What reading a sheet's rules gathers: the sheet (its arena), its style
 * rules, and the URLs and media lists of its @import rules.
 */
struct parser_state {
	struct vm_heap *heap;
	struct css_sheet *sheet;
	struct wb_vector rules;
	struct wb_vector imports;
	struct wb_vector import_media;
	struct wb_vector font_faces;
	struct wb_vector try_rules;
	struct wb_vector keyframes;
	/* The @scope whose block is being read (NULL outside one). */
	const struct css_scope *scope;
	/* The cascade layers named so far (full names, in the order they appeared) and the one whose block is being read (0 for none). */
	struct wb_vector layers;
	int layer;
};

/*
 * A pseudo-class name and what it is.
 */
struct parser_pseudo_name {
	const char *name;
	int pseudo;
};

/* The pseudo-classes this pass evaluates. */
static const struct parser_pseudo_name parser_pseudo_names[] = {
	{ "root", CSS_PSEUDO_ROOT },
	{ "first-child", CSS_PSEUDO_FIRST_CHILD },
	{ "last-child", CSS_PSEUDO_LAST_CHILD },
	{ "only-child", CSS_PSEUDO_ONLY_CHILD },
	{ "empty", CSS_PSEUDO_EMPTY },
	{ "link", CSS_PSEUDO_LINK },
	{ "visited", CSS_PSEUDO_VISITED },
	{ "any-link", CSS_PSEUDO_ANY_LINK },
	{ "first-of-type", CSS_PSEUDO_FIRST_OF_TYPE },
	{ "last-of-type", CSS_PSEUDO_LAST_OF_TYPE },
	{ "only-of-type", CSS_PSEUDO_ONLY_OF_TYPE },
	{ "disabled", CSS_PSEUDO_DISABLED },
	{ "enabled", CSS_PSEUDO_ENABLED },
	{ "checked", CSS_PSEUDO_CHECKED },
	{ "placeholder-shown", CSS_PSEUDO_PLACEHOLDER_SHOWN },
	{ "required", CSS_PSEUDO_REQUIRED },
	{ "optional", CSS_PSEUDO_OPTIONAL },
	{ "defined", CSS_PSEUDO_ALWAYS },
	{ "scope", CSS_PSEUDO_SCOPE },
	{ "target", CSS_PSEUDO_TARGET },
	{ "popover-open", CSS_PSEUDO_POPOVER_OPEN },
	{ "active-view-transition", CSS_PSEUDO_ACTIVE_VT },
	{ NULL, 0 }
};

/*
 * The functional pseudo-classes by their names (the ones not listed never
 * match); the state pseudo-classes (:hover, :focus and the like) are not
 * listed either: this pass draws a page at rest.
 */
static const struct parser_pseudo_name parser_pseudo_functions[] = {
	{ "not", CSS_PSEUDO_NOT },
	{ "is", CSS_PSEUDO_IS },
	{ "matches", CSS_PSEUDO_IS },
	{ "-webkit-any", CSS_PSEUDO_IS },
	{ "where", CSS_PSEUDO_IS },
	{ "has", CSS_PSEUDO_HAS },
	{ "lang", CSS_PSEUDO_LANG },
	{ "active-view-transition-type", CSS_PSEUDO_ACTIVE_VT_TYPE },
	{ "nth-child", CSS_PSEUDO_NTH_CHILD },
	{ "nth-last-child", CSS_PSEUDO_NTH_LAST_CHILD },
	{ "nth-of-type", CSS_PSEUDO_NTH_OF_TYPE },
	{ "nth-last-of-type", CSS_PSEUDO_NTH_LAST_OF_TYPE },
	{ NULL, 0 }
};

static size_t parser_skip_block(const struct css_token *tokens, size_t count, size_t index);
static size_t parser_skip_at_rule(const struct css_token *tokens, size_t count, size_t index);
static int parser_rules(struct parser_state *state, const struct css_token *tokens, size_t count, const struct css_media *media);
static int parser_at_rule(struct parser_state *state, const struct css_token *tokens, size_t count, const struct css_media *media);
static int parser_keep_imports(struct parser_state *state);
static int parser_supports(struct parser_state *state, const struct css_token *tokens, size_t count, int *status);
static int parser_supports_group(struct parser_state *state, const struct css_token *tokens, size_t count, int *status);
static int parser_import(struct parser_state *state, const struct css_token *tokens, size_t count);
static int parser_font_face(struct parser_state *state, const struct css_token *tokens, size_t count);
static int parser_keyframes(struct parser_state *state, const struct css_token *prelude, size_t prelude_count, const struct css_token *body, size_t body_count);
static int parser_position_try(struct parser_state *state, const struct css_token *prelude, size_t prelude_count, const struct css_token *body, size_t body_count);
static int parser_font_descriptor(struct parser_state *state, const struct css_token *name, const struct css_token *tokens, size_t count, struct css_font_face *face);
static int parser_font_family(struct parser_state *state, const struct css_token *tokens, size_t count, struct vm_string **family);
static int parser_font_sources(struct parser_state *state, const struct css_token *tokens, size_t count, struct css_font_face *face);
static int parser_font_weight(const struct css_token *token, int *weight);
static int parser_font_format(const struct css_token *format);
static int parser_keep_font_faces(struct parser_state *state);
static int parser_add_rule(struct parser_state *state, struct token_range prelude, struct token_range block, const struct css_media *media);
static int parser_selectors(struct vm_heap *heap, struct wb_arena *arena, struct token_range prelude, struct css_selector **selectors, size_t *count);
static int parser_layer_path(struct parser_state *state, const struct css_token *tokens, size_t count, int *index);
static int parser_layer_names(struct parser_state *state, const struct css_token *tokens, size_t count);
static int parser_layer_block(struct parser_state *state, const struct css_token *tokens, size_t count);
static int parser_scope(struct parser_state *state, const struct css_token *prelude, size_t prelude_count, const struct css_token *body, size_t body_count, const struct css_media *media);
static int parser_scope_selectors(struct parser_state *state, struct token_range range, const struct css_selector **selectors, size_t *count);
static int parser_scoped_selectors(struct parser_state *state, struct token_range prelude, struct css_selector **selectors, size_t *count);
static int parser_names_scope(const struct css_selector *selector);
static int parser_prepend_scope(struct wb_arena *arena, struct css_selector *selector, int combinator);
static size_t parser_scope_declaration(const struct css_token *tokens, size_t count, size_t index);
static int parser_scope_self(struct parser_state *state, const struct css_token *tokens, size_t count, const struct css_media *media);
static int parser_selector(struct vm_heap *heap, struct wb_arena *arena, struct token_range tokens, struct css_selector *selector);
static int parser_compound(struct vm_heap *heap, struct wb_arena *arena, const struct css_token *tokens, size_t count, size_t *index, struct css_compound *compound, uint32_t *specificity);
static int parser_attribute(struct vm_heap *heap, const struct css_token *tokens, size_t count, struct css_simple *simple);
static int parser_pseudo(struct vm_heap *heap, struct wb_arena *arena, const struct css_token *tokens, size_t count, size_t *index, struct css_simple *simple, uint32_t *specificity);
static void parser_pseudo_element(const struct css_token *token, struct css_simple *simple);
static int parser_type_list(struct vm_heap *heap, struct token_range arguments, struct css_simple *simple);
static int parser_view_transition_argument(struct vm_heap *heap, const struct css_token *tokens, size_t count, size_t *index, struct css_compound *compound);
static int parser_pseudo_arguments(struct vm_heap *heap, struct wb_arena *arena, struct token_range arguments, int relative, struct css_simple *simple, uint32_t *specificity);
static int parser_nth(struct token_range arguments, int *a, int *b);
static int parser_declarations(struct vm_heap *heap, struct wb_arena *arena, struct token_range block, struct css_declaration **declarations, size_t *count);
static int parser_special(struct vm_heap *heap, const struct css_token *name, struct token_range value, struct css_declaration *out, size_t *made);
static struct vm_string *parser_atom(struct vm_heap *heap, const struct css_token *token, int lower);
static struct token_range parser_trim(struct token_range range);

/*
 * Parses a style sheet into its rules.
 */
int
css_parse_sheet(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length,
	int origin,
	struct css_sheet *sheet)
{
	struct css_token *tokens;
	struct parser_state state;
	size_t count;
	int error;

	/* Tokenizes the text into the sheet's arena. */
	memset(sheet, 0, sizeof(*sheet));
	wb_arena_init(&sheet->arena, 0);
	sheet->origin = origin;
	error = css_tokenize(&sheet->arena, units, length, &tokens, &count);
	if (error != 0)
		return error;

	/* Reads the rules, the ones in @media and @supports blocks too. */
	state.heap = heap;
	state.sheet = sheet;
	state.scope = NULL;
	state.layer = 0;
	wb_vector_init(&state.layers, sizeof(struct vm_string *));
	wb_vector_init(&state.rules, sizeof(struct css_rule));
	wb_vector_init(&state.imports, sizeof(struct vm_string *));
	wb_vector_init(&state.import_media, sizeof(struct css_media *));
	wb_vector_init(&state.font_faces, sizeof(struct css_font_face));
	wb_vector_init(&state.try_rules, sizeof(struct css_try_rule));
	wb_vector_init(&state.keyframes, sizeof(struct css_keyframes_rule));
	error = parser_rules(&state, tokens, count, NULL);
	if (error == 0)
		error = parser_keep_imports(&state);
	if (error == 0)
		error = parser_keep_font_faces(&state);
	if (error == 0 && state.try_rules.count != 0) {
		sheet->try_rules = wb_arena_alloc(&sheet->arena, state.try_rules.count * sizeof(struct css_try_rule));
		if (sheet->try_rules == NULL)
			error = ENOMEM;
		else
			memcpy(sheet->try_rules, state.try_rules.items, state.try_rules.count * sizeof(struct css_try_rule));
		sheet->try_rule_count = state.try_rules.count;
	}
	wb_vector_release(&state.imports);
	wb_vector_release(&state.import_media);
	wb_vector_release(&state.font_faces);
	if (error == 0 && state.keyframes.count != 0) {
		sheet->keyframes = wb_arena_alloc(&sheet->arena, state.keyframes.count * sizeof(struct css_keyframes_rule));
		if (sheet->keyframes == NULL)
			error = ENOMEM;
		else
			memcpy(sheet->keyframes, state.keyframes.items, state.keyframes.count * sizeof(struct css_keyframes_rule));
		sheet->keyframe_count = state.keyframes.count;
	}
	wb_vector_release(&state.keyframes);
	wb_vector_release(&state.try_rules);
	wb_vector_release(&state.layers);
	if (error != 0) {
		wb_vector_release(&state.rules);
		return error;
	}

	/* Moves the rules into the arena. */
	if (state.rules.count != 0) {
		sheet->rules = wb_arena_alloc(&sheet->arena, state.rules.count * sizeof(struct css_rule));
		if (sheet->rules == NULL) {
			wb_vector_release(&state.rules);
			return ENOMEM;
		}

		/* Copies them. */
		memcpy(sheet->rules, state.rules.items, state.rules.count * sizeof(struct css_rule));
		sheet->rule_count = state.rules.count;
	}

	/* The list is no longer needed. */
	wb_vector_release(&state.rules);

	/* Files the selectors in the rule index. */
	error = css_index_build(sheet);
	if (error != 0)
		return error;

	/* Succeeded: the sheet holds its rules. */
	return 0;
}

/*
 * Parses the declarations of a style attribute (in an arena the caller
 * owns).
 */
int
css_parse_declarations(
	struct vm_heap *heap,
	struct wb_arena *arena,
	const uint16_t *units,
	size_t length,
	struct css_declaration **declarations,
	size_t *count)
{
	struct css_token *tokens;
	struct token_range block;
	size_t token_count;
	int error;

	/* Tokenizes the text. */
	error = css_tokenize(arena, units, length, &tokens, &token_count);
	if (error != 0)
		return error;

	/* Everything but the end-of-file token is one declaration block. */
	block.tokens = tokens;
	block.count = token_count - 1U;
	error = parser_declarations(heap, arena, block, declarations, count);
	if (error != 0)
		return error;

	/* Succeeded: the declarations are in the arena. */
	return 0;
}

/*
 * Frees a parsed style sheet.
 */
void
css_sheet_release(
	struct css_sheet *sheet)
{
	/* Everything the sheet holds is in its arena. */
	wb_arena_release(&sheet->arena);
	sheet->rules = NULL;
	sheet->rule_count = 0;
}

/*
 * Parses an author style sheet into a sheet of its own.
 */
int
css_sheet_create(
	struct css_sheet **sheet,
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length)
{
	struct css_sheet *made;
	int error;

	/* Allocates the sheet. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Parses the text into it. */
	error = css_parse_sheet(heap, units, length, CSS_ORIGIN_AUTHOR, made);
	if (error != 0) {
		css_sheet_release(made);
		free(made);
		return error;
	}

	/* Succeeded: the caller owns the sheet. */
	*sheet = made;
	return 0;
}

/*
 * Frees a sheet made by css_sheet_create.
 */
void
css_sheet_destroy(
	struct css_sheet *sheet)
{
	/* A NULL sheet is nothing to free. */
	if (sheet == NULL)
		return;

	/* Its arena, then the sheet. */
	css_sheet_release(sheet);
	free(sheet);
}

/*
 * Parses a selector list a script gave (querySelector, matches and
 * closest).  Returns 0 with a list the caller frees with
 * css_query_destroy, EINVAL when the text is not a selector list (the
 * script's SyntaxError), or ENOMEM.
 */
int
css_query_parse(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length,
	struct css_query **query)
{
	struct css_query *made;
	struct css_token *tokens;
	struct token_range range;
	size_t count;
	int error;

	/* The list, with its own arena. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	wb_arena_init(&made->arena, 0);

	/* The text's tokens, without the end-of-file token. */
	error = css_tokenize(&made->arena, units, length, &tokens, &count);
	if (error != 0) {
		css_query_destroy(made);
		return error;
	}

	/* The last token is the end of the file. */
	if (count > 0)
		count--;

	/* The selectors, as a rule's prelude is read; nothing but whitespace is no list. */
	range.tokens = tokens;
	range.count = count;
	range = parser_trim(range);
	if (range.count == 0) {
		css_query_destroy(made);
		return EINVAL;
	}

	/* A list that does not parse is the script's syntax error. */
	error = parser_selectors(heap, &made->arena, range, &made->selectors, &made->count);
	if (error != 0) {
		css_query_destroy(made);
		return error;
	}

	/* Succeeded: the list is the caller's. */
	*query = made;
	return 0;
}

/*
 * Tells whether a declaration's text ("name: value") is one this pass
 * reads, a known property with a value it takes, or a custom property
 * (the inline style of scripts keeps only those, as other browsers keep
 * only what they read).  Reports 0 with the answer in *valid, or ENOMEM.
 */
int
css_declaration_valid(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length,
	int *valid)
{
	struct css_declaration *declarations;
	struct wb_arena arena;
	size_t count;
	int error;

	/* The declarations, in an arena of their own. */
	*valid = 0;
	wb_arena_init(&arena, 0);
	error = css_parse_declarations(heap, &arena, units, length, &declarations, &count);
	wb_arena_release(&arena);
	if (error == ENOMEM)
		return error;

	/* Succeeded: a declaration that parses leaves at least one behind. */
	if (error == 0 && count > 0)
		*valid = 1;
	return 0;
}

/*
 * Frees a selector list made by css_query_parse.
 */
void
css_query_destroy(
	struct css_query *query)
{
	/* A NULL list is nothing to free. */
	if (query == NULL)
		return;

	/* Its arena, then the list. */
	wb_arena_release(&query->arena);
	free(query);
}

/*
 * Tells how many sheets a sheet's @import rules name.
 */
size_t
css_sheet_import_count(
	const struct css_sheet *sheet)
{
	/* The number of imports. */
	return sheet->import_count;
}

/*
 * Gives the URL (an atom, as the sheet wrote it) of one of a sheet's
 * @import rules.
 */
struct vm_string *
css_sheet_import(
	const struct css_sheet *sheet,
	size_t index)
{
	/* The import's URL. */
	return sheet->imports[index];
}

/*
 * Gives the media list of one of a sheet's @import rules (NULL: none, the
 * sheet applies to every medium).
 */
const struct css_media *
css_sheet_import_media(
	const struct css_sheet *sheet,
	size_t index)
{
	/* The import's list. */
	return sheet->import_media[index];
}

/*
 * Tells how many @font-face rules a sheet holds (ws074-p070).
 */
size_t
css_sheet_font_face_count(
	const struct css_sheet *sheet)
{
	/* The number of faces. */
	return sheet->font_face_count;
}

/*
 * Gives one of a sheet's @font-face rules.
 */
const struct css_font_face *
css_sheet_font_face(
	const struct css_sheet *sheet,
	size_t index)
{
	/* The face. */
	return &sheet->font_faces[index];
}

/*
 * Tells how many style rules a sheet holds.
 */
size_t
css_sheet_rule_count(
	const struct css_sheet *sheet)
{
	/* The number of rules. */
	return sheet->rule_count;
}

/*
 * Resolves every URL the sheet's declarations name (and its imports)
 * with the caller's resolver, so that they no longer depend on where the
 * sheet came from.
 */
int
css_sheet_resolve_urls(
	struct css_sheet *sheet,
	css_url_resolver resolve,
	void *context)
{
	struct css_declaration *declaration;
	struct vm_string *resolved;
	size_t rule;
	size_t position;
	int error;

	/* The declarations of every rule. */
	for (rule = 0; rule < sheet->rule_count; rule++) {
		for (position = 0; position < sheet->rules[rule].declaration_count; position++) {
			declaration = &sheet->rules[rule].declarations[position];

			/* Only a URL value is resolved. */
			if (declaration->value.kind != CSS_VALUE_URL || declaration->value.url == NULL)
				continue;

			/* The resolver's URL replaces the written one; one it cannot resolve stays. */
			error = resolve(context, declaration->value.url, &resolved);
			if (error == ENOMEM)
				return error;
			if (error == 0)
				declaration->value.url = resolved;
		}
	}

	/* The imports. */
	for (position = 0; position < sheet->import_count; position++) {
		error = resolve(context, sheet->imports[position], &resolved);
		if (error == ENOMEM)
			return error;
		if (error == 0)
			sheet->imports[position] = resolved;
	}

	/* The sources of the @font-face rules. */
	for (rule = 0; rule < sheet->font_face_count; rule++) {
		for (position = 0; position < sheet->font_faces[rule].source_count; position++) {
			error = resolve(context, sheet->font_faces[rule].sources[position], &resolved);
			if (error == ENOMEM)
				return error;
			if (error == 0)
				sheet->font_faces[rule].sources[position] = resolved;
		}
	}

	/* Succeeded: the URLs are resolved. */
	return 0;
}

/*
 * Tells whether a token's text is an ASCII word, ignoring ASCII case.
 */
int
css_ident_equal(
	const struct css_token *token,
	const char *ascii)
{
	int same;

	/* Compares the text. */
	same = css_units_equal_ascii(token->text, token->length, ascii);

	/* Reports the answer. */
	return same;
}

/*
 * Tells whether UTF-16 units are an ASCII word, ignoring ASCII case.
 */
int
css_units_equal_ascii(
	const uint16_t *units,
	size_t length,
	const char *ascii)
{
	uint16_t unit;
	size_t index;

	/* Compares unit by unit, folding ASCII upper case. */
	for (index = 0; index < length; index++) {
		if (ascii[index] == '\0')
			return 0;
		unit = units[index];
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (unit != (unsigned char)ascii[index])
			return 0;
	}

	/* The word must end where the units do. */
	if (ascii[length] != '\0')
		return 0;

	/* The two are equal. */
	return 1;
}

/* Skips a block that starts at index (a function, parenthesis, bracket or brace) to just past its end. */
static size_t
parser_skip_block(
	const struct css_token *tokens,
	size_t count,
	size_t index)
{
	int depth;
	int type;

	/* Counts the opening and closing tokens of every kind together. */
	depth = 0;
	while (index < count) {
		type = tokens[index].type;
		index++;
		if (type == CSS_TOKEN_EOF)
			return index - 1U;
		if (type == CSS_TOKEN_OPEN_CURLY || type == CSS_TOKEN_OPEN_PAREN ||
		    type == CSS_TOKEN_OPEN_SQUARE || type == CSS_TOKEN_FUNCTION)
			depth++;
		if (type == CSS_TOKEN_CLOSE_CURLY || type == CSS_TOKEN_CLOSE_PAREN || type == CSS_TOKEN_CLOSE_SQUARE)
			depth--;
		if (depth == 0)
			return index;
	}

	/* The block runs to the end. */
	return index;
}

/* Skips an at-rule: to its semicolon, or past its block. */
static size_t
parser_skip_at_rule(
	const struct css_token *tokens,
	size_t count,
	size_t index)
{
	/* Moves past the at-keyword, then to the end of the rule. */
	index++;
	while (index < count && tokens[index].type != CSS_TOKEN_EOF) {
		if (tokens[index].type == CSS_TOKEN_SEMICOLON)
			return index + 1U;
		if (tokens[index].type == CSS_TOKEN_OPEN_CURLY)
			return parser_skip_block(tokens, count, index);
		if (tokens[index].type == CSS_TOKEN_OPEN_PAREN || tokens[index].type == CSS_TOKEN_OPEN_SQUARE ||
		    tokens[index].type == CSS_TOKEN_FUNCTION) {
			index = parser_skip_block(tokens, count, index);
			continue;
		}

		/* Anything else belongs to the rule. */
		index++;
	}

	/* The rule ran to the end. */
	return index;
}

/*
 * Reads a list of rules (a whole sheet, or the block of an @media or
 * @supports rule) into the state; media is the list the rules are nested
 * in (NULL at the top).  A rule that does not parse is dropped.
 */
static int
parser_rules(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	const struct css_media *media)
{
	struct token_range prelude;
	struct token_range block;
	size_t index;
	size_t start;
	size_t end;
	int error;

	/* Reads rule after rule. */
	index = 0;
	while (index < count && tokens[index].type != CSS_TOKEN_EOF) {
		/* Whitespace and the HTML comment markers between rules are skipped. */
		if (tokens[index].type == CSS_TOKEN_WHITESPACE ||
		    tokens[index].type == CSS_TOKEN_CDO ||
		    tokens[index].type == CSS_TOKEN_CDC) {
			index++;
			continue;
		}

		/* An at-rule runs to its semicolon or past its block. */
		if (tokens[index].type == CSS_TOKEN_AT_KEYWORD) {
			end = parser_skip_at_rule(tokens, count, index);
			error = parser_at_rule(state, tokens + index, end - index, media);
			if (error == ENOMEM)
				return error;
			index = end;
			continue;
		}

		/* Inside @scope, a declaration (a name and a colon, ending before any block) applies to the scope's root. */
		if (state->scope != NULL) {
			end = parser_scope_declaration(tokens, count, index);
			if (end > index) {
				error = parser_scope_self(state, tokens + index, end - index, media);
				if (error == ENOMEM)
					return error;
				index = end;
				continue;
			}
		}

		/* A qualified rule: the prelude up to the block, then the block. */
		start = index;
		while (index < count && tokens[index].type != CSS_TOKEN_OPEN_CURLY && tokens[index].type != CSS_TOKEN_EOF) {
			if (tokens[index].type == CSS_TOKEN_OPEN_PAREN || tokens[index].type == CSS_TOKEN_OPEN_SQUARE ||
			    tokens[index].type == CSS_TOKEN_FUNCTION) {
				index = parser_skip_block(tokens, count, index);
				continue;
			}

			/* Anything else is part of the prelude. */
			index++;
		}

		/* A prelude without a block ends the list. */
		if (index >= count || tokens[index].type == CSS_TOKEN_EOF)
			break;
		prelude.tokens = tokens + start;
		prelude.count = index - start;
		end = parser_skip_block(tokens, count, index);
		block.tokens = tokens + index + 1U;
		block.count = end - index - 1U;
		if (end > index + 1U && tokens[end - 1U].type == CSS_TOKEN_CLOSE_CURLY)
			block.count--;
		index = end;

		/* Adds the rule (a rule that does not parse is dropped). */
		error = parser_add_rule(state, prelude, block, media);
		if (error == ENOMEM)
			return error;
	}

	/* Succeeded: the list's rules are read. */
	return 0;
}

/*
 * Reads one layer name path (identifiers joined by dots, under the layer being read) from tokens and makes it a
 * layer of the sheet when it is new; reports its place (1 for the first layer; 0 when the tokens are no name).
 */
static int
parser_layer_path(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	int *index)
{
	uint16_t units[256];
	struct vm_string **names;
	struct vm_string *name;
	size_t length;
	size_t at;
	size_t unit;
	int found;
	int error;

	/* The enclosing layer's name, a dot, then the path. */
	*index = 0;
	length = 0;
	if (state->layer != 0) {
		names = state->layers.items;
		for (unit = 0; unit < names[state->layer - 1]->length && length < 250U; unit++)
			units[length++] = vm_string_at(names[state->layer - 1], unit);
		units[length++] = (uint16_t)'.';
	}
	found = 0;
	for (at = 0; at < count; at++) {
		if (tokens[at].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[at].type == CSS_TOKEN_DELIM && tokens[at].length == 1U && tokens[at].text[0] == '.') {
			if (length < 250U)
				units[length++] = (uint16_t)'.';
			continue;
		}
		if (tokens[at].type != CSS_TOKEN_IDENT)
			return 0;
		for (unit = 0; unit < tokens[at].length && length < 250U; unit++)
			units[length++] = tokens[at].text[unit];
		found = 1;
	}
	if (!found)
		return 0;

	/* A name the sheet has is its place; a new one is added at the end. */
	name = vm_atom_from_units(state->heap, units, length);
	if (name == NULL)
		return ENOMEM;
	names = state->layers.items;
	for (at = 0; at < state->layers.count; at++) {
		if (names[at] == name) {
			*index = (int)at + 1;
			return 0;
		}
	}
	error = wb_vector_push(&state->layers, &name);
	if (error != 0)
		return error;
	*index = (int)state->layers.count;
	return 0;
}

/* Names the layers of an @layer statement (comma separated paths), in the order given. */
static int
parser_layer_names(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count)
{
	size_t start;
	size_t at;
	int index;
	int error;

	start = 0;
	for (at = 0; at <= count; at++) {
		if (at < count && tokens[at].type != CSS_TOKEN_COMMA && tokens[at].type != CSS_TOKEN_SEMICOLON)
			continue;
		error = parser_layer_path(state, tokens + start, at - start, &index);
		if (error != 0 && error != EINVAL)
			return error;
		start = at + 1U;
	}
	return 0;
}

/*
 * The layer an @layer block's rules are in: the one its name makes, or a layer of its own (an anonymous one is
 * the next after those named so far).
 */
static int
parser_layer_block(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count)
{
	struct vm_string *name;
	size_t at;
	int named;
	int index;

	named = 0;
	for (at = 0; at < count; at++) {
		if (tokens[at].type != CSS_TOKEN_WHITESPACE)
			named = 1;
	}
	if (named) {
		if (parser_layer_path(state, tokens, count, &index) != 0)
			return state->layer;
		return index;
	}

	/* An anonymous layer: a name no path can have (a space and its place). */
	{
		uint16_t units[16];
		size_t length;
		unsigned number;

		length = 0;
		units[length++] = (uint16_t)' ';
		number = (unsigned)state->layers.count + 1U;
		do {
			units[length++] = (uint16_t)('0' + number % 10U);
			number /= 10U;
		} while (number != 0 && length < 15U);
		name = vm_atom_from_units(state->heap, units, length);
		if (name == NULL || wb_vector_push(&state->layers, &name) != 0)
			return state->layer;
		return (int)state->layers.count;
	}
}

/*
 * Reads one at-rule (its tokens from the at-keyword to its semicolon or
 * the end of its block): @import before the style rules, the rules of an
 * @media block under its media list, of an @supports block whose
 * condition holds, and of an @layer block, and an @font-face rule's
 * descriptors; any other at-rule is skipped.
 */
static int
parser_at_rule(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	const struct css_media *media)
{
	struct css_media *nested;
	size_t open;
	size_t body_count;
	int outer_layer;
	int is_media;
	int is_supports;
	int is_layer;
	int is_import;
	int is_font_face;
	int is_scope;
	int is_try;
	int is_keyframes;
	int holds;
	int error;

	/* An @import counts only before the style rules and at the top. */
	is_import = css_ident_equal(&tokens[0], "import");
	if (is_import) {
		if (state->rules.count != 0 || media != NULL)
			return 0;
		error = parser_import(state, tokens + 1, count - 1U);
		if (error == ENOMEM)
			return error;
		return 0;
	}

	/* The rest that matter have a block: its opening brace. */
	is_media = css_ident_equal(&tokens[0], "media");
	is_supports = css_ident_equal(&tokens[0], "supports");
	is_layer = css_ident_equal(&tokens[0], "layer");
	is_font_face = css_ident_equal(&tokens[0], "font-face");
	is_scope = css_ident_equal(&tokens[0], "scope");
	is_try = css_ident_equal(&tokens[0], "position-try");
	is_keyframes = css_ident_equal(&tokens[0], "keyframes") || css_ident_equal(&tokens[0], "-webkit-keyframes");
	if (!is_media && !is_supports && !is_layer && !is_font_face && !is_scope && !is_try && !is_keyframes)
		return 0;
	open = 1;
	while (open < count && tokens[open].type != CSS_TOKEN_OPEN_CURLY)
		open++;
	if (open >= count) {
		/* @layer a, b; names the layers, in this order. */
		if (is_layer) {
			error = parser_layer_names(state, tokens + 1, count - 1U);
			if (error == ENOMEM)
				return error;
		}
		return 0;
	}

	/* The block's rules, without its closing brace. */
	body_count = count - open - 1U;
	if (body_count > 0 && tokens[count - 1U].type == CSS_TOKEN_CLOSE_CURLY)
		body_count--;

	/* An @font-face block's descriptors (under an @media list too: the face is kept whatever the medium). */
	if (is_font_face) {
		error = parser_font_face(state, tokens + open + 1U, body_count);
		if (error == ENOMEM)
			return error;
		return 0;
	}

	/* An @keyframes block's keyframes, kept under its name. */
	if (is_keyframes) {
		error = parser_keyframes(state, tokens + 1, open - 1U, tokens + open + 1U, body_count);
		return error;
	}

	/* An @position-try block's declarations, kept under its name. */
	if (is_try) {
		error = parser_position_try(state, tokens + 1, open - 1U, tokens + open + 1U, body_count);
		return error;
	}

	/* An @scope block's rules apply in its scope. */
	if (is_scope) {
		error = parser_scope(state, tokens + 1, open - 1U, tokens + open + 1U, body_count, media);
		return error;
	}

	/* An @media block's rules hold under its list and the ones it is nested in. */
	if (is_media) {
		error = css_media_parse(&state->sheet->arena, tokens + 1, open - 1U, media, &nested);
		if (error != 0)
			return error;
		error = parser_rules(state, tokens + open + 1U, body_count, nested);
		return error;
	}

	/* An @supports block counts when its condition holds (decided now: it does not depend on the page). */
	if (is_supports) {
		error = 0;
		holds = parser_supports(state, tokens + 1, open - 1U, &error);
		if (error != 0)
			return error;
		if (!holds)
			return 0;
		error = parser_rules(state, tokens + open + 1U, body_count, media);
		return error;
	}

	/* An @layer block's rules are in the layer it names (or one of its own without a name). */
	outer_layer = state->layer;
	state->layer = parser_layer_block(state, tokens + 1, open - 1U);
	error = parser_rules(state, tokens + open + 1U, body_count, media);
	state->layer = outer_layer;
	if (error != 0)
		return error;

	/* Succeeded: the at-rule is read. */
	return 0;
}

/*
 * Reads an @keyframes rule: its name (an identifier or a string) and its keyframes, each a list of offsets (from, to or
 * a percentage) with the declarations of its block (one keyframe a offset).
 */
static int
parser_keyframes(
	struct parser_state *state,
	const struct css_token *prelude,
	size_t prelude_count,
	const struct css_token *body,
	size_t body_count)
{
	struct css_keyframes_rule rule;
	struct css_keyframe frame;
	struct wb_vector frames;
	struct token_range block;
	size_t index;
	size_t name;
	size_t open;
	size_t end;
	size_t at;
	int error;

	/* The name, alone. */
	name = prelude_count;
	for (index = 0; index < prelude_count; index++) {
		if (prelude[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (name != prelude_count || (prelude[index].type != CSS_TOKEN_IDENT && prelude[index].type != CSS_TOKEN_STRING))
			return 0;
		name = index;
	}
	if (name == prelude_count)
		return 0;
	memset(&rule, 0, sizeof(rule));
	rule.name = vm_atom_from_units(state->heap, prelude[name].text, prelude[name].length);
	if (rule.name == NULL)
		return ENOMEM;

	/* Each keyframe rule: its offsets, then its block. */
	wb_vector_init(&frames, sizeof(struct css_keyframe));
	index = 0;
	error = 0;
	while (index < body_count && error == 0) {
		open = index;
		while (open < body_count && body[open].type != CSS_TOKEN_OPEN_CURLY)
			open++;
		if (open >= body_count)
			break;
		end = parser_skip_block(body, body_count, open);
		block.tokens = body + open + 1U;
		block.count = end - open - 1U;
		if (block.count > 0 && block.tokens[block.count - 1U].type == CSS_TOKEN_CLOSE_CURLY)
			block.count--;
		memset(&frame, 0, sizeof(frame));
		error = parser_declarations(state->heap, &state->sheet->arena, block, &frame.declarations, &frame.declaration_count);
		if (error != 0)
			break;

		/* One keyframe for each offset of the list. */
		for (at = index; at < open; at++) {
			if (body[at].type == CSS_TOKEN_IDENT && css_ident_equal(&body[at], "from"))
				frame.offset = 0.0f;
			else if (body[at].type == CSS_TOKEN_IDENT && css_ident_equal(&body[at], "to"))
				frame.offset = 1.0f;
			else if (body[at].type == CSS_TOKEN_PERCENTAGE && body[at].number >= 0 && body[at].number <= 100)
				frame.offset = (float)(body[at].number / 100.0);
			else
				continue;
			error = wb_vector_push(&frames, &frame);
			if (error != 0)
				break;
		}
		index = end;
	}
	if (error == 0 && frames.count != 0) {
		rule.frames = wb_arena_alloc(&state->sheet->arena, frames.count * sizeof(struct css_keyframe));
		if (rule.frames == NULL) {
			error = ENOMEM;
		} else {
			memcpy(rule.frames, frames.items, frames.count * sizeof(struct css_keyframe));
			rule.frame_count = frames.count;
			error = wb_vector_push(&state->keyframes, &rule);
		}
	}
	wb_vector_release(&frames);
	return error == ENOMEM ? error : 0;
}

/*
 * Reads an @position-try rule: its name (a dashed identifier) and the
 * declarations of its block, kept for the fallbacks that name it.
 * https://drafts.csswg.org/css-anchor-position-1/#fallback-rule
 */
static int
parser_position_try(
	struct parser_state *state,
	const struct css_token *prelude,
	size_t prelude_count,
	const struct css_token *body,
	size_t body_count)
{
	struct css_try_rule rule;
	struct token_range block;
	size_t index;
	size_t name;
	int error;

	/* The name, alone. */
	name = prelude_count;
	for (index = 0; index < prelude_count; index++) {
		if (prelude[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (name != prelude_count || prelude[index].type != CSS_TOKEN_IDENT || prelude[index].length < 3 ||
		    prelude[index].text[0] != '-' || prelude[index].text[1] != '-')
			return 0;
		name = index;
	}
	if (name == prelude_count)
		return 0;
	memset(&rule, 0, sizeof(rule));
	rule.layer = state->layer;
	rule.name = vm_atom_from_units(state->heap, prelude[name].text, prelude[name].length);
	if (rule.name == NULL)
		return ENOMEM;

	/* The declarations. */
	block.tokens = body;
	block.count = body_count;
	error = parser_declarations(state->heap, &state->sheet->arena, block, &rule.declarations, &rule.declaration_count);
	if (error != 0)
		return error == ENOMEM ? error : 0;

	/* Succeeded: the rule is kept. */
	return wb_vector_push(&state->try_rules, &rule);
}

/*
 * Reads an @scope rule: its prelude ((roots) and "to (limits)", both
 * optional) and the rules of its block, which apply in the scope.  A
 * prelude that does not parse drops the rule.
 */
static int
parser_scope(
	struct parser_state *state,
	const struct css_token *prelude,
	size_t prelude_count,
	const struct css_token *body,
	size_t body_count,
	const struct css_media *media)
{
	const struct css_scope *outer;
	struct css_scope *scope;
	struct token_range range;
	size_t index;
	size_t end;
	int is_to;
	int error;

	/* The scope, in the sheet's arena. */
	scope = wb_arena_zalloc(&state->sheet->arena, sizeof(*scope));
	if (scope == NULL)
		return ENOMEM;
	scope->parent = state->scope;

	/* The roots in parentheses, if any. */
	index = 0;
	while (index < prelude_count && prelude[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index < prelude_count && prelude[index].type == CSS_TOKEN_OPEN_PAREN) {
		end = parser_skip_block(prelude, prelude_count, index);
		range.tokens = prelude + index + 1U;
		range.count = end - index - 2U;
		range = parser_trim(range);
		error = parser_scope_selectors(state, range, &scope->roots, &scope->root_count);
		if (error != 0)
			return error;
		index = end;
		while (index < prelude_count && prelude[index].type == CSS_TOKEN_WHITESPACE)
			index++;
	}

	/* The limits after "to", if any. */
	if (index < prelude_count) {
		is_to = 0;
		if (prelude[index].type == CSS_TOKEN_IDENT)
			is_to = css_ident_equal(&prelude[index], "to");
		if (!is_to)
			return 0;
		index++;
		while (index < prelude_count && prelude[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (index >= prelude_count || prelude[index].type != CSS_TOKEN_OPEN_PAREN)
			return 0;
		end = parser_skip_block(prelude, prelude_count, index);
		range.tokens = prelude + index + 1U;
		range.count = end - index - 2U;
		range = parser_trim(range);

		/* The limits are relative to the root, like the scope's own selectors. */
		outer = state->scope;
		state->scope = scope;
		error = parser_scoped_selectors(state, range, (struct css_selector **)&scope->limits, &scope->limit_count);
		state->scope = outer;
		if (error != 0)
			return error == ENOMEM ? error : 0;
		index = end;
		while (index < prelude_count && prelude[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (index < prelude_count)
			return 0;
	}

	/* The block's rules, read in the scope. */
	outer = state->scope;
	state->scope = scope;
	error = parser_rules(state, body, body_count, media);
	state->scope = outer;
	if (error != 0)
		return error;

	/* Succeeded: the scope's rules are added. */
	return 0;
}

/*
 * Parses the root selectors of an @scope: plain selectors, or, nested in
 * another @scope, selectors relative to that scope's root.
 */
static int
parser_scope_selectors(
	struct parser_state *state,
	struct token_range range,
	const struct css_selector **selectors,
	size_t *count)
{
	struct css_selector *list;
	int error;

	/* Nested in another scope, relative to its root. */
	if (state->scope != NULL) {
		error = parser_scoped_selectors(state, range, &list, count);
		if (error != 0)
			return error;
		*selectors = list;
		return 0;
	}

	/* Otherwise ordinary selectors. */
	error = parser_selectors(state->heap, &state->sheet->arena, range, &list, count);
	if (error != 0)
		return error;

	/* Succeeded: the roots are parsed. */
	*selectors = list;
	return 0;
}

/*
 * Parses a selector list inside @scope.  Each selector is relative to the
 * scope's root: one that starts with a combinator is joined to the root by
 * it, and one with neither :scope nor & anywhere in its compounds is below
 * the root.  The root adds no specificity (:where(:scope)).
 */
static int
parser_scoped_selectors(
	struct parser_state *state,
	struct token_range prelude,
	struct css_selector **selectors,
	size_t *count)
{
	struct css_selector *list;
	struct token_range one;
	size_t start;
	size_t index;
	size_t made;
	size_t commas;
	int depth;
	int combinator;
	int error;

	/* Counts the selectors (commas inside functions do not part them). */
	commas = 0;
	depth = 0;
	for (index = 0; index < prelude.count; index++) {
		if (prelude.tokens[index].type == CSS_TOKEN_FUNCTION || prelude.tokens[index].type == CSS_TOKEN_OPEN_PAREN)
			depth++;
		if (prelude.tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
			depth--;
		if (prelude.tokens[index].type == CSS_TOKEN_COMMA && depth == 0)
			commas++;
	}
	list = wb_arena_zalloc(&state->sheet->arena, (commas + 1U) * sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Each selector between the commas. */
	made = 0;
	start = 0;
	depth = 0;
	for (index = 0; index <= prelude.count; index++) {
		if (index < prelude.count) {
			if (prelude.tokens[index].type == CSS_TOKEN_FUNCTION || prelude.tokens[index].type == CSS_TOKEN_OPEN_PAREN)
				depth++;
			if (prelude.tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
				depth--;
			if (prelude.tokens[index].type != CSS_TOKEN_COMMA || depth != 0)
				continue;
		}
		one.tokens = prelude.tokens + start;
		one.count = index - start;
		one = parser_trim(one);
		start = index + 1U;

		/* A leading combinator relates the selector to the root. */
		combinator = CSS_COMBINATOR_NONE;
		if (one.count > 0 && one.tokens[0].type == CSS_TOKEN_DELIM) {
			if (one.tokens[0].delim == '>')
				combinator = CSS_COMBINATOR_CHILD;
			if (one.tokens[0].delim == '+')
				combinator = CSS_COMBINATOR_NEXT;
			if (one.tokens[0].delim == '~')
				combinator = CSS_COMBINATOR_SUBSEQUENT;
			if (combinator != CSS_COMBINATOR_NONE) {
				one.tokens++;
				one.count--;
				one = parser_trim(one);
			}
		}

		/* The selector itself. */
		error = parser_selector(state->heap, &state->sheet->arena, one, &list[made]);
		if (error != 0)
			return error;

		/* Below the root unless it names the root itself. */
		if (combinator == CSS_COMBINATOR_NONE && !parser_names_scope(&list[made]))
			combinator = CSS_COMBINATOR_DESCENDANT;
		if (combinator != CSS_COMBINATOR_NONE) {
			error = parser_prepend_scope(&state->sheet->arena, &list[made], combinator);
			if (error != 0)
				return error;
		}
		made++;
	}

	/* Succeeded: the list is relative to the scope's root. */
	*selectors = list;
	*count = made;
	return 0;
}

/* Tells whether a selector names the scope's root itself: :scope or & in one of its compounds. */
static int
parser_names_scope(
	const struct css_selector *selector)
{
	const struct css_compound *compound;
	size_t index;
	size_t simple;
	size_t argument;
	int named;

	/* Each simple selector of each compound. */
	for (index = 0; index < selector->count; index++) {
		compound = &selector->compounds[index];
		for (simple = 0; simple < compound->count; simple++) {
			if (compound->simples[simple].kind != CSS_SIMPLE_PSEUDO_CLASS)
				continue;
			if (compound->simples[simple].pseudo == CSS_PSEUDO_SCOPE ||
			    compound->simples[simple].pseudo == CSS_PSEUDO_SCOPE_NEST)
				return 1;

			/* Inside :is(), :where(), :not() and :has() too. */
			for (argument = 0; argument < compound->simples[simple].argument_count; argument++) {
				named = parser_names_scope(&compound->simples[simple].arguments[argument]);
				if (named)
					return 1;
			}
		}
	}

	/* No compound names the root. */
	return 0;
}

/* Puts the scope's root (:where(:scope), no specificity) before a selector, joined by a combinator. */
static int
parser_prepend_scope(
	struct wb_arena *arena,
	struct css_selector *selector,
	int combinator)
{
	struct css_compound *compounds;
	struct css_simple *simple;

	/* The root's compound: one simple selector. */
	simple = wb_arena_zalloc(arena, sizeof(*simple));
	if (simple == NULL)
		return ENOMEM;
	simple->kind = CSS_SIMPLE_PSEUDO_CLASS;
	simple->pseudo = CSS_PSEUDO_SCOPE_NEST;

	/* The compounds with the root's first. */
	compounds = wb_arena_alloc(arena, (selector->count + 1U) * sizeof(*compounds));
	if (compounds == NULL)
		return ENOMEM;
	memset(&compounds[0], 0, sizeof(compounds[0]));
	compounds[0].simples = simple;
	compounds[0].count = 1;
	compounds[0].combinator = CSS_COMBINATOR_NONE;
	compounds[0].pseudo_element = CSS_PSEUDO_ELEMENT_NONE;
	memcpy(compounds + 1, selector->compounds, selector->count * sizeof(*compounds));
	compounds[1].combinator = combinator;

	/* Succeeded: the selector starts at the root. */
	selector->compounds = compounds;
	selector->count++;
	return 0;
}

/*
 * Finds where a declaration inside an @scope block ends: a name, a colon,
 * and a value up to a semicolon (or the end) with no block before it.
 * Reports index itself when what starts there is not a declaration.
 */
static size_t
parser_scope_declaration(
	const struct css_token *tokens,
	size_t count,
	size_t index)
{
	size_t at;

	/* A name, then a colon. */
	if (tokens[index].type != CSS_TOKEN_IDENT)
		return index;
	at = index + 1U;
	while (at < count && tokens[at].type == CSS_TOKEN_WHITESPACE)
		at++;
	if (at >= count || tokens[at].type != CSS_TOKEN_COLON)
		return index;

	/* The value runs to a semicolon; a block first makes it a rule. */
	while (at < count && tokens[at].type != CSS_TOKEN_SEMICOLON && tokens[at].type != CSS_TOKEN_EOF) {
		if (tokens[at].type == CSS_TOKEN_OPEN_CURLY)
			return index;
		if (tokens[at].type == CSS_TOKEN_OPEN_PAREN || tokens[at].type == CSS_TOKEN_OPEN_SQUARE ||
		    tokens[at].type == CSS_TOKEN_FUNCTION) {
			at = parser_skip_block(tokens, count, at);
			continue;
		}
		at++;
	}

	/* Past the semicolon. */
	if (at < count && tokens[at].type == CSS_TOKEN_SEMICOLON)
		at++;
	return at;
}

/* Adds a rule for declarations written directly in an @scope block: they apply to the scope's root (:where(:scope)). */
static int
parser_scope_self(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	const struct css_media *media)
{
	struct token_range block;
	struct css_selector *selector;
	struct css_compound *compound;
	struct css_simple *simple;
	struct css_rule rule;
	int error;

	/* The selector: the root, adding no specificity. */
	simple = wb_arena_zalloc(&state->sheet->arena, sizeof(*simple));
	if (simple == NULL)
		return ENOMEM;
	simple->kind = CSS_SIMPLE_PSEUDO_CLASS;
	simple->pseudo = CSS_PSEUDO_SCOPE_NEST;
	compound = wb_arena_zalloc(&state->sheet->arena, sizeof(*compound));
	if (compound == NULL)
		return ENOMEM;
	compound->simples = simple;
	compound->count = 1;
	compound->combinator = CSS_COMBINATOR_NONE;
	compound->pseudo_element = CSS_PSEUDO_ELEMENT_NONE;
	selector = wb_arena_zalloc(&state->sheet->arena, sizeof(*selector));
	if (selector == NULL)
		return ENOMEM;
	selector->compounds = compound;
	selector->count = 1;

	/* The declarations. */
	memset(&rule, 0, sizeof(rule));
	rule.media = media;
	rule.scope = state->scope;
	rule.layer = state->layer;
	rule.selectors = selector;
	rule.selector_count = 1;
	block.tokens = tokens;
	block.count = count;
	error = parser_declarations(state->heap, &state->sheet->arena, block, &rule.declarations, &rule.declaration_count);
	if (error != 0)
		return error;

	/* Appends the rule. */
	error = wb_vector_push(&state->rules, &rule);
	if (error != 0)
		return ENOMEM;

	/* Succeeded: the declarations apply to the root. */
	return 0;
}

/* Moves the imports' URLs and media lists into the sheet's arena. */
static int
parser_keep_imports(
	struct parser_state *state)
{
	struct css_sheet *sheet;
	size_t count;

	/* A sheet without imports keeps nothing. */
	sheet = state->sheet;
	count = state->imports.count;
	if (count == 0)
		return 0;

	/* The URLs. */
	sheet->imports = wb_arena_alloc(&sheet->arena, count * sizeof(struct vm_string *));
	if (sheet->imports == NULL)
		return ENOMEM;
	memcpy(sheet->imports, state->imports.items, count * sizeof(struct vm_string *));

	/* Their media lists. */
	sheet->import_media = wb_arena_alloc(&sheet->arena, count * sizeof(struct css_media *));
	if (sheet->import_media == NULL)
		return ENOMEM;
	memcpy(sheet->import_media, state->import_media.items, count * sizeof(struct css_media *));

	/* Succeeded: the sheet has its imports. */
	sheet->import_count = count;
	return 0;
}

/*
 * Tells whether an @supports condition holds: not, and and or over
 * groups in parentheses, each a declaration this pass reads, a nested
 * condition, or selector() (which holds).
 */
static int
parser_supports(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	int *status)
{
	size_t index;
	size_t end;
	int holds;
	int group;
	int is_not;
	int is_and;
	int is_or;
	int joined_by_or;

	/* not in front turns the rest round. */
	index = 0;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= count)
		return 0;
	if (tokens[index].type == CSS_TOKEN_IDENT) {
		is_not = css_ident_equal(&tokens[index], "not");
		if (!is_not)
			return 0;
		holds = parser_supports(state, tokens + index + 1U, count - index - 1U, status);
		if (*status != 0)
			return 0;
		return !holds;
	}

	/* Groups joined by and or by or. */
	holds = 1;
	joined_by_or = 0;
	for (;;) {
		/* One group: a parenthesis or a function to its end. */
		if (index >= count)
			return 0;
		if (tokens[index].type != CSS_TOKEN_OPEN_PAREN && tokens[index].type != CSS_TOKEN_FUNCTION)
			return 0;
		end = parser_skip_block(tokens, count, index);
		group = parser_supports_group(state, tokens + index, end - index, status);
		if (*status != 0)
			return 0;
		if (joined_by_or) {
			if (group)
				holds = 1;
		} else if (!group) {
			holds = 0;
		}

		/* The word before the next group, or the end. */
		index = end;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (index >= count)
			break;
		if (tokens[index].type != CSS_TOKEN_IDENT)
			return 0;
		is_and = css_ident_equal(&tokens[index], "and");
		is_or = css_ident_equal(&tokens[index], "or");
		if (!is_and && !is_or)
			return 0;

		/* An or after the first group makes the answer the first that holds. */
		if (is_or)
			joined_by_or = 1;

		/* Past the word to the next group. */
		index++;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
	}

	/* Reports the answer. */
	return holds;
}

/*
 * Tells whether one group of an @supports condition holds: its tokens
 * from the opening parenthesis (or function) to the closing one.
 */
static int
parser_supports_group(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	int *status)
{
	struct css_declaration expanded[PARSER_EXPANSION_MAX];
	struct css_parse parse;
	struct token_range value;
	const struct css_token *inner;
	size_t inner_count;
	size_t index;
	size_t made;
	int is_selector;
	int custom;
	int property;
	int holds;
	int error;

	/* selector() holds: the selectors this pass reads are the ones it cannot fail on. */
	if (tokens[0].type == CSS_TOKEN_FUNCTION) {
		is_selector = css_ident_equal(&tokens[0], "selector");
		return is_selector;
	}

	/* The tokens inside the parentheses. */
	inner = tokens + 1;
	inner_count = count - 1U;
	if (inner_count > 0 && tokens[count - 1U].type == CSS_TOKEN_CLOSE_PAREN)
		inner_count--;
	index = 0;
	while (index < inner_count && inner[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= inner_count)
		return 0;

	/* A nested condition. */
	if (inner[index].type != CSS_TOKEN_IDENT) {
		holds = parser_supports(state, inner, inner_count, status);
		return holds;
	}

	/*
	 * A declaration: a property this pass knows and a value it reads.
	 * Custom properties are supported too; sites use (--css: variables)
	 * as the feature query for CSS variables.
	 */
	custom = inner[index].length > 2U &&
	    inner[index].text[0] == '-' && inner[index].text[1] == '-';
	property = css_property_lookup(&inner[index]);
	if (property < 0 && !custom)
		return 0;
	index++;
	while (index < inner_count && inner[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= inner_count || inner[index].type != CSS_TOKEN_COLON)
		return 0;
	value.tokens = inner + index + 1U;
	value.count = inner_count - index - 1U;
	value = parser_trim(value);
	if (custom)
		return value.count > 0;

	/* Parses the value; a calculation it makes stays in the sheet's arena. */
	parse.heap = state->heap;
	parse.arena = &state->sheet->arena;
	made = 0;
	error = css_parse_property(&parse, property, value.tokens, value.count, expanded, &made);
	if (error == ENOMEM) {
		*status = error;
		return 0;
	}

	/* An unsupported value simply makes the condition false. */
	if (error != 0 || made == 0)
		return 0;

	/* The declaration is supported. */
	return 1;
}

/*
 * Reads an @import rule (the tokens after its at-keyword): a string or
 * url() first, then an optional media query list.  An @import without a
 * URL is ignored.
 */
static int
parser_import(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count)
{
	struct vm_heap *heap;
	struct vm_string *url;
	struct css_media *media;
	size_t index;
	int is_url;
	int error;

	/* The atoms are the sheet's heap's. */
	heap = state->heap;

	/* Skips the whitespace before the URL. */
	index = 0;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= count)
		return EINVAL;

	/* A string, a url token, or url("...") with a string inside. */
	url = NULL;
	if (tokens[index].type == CSS_TOKEN_STRING || tokens[index].type == CSS_TOKEN_URL) {
		url = vm_atom_from_units(heap, tokens[index].text, tokens[index].length);
		if (url == NULL)
			return ENOMEM;
	} else if (tokens[index].type == CSS_TOKEN_FUNCTION) {
		is_url = css_ident_equal(&tokens[index], "url");
		index++;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (is_url && index < count && tokens[index].type == CSS_TOKEN_STRING) {
			url = vm_atom_from_units(heap, tokens[index].text, tokens[index].length);
			if (url == NULL)
				return ENOMEM;
		}

		/* Past the function's closing parenthesis. */
		while (index < count && tokens[index].type != CSS_TOKEN_CLOSE_PAREN)
			index++;
	}

	/* Anything else names no sheet. */
	if (url == NULL)
		return EINVAL;

	/* The media list after the URL (up to the semicolon), when there is one. */
	index++;
	while (count > index && (tokens[count - 1U].type == CSS_TOKEN_SEMICOLON || tokens[count - 1U].type == CSS_TOKEN_WHITESPACE))
		count--;
	media = NULL;
	if (index < count) {
		error = css_media_parse(&state->sheet->arena, tokens + index, count - index, NULL, &media);
		if (error != 0)
			return error;
	}

	/* Keeps the URL and its media. */
	error = wb_vector_push(&state->imports, &url);
	if (error != 0)
		return ENOMEM;
	error = wb_vector_push(&state->import_media, &media);
	if (error != 0)
		return ENOMEM;

	/* Succeeded: the import is recorded. */
	return 0;
}

/* Parses one qualified rule and appends it to the list; returns EINVAL for a rule that is dropped. */
static int
parser_add_rule(
	struct parser_state *state,
	struct token_range prelude,
	struct token_range block,
	const struct css_media *media)
{
	struct css_rule rule;
	int error;

	/* Reads the selectors (relative to the scope's root inside @scope); a selector list that does not parse drops the rule. */
	memset(&rule, 0, sizeof(rule));
	rule.media = media;
	rule.scope = state->scope;
	rule.layer = state->layer;
	if (state->scope != NULL) {
		error = parser_scoped_selectors(state, prelude, &rule.selectors, &rule.selector_count);
	} else {
		error = parser_selectors(state->heap, &state->sheet->arena, prelude, &rule.selectors, &rule.selector_count);
	}
	if (error != 0)
		return error;

	/* Reads the declarations. */
	error = parser_declarations(state->heap, &state->sheet->arena, block, &rule.declarations, &rule.declaration_count);
	if (error != 0)
		return error;

	/* Appends the rule. */
	error = wb_vector_push(&state->rules, &rule);
	if (error != 0)
		return ENOMEM;

	/* Succeeded: the rule is the sheet's. */
	return 0;
}

/* Parses a selector list (comma separated). */
static int
parser_selectors(
	struct vm_heap *heap,
	struct wb_arena *arena,
	struct token_range prelude,
	struct css_selector **selectors,
	size_t *count)
{
	struct token_range one;
	struct css_selector *list;
	size_t start;
	size_t index;
	size_t commas;
	size_t made;
	int depth;
	int error;

	/* Counts the selectors to size the list (commas inside a function's arguments do not part them). */
	commas = 0;
	depth = 0;
	for (index = 0; index < prelude.count; index++) {
		if (prelude.tokens[index].type == CSS_TOKEN_FUNCTION || prelude.tokens[index].type == CSS_TOKEN_OPEN_PAREN)
			depth++;
		if (prelude.tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
			depth--;
		if (prelude.tokens[index].type == CSS_TOKEN_COMMA && depth == 0)
			commas++;
	}

	/* Makes room for them. */
	list = wb_arena_zalloc(arena, (commas + 1U) * sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Parses each selector between the top-level commas. */
	made = 0;
	start = 0;
	depth = 0;
	for (index = 0; index <= prelude.count; index++) {
		if (index < prelude.count) {
			if (prelude.tokens[index].type == CSS_TOKEN_FUNCTION || prelude.tokens[index].type == CSS_TOKEN_OPEN_PAREN)
				depth++;
			if (prelude.tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
				depth--;
			if (prelude.tokens[index].type != CSS_TOKEN_COMMA || depth != 0)
				continue;
		}

		/* The selector between the last comma and this one. */
		one.tokens = prelude.tokens + start;
		one.count = index - start;
		one = parser_trim(one);
		error = parser_selector(heap, arena, one, &list[made]);
		if (error != 0)
			return error;
		made++;
		start = index + 1U;
	}

	/* Succeeded: the list holds every selector. */
	*selectors = list;
	*count = made;
	return 0;
}

/* Parses one complex selector: compounds joined by combinators. */
static int
parser_selector(
	struct vm_heap *heap,
	struct wb_arena *arena,
	struct token_range tokens,
	struct css_selector *selector)
{
	struct wb_vector compounds;
	struct css_compound compound;
	size_t index;
	int combinator;
	int separated;
	int error;

	/* An empty selector is invalid. */
	if (tokens.count == 0)
		return EINVAL;

	/* Reads compounds and the combinators between them. */
	wb_vector_init(&compounds, sizeof(struct css_compound));
	selector->specificity = 0;
	index = 0;
	combinator = CSS_COMBINATOR_NONE;
	while (index < tokens.count) {
		/* One compound. */
		error = parser_compound(heap, arena, tokens.tokens, tokens.count, &index, &compound, &selector->specificity);
		if (error != 0) {
			wb_vector_release(&compounds);
			return error;
		}

		/* The combinator joins it to the compound before. */
		compound.combinator = combinator;
		error = wb_vector_push(&compounds, &compound);
		if (error != 0) {
			wb_vector_release(&compounds);
			return ENOMEM;
		}

		/* The combinator to the next compound: whitespace, or >, + or ~ with optional whitespace. */
		if (index >= tokens.count)
			break;
		combinator = CSS_COMBINATOR_DESCENDANT;
		separated = 0;
		while (index < tokens.count && tokens.tokens[index].type == CSS_TOKEN_WHITESPACE) {
			separated = 1;
			index++;
		}

		/* Recognizes an explicit combinator even without surrounding spaces. */
		if (index < tokens.count && tokens.tokens[index].type == CSS_TOKEN_DELIM) {
			if (tokens.tokens[index].delim == '>') {
				combinator = CSS_COMBINATOR_CHILD;
			} else if (tokens.tokens[index].delim == '+') {
				combinator = CSS_COMBINATOR_NEXT;
			} else if (tokens.tokens[index].delim == '~') {
				combinator = CSS_COMBINATOR_SUBSEQUENT;
			}

			/* An explicit combinator may be followed by whitespace. */
			if (combinator != CSS_COMBINATOR_DESCENDANT) {
				index++;
				while (index < tokens.count && tokens.tokens[index].type == CSS_TOKEN_WHITESPACE)
					index++;
			}
		}

		/* Adjacent type or universal tokens cannot begin another compound. */
		if (combinator == CSS_COMBINATOR_DESCENDANT && !separated) {
			wb_vector_release(&compounds);
			return EINVAL;
		}

		/* A combinator needs a compound after it. */
		if (index >= tokens.count) {
			wb_vector_release(&compounds);
			return EINVAL;
		}
	}

	/* Moves the compounds into the arena. */
	selector->compounds = wb_arena_alloc(arena, compounds.count * sizeof(struct css_compound));
	if (selector->compounds == NULL) {
		wb_vector_release(&compounds);
		return ENOMEM;
	}

	/* Copies them. */
	memcpy(selector->compounds, compounds.items, compounds.count * sizeof(struct css_compound));
	selector->count = compounds.count;
	wb_vector_release(&compounds);

	/* Succeeded: the selector is complete. */
	return 0;
}

/* Parses one compound selector starting at *index, adding to the specificity. */
static int
parser_compound(
	struct vm_heap *heap,
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	size_t *index,
	struct css_compound *compound,
	uint32_t *specificity)
{
	struct css_simple simples[32];
	struct css_simple *simple;
	const struct css_token *token;
	size_t made;
	size_t end;
	int error;

	/* Reads simple selectors until whitespace, a combinator or the end. */
	made = 0;
	compound->pseudo_element = CSS_PSEUDO_ELEMENT_NONE;
	compound->pseudo_name = NULL;
	compound->pseudo_class_count = 0;
	while (*index < count && made < 32U) {
		token = &tokens[*index];
		simple = &simples[made];
		memset(simple, 0, sizeof(*simple));

		/* A type selector (lower case: HTML element names are), or the universal selector. */
		if (token->type == CSS_TOKEN_IDENT && made == 0) {
			simple->kind = CSS_SIMPLE_TYPE;
			simple->name = parser_atom(heap, token, 1);
			if (simple->name == NULL)
				return ENOMEM;
			*specificity += 1U;
		} else if (token->type == CSS_TOKEN_DELIM && token->delim == '*' && made == 0) {
			simple->kind = CSS_SIMPLE_UNIVERSAL;
		} else if (token->type == CSS_TOKEN_DELIM && token->delim == '&') {
			/* The nesting selector: the scope's root (or the root element), adding no specificity. */
			simple->kind = CSS_SIMPLE_PSEUDO_CLASS;
			simple->pseudo = CSS_PSEUDO_SCOPE_NEST;
		} else if (token->type == CSS_TOKEN_HASH && token->hash_is_id) {
			/* An id selector. */
			simple->kind = CSS_SIMPLE_ID;
			simple->name = parser_atom(heap, token, 0);
			if (simple->name == NULL)
				return ENOMEM;
			*specificity += 1U << 16;
		} else if (token->type == CSS_TOKEN_DELIM && token->delim == '.' &&
		    *index + 1U < count && tokens[*index + 1U].type == CSS_TOKEN_IDENT) {
			/* A class selector. */
			(*index)++;
			simple->kind = CSS_SIMPLE_CLASS;
			simple->name = parser_atom(heap, &tokens[*index], 0);
			if (simple->name == NULL)
				return ENOMEM;
			*specificity += 1U << 8;
		} else if (token->type == CSS_TOKEN_OPEN_SQUARE) {
			/* An attribute selector up to its closing bracket. */
			end = parser_skip_block(tokens, count, *index);
			if (end <= *index + 1U || tokens[end - 1U].type != CSS_TOKEN_CLOSE_SQUARE)
				return EINVAL;
			error = parser_attribute(heap, tokens + *index + 1U, end - *index - 2U, simple);
			if (error != 0)
				return error;
			*index = end - 1U;
			*specificity += 1U << 8;
		} else if (token->type == CSS_TOKEN_COLON) {
			/* A pseudo-class, or with a second colon a pseudo-element. */
			(*index)++;
			if (*index >= count)
				return EINVAL;
			if (tokens[*index].type == CSS_TOKEN_COLON) {
				(*index)++;
				if (*index >= count)
					return EINVAL;
				parser_pseudo_element(&tokens[*index], simple);
				compound->pseudo_element = simple->pseudo;
				*specificity += 1U;

				/* A view transition one reads its name; a functional pseudo-element's other arguments are skipped. */
				if (simple->pseudo >= CSS_PSEUDO_ELEMENT_VT_ROOT && simple->pseudo <= CSS_PSEUDO_ELEMENT_VT_NEW) {
					error = parser_view_transition_argument(heap, tokens, count, index, compound);
					if (error != 0)
						return error;

					/* A name counts as a type selector, and the classes together as one class selector. */
					if (compound->pseudo_name != NULL)
						*specificity += 1U;
					if (compound->pseudo_class_count != 0)
						*specificity += 1U << 8;
				} else if (tokens[*index].type == CSS_TOKEN_FUNCTION) {
					*index = parser_skip_block(tokens, count, *index) - 1U;
				}
			} else {
				error = parser_pseudo(heap, arena, tokens, count, index, simple, specificity);
				if (error != 0)
					return error;
				if (simple->kind == CSS_SIMPLE_PSEUDO_ELEMENT)
					compound->pseudo_element = simple->pseudo;
			}
		} else {
			break;
		}

		/* The simple selector is complete. */
		made++;
		(*index)++;
	}

	/* A compound needs at least one simple selector. */
	if (made == 0)
		return EINVAL;

	/* Moves the simple selectors into the arena. */
	compound->simples = wb_arena_alloc(arena, made * sizeof(struct css_simple));
	if (compound->simples == NULL)
		return ENOMEM;
	memcpy(compound->simples, simples, made * sizeof(struct css_simple));
	compound->count = made;
	compound->combinator = CSS_COMBINATOR_NONE;

	/* Succeeded: the compound is complete. */
	return 0;
}

/* Parses the inside of an attribute selector: a name, and optionally a comparison and a value. */
static int
parser_attribute(
	struct vm_heap *heap,
	const struct css_token *tokens,
	size_t count,
	struct css_simple *simple)
{
	struct token_range range;
	const struct css_token *operator_token;
	size_t index;
	int insensitive;
	int sensitive;

	/* The name comes first. */
	range.tokens = tokens;
	range.count = count;
	range = parser_trim(range);
	if (range.count == 0 || range.tokens[0].type != CSS_TOKEN_IDENT)
		return EINVAL;
	simple->kind = CSS_SIMPLE_ATTRIBUTE;
	simple->match = CSS_MATCH_EXISTS;
	simple->name = parser_atom(heap, &range.tokens[0], 1);
	if (simple->name == NULL)
		return ENOMEM;
	index = 1;
	while (index < range.count && range.tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= range.count)
		return 0;

	/* The comparison: = alone, or one of ~ | ^ $ * before =. */
	operator_token = &range.tokens[index];
	if (operator_token->type != CSS_TOKEN_DELIM)
		return EINVAL;
	switch (operator_token->delim) {
	case '=':
		simple->match = CSS_MATCH_EQUAL;
		break;
	case '~':
		simple->match = CSS_MATCH_INCLUDES;
		break;
	case '|':
		simple->match = CSS_MATCH_DASH;
		break;
	case '^':
		simple->match = CSS_MATCH_PREFIX;
		break;
	case '$':
		simple->match = CSS_MATCH_SUFFIX;
		break;
	case '*':
		simple->match = CSS_MATCH_SUBSTRING;
		break;
	default:
		return EINVAL;
	}

	/* Skips whitespace after the value. */
	index++;
	if (simple->match != CSS_MATCH_EQUAL) {
		if (index >= range.count || range.tokens[index].type != CSS_TOKEN_DELIM || range.tokens[index].delim != '=')
			return EINVAL;
		index++;
	}

	/* The value: an identifier or a string, then optionally the i or s flag. */
	while (index < range.count && range.tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= range.count)
		return EINVAL;
	if (range.tokens[index].type != CSS_TOKEN_IDENT && range.tokens[index].type != CSS_TOKEN_STRING)
		return EINVAL;
	simple->value = parser_atom(heap, &range.tokens[index], 0);
	if (simple->value == NULL)
		return ENOMEM;
	index++;
	while (index < range.count && range.tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index < range.count) {
		if (range.tokens[index].type != CSS_TOKEN_IDENT)
			return EINVAL;
		insensitive = css_ident_equal(&range.tokens[index], "i");
		sensitive = css_ident_equal(&range.tokens[index], "s");
		if (!insensitive && !sensitive)
			return EINVAL;
		simple->case_insensitive = insensitive;
		simple->case_sensitive = sensitive;
		index++;
	}

	/* A flag must be the selector's final token. */
	if (index != range.count)
		return EINVAL;

	/* Succeeded: the attribute selector is complete. */
	return 0;
}

/*
 * Reads a pseudo-class at *index (a name, or a function to its closing
 * parenthesis, where *index is left) into a simple selector and adds its
 * specificity; one this pass does not know never matches.  The legacy
 * single-colon :before and :after are pseudo-elements.
 */
static int
parser_pseudo(
	struct vm_heap *heap,
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	size_t *index,
	struct css_simple *simple,
	uint32_t *specificity)
{
	const struct css_token *token;
	struct token_range arguments;
	size_t end;
	size_t position;
	int same;
	int error;

	/* Only identifiers and functions name pseudo-classes. */
	token = &tokens[*index];
	if (token->type != CSS_TOKEN_IDENT && token->type != CSS_TOKEN_FUNCTION)
		return EINVAL;
	simple->kind = CSS_SIMPLE_PSEUDO_CLASS;
	simple->pseudo = CSS_PSEUDO_NEVER;

	/* A name: the legacy pseudo-elements, then the pseudo-classes this pass evaluates. */
	if (token->type == CSS_TOKEN_IDENT) {
		same = css_ident_equal(token, "before");
		if (!same)
			same = css_ident_equal(token, "after");
		if (!same)
			same = css_ident_equal(token, "first-line");
		if (!same)
			same = css_ident_equal(token, "first-letter");
		if (same) {
			parser_pseudo_element(token, simple);
			*specificity += 1U;
			return 0;
		}

		/* Looks the name up. */
		for (position = 0; parser_pseudo_names[position].name != NULL; position++) {
			same = css_ident_equal(token, parser_pseudo_names[position].name);
			if (same) {
				simple->pseudo = parser_pseudo_names[position].pseudo;
				break;
			}
		}

		/* A pseudo-class counts as a class. */
		*specificity += 1U << 8;
		return 0;
	}

	/* A function: its arguments run to its closing parenthesis, where the index is left. */
	end = parser_skip_block(tokens, count, *index);
	if (end <= *index + 1U || tokens[end - 1U].type != CSS_TOKEN_CLOSE_PAREN)
		return EINVAL;
	arguments.tokens = tokens + *index + 1U;
	arguments.count = end - *index - 1U;
	if (arguments.count > 0 && tokens[end - 1U].type == CSS_TOKEN_CLOSE_PAREN)
		arguments.count--;
	arguments = parser_trim(arguments);
	*index = end - 1U;

	/* Looks the function up. */
	for (position = 0; parser_pseudo_functions[position].name != NULL; position++) {
		same = css_ident_equal(token, parser_pseudo_functions[position].name);
		if (same) {
			simple->pseudo = parser_pseudo_functions[position].pseudo;
			break;
		}
	}

	/* The kind of function picks how its arguments are read. */
	switch (simple->pseudo) {
	case CSS_PSEUDO_NOT:
	case CSS_PSEUDO_IS:
		/* Selectors; :where() adds nothing to the specificity. */
		same = css_ident_equal(token, "where");
		if (same) {
			error = parser_pseudo_arguments(heap, arena, arguments, 0, simple, NULL);
		} else {
			error = parser_pseudo_arguments(heap, arena, arguments, 0, simple, specificity);
		}

		/* :not() with an argument it cannot read drops the rule; :is() and :where() forgive it. */
		if (error == ENOMEM)
			return error;
		if (error != 0 && simple->pseudo == CSS_PSEUDO_NOT)
			return error;
		if (error != 0)
			simple->pseudo = CSS_PSEUDO_NEVER;
		return 0;
	case CSS_PSEUDO_HAS:
		/* Relative selectors. */
		error = parser_pseudo_arguments(heap, arena, arguments, 1, simple, specificity);
		if (error == ENOMEM)
			return error;
		if (error != 0)
			simple->pseudo = CSS_PSEUDO_NEVER;
		return 0;
	case CSS_PSEUDO_LANG:
		/* A functional language selector must actually close its argument list. */
		if (tokens[end - 1U].type != CSS_TOKEN_CLOSE_PAREN)
			return EINVAL;

		/* A language selector takes one nonempty CSS identifier. */
		if (arguments.count != 1U)
			return EINVAL;

		/* Rejects malformed arguments rather than treating them as unknown language. */
		if (arguments.tokens[0].type != CSS_TOKEN_IDENT)
			return EINVAL;

		/* Keeps the language parameter as an ordinary traced atom. */
		simple->value = parser_atom(heap, &arguments.tokens[0], 0);
		if (simple->value == NULL)
			return ENOMEM;

		/* Language contributes the specificity of a pseudo-class. */
		*specificity += 1U << 8;
		return 0;
	case CSS_PSEUDO_ACTIVE_VT_TYPE:
		/* Identifiers separated by commas, kept as one space-separated atom. */
		error = parser_type_list(heap, arguments, simple);
		if (error != 0)
			return error;
		*specificity += 1U << 8;
		return 0;
	case CSS_PSEUDO_NTH_CHILD:
	case CSS_PSEUDO_NTH_LAST_CHILD:
	case CSS_PSEUDO_NTH_OF_TYPE:
	case CSS_PSEUDO_NTH_LAST_OF_TYPE:
		/* an+b, counted as a class. */
		error = parser_nth(arguments, &simple->nth_a, &simple->nth_b);
		if (error != 0)
			return error;
		*specificity += 1U << 8;
		return 0;
	default:
		/* Any other function never matches, and counts as a class. */
		*specificity += 1U << 8;
		return 0;
	}
}

/* Reads the identifiers of :active-view-transition-type() into one space-separated atom. */
static int
parser_type_list(
	struct vm_heap *heap,
	struct token_range arguments,
	struct css_simple *simple)
{
	uint16_t joined[256];
	size_t used;
	size_t index;
	size_t at;
	int expect_ident;

	used = 0;
	expect_ident = 1;
	for (index = 0; index < arguments.count; index++) {
		if (arguments.tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (expect_ident) {
			if (arguments.tokens[index].type != CSS_TOKEN_IDENT)
				return EINVAL;
			if (used + arguments.tokens[index].length + 1U > 256U)
				return EINVAL;
			if (used != 0)
				joined[used++] = ' ';
			for (at = 0; at < arguments.tokens[index].length; at++)
				joined[used++] = arguments.tokens[index].text[at];
			expect_ident = 0;
		} else {
			if (arguments.tokens[index].type != CSS_TOKEN_COMMA)
				return EINVAL;
			expect_ident = 1;
		}
	}
	if (used == 0 || expect_ident)
		return EINVAL;
	simple->value = vm_atom_from_units(heap, joined, used);
	return simple->value == NULL ? ENOMEM : 0;
}

/* Reads a pseudo-element's name: ::before and ::after make boxes, any other never matches. */
static void
parser_pseudo_element(
	const struct css_token *token,
	struct css_simple *simple)
{
	int same;

	/* ::before. */
	simple->kind = CSS_SIMPLE_PSEUDO_ELEMENT;
	simple->pseudo = CSS_PSEUDO_ELEMENT_BEFORE;
	same = css_ident_equal(token, "before");
	if (same)
		return;

	/* ::after. */
	simple->pseudo = CSS_PSEUDO_ELEMENT_AFTER;
	same = css_ident_equal(token, "after");
	if (same)
		return;

	/* The view transition tree. */
	simple->pseudo = CSS_PSEUDO_ELEMENT_VT_ROOT;
	if (css_ident_equal(token, "view-transition"))
		return;
	simple->pseudo = CSS_PSEUDO_ELEMENT_VT_GROUP;
	if (css_ident_equal(token, "view-transition-group"))
		return;
	simple->pseudo = CSS_PSEUDO_ELEMENT_VT_PAIR;
	if (css_ident_equal(token, "view-transition-image-pair"))
		return;
	simple->pseudo = CSS_PSEUDO_ELEMENT_VT_OLD;
	if (css_ident_equal(token, "view-transition-old"))
		return;
	simple->pseudo = CSS_PSEUDO_ELEMENT_VT_NEW;
	if (css_ident_equal(token, "view-transition-new"))
		return;

	/* The box around a details element's content. */
	simple->pseudo = CSS_PSEUDO_ELEMENT_DETAILS_CONTENT;
	if (css_ident_equal(token, "details-content"))
		return;

	/* Any other (::placeholder, ::-webkit-scrollbar, ::first-line ...) is not drawn. */
	simple->pseudo = CSS_PSEUDO_ELEMENT_OTHER;
}

/*
 * Reads the argument of a view transition pseudo-element, (name), (*), (name.class) or (.class): the name
 * (NULL for any) and up to two classes.  The index is the function token and ends on its closing parenthesis.
 * A malformed argument makes the selector invalid.
 */
static int
parser_view_transition_argument(
	struct vm_heap *heap,
	const struct css_token *tokens,
	size_t count,
	size_t *index,
	struct css_compound *compound)
{
	size_t end;
	size_t position;

	compound->pseudo_name = NULL;
	compound->pseudo_class_count = 0;
	if (tokens[*index].type != CSS_TOKEN_FUNCTION) {
		/* ::view-transition takes none, and the others need one. */
		return compound->pseudo_element == CSS_PSEUDO_ELEMENT_VT_ROOT ? 0 : EINVAL;
	}
	if (compound->pseudo_element == CSS_PSEUDO_ELEMENT_VT_ROOT)
		return EINVAL;
	end = parser_skip_block(tokens, count, *index);
	if (end <= *index + 1U || tokens[end - 1U].type != CSS_TOKEN_CLOSE_PAREN)
		return EINVAL;
	position = *index + 1U;
	while (position < end - 1U && tokens[position].type == CSS_TOKEN_WHITESPACE)
		position++;
	if (position < end - 1U && tokens[position].type == CSS_TOKEN_IDENT) {
		compound->pseudo_name = parser_atom(heap, &tokens[position], 0);
		if (compound->pseudo_name == NULL)
			return ENOMEM;
		position++;
	} else if (position < end - 1U && tokens[position].type == CSS_TOKEN_DELIM && tokens[position].delim == '*') {
		position++;
	}
	while (position + 1U < end - 1U && tokens[position].type == CSS_TOKEN_DELIM && tokens[position].delim == '.' &&
	    tokens[position + 1U].type == CSS_TOKEN_IDENT) {
		if (compound->pseudo_class_count >= 2)
			return EINVAL;
		compound->pseudo_classes[compound->pseudo_class_count] = parser_atom(heap, &tokens[position + 1U], 0);
		if (compound->pseudo_classes[compound->pseudo_class_count] == NULL)
			return ENOMEM;
		compound->pseudo_class_count++;
		position += 2U;
	}
	while (position < end - 1U && tokens[position].type == CSS_TOKEN_WHITESPACE)
		position++;
	if (position != end - 1U)
		return EINVAL;
	*index = end - 1U;
	return 0;
}

/*
 * Reads the selector list of :not(), :is(), :where() or (relative) :has()
 * into the simple selector, and adds the most specific one's specificity
 * (none when specificity is NULL).  A relative selector may start with a
 * combinator, which relates its first compound to the element the
 * pseudo-class is on; without one it is the descendant combinator.
 */
static int
parser_pseudo_arguments(
	struct vm_heap *heap,
	struct wb_arena *arena,
	struct token_range arguments,
	int relative,
	struct css_simple *simple,
	uint32_t *specificity)
{
	struct css_selector *list;
	struct token_range one;
	uint32_t best;
	size_t made;
	size_t start;
	size_t index;
	int depth;
	int combinator;
	int error;

	/* An empty list is not a list. */
	if (arguments.count == 0)
		return EINVAL;

	/* The plain selectors of :not(), :is() and :where(). */
	if (!relative) {
		error = parser_selectors(heap, arena, arguments, &list, &made);
		if (error != 0)
			return error;
	} else {
		/* Relative selectors: counted between the top-level commas. */
		made = 1;
		depth = 0;
		for (index = 0; index < arguments.count; index++) {
			if (arguments.tokens[index].type == CSS_TOKEN_FUNCTION || arguments.tokens[index].type == CSS_TOKEN_OPEN_PAREN)
				depth++;
			if (arguments.tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
				depth--;
			if (arguments.tokens[index].type == CSS_TOKEN_COMMA && depth == 0)
				made++;
		}

		/* Makes room for them. */
		list = wb_arena_zalloc(arena, made * sizeof(*list));
		if (list == NULL)
			return ENOMEM;

		/* Each one: an optional leading combinator, then a selector. */
		made = 0;
		start = 0;
		depth = 0;
		for (index = 0; index <= arguments.count; index++) {
			if (index < arguments.count) {
				if (arguments.tokens[index].type == CSS_TOKEN_FUNCTION || arguments.tokens[index].type == CSS_TOKEN_OPEN_PAREN)
					depth++;
				if (arguments.tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
					depth--;
				if (arguments.tokens[index].type != CSS_TOKEN_COMMA || depth != 0)
					continue;
			}

			/* The selector's tokens, and its leading combinator. */
			one.tokens = arguments.tokens + start;
			one.count = index - start;
			one = parser_trim(one);
			start = index + 1U;
			combinator = CSS_COMBINATOR_DESCENDANT;
			if (one.count > 0 && one.tokens[0].type == CSS_TOKEN_DELIM) {
				if (one.tokens[0].delim == '>')
					combinator = CSS_COMBINATOR_CHILD;
				if (one.tokens[0].delim == '+')
					combinator = CSS_COMBINATOR_NEXT;
				if (one.tokens[0].delim == '~')
					combinator = CSS_COMBINATOR_SUBSEQUENT;
				if (combinator != CSS_COMBINATOR_DESCENDANT) {
					one.tokens++;
					one.count--;
					one = parser_trim(one);
				}
			}

			/* The selector, whose first compound carries the combinator to the anchor. */
			error = parser_selector(heap, arena, one, &list[made]);
			if (error != 0)
				return error;
			list[made].compounds[0].combinator = combinator;
			made++;
		}
	}

	/* The most specific argument. */
	best = 0;
	for (index = 0; index < made; index++) {
		if (list[index].specificity > best)
			best = list[index].specificity;
	}

	/* It counts toward the compound's specificity, unless the caller counts nothing. */
	if (specificity != NULL)
		*specificity += best;

	/* Succeeded: the arguments are the simple selector's. */
	simple->arguments = list;
	simple->argument_count = made;
	return 0;
}

/*
 * Reads the an+b of an :nth-*() pseudo-class: odd, even, an integer, or
 * the forms the tokenizer splits in several ways (2n+1, -n+3, n, 2n-1,
 * 3n + 2).  A trailing "of S" is not read in this pass.
 */
static int
parser_nth(
	struct token_range arguments,
	int *a,
	int *b)
{
	const struct css_token *token;
	char text[64];
	size_t length;
	size_t index;
	size_t part;
	uint64_t value;
	uint64_t limit;
	uint64_t digit;
	int written;
	int sign;
	int number;
	int has_digits;
	int is_word;
	int differs;
	int after_sign;

	/* Spells the tokens out as ASCII, up to an "of". */
	length = 0;
	for (index = 0; index < arguments.count; index++) {
		token = &arguments.tokens[index];
		if (token->type == CSS_TOKEN_WHITESPACE)
			continue;
		if (token->type == CSS_TOKEN_IDENT) {
			is_word = css_ident_equal(token, "of");
			if (is_word)
				break;
		}

		/* A number after an n keeps its sign; after a + or - it is the operand. */
		after_sign = 0;
		if (length > 0 && (text[length - 1U] == '+' || text[length - 1U] == '-'))
			after_sign = 1;

		/* Numbers with their sign, then the text of identifiers and units. */
		written = 0;
		if (token->type == CSS_TOKEN_NUMBER || token->type == CSS_TOKEN_DIMENSION) {
			if (!token->integer || !(token->number >= INT_MIN && token->number <= INT_MAX))
				return EINVAL;
			number = (int)token->number;
			if (token->type == CSS_TOKEN_NUMBER && length > 0 && !after_sign) {
				written = snprintf(text + length, sizeof(text) - length, "%+d", number);
			} else {
				written = snprintf(text + length, sizeof(text) - length, "%d", number);
			}

			/* The number must fit. */
			if (written < 0 || (size_t)written >= sizeof(text) - length)
				return EINVAL;
			length += (size_t)written;
		}

		/* An operator's character. */
		if (token->type == CSS_TOKEN_DELIM) {
			if (length + 1U >= sizeof(text))
				return EINVAL;
			text[length] = (char)token->delim;
			length++;
		}

		/* An identifier's or a unit's letters, in lower case. */
		if (token->type == CSS_TOKEN_IDENT || token->type == CSS_TOKEN_DIMENSION) {
			for (part = 0; part < token->length; part++) {
				if (length + 1U >= sizeof(text) || token->text[part] > 0x7fU)
					return EINVAL;
				text[length] = (char)token->text[part];
				if (text[length] >= 'A' && text[length] <= 'Z')
					text[length] = (char)(text[length] + 0x20);
				length++;
			}
		}
	}

	/* The spelling ends here. */
	text[length] = '\0';

	/* odd and even. */
	differs = strcmp(text, "odd");
	if (differs == 0) {
		*a = 2;
		*b = 1;
		return 0;
	}

	/* even. */
	differs = strcmp(text, "even");
	if (differs == 0) {
		*a = 2;
		*b = 0;
		return 0;
	}

	/* a: a signed number before n (a bare sign or none is one). */
	index = 0;
	sign = 1;
	*a = 0;
	*b = 0;
	if (text[index] == '+' || text[index] == '-') {
		if (text[index] == '-')
			sign = -1;
		index++;
	}

	/* The digits of a. */
	value = 0;
	has_digits = 0;
	limit = INT_MAX;
	if (sign < 0)
		limit = (uint64_t)INT_MAX + 1U;
	while (text[index] >= '0' && text[index] <= '9') {
		digit = (uint64_t)(text[index] - '0');
		if (value > (limit - digit) / 10U)
			return EINVAL;
		value = value * 10U + digit;
		has_digits = 1;
		index++;
	}

	/* Without an n the number is b alone. */
	if (text[index] != 'n') {
		if (!has_digits || text[index] != '\0')
			return EINVAL;
		*b = (int)((int64_t)sign * (int64_t)value);
		return 0;
	}

	/* A bare n is 1n. */
	if (!has_digits)
		value = 1;
	*a = (int)((int64_t)sign * (int64_t)value);
	index++;

	/* b: an optional signed number after the n. */
	if (text[index] == '\0')
		return 0;
	if (text[index] != '+' && text[index] != '-')
		return EINVAL;
	sign = 1;
	if (text[index] == '-')
		sign = -1;
	index++;
	value = 0;
	has_digits = 0;
	limit = INT_MAX;
	if (sign < 0)
		limit = (uint64_t)INT_MAX + 1U;
	while (text[index] >= '0' && text[index] <= '9') {
		digit = (uint64_t)(text[index] - '0');
		if (value > (limit - digit) / 10U)
			return EINVAL;
		value = value * 10U + digit;
		has_digits = 1;
		index++;
	}

	/* b must be digits to the end. */
	if (!has_digits || text[index] != '\0')
		return EINVAL;
	*b = (int)((int64_t)sign * (int64_t)value);

	/* Succeeded: a and b are read. */
	return 0;
}

/* Parses a declaration block into declarations (shorthands expanded). */
static int
parser_declarations(
	struct vm_heap *heap,
	struct wb_arena *arena,
	struct token_range block,
	struct css_declaration **declarations,
	size_t *count)
{
	struct css_declaration expanded[PARSER_EXPANSION_MAX];
	struct css_parse parse;
	struct wb_vector list;
	struct token_range value;
	const struct css_token *name;
	size_t index;
	size_t start;
	size_t end;
	size_t made;
	size_t item;
	int important;
	int last_important;
	int error;

	/* Values are parsed with the heap's names and a calculation kept in the arena. */
	parse.heap = heap;
	parse.arena = arena;

	/* Reads each declaration up to its semicolon. */
	wb_vector_init(&list, sizeof(struct css_declaration));
	index = 0;
	while (index < block.count) {
		/* Skips whitespace and stray semicolons. */
		if (block.tokens[index].type == CSS_TOKEN_WHITESPACE || block.tokens[index].type == CSS_TOKEN_SEMICOLON) {
			index++;
			continue;
		}

		/* The declaration runs to the next semicolon outside blocks. */
		start = index;
		while (index < block.count && block.tokens[index].type != CSS_TOKEN_SEMICOLON) {
			if (block.tokens[index].type == CSS_TOKEN_OPEN_PAREN || block.tokens[index].type == CSS_TOKEN_OPEN_SQUARE ||
			    block.tokens[index].type == CSS_TOKEN_FUNCTION || block.tokens[index].type == CSS_TOKEN_OPEN_CURLY) {
				index = parser_skip_block(block.tokens, block.count, index);
				continue;
			}

			/* Anything else belongs to the declaration. */
			index++;
		}

		/* The declaration ends here. */
		end = index;

		/* A name, a colon and a value. */
		if (block.tokens[start].type != CSS_TOKEN_IDENT)
			continue;
		name = &block.tokens[start];
		start++;
		while (start < end && block.tokens[start].type == CSS_TOKEN_WHITESPACE)
			start++;
		if (start >= end || block.tokens[start].type != CSS_TOKEN_COLON)
			continue;
		value.tokens = block.tokens + start + 1U;
		value.count = end - start - 1U;
		value = parser_trim(value);

		/* !important at the end. */
		important = 0;
		last_important = 0;
		if (value.count >= 2U && value.tokens[value.count - 1U].type == CSS_TOKEN_IDENT)
			last_important = css_ident_equal(&value.tokens[value.count - 1U], "important");
		if (last_important) {
			item = value.count - 2U;
			while (item > 0 && value.tokens[item].type == CSS_TOKEN_WHITESPACE)
				item--;
			if (value.tokens[item].type == CSS_TOKEN_DELIM && value.tokens[item].delim == '!') {
				important = 1;
				value.count = item;
				value = parser_trim(value);
			}
		}

		/* A custom property keeps its tokens; a value with var() waits for the element's custom properties. */
		made = 0;
		error = parser_special(heap, name, value, &expanded[0], &made);
		if (error == ENOMEM) {
			wb_vector_release(&list);
			return error;
		}

		/* Otherwise parses the value into one or more declarations (an invalid one is dropped). */
		if (made == 0 && error == 0)
			error = css_parse_value(&parse, value.tokens, value.count, name, expanded, &made, PARSER_EXPANSION_MAX);
		if (error == ENOMEM) {
			wb_vector_release(&list);
			return error;
		}

		/* Keeps each longhand with the importance. */
		for (item = 0; item < made; item++) {
			expanded[item].important = important;
			error = wb_vector_push(&list, &expanded[item]);
			if (error != 0) {
				wb_vector_release(&list);
				return ENOMEM;
			}
		}
	}

	/* Moves the declarations into the arena. */
	*declarations = NULL;
	*count = list.count;
	if (list.count != 0) {
		*declarations = wb_arena_alloc(arena, list.count * sizeof(struct css_declaration));
		if (*declarations == NULL) {
			wb_vector_release(&list);
			return ENOMEM;
		}

		/* Copies them. */
		memcpy(*declarations, list.items, list.count * sizeof(struct css_declaration));
	}

	/* The list is no longer needed. */
	wb_vector_release(&list);

	/* Succeeded: the declarations are in the arena. */
	return 0;
}

/*
 * Makes the declarations the value parser does not: a custom property
 * (--name: tokens, kept as they are) and a declaration whose value uses
 * var() (kept as tokens for the property it names).  *made is 0 for an
 * ordinary declaration; EINVAL drops a var() value of an unknown property.
 */
static int
parser_special(
	struct vm_heap *heap,
	const struct css_token *name,
	struct token_range value,
	struct css_declaration *out,
	size_t *made)
{
	size_t index;
	int is_var;
	int property;

	/* A name that starts with two dashes is a custom property's. */
	*made = 0;
	memset(out, 0, sizeof(*out));
	if (name->length > 2U && name->text[0] == '-' && name->text[1] == '-') {
		out->property = CSS_PROP_CUSTOM;
		out->custom_name = vm_atom_from_units(heap, name->text, name->length);
		if (out->custom_name == NULL)
			return ENOMEM;
		out->raw = value.tokens;
		out->raw_count = value.count;
		*made = 1;
		return 0;
	}

	/* Looks for var() anywhere in the value. */
	is_var = 0;
	for (index = 0; index < value.count && !is_var; index++) {
		if (value.tokens[index].type == CSS_TOKEN_FUNCTION)
			is_var = css_ident_equal(&value.tokens[index], "var");
	}

	/* An ordinary value. */
	if (!is_var)
		return 0;

	/* The property must be one this pass knows. */
	property = css_property_lookup(name);
	if (property < 0)
		return EINVAL;

	/* The tokens wait for the element's custom properties. */
	out->property = CSS_PROP_PENDING;
	out->pending_property = property;
	out->raw = value.tokens;
	out->raw_count = value.count;
	*made = 1;

	/* Succeeded: the declaration is kept as tokens. */
	return 0;
}

/* Interns a token's text as an atom, folding ASCII upper case when asked. */
static struct vm_string *
parser_atom(
	struct vm_heap *heap,
	const struct css_token *token,
	int lower)
{
	struct vm_string *atom;
	uint16_t folded[256];
	size_t index;

	/* Long names and names kept as they are are interned directly. */
	if (!lower || token->length > 256U) {
		atom = vm_atom_from_units(heap, token->text, token->length);
		return atom;
	}

	/* Folds ASCII upper case and interns the folded name. */
	for (index = 0; index < token->length; index++) {
		folded[index] = token->text[index];
		if (folded[index] >= 'A' && folded[index] <= 'Z')
			folded[index] = (uint16_t)(folded[index] + 0x20U);
	}

	/* Interns the folded name. */
	atom = vm_atom_from_units(heap, folded, token->length);

	/* Reports the atom (NULL when memory ran out). */
	return atom;
}

/* Drops the whitespace tokens at both ends of a range. */
static struct token_range
parser_trim(
	struct token_range range)
{
	/* Leading whitespace. */
	while (range.count > 0 && range.tokens[0].type == CSS_TOKEN_WHITESPACE) {
		range.tokens++;
		range.count--;
	}

	/* Trailing whitespace. */
	while (range.count > 0 && range.tokens[range.count - 1U].type == CSS_TOKEN_WHITESPACE)
		range.count--;

	/* Reports the trimmed range. */
	return range;
}

/*
 * Reads an @font-face rule's descriptors (ws074-p070): font-family,
 * font-weight (a weight or a range), font-style and src.  A face without a
 * family or a source is not kept.
 */
static int
parser_font_face(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count)
{
	struct css_font_face face;
	size_t start;
	size_t index;
	size_t colon;
	size_t name;
	int error;

	/* A face covers every weight at normal style until it says otherwise. */
	memset(&face, 0, sizeof(face));
	face.weight_min = 400;
	face.weight_max = 400;

	/* Each descriptor runs to a semicolon. */
	start = 0;
	while (start < count) {
		index = start;
		while (index < count && tokens[index].type != CSS_TOKEN_SEMICOLON)
			index++;

		/* Its name, a colon, then its value. */
		name = start;
		while (name < index && tokens[name].type == CSS_TOKEN_WHITESPACE)
			name++;
		colon = name + 1U;
		while (colon < index && tokens[colon].type == CSS_TOKEN_WHITESPACE)
			colon++;
		if (name < index && tokens[name].type == CSS_TOKEN_IDENT && colon < index && tokens[colon].type == CSS_TOKEN_COLON) {
			error = parser_font_descriptor(state, &tokens[name], tokens + colon + 1U, index - colon - 1U, &face);
			if (error == ENOMEM)
				return error;
		}

		/* The next descriptor starts after the semicolon. */
		start = index + 1U;
	}

	/* A face without a family or a source names nothing to load. */
	if (face.family == NULL || face.source_count == 0)
		return 0;

	/* Keeps the face. */
	error = wb_vector_push(&state->font_faces, &face);
	if (error != 0)
		return error;

	/* Succeeded: the face is kept. */
	return 0;
}

/* Reads one descriptor of an @font-face rule into the face (one it does not know, or cannot read, is passed by). */
static int
parser_font_descriptor(
	struct parser_state *state,
	const struct css_token *name,
	const struct css_token *tokens,
	size_t count,
	struct css_font_face *face)
{
	size_t second;
	int is_family;
	int is_weight;
	int is_style;
	int is_src;
	int is_italic;
	int error;

	/* The value without the whitespace around it. */
	while (count > 0 && tokens[0].type == CSS_TOKEN_WHITESPACE) {
		tokens++;
		count--;
	}
	while (count > 0 && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	if (count == 0)
		return 0;

	/* font-family: a string, or words. */
	is_family = css_ident_equal(name, "font-family");
	if (is_family) {
		error = parser_font_family(state, tokens, count, &face->family);
		return error;
	}

	/* font-weight: a weight, or two for a range. */
	is_weight = css_ident_equal(name, "font-weight");
	if (is_weight) {
		error = parser_font_weight(&tokens[0], &face->weight_min);
		if (error != 0)
			return 0;
		face->weight_max = face->weight_min;

		/* The second weight of a range. */
		second = 1;
		while (second < count && tokens[second].type == CSS_TOKEN_WHITESPACE)
			second++;
		if (second < count)
			parser_font_weight(&tokens[second], &face->weight_max);
		if (face->weight_max < face->weight_min)
			face->weight_max = face->weight_min;
		return 0;
	}

	/* font-style: italic and oblique are italic. */
	is_style = css_ident_equal(name, "font-style");
	if (is_style) {
		face->italic = 0;
		is_italic = css_ident_equal(&tokens[0], "italic");
		if (is_italic)
			face->italic = 1;
		is_italic = css_ident_equal(&tokens[0], "oblique");
		if (is_italic)
			face->italic = 1;
		return 0;
	}

	/* src: the sources. */
	is_src = css_ident_equal(name, "src");
	if (is_src) {
		error = parser_font_sources(state, tokens, count, face);
		return error;
	}

	/* A descriptor this pass does not use (font-display, unicode-range, ...). */
	return 0;
}

/* Reads a family name: a string, or words joined by single spaces, as an atom. */
static int
parser_font_family(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	struct vm_string **family)
{
	struct wb_units units;
	uint16_t space;
	size_t index;
	int error;

	/* A string is the name. */
	if (tokens[0].type == CSS_TOKEN_STRING) {
		*family = vm_atom_from_units(state->heap, tokens[0].text, tokens[0].length);
		if (*family == NULL)
			return ENOMEM;
		return 0;
	}

	/* Otherwise the words, each space between them one space. */
	wb_units_init(&units);
	space = ' ';
	error = 0;
	for (index = 0; index < count && error == 0; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			error = wb_units_append(&units, &space, 1);
			continue;
		}

		/* Only words make a name. */
		if (tokens[index].type != CSS_TOKEN_IDENT) {
			wb_units_release(&units);
			return 0;
		}

		/* The word joins the name. */
		error = wb_units_append(&units, tokens[index].text, tokens[index].length);
	}

	/* The name as an atom. */
	if (error == 0) {
		*family = vm_atom_from_units(state->heap, units.data, units.length);
		if (*family == NULL)
			error = ENOMEM;
	}

	/* The name's characters are the atom's now. */
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Succeeded: the family is named. */
	return 0;
}

/*
 * Reads src: a comma-separated list of url() sources, each optionally
 * followed by format(); local() sources are passed by.
 */
static int
parser_font_sources(
	struct parser_state *state,
	const struct css_token *tokens,
	size_t count,
	struct css_font_face *face)
{
	struct vm_string *url;
	const struct css_token *format;
	size_t index;
	size_t inner;
	int is_url;
	int is_format;
	int format_kind;

	/* Walks the list, one source up to each comma. */
	index = 0;
	url = NULL;
	format_kind = CSS_FONT_FORMAT_UNKNOWN;
	while (index <= count) {
		/* A comma, or the end, ends the source: a URL is kept with its format. */
		if (index == count || tokens[index].type == CSS_TOKEN_COMMA) {
			if (url != NULL && face->source_count < CSS_FONT_SOURCES) {
				face->sources[face->source_count] = url;
				face->formats[face->source_count] = format_kind;
				face->source_count++;
			}

			/* The next source starts empty. */
			url = NULL;
			format_kind = CSS_FONT_FORMAT_UNKNOWN;
			index++;
			continue;
		}

		/* An unquoted url(...). */
		if (tokens[index].type == CSS_TOKEN_URL) {
			url = vm_atom_from_units(state->heap, tokens[index].text, tokens[index].length);
			if (url == NULL)
				return ENOMEM;
			index++;
			continue;
		}

		/* Other things than functions (whitespace) are passed by. */
		if (tokens[index].type != CSS_TOKEN_FUNCTION) {
			index++;
			continue;
		}

		/* A function: url("...") or format("..."), and its string argument. */
		is_url = css_ident_equal(&tokens[index], "url");
		is_format = css_ident_equal(&tokens[index], "format");
		inner = index + 1U;
		while (inner < count && tokens[inner].type == CSS_TOKEN_WHITESPACE)
			inner++;
		format = NULL;
		if (inner < count && tokens[inner].type == CSS_TOKEN_STRING)
			format = &tokens[inner];
		if (is_url && format != NULL) {
			url = vm_atom_from_units(state->heap, format->text, format->length);
			if (url == NULL)
				return ENOMEM;
		}

		/* The format's name. */
		if (is_format && format != NULL)
			format_kind = parser_font_format(format);

		/* Past the function's closing parenthesis. */
		while (index < count && tokens[index].type != CSS_TOKEN_CLOSE_PAREN)
			index++;
		index++;
	}

	/* Succeeded: the sources are read. */
	return 0;
}

/* Names the format a format() string declares. */
static int
parser_font_format(
	const struct css_token *format)
{
	int same;

	/* WOFF. */
	same = css_units_equal_ascii(format->text, format->length, "woff");
	if (same)
		return CSS_FONT_FORMAT_WOFF;

	/* WOFF 2. */
	same = css_units_equal_ascii(format->text, format->length, "woff2");
	if (same)
		return CSS_FONT_FORMAT_WOFF2;

	/* TrueType, and OpenType (which this pass reads when its outlines are TrueType's). */
	same = css_units_equal_ascii(format->text, format->length, "truetype");
	if (same)
		return CSS_FONT_FORMAT_TRUETYPE;
	same = css_units_equal_ascii(format->text, format->length, "opentype");
	if (same)
		return CSS_FONT_FORMAT_TRUETYPE;

	/* Any other format. */
	return CSS_FONT_FORMAT_OTHER;
}

/* Reads a weight: a number from 1 to 1000, normal (400) or bold (700). */
static int
parser_font_weight(
	const struct css_token *token,
	int *weight)
{
	int is_word;

	/* A number. */
	if (token->type == CSS_TOKEN_NUMBER) {
		if (token->number < 1 || token->number > 1000)
			return EINVAL;
		*weight = (int)token->number;
		return 0;
	}

	/* normal. */
	is_word = css_ident_equal(token, "normal");
	if (is_word) {
		*weight = 400;
		return 0;
	}

	/* bold. */
	is_word = css_ident_equal(token, "bold");
	if (is_word) {
		*weight = 700;
		return 0;
	}

	/* Anything else is not a weight. */
	return EINVAL;
}

/* Moves the @font-face rules into the sheet's arena. */
static int
parser_keep_font_faces(
	struct parser_state *state)
{
	struct css_sheet *sheet;
	size_t count;

	/* A sheet without faces keeps nothing. */
	sheet = state->sheet;
	count = state->font_faces.count;
	if (count == 0)
		return 0;

	/* The faces. */
	sheet->font_faces = wb_arena_alloc(&sheet->arena, count * sizeof(struct css_font_face));
	if (sheet->font_faces == NULL)
		return ENOMEM;
	memcpy(sheet->font_faces, state->font_faces.items, count * sizeof(struct css_font_face));

	/* Succeeded: the sheet has its faces. */
	sheet->font_face_count = count;
	return 0;
}
