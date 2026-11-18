/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HTML tokenizer: the state machine of the WHATWG HTML Standard's
 * "Tokenization" section, state for state.
 *
 * Each call runs the machine until a token is complete or the input runs
 * out.  Characters are gathered into one run and handed out before the
 * next tag, comment, DOCTYPE or end of file, so a consumer sees as few
 * character tokens as the text allows.  The states are grouped by family
 * (text, tags, attributes, comments, DOCTYPEs, script data, CDATA and
 * character references), one function per family.
 */

#include "html/html.h"
#include "html/entities.h"

#include <stdlib.h>
#include <string.h>

/* What consuming reports besides a code point: end of file, or wait for more input. */
#define TOKENIZER_EOF		(-1)
#define TOKENIZER_WAIT		(-2)

/* What one step of the machine reports: go on, or stop until more input comes. */
#define STEP_CONTINUE		0
#define STEP_WAIT		1

/* What a code point is, as bits of char_kind's answer. */
#define KIND_ALPHA		0x01U
#define KIND_UPPER		0x02U
#define KIND_DIGIT		0x04U
#define KIND_HEX		0x08U
#define KIND_SPACE		0x10U
#define KIND_ALNUM		0x20U

/* A lookahead's answer: the text is there, is not, or cannot be told yet. */
#define LOOKAHEAD_MATCH		0
#define LOOKAHEAD_MISMATCH	1
#define LOOKAHEAD_WAIT		2

/* The characters the state machine names. */
#define CHAR_NULL		0x00
#define CHAR_TAB		0x09
#define CHAR_LF			0x0a
#define CHAR_FF			0x0c
#define CHAR_CR			0x0d
#define CHAR_SPACE		0x20

/*
 * The states of the machine, in the standard's order.
 *
 * The families start at the first state of each group, which is how
 * tokenizer_step picks the function that runs a state.
 */
enum tokenizer_state {
	S_DATA,
	S_RCDATA,
	S_RAWTEXT,
	S_SCRIPT_DATA,
	S_PLAINTEXT,
	S_TAG_OPEN,
	S_END_TAG_OPEN,
	S_TAG_NAME,
	S_RCDATA_LESS_THAN,
	S_RCDATA_END_TAG_OPEN,
	S_RCDATA_END_TAG_NAME,
	S_RAWTEXT_LESS_THAN,
	S_RAWTEXT_END_TAG_OPEN,
	S_RAWTEXT_END_TAG_NAME,
	S_SCRIPT_LESS_THAN,
	S_SCRIPT_END_TAG_OPEN,
	S_SCRIPT_END_TAG_NAME,
	S_SCRIPT_ESCAPE_START,
	S_SCRIPT_ESCAPE_START_DASH,
	S_SCRIPT_ESCAPED,
	S_SCRIPT_ESCAPED_DASH,
	S_SCRIPT_ESCAPED_DASH_DASH,
	S_SCRIPT_ESCAPED_LESS_THAN,
	S_SCRIPT_ESCAPED_END_TAG_OPEN,
	S_SCRIPT_ESCAPED_END_TAG_NAME,
	S_SCRIPT_DOUBLE_ESCAPE_START,
	S_SCRIPT_DOUBLE_ESCAPED,
	S_SCRIPT_DOUBLE_ESCAPED_DASH,
	S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH,
	S_SCRIPT_DOUBLE_ESCAPED_LESS_THAN,
	S_SCRIPT_DOUBLE_ESCAPE_END,
	S_BEFORE_ATTRIBUTE_NAME,
	S_ATTRIBUTE_NAME,
	S_AFTER_ATTRIBUTE_NAME,
	S_BEFORE_ATTRIBUTE_VALUE,
	S_ATTRIBUTE_VALUE_DOUBLE,
	S_ATTRIBUTE_VALUE_SINGLE,
	S_ATTRIBUTE_VALUE_UNQUOTED,
	S_AFTER_ATTRIBUTE_VALUE_QUOTED,
	S_SELF_CLOSING_START_TAG,
	S_BOGUS_COMMENT,
	S_MARKUP_DECLARATION_OPEN,
	S_COMMENT_START,
	S_COMMENT_START_DASH,
	S_COMMENT,
	S_COMMENT_LESS_THAN,
	S_COMMENT_LESS_THAN_BANG,
	S_COMMENT_LESS_THAN_BANG_DASH,
	S_COMMENT_LESS_THAN_BANG_DASH_DASH,
	S_COMMENT_END_DASH,
	S_COMMENT_END,
	S_COMMENT_END_BANG,
	S_DOCTYPE,
	S_BEFORE_DOCTYPE_NAME,
	S_DOCTYPE_NAME,
	S_AFTER_DOCTYPE_NAME,
	S_AFTER_DOCTYPE_PUBLIC_KEYWORD,
	S_BEFORE_DOCTYPE_PUBLIC_ID,
	S_DOCTYPE_PUBLIC_ID_DOUBLE,
	S_DOCTYPE_PUBLIC_ID_SINGLE,
	S_AFTER_DOCTYPE_PUBLIC_ID,
	S_BETWEEN_DOCTYPE_IDS,
	S_AFTER_DOCTYPE_SYSTEM_KEYWORD,
	S_BEFORE_DOCTYPE_SYSTEM_ID,
	S_DOCTYPE_SYSTEM_ID_DOUBLE,
	S_DOCTYPE_SYSTEM_ID_SINGLE,
	S_AFTER_DOCTYPE_SYSTEM_ID,
	S_BOGUS_DOCTYPE,
	S_CDATA_SECTION,
	S_CDATA_SECTION_BRACKET,
	S_CDATA_SECTION_END,
	S_CHARACTER_REFERENCE,
	S_NAMED_CHARACTER_REFERENCE,
	S_AMBIGUOUS_AMPERSAND,
	S_NUMERIC_CHARACTER_REFERENCE,
	S_HEX_REFERENCE_START,
	S_DECIMAL_REFERENCE_START,
	S_HEX_REFERENCE,
	S_DECIMAL_REFERENCE,
	S_NUMERIC_REFERENCE_END
};

/*
 * The parse errors, in the order of html_error_names.
 */
enum tokenizer_error {
	E_ABRUPT_CLOSING_OF_EMPTY_COMMENT,
	E_ABRUPT_DOCTYPE_PUBLIC_IDENTIFIER,
	E_ABRUPT_DOCTYPE_SYSTEM_IDENTIFIER,
	E_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE,
	E_CDATA_IN_HTML_CONTENT,
	E_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE,
	E_CONTROL_CHARACTER_IN_INPUT_STREAM,
	E_CONTROL_CHARACTER_REFERENCE,
	E_DUPLICATE_ATTRIBUTE,
	E_END_TAG_WITH_ATTRIBUTES,
	E_END_TAG_WITH_TRAILING_SOLIDUS,
	E_EOF_BEFORE_TAG_NAME,
	E_EOF_IN_CDATA,
	E_EOF_IN_COMMENT,
	E_EOF_IN_DOCTYPE,
	E_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT,
	E_EOF_IN_TAG,
	E_INCORRECTLY_CLOSED_COMMENT,
	E_INCORRECTLY_OPENED_COMMENT,
	E_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME,
	E_INVALID_FIRST_CHARACTER_OF_TAG_NAME,
	E_MISSING_ATTRIBUTE_VALUE,
	E_MISSING_DOCTYPE_NAME,
	E_MISSING_DOCTYPE_PUBLIC_IDENTIFIER,
	E_MISSING_DOCTYPE_SYSTEM_IDENTIFIER,
	E_MISSING_END_TAG_NAME,
	E_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER,
	E_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER,
	E_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE,
	E_MISSING_WHITESPACE_AFTER_DOCTYPE_PUBLIC_KEYWORD,
	E_MISSING_WHITESPACE_AFTER_DOCTYPE_SYSTEM_KEYWORD,
	E_MISSING_WHITESPACE_BEFORE_DOCTYPE_NAME,
	E_MISSING_WHITESPACE_BETWEEN_ATTRIBUTES,
	E_MISSING_WHITESPACE_BETWEEN_DOCTYPE_IDS,
	E_NESTED_COMMENT,
	E_NONCHARACTER_CHARACTER_REFERENCE,
	E_NONCHARACTER_IN_INPUT_STREAM,
	E_NULL_CHARACTER_REFERENCE,
	E_SURROGATE_CHARACTER_REFERENCE,
	E_SURROGATE_IN_INPUT_STREAM,
	E_UNEXPECTED_CHARACTER_AFTER_DOCTYPE_SYSTEM_IDENTIFIER,
	E_UNEXPECTED_CHARACTER_IN_ATTRIBUTE_NAME,
	E_UNEXPECTED_CHARACTER_IN_UNQUOTED_ATTRIBUTE_VALUE,
	E_UNEXPECTED_EQUALS_SIGN_BEFORE_ATTRIBUTE_NAME,
	E_UNEXPECTED_NULL_CHARACTER,
	E_UNEXPECTED_QUESTION_MARK_INSTEAD_OF_TAG_NAME,
	E_UNEXPECTED_SOLIDUS_IN_TAG,
	E_UNKNOWN_NAMED_CHARACTER_REFERENCE,
	E_COUNT
};

/* The standard's names of the parse errors, for tests and diagnostics. */
static const char *const tokenizer_error_names[E_COUNT] = {
	"abrupt-closing-of-empty-comment",
	"abrupt-doctype-public-identifier",
	"abrupt-doctype-system-identifier",
	"absence-of-digits-in-numeric-character-reference",
	"cdata-in-html-content",
	"character-reference-outside-unicode-range",
	"control-character-in-input-stream",
	"control-character-reference",
	"duplicate-attribute",
	"end-tag-with-attributes",
	"end-tag-with-trailing-solidus",
	"eof-before-tag-name",
	"eof-in-cdata",
	"eof-in-comment",
	"eof-in-doctype",
	"eof-in-script-html-comment-like-text",
	"eof-in-tag",
	"incorrectly-closed-comment",
	"incorrectly-opened-comment",
	"invalid-character-sequence-after-doctype-name",
	"invalid-first-character-of-tag-name",
	"missing-attribute-value",
	"missing-doctype-name",
	"missing-doctype-public-identifier",
	"missing-doctype-system-identifier",
	"missing-end-tag-name",
	"missing-quote-before-doctype-public-identifier",
	"missing-quote-before-doctype-system-identifier",
	"missing-semicolon-after-character-reference",
	"missing-whitespace-after-doctype-public-keyword",
	"missing-whitespace-after-doctype-system-keyword",
	"missing-whitespace-before-doctype-name",
	"missing-whitespace-between-attributes",
	"missing-whitespace-between-doctype-public-and-system-identifiers",
	"nested-comment",
	"noncharacter-character-reference",
	"noncharacter-in-input-stream",
	"null-character-reference",
	"surrogate-character-reference",
	"surrogate-in-input-stream",
	"unexpected-character-after-doctype-system-identifier",
	"unexpected-character-in-attribute-name",
	"unexpected-character-in-unquoted-attribute-value",
	"unexpected-equals-sign-before-attribute-name",
	"unexpected-null-character",
	"unexpected-question-mark-instead-of-tag-name",
	"unexpected-solidus-in-tag",
	"unknown-named-character-reference"
};

/*
 * What numeric references to the C1 controls 0x80 to 0x9F stand for (the
 * windows-1252 characters there); zero means the code point stays.
 */
static const uint16_t tokenizer_c1_replacements[32] = {
	0x20ac, 0x0000, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
	0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017d, 0x0000,
	0x0000, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
	0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x0000, 0x017e, 0x0178
};

static int tokenizer_step(struct html_tokenizer *t);
static int tokenizer_text_states(struct html_tokenizer *t);
static int tokenizer_tag_states(struct html_tokenizer *t);
static int tokenizer_end_tag_states(struct html_tokenizer *t);
static int tokenizer_script_states(struct html_tokenizer *t);
static int tokenizer_attribute_states(struct html_tokenizer *t);
static int tokenizer_comment_states(struct html_tokenizer *t);
static int tokenizer_doctype_states(struct html_tokenizer *t);
static int tokenizer_doctype_id_states(struct html_tokenizer *t);
static int tokenizer_cdata_states(struct html_tokenizer *t);
static int tokenizer_reference_states(struct html_tokenizer *t);
static int tokenizer_named_reference(struct html_tokenizer *t);
static void tokenizer_numeric_end(struct html_tokenizer *t);
static int32_t tokenizer_consume(struct html_tokenizer *t);
static void tokenizer_reconsume(struct html_tokenizer *t, int state);
static void tokenizer_check_input(struct html_tokenizer *t, uint32_t code_point, size_t position);
static int tokenizer_lookahead(struct html_tokenizer *t, const char *text, int fold_case);
static void tokenizer_error(struct html_tokenizer *t, int error);
static void tokenizer_emit_char(struct html_tokenizer *t, uint32_t code_point);
static void tokenizer_emit_units(struct html_tokenizer *t, const struct wb_units *units);
static void tokenizer_emit_current(struct html_tokenizer *t);
static void tokenizer_emit_eof(struct html_tokenizer *t);
static void tokenizer_append(struct html_tokenizer *t, struct wb_units *units, uint32_t code_point);
static void tokenizer_begin(struct html_tokenizer *t, enum html_token_type type);
static void tokenizer_begin_attribute(struct html_tokenizer *t);
static void tokenizer_finish_attribute_name(struct html_tokenizer *t);
static struct html_token_attribute *tokenizer_attribute(struct html_tokenizer *t);
static int tokenizer_appropriate_end_tag(const struct html_tokenizer *t);
static int tokenizer_in_attribute(int state);
static void tokenizer_flush_reference(struct html_tokenizer *t);
static int tokenizer_temporary_is_script(const struct html_tokenizer *t);
static void tokenizer_clear_token(struct html_token *token);
static void tokenizer_release_token(struct html_token *token);
static unsigned char_kind(int32_t c);
static int is_noncharacter(uint32_t c);
static int is_control(uint32_t c);

/*
 * Prepares a tokenizer that reads input from the data state.
 */
void
html_tokenizer_init(
	struct html_tokenizer *tokenizer,
	struct html_input *input)
{
	/* Starts empty, in the data state, reading from the input's current position. */
	memset(tokenizer, 0, sizeof(*tokenizer));
	tokenizer->input = input;
	tokenizer->state = S_DATA;
	tokenizer->return_state = S_DATA;
	tokenizer->characters.type = HTML_TOKEN_CHARACTERS;
	wb_units_init(&tokenizer->temporary);
	wb_units_init(&tokenizer->last_start_tag);
}

/*
 * Frees what the tokenizer holds (not the input).
 */
void
html_tokenizer_release(
	struct html_tokenizer *tokenizer)
{
	/* Frees the tokens and the buffers. */
	tokenizer_release_token(&tokenizer->current);
	tokenizer_release_token(&tokenizer->characters);
	wb_units_release(&tokenizer->temporary);
	wb_units_release(&tokenizer->last_start_tag);
}

/*
 * Switches the tokenizer to one of the states its consumer may choose.
 */
void
html_tokenizer_set_state(
	struct html_tokenizer *tokenizer,
	enum html_tokenizer_start state)
{
	/* Maps the consumer's name of the state to the machine's. */
	switch (state) {
	case HTML_TOKENIZE_DATA:
		tokenizer->state = S_DATA;
		break;
	case HTML_TOKENIZE_RCDATA:
		tokenizer->state = S_RCDATA;
		break;
	case HTML_TOKENIZE_RAWTEXT:
		tokenizer->state = S_RAWTEXT;
		break;
	case HTML_TOKENIZE_SCRIPT_DATA:
		tokenizer->state = S_SCRIPT_DATA;
		break;
	case HTML_TOKENIZE_PLAINTEXT:
		tokenizer->state = S_PLAINTEXT;
		break;
	case HTML_TOKENIZE_CDATA_SECTION:
		tokenizer->state = S_CDATA_SECTION;
		break;
	}
}

/*
 * Sets the name an end tag must have to close RCDATA, RAWTEXT or script
 * data (normally the last start tag the tokenizer emitted).
 */
int
html_tokenizer_set_last_start_tag(
	struct html_tokenizer *tokenizer,
	const uint16_t *name,
	size_t length)
{
	int error;

	/* Replaces the remembered name. */
	wb_units_clear(&tokenizer->last_start_tag);
	error = wb_units_append(&tokenizer->last_start_tag, name, length);
	if (error != 0)
		return error;

	/* Succeeded: end tags are compared with the new name. */
	return 0;
}

/*
 * Runs the tokenizer until the next token.
 *
 * Returns the token's type and points token at it; HTML_TOKEN_NONE means
 * the input ran out before it was closed.  After end of file every call
 * returns HTML_TOKEN_EOF.
 */
enum html_token_type
html_tokenizer_next(
	struct html_tokenizer *tokenizer,
	const struct html_token **token)
{
	int step;

	/* The characters handed out last time are done with. */
	if (tokenizer->characters_out) {
		tokenizer->characters.data.length = 0;
		tokenizer->characters_out = 0;
	}

	/* Runs the machine until a token is complete or the input runs out. */
	for (;;) {
		/* A complete token, or end of file, waits behind the characters gathered before it. */
		if (tokenizer->current_ready || tokenizer->eof_emitted || tokenizer->failed) {
			if (tokenizer->characters.data.length != 0) {
				*token = &tokenizer->characters;
				tokenizer->characters_out = 1;
				return HTML_TOKEN_CHARACTERS;
			}
		}

		/* Hands out the complete token. */
		if (tokenizer->current_ready) {
			tokenizer->current_ready = 0;
			*token = &tokenizer->current;
			return tokenizer->current.type;
		}

		/* After end of file, or after running out of memory, only end of file remains. */
		if (tokenizer->eof_emitted || tokenizer->failed) {
			tokenizer_clear_token(&tokenizer->current);
			tokenizer->current.type = HTML_TOKEN_EOF;
			*token = &tokenizer->current;
			return HTML_TOKEN_EOF;
		}

		/* Runs one step; running out of input hands out what was gathered. */
		step = tokenizer_step(tokenizer);
		if (step == STEP_WAIT)
			break;
	}

	/* The characters gathered so far go out now; the rest follows with more input. */
	if (tokenizer->characters.data.length != 0) {
		*token = &tokenizer->characters;
		tokenizer->characters_out = 1;
		return HTML_TOKEN_CHARACTERS;
	}

	/* Nothing is complete until more input arrives. */
	*token = NULL;
	return HTML_TOKEN_NONE;
}

/*
 * Names a parse error the tokenizer recorded (html_tokenizer.errors).
 */
const char *
html_error_name(
	int error)
{
	/* Anything outside the table is not a tokenizer error. */
	if (error < 0 || error >= E_COUNT)
		return "unknown";

	/* Reports the standard's name. */
	return tokenizer_error_names[error];
}

/* Runs the state the machine is in once, through the function of its family. */
static int
tokenizer_step(
	struct html_tokenizer *t)
{
	int state;

	/* Picks the family by the state's place in the standard's order. */
	state = t->state;
	if (state <= S_PLAINTEXT)
		return tokenizer_text_states(t);
	if (state <= S_TAG_NAME)
		return tokenizer_tag_states(t);
	if (state <= S_RAWTEXT_END_TAG_NAME)
		return tokenizer_end_tag_states(t);
	if (state <= S_SCRIPT_DOUBLE_ESCAPE_END)
		return tokenizer_script_states(t);
	if (state <= S_SELF_CLOSING_START_TAG)
		return tokenizer_attribute_states(t);
	if (state <= S_COMMENT_END_BANG)
		return tokenizer_comment_states(t);
	if (state <= S_AFTER_DOCTYPE_NAME)
		return tokenizer_doctype_states(t);
	if (state <= S_BOGUS_DOCTYPE)
		return tokenizer_doctype_id_states(t);
	if (state <= S_CDATA_SECTION_END)
		return tokenizer_cdata_states(t);

	/* The rest are the character reference states. */
	return tokenizer_reference_states(t);
}

/* The data, RCDATA, RAWTEXT, script data and PLAINTEXT states. */
static int
tokenizer_text_states(
	struct html_tokenizer *t)
{
	int32_t c;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* End of file ends every text state. */
	if (c == TOKENIZER_EOF) {
		tokenizer_emit_eof(t);
		return STEP_CONTINUE;
	}

	/* A character reference, where the state allows one. */
	if (c == '&' && (t->state == S_DATA || t->state == S_RCDATA)) {
		t->return_state = t->state;
		t->state = S_CHARACTER_REFERENCE;
		return STEP_CONTINUE;
	}

	/* A less-than sign may start a tag, where the state allows one. */
	if (c == '<') {
		switch (t->state) {
		case S_DATA:
			t->state = S_TAG_OPEN;
			return STEP_CONTINUE;
		case S_RCDATA:
			t->state = S_RCDATA_LESS_THAN;
			return STEP_CONTINUE;
		case S_RAWTEXT:
			t->state = S_RAWTEXT_LESS_THAN;
			return STEP_CONTINUE;
		case S_SCRIPT_DATA:
			t->state = S_SCRIPT_LESS_THAN;
			return STEP_CONTINUE;
		default:
			break;
		}
	}

	/* A NULL is an error; data keeps it, the other states replace it. */
	if (c == CHAR_NULL) {
		tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
		if (t->state == S_DATA) {
			tokenizer_emit_char(t, CHAR_NULL);
		} else {
			tokenizer_emit_char(t, WB_REPLACEMENT);
		}

		/* The replacement or the NULL is the step's text. */
		return STEP_CONTINUE;
	}

	/* Anything else is text. */
	tokenizer_emit_char(t, (uint32_t)c);
	return STEP_CONTINUE;
}

/* The tag open, end tag open and tag name states. */
static int
tokenizer_tag_states(
	struct html_tokenizer *t)
{
	int32_t c;
	unsigned kind;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Classifies the code point once for the decisions below. */
	kind = char_kind(c);

	/* Runs the state. */
	switch (t->state) {
	case S_TAG_OPEN:
		if (c == '!') {
			t->state = S_MARKUP_DECLARATION_OPEN;
		} else if (c == '/') {
			t->state = S_END_TAG_OPEN;
		} else if ((kind & KIND_ALPHA) != 0) {
			tokenizer_begin(t, HTML_TOKEN_START_TAG);
			tokenizer_reconsume(t, S_TAG_NAME);
		} else if (c == '?') {
			tokenizer_error(t, E_UNEXPECTED_QUESTION_MARK_INSTEAD_OF_TAG_NAME);
			tokenizer_begin(t, HTML_TOKEN_COMMENT);
			tokenizer_reconsume(t, S_BOGUS_COMMENT);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_BEFORE_TAG_NAME);
			tokenizer_emit_char(t, '<');
			tokenizer_emit_eof(t);
		} else {
			tokenizer_error(t, E_INVALID_FIRST_CHARACTER_OF_TAG_NAME);
			tokenizer_emit_char(t, '<');
			tokenizer_reconsume(t, S_DATA);
		}

		break;
	case S_END_TAG_OPEN:
		if ((kind & KIND_ALPHA) != 0) {
			tokenizer_begin(t, HTML_TOKEN_END_TAG);
			tokenizer_reconsume(t, S_TAG_NAME);
		} else if (c == '>') {
			tokenizer_error(t, E_MISSING_END_TAG_NAME);
			t->state = S_DATA;
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_BEFORE_TAG_NAME);
			tokenizer_emit_char(t, '<');
			tokenizer_emit_char(t, '/');
			tokenizer_emit_eof(t);
		} else {
			tokenizer_error(t, E_INVALID_FIRST_CHARACTER_OF_TAG_NAME);
			tokenizer_begin(t, HTML_TOKEN_COMMENT);
			tokenizer_reconsume(t, S_BOGUS_COMMENT);
		}

		break;
	case S_TAG_NAME:
		if ((kind & KIND_SPACE) != 0) {
			t->state = S_BEFORE_ATTRIBUTE_NAME;
		} else if (c == '/') {
			t->state = S_SELF_CLOSING_START_TAG;
		} else if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &t->current.name, WB_REPLACEMENT);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_TAG);
			tokenizer_emit_eof(t);
		} else if ((kind & KIND_UPPER) != 0) {
			tokenizer_append(t, &t->current.name, (uint32_t)c + 0x20U);
		} else {
			tokenizer_append(t, &t->current.name, (uint32_t)c);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/*
 * The less-than sign, end tag open and end tag name states of RCDATA and
 * RAWTEXT, which differ only in the state they fall back to.
 */
static int
tokenizer_end_tag_states(
	struct html_tokenizer *t)
{
	int32_t c;
	unsigned kind;
	int text_state;
	int appropriate;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Classifies the code point once for the decisions below. */
	kind = char_kind(c);

	/* The text state these three states belong to. */
	text_state = S_RAWTEXT;
	if (t->state <= S_RCDATA_END_TAG_NAME)
		text_state = S_RCDATA;

	/* Runs the state. */
	switch (t->state) {
	case S_RCDATA_LESS_THAN:
	case S_RAWTEXT_LESS_THAN:
		if (c == '/') {
			wb_units_clear(&t->temporary);
			t->state = t->state + 1;
		} else {
			tokenizer_emit_char(t, '<');
			tokenizer_reconsume(t, text_state);
		}

		break;
	case S_RCDATA_END_TAG_OPEN:
	case S_RAWTEXT_END_TAG_OPEN:
		if ((kind & KIND_ALPHA) != 0) {
			tokenizer_begin(t, HTML_TOKEN_END_TAG);
			tokenizer_reconsume(t, t->state + 1);
		} else {
			tokenizer_emit_char(t, '<');
			tokenizer_emit_char(t, '/');
			tokenizer_reconsume(t, text_state);
		}

		break;
	case S_RCDATA_END_TAG_NAME:
	case S_RAWTEXT_END_TAG_NAME:
		/* Only an end tag that closes the last start tag leaves the text. */
		appropriate = tokenizer_appropriate_end_tag(t);
		if ((kind & KIND_SPACE) != 0 && appropriate) {
			t->state = S_BEFORE_ATTRIBUTE_NAME;
		} else if (c == '/' && appropriate) {
			t->state = S_SELF_CLOSING_START_TAG;
		} else if (c == '>' && appropriate) {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if ((kind & KIND_ALPHA) != 0) {
			tokenizer_append(t, &t->temporary, (uint32_t)c);
			if ((kind & KIND_UPPER) != 0)
				c += 0x20;
			tokenizer_append(t, &t->current.name, (uint32_t)c);
		} else {
			tokenizer_emit_char(t, '<');
			tokenizer_emit_char(t, '/');
			tokenizer_emit_units(t, &t->temporary);
			tokenizer_reconsume(t, text_state);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The script data states after the first: end tags, escapes and double escapes. */
static int
tokenizer_script_states(
	struct html_tokenizer *t)
{
	int32_t c;
	unsigned kind;
	int appropriate;
	int fallback;
	int script;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Classifies the code point once for the decisions below. */
	kind = char_kind(c);

	/* Runs the state. */
	switch (t->state) {
	case S_SCRIPT_LESS_THAN:
		if (c == '/') {
			wb_units_clear(&t->temporary);
			t->state = S_SCRIPT_END_TAG_OPEN;
		} else if (c == '!') {
			t->state = S_SCRIPT_ESCAPE_START;
			tokenizer_emit_char(t, '<');
			tokenizer_emit_char(t, '!');
		} else {
			tokenizer_emit_char(t, '<');
			tokenizer_reconsume(t, S_SCRIPT_DATA);
		}

		break;
	case S_SCRIPT_END_TAG_OPEN:
	case S_SCRIPT_ESCAPED_END_TAG_OPEN:
		/* Both fall back to the text they came from. */
		fallback = S_SCRIPT_DATA;
		if (t->state == S_SCRIPT_ESCAPED_END_TAG_OPEN)
			fallback = S_SCRIPT_ESCAPED;
		if ((kind & KIND_ALPHA) != 0) {
			tokenizer_begin(t, HTML_TOKEN_END_TAG);
			tokenizer_reconsume(t, t->state + 1);
		} else {
			tokenizer_emit_char(t, '<');
			tokenizer_emit_char(t, '/');
			tokenizer_reconsume(t, fallback);
		}

		break;
	case S_SCRIPT_END_TAG_NAME:
	case S_SCRIPT_ESCAPED_END_TAG_NAME:
		/* Only an end tag that closes the script leaves the script data. */
		fallback = S_SCRIPT_DATA;
		if (t->state == S_SCRIPT_ESCAPED_END_TAG_NAME)
			fallback = S_SCRIPT_ESCAPED;
		appropriate = tokenizer_appropriate_end_tag(t);
		if ((kind & KIND_SPACE) != 0 && appropriate) {
			t->state = S_BEFORE_ATTRIBUTE_NAME;
		} else if (c == '/' && appropriate) {
			t->state = S_SELF_CLOSING_START_TAG;
		} else if (c == '>' && appropriate) {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if ((kind & KIND_ALPHA) != 0) {
			tokenizer_append(t, &t->temporary, (uint32_t)c);
			if ((kind & KIND_UPPER) != 0)
				c += 0x20;
			tokenizer_append(t, &t->current.name, (uint32_t)c);
		} else {
			tokenizer_emit_char(t, '<');
			tokenizer_emit_char(t, '/');
			tokenizer_emit_units(t, &t->temporary);
			tokenizer_reconsume(t, fallback);
		}

		break;
	case S_SCRIPT_ESCAPE_START:
	case S_SCRIPT_ESCAPE_START_DASH:
		if (c == '-') {
			t->state = t->state + 1;
			if (t->state == S_SCRIPT_ESCAPED)
				t->state = S_SCRIPT_ESCAPED_DASH_DASH;
			tokenizer_emit_char(t, '-');
		} else {
			tokenizer_reconsume(t, S_SCRIPT_DATA);
		}

		break;
	case S_SCRIPT_ESCAPED:
	case S_SCRIPT_ESCAPED_DASH:
	case S_SCRIPT_ESCAPED_DASH_DASH:
		if (c == '-') {
			if (t->state == S_SCRIPT_ESCAPED) {
				t->state = S_SCRIPT_ESCAPED_DASH;
			} else {
				t->state = S_SCRIPT_ESCAPED_DASH_DASH;
			}

			/* The dash is text too. */
			tokenizer_emit_char(t, '-');
		} else if (c == '<') {
			t->state = S_SCRIPT_ESCAPED_LESS_THAN;
		} else if (c == '>' && t->state == S_SCRIPT_ESCAPED_DASH_DASH) {
			t->state = S_SCRIPT_DATA;
			tokenizer_emit_char(t, '>');
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			t->state = S_SCRIPT_ESCAPED;
			tokenizer_emit_char(t, WB_REPLACEMENT);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
			tokenizer_emit_eof(t);
		} else {
			t->state = S_SCRIPT_ESCAPED;
			tokenizer_emit_char(t, (uint32_t)c);
		}

		break;
	case S_SCRIPT_ESCAPED_LESS_THAN:
		if (c == '/') {
			wb_units_clear(&t->temporary);
			t->state = S_SCRIPT_ESCAPED_END_TAG_OPEN;
		} else if ((kind & KIND_ALPHA) != 0) {
			wb_units_clear(&t->temporary);
			tokenizer_emit_char(t, '<');
			tokenizer_reconsume(t, S_SCRIPT_DOUBLE_ESCAPE_START);
		} else {
			tokenizer_emit_char(t, '<');
			tokenizer_reconsume(t, S_SCRIPT_ESCAPED);
		}

		break;
	case S_SCRIPT_DOUBLE_ESCAPE_START:
	case S_SCRIPT_DOUBLE_ESCAPE_END:
		/* A whole "script" in the temporary buffer switches between escaped and double escaped. */
		if ((kind & KIND_SPACE) != 0 ||
		    c == '/' ||
		    c == '>') {
			script = tokenizer_temporary_is_script(t);
			if (t->state == S_SCRIPT_DOUBLE_ESCAPE_START) {
				t->state = S_SCRIPT_ESCAPED;
				if (script)
					t->state = S_SCRIPT_DOUBLE_ESCAPED;
			} else {
				t->state = S_SCRIPT_DOUBLE_ESCAPED;
				if (script)
					t->state = S_SCRIPT_ESCAPED;
			}

			/* The character that ended the name is text either way. */
			tokenizer_emit_char(t, (uint32_t)c);
		} else if ((kind & KIND_ALPHA) != 0) {
			tokenizer_emit_char(t, (uint32_t)c);
			if ((kind & KIND_UPPER) != 0)
				c += 0x20;
			tokenizer_append(t, &t->temporary, (uint32_t)c);
		} else if (t->state == S_SCRIPT_DOUBLE_ESCAPE_START) {
			tokenizer_reconsume(t, S_SCRIPT_ESCAPED);
		} else {
			tokenizer_reconsume(t, S_SCRIPT_DOUBLE_ESCAPED);
		}

		break;
	case S_SCRIPT_DOUBLE_ESCAPED:
	case S_SCRIPT_DOUBLE_ESCAPED_DASH:
	case S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH:
		if (c == '-') {
			if (t->state == S_SCRIPT_DOUBLE_ESCAPED) {
				t->state = S_SCRIPT_DOUBLE_ESCAPED_DASH;
			} else {
				t->state = S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH;
			}

			/* The dash is text too. */
			tokenizer_emit_char(t, '-');
		} else if (c == '<') {
			t->state = S_SCRIPT_DOUBLE_ESCAPED_LESS_THAN;
			tokenizer_emit_char(t, '<');
		} else if (c == '>' && t->state == S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH) {
			t->state = S_SCRIPT_DATA;
			tokenizer_emit_char(t, '>');
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			t->state = S_SCRIPT_DOUBLE_ESCAPED;
			tokenizer_emit_char(t, WB_REPLACEMENT);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
			tokenizer_emit_eof(t);
		} else {
			t->state = S_SCRIPT_DOUBLE_ESCAPED;
			tokenizer_emit_char(t, (uint32_t)c);
		}

		break;
	case S_SCRIPT_DOUBLE_ESCAPED_LESS_THAN:
		if (c == '/') {
			wb_units_clear(&t->temporary);
			t->state = S_SCRIPT_DOUBLE_ESCAPE_END;
			tokenizer_emit_char(t, '/');
		} else {
			tokenizer_reconsume(t, S_SCRIPT_DOUBLE_ESCAPED);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The attribute states and the self-closing start tag state. */
static int
tokenizer_attribute_states(
	struct html_tokenizer *t)
{
	struct html_token_attribute *attribute;
	int32_t c;
	unsigned kind;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Classifies the code point once for the decisions below. */
	kind = char_kind(c);

	/* Runs the state. */
	attribute = tokenizer_attribute(t);
	switch (t->state) {
	case S_BEFORE_ATTRIBUTE_NAME:
		if ((kind & KIND_SPACE) != 0) {
			break;
		} else if (c == '/' ||
			   c == '>' ||
			   c == TOKENIZER_EOF) {
			tokenizer_reconsume(t, S_AFTER_ATTRIBUTE_NAME);
		} else if (c == '=') {
			tokenizer_error(t, E_UNEXPECTED_EQUALS_SIGN_BEFORE_ATTRIBUTE_NAME);
			tokenizer_begin_attribute(t);
			attribute = tokenizer_attribute(t);
			if (attribute != NULL)
				tokenizer_append(t, &attribute->name, (uint32_t)c);
			t->state = S_ATTRIBUTE_NAME;
		} else {
			tokenizer_begin_attribute(t);
			tokenizer_reconsume(t, S_ATTRIBUTE_NAME);
		}

		break;
	case S_ATTRIBUTE_NAME:
		if ((kind & KIND_SPACE) != 0 ||
		    c == '/' ||
		    c == '>' ||
		    c == TOKENIZER_EOF) {
			tokenizer_finish_attribute_name(t);
			tokenizer_reconsume(t, S_AFTER_ATTRIBUTE_NAME);
		} else if (c == '=') {
			tokenizer_finish_attribute_name(t);
			t->state = S_BEFORE_ATTRIBUTE_VALUE;
		} else if (attribute == NULL) {
			break;
		} else if ((kind & KIND_UPPER) != 0) {
			tokenizer_append(t, &attribute->name, (uint32_t)c + 0x20U);
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &attribute->name, WB_REPLACEMENT);
		} else {
			if (c == '"' ||
			    c == '\'' ||
			    c == '<')
				tokenizer_error(t, E_UNEXPECTED_CHARACTER_IN_ATTRIBUTE_NAME);
			tokenizer_append(t, &attribute->name, (uint32_t)c);
		}

		break;
	case S_AFTER_ATTRIBUTE_NAME:
		if ((kind & KIND_SPACE) != 0) {
			break;
		} else if (c == '/') {
			t->state = S_SELF_CLOSING_START_TAG;
		} else if (c == '=') {
			t->state = S_BEFORE_ATTRIBUTE_VALUE;
		} else if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_TAG);
			tokenizer_emit_eof(t);
		} else {
			tokenizer_begin_attribute(t);
			tokenizer_reconsume(t, S_ATTRIBUTE_NAME);
		}

		break;
	case S_BEFORE_ATTRIBUTE_VALUE:
		if ((kind & KIND_SPACE) != 0) {
			break;
		} else if (c == '"') {
			t->state = S_ATTRIBUTE_VALUE_DOUBLE;
		} else if (c == '\'') {
			t->state = S_ATTRIBUTE_VALUE_SINGLE;
		} else if (c == '>') {
			tokenizer_error(t, E_MISSING_ATTRIBUTE_VALUE);
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_reconsume(t, S_ATTRIBUTE_VALUE_UNQUOTED);
		}

		break;
	case S_ATTRIBUTE_VALUE_DOUBLE:
	case S_ATTRIBUTE_VALUE_SINGLE:
		if ((c == '"' && t->state == S_ATTRIBUTE_VALUE_DOUBLE) ||
		    (c == '\'' && t->state == S_ATTRIBUTE_VALUE_SINGLE)) {
			t->state = S_AFTER_ATTRIBUTE_VALUE_QUOTED;
		} else if (c == '&') {
			t->return_state = t->state;
			t->state = S_CHARACTER_REFERENCE;
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_TAG);
			tokenizer_emit_eof(t);
		} else if (attribute == NULL) {
			break;
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &attribute->value, WB_REPLACEMENT);
		} else {
			tokenizer_append(t, &attribute->value, (uint32_t)c);
		}

		break;
	case S_ATTRIBUTE_VALUE_UNQUOTED:
		if ((kind & KIND_SPACE) != 0) {
			t->state = S_BEFORE_ATTRIBUTE_NAME;
		} else if (c == '&') {
			t->return_state = S_ATTRIBUTE_VALUE_UNQUOTED;
			t->state = S_CHARACTER_REFERENCE;
		} else if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_TAG);
			tokenizer_emit_eof(t);
		} else if (attribute == NULL) {
			break;
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &attribute->value, WB_REPLACEMENT);
		} else {
			if (c == '"' ||
			    c == '\'' ||
			    c == '<' ||
			    c == '=' ||
			    c == '`')
				tokenizer_error(t, E_UNEXPECTED_CHARACTER_IN_UNQUOTED_ATTRIBUTE_VALUE);
			tokenizer_append(t, &attribute->value, (uint32_t)c);
		}

		break;
	case S_AFTER_ATTRIBUTE_VALUE_QUOTED:
		if ((kind & KIND_SPACE) != 0) {
			t->state = S_BEFORE_ATTRIBUTE_NAME;
		} else if (c == '/') {
			t->state = S_SELF_CLOSING_START_TAG;
		} else if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_TAG);
			tokenizer_emit_eof(t);
		} else {
			tokenizer_error(t, E_MISSING_WHITESPACE_BETWEEN_ATTRIBUTES);
			tokenizer_reconsume(t, S_BEFORE_ATTRIBUTE_NAME);
		}

		break;
	case S_SELF_CLOSING_START_TAG:
		if (c == '>') {
			t->current.self_closing = 1;
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_TAG);
			tokenizer_emit_eof(t);
		} else {
			tokenizer_error(t, E_UNEXPECTED_SOLIDUS_IN_TAG);
			tokenizer_reconsume(t, S_BEFORE_ATTRIBUTE_NAME);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The bogus comment, markup declaration open and comment states. */
static int
tokenizer_comment_states(
	struct html_tokenizer *t)
{
	int32_t c;
	int match;

	/* The markup declaration open state looks ahead before it consumes anything. */
	if (t->state == S_MARKUP_DECLARATION_OPEN) {
		match = tokenizer_lookahead(t, "--", 0);
		if (match == LOOKAHEAD_WAIT)
			return STEP_WAIT;
		if (match == LOOKAHEAD_MATCH) {
			t->input->position += 2;
			tokenizer_begin(t, HTML_TOKEN_COMMENT);
			t->state = S_COMMENT_START;
			return STEP_CONTINUE;
		}

		/* A DOCTYPE, in any case. */
		match = tokenizer_lookahead(t, "DOCTYPE", 1);
		if (match == LOOKAHEAD_WAIT)
			return STEP_WAIT;
		if (match == LOOKAHEAD_MATCH) {
			t->input->position += 7;
			t->state = S_DOCTYPE;
			return STEP_CONTINUE;
		}

		/* A CDATA section opens only in foreign content; elsewhere it is a bogus comment. */
		match = tokenizer_lookahead(t, "[CDATA[", 0);
		if (match == LOOKAHEAD_WAIT)
			return STEP_WAIT;
		if (match == LOOKAHEAD_MATCH) {
			t->input->position += 7;
			if (t->allow_cdata) {
				t->state = S_CDATA_SECTION;
				return STEP_CONTINUE;
			}

			/* Outside foreign content the section is a bogus comment of its text. */
			tokenizer_error(t, E_CDATA_IN_HTML_CONTENT);
			tokenizer_begin(t, HTML_TOKEN_COMMENT);
			tokenizer_append(t, &t->current.data, '[');
			tokenizer_append(t, &t->current.data, 'C');
			tokenizer_append(t, &t->current.data, 'D');
			tokenizer_append(t, &t->current.data, 'A');
			tokenizer_append(t, &t->current.data, 'T');
			tokenizer_append(t, &t->current.data, 'A');
			tokenizer_append(t, &t->current.data, '[');
			t->state = S_BOGUS_COMMENT;
			return STEP_CONTINUE;
		}

		/* Anything else opens a bogus comment without consuming. */
		tokenizer_error(t, E_INCORRECTLY_OPENED_COMMENT);
		tokenizer_begin(t, HTML_TOKEN_COMMENT);
		t->state = S_BOGUS_COMMENT;
		return STEP_CONTINUE;
	}

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* End of file inside any comment state emits the comment first. */
	if (c == TOKENIZER_EOF) {
		if (t->state != S_BOGUS_COMMENT)
			tokenizer_error(t, E_EOF_IN_COMMENT);
		tokenizer_emit_current(t);
		tokenizer_emit_eof(t);
		return STEP_CONTINUE;
	}

	/* Runs the state. */
	switch (t->state) {
	case S_BOGUS_COMMENT:
		if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &t->current.data, WB_REPLACEMENT);
		} else {
			tokenizer_append(t, &t->current.data, (uint32_t)c);
		}

		break;
	case S_COMMENT_START:
		if (c == '-') {
			t->state = S_COMMENT_START_DASH;
		} else if (c == '>') {
			tokenizer_error(t, E_ABRUPT_CLOSING_OF_EMPTY_COMMENT);
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_reconsume(t, S_COMMENT);
		}

		break;
	case S_COMMENT_START_DASH:
		if (c == '-') {
			t->state = S_COMMENT_END;
		} else if (c == '>') {
			tokenizer_error(t, E_ABRUPT_CLOSING_OF_EMPTY_COMMENT);
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_reconsume(t, S_COMMENT);
		}

		break;
	case S_COMMENT:
		if (c == '<') {
			tokenizer_append(t, &t->current.data, '<');
			t->state = S_COMMENT_LESS_THAN;
		} else if (c == '-') {
			t->state = S_COMMENT_END_DASH;
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &t->current.data, WB_REPLACEMENT);
		} else {
			tokenizer_append(t, &t->current.data, (uint32_t)c);
		}

		break;
	case S_COMMENT_LESS_THAN:
		if (c == '!') {
			tokenizer_append(t, &t->current.data, '!');
			t->state = S_COMMENT_LESS_THAN_BANG;
		} else if (c == '<') {
			tokenizer_append(t, &t->current.data, '<');
		} else {
			tokenizer_reconsume(t, S_COMMENT);
		}

		break;
	case S_COMMENT_LESS_THAN_BANG:
		if (c == '-') {
			t->state = S_COMMENT_LESS_THAN_BANG_DASH;
		} else {
			tokenizer_reconsume(t, S_COMMENT);
		}

		break;
	case S_COMMENT_LESS_THAN_BANG_DASH:
		if (c == '-') {
			t->state = S_COMMENT_LESS_THAN_BANG_DASH_DASH;
		} else {
			tokenizer_reconsume(t, S_COMMENT_END_DASH);
		}

		break;
	case S_COMMENT_LESS_THAN_BANG_DASH_DASH:
		if (c != '>')
			tokenizer_error(t, E_NESTED_COMMENT);
		tokenizer_reconsume(t, S_COMMENT_END);
		break;
	case S_COMMENT_END_DASH:
		if (c == '-') {
			t->state = S_COMMENT_END;
		} else {
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_reconsume(t, S_COMMENT);
		}

		break;
	case S_COMMENT_END:
		if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == '!') {
			t->state = S_COMMENT_END_BANG;
		} else if (c == '-') {
			tokenizer_append(t, &t->current.data, '-');
		} else {
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_reconsume(t, S_COMMENT);
		}

		break;
	case S_COMMENT_END_BANG:
		if (c == '-') {
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_append(t, &t->current.data, '!');
			t->state = S_COMMENT_END_DASH;
		} else if (c == '>') {
			tokenizer_error(t, E_INCORRECTLY_CLOSED_COMMENT);
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_append(t, &t->current.data, '-');
			tokenizer_append(t, &t->current.data, '!');
			tokenizer_reconsume(t, S_COMMENT);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The DOCTYPE states up to the keywords after the name. */
static int
tokenizer_doctype_states(
	struct html_tokenizer *t)
{
	int32_t c;
	unsigned kind;
	int match;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Classifies the code point once for the decisions below. */
	kind = char_kind(c);

	/* End of file in a DOCTYPE emits one that forces quirks mode. */
	if (c == TOKENIZER_EOF) {
		tokenizer_error(t, E_EOF_IN_DOCTYPE);
		if (t->state == S_DOCTYPE || t->state == S_BEFORE_DOCTYPE_NAME)
			tokenizer_begin(t, HTML_TOKEN_DOCTYPE);
		t->current.force_quirks = 1;
		tokenizer_emit_current(t);
		tokenizer_emit_eof(t);
		return STEP_CONTINUE;
	}

	/* Runs the state. */
	switch (t->state) {
	case S_DOCTYPE:
		if ((kind & KIND_SPACE) != 0) {
			t->state = S_BEFORE_DOCTYPE_NAME;
		} else if (c == '>') {
			tokenizer_reconsume(t, S_BEFORE_DOCTYPE_NAME);
		} else {
			tokenizer_error(t, E_MISSING_WHITESPACE_BEFORE_DOCTYPE_NAME);
			tokenizer_reconsume(t, S_BEFORE_DOCTYPE_NAME);
		}

		break;
	case S_BEFORE_DOCTYPE_NAME:
		if ((kind & KIND_SPACE) != 0)
			break;

		/* Every other character starts the DOCTYPE token. */
		tokenizer_begin(t, HTML_TOKEN_DOCTYPE);
		if (c == '>') {
			tokenizer_error(t, E_MISSING_DOCTYPE_NAME);
			t->current.force_quirks = 1;
			t->state = S_DATA;
			tokenizer_emit_current(t);
			break;
		}

		/* The first character of the name. */
		t->current.has_name = 1;
		if ((kind & KIND_UPPER) != 0) {
			tokenizer_append(t, &t->current.name, (uint32_t)c + 0x20U);
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &t->current.name, WB_REPLACEMENT);
		} else {
			tokenizer_append(t, &t->current.name, (uint32_t)c);
		}

		/* The rest of the name follows. */
		t->state = S_DOCTYPE_NAME;
		break;
	case S_DOCTYPE_NAME:
		if ((kind & KIND_SPACE) != 0) {
			t->state = S_AFTER_DOCTYPE_NAME;
		} else if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if ((kind & KIND_UPPER) != 0) {
			tokenizer_append(t, &t->current.name, (uint32_t)c + 0x20U);
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, &t->current.name, WB_REPLACEMENT);
		} else {
			tokenizer_append(t, &t->current.name, (uint32_t)c);
		}

		break;
	case S_AFTER_DOCTYPE_NAME:
		if ((kind & KIND_SPACE) != 0)
			break;
		if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
			break;
		}

		/* The keywords are read from the character just consumed. */
		tokenizer_reconsume(t, S_AFTER_DOCTYPE_NAME);
		match = tokenizer_lookahead(t, "PUBLIC", 1);
		if (match == LOOKAHEAD_WAIT)
			return STEP_WAIT;
		if (match == LOOKAHEAD_MATCH) {
			t->input->position += 6;
			t->state = S_AFTER_DOCTYPE_PUBLIC_KEYWORD;
			break;
		}

		/* Or the SYSTEM keyword. */
		match = tokenizer_lookahead(t, "SYSTEM", 1);
		if (match == LOOKAHEAD_WAIT)
			return STEP_WAIT;
		if (match == LOOKAHEAD_MATCH) {
			t->input->position += 6;
			t->state = S_AFTER_DOCTYPE_SYSTEM_KEYWORD;
			break;
		}

		/* Anything else makes the DOCTYPE bogus. */
		tokenizer_error(t, E_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME);
		t->current.force_quirks = 1;
		t->state = S_BOGUS_DOCTYPE;
		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The DOCTYPE states of the public and system identifiers, and the bogus DOCTYPE state. */
static int
tokenizer_doctype_id_states(
	struct html_tokenizer *t)
{
	struct wb_units *identifier;
	int32_t c;
	unsigned kind;
	int quote;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Classifies the code point once for the decisions below. */
	kind = char_kind(c);

	/* End of file emits the DOCTYPE; only the bogus state keeps its quirks flag. */
	if (c == TOKENIZER_EOF) {
		if (t->state != S_BOGUS_DOCTYPE) {
			tokenizer_error(t, E_EOF_IN_DOCTYPE);
			t->current.force_quirks = 1;
		}

		/* The DOCTYPE goes out, then end of file. */
		tokenizer_emit_current(t);
		tokenizer_emit_eof(t);
		return STEP_CONTINUE;
	}

	/* Runs the state. */
	switch (t->state) {
	case S_AFTER_DOCTYPE_PUBLIC_KEYWORD:
	case S_BEFORE_DOCTYPE_PUBLIC_ID:
		if ((kind & KIND_SPACE) != 0) {
			if (t->state == S_AFTER_DOCTYPE_PUBLIC_KEYWORD)
				t->state = S_BEFORE_DOCTYPE_PUBLIC_ID;
		} else if (c == '"' || c == '\'') {
			if (t->state == S_AFTER_DOCTYPE_PUBLIC_KEYWORD)
				tokenizer_error(t, E_MISSING_WHITESPACE_AFTER_DOCTYPE_PUBLIC_KEYWORD);
			t->current.has_public_id = 1;
			wb_units_clear(&t->current.public_id);
			t->state = S_DOCTYPE_PUBLIC_ID_SINGLE;
			if (c == '"')
				t->state = S_DOCTYPE_PUBLIC_ID_DOUBLE;
		} else if (c == '>') {
			tokenizer_error(t, E_MISSING_DOCTYPE_PUBLIC_IDENTIFIER);
			t->current.force_quirks = 1;
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_error(t, E_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER);
			t->current.force_quirks = 1;
			tokenizer_reconsume(t, S_BOGUS_DOCTYPE);
		}

		break;
	case S_AFTER_DOCTYPE_SYSTEM_KEYWORD:
	case S_BEFORE_DOCTYPE_SYSTEM_ID:
		if ((kind & KIND_SPACE) != 0) {
			if (t->state == S_AFTER_DOCTYPE_SYSTEM_KEYWORD)
				t->state = S_BEFORE_DOCTYPE_SYSTEM_ID;
		} else if (c == '"' || c == '\'') {
			if (t->state == S_AFTER_DOCTYPE_SYSTEM_KEYWORD)
				tokenizer_error(t, E_MISSING_WHITESPACE_AFTER_DOCTYPE_SYSTEM_KEYWORD);
			t->current.has_system_id = 1;
			wb_units_clear(&t->current.system_id);
			t->state = S_DOCTYPE_SYSTEM_ID_SINGLE;
			if (c == '"')
				t->state = S_DOCTYPE_SYSTEM_ID_DOUBLE;
		} else if (c == '>') {
			tokenizer_error(t, E_MISSING_DOCTYPE_SYSTEM_IDENTIFIER);
			t->current.force_quirks = 1;
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_error(t, E_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
			t->current.force_quirks = 1;
			tokenizer_reconsume(t, S_BOGUS_DOCTYPE);
		}

		break;
	case S_DOCTYPE_PUBLIC_ID_DOUBLE:
	case S_DOCTYPE_PUBLIC_ID_SINGLE:
	case S_DOCTYPE_SYSTEM_ID_DOUBLE:
	case S_DOCTYPE_SYSTEM_ID_SINGLE:
		/* The identifier and the quote that ends it. */
		identifier = &t->current.system_id;
		if (t->state == S_DOCTYPE_PUBLIC_ID_DOUBLE || t->state == S_DOCTYPE_PUBLIC_ID_SINGLE)
			identifier = &t->current.public_id;
		quote = '\'';
		if (t->state == S_DOCTYPE_PUBLIC_ID_DOUBLE || t->state == S_DOCTYPE_SYSTEM_ID_DOUBLE)
			quote = '"';
		if (c == quote) {
			t->state = S_AFTER_DOCTYPE_SYSTEM_ID;
			if (identifier == &t->current.public_id)
				t->state = S_AFTER_DOCTYPE_PUBLIC_ID;
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
			tokenizer_append(t, identifier, WB_REPLACEMENT);
		} else if (c == '>') {
			if (identifier == &t->current.public_id) {
				tokenizer_error(t, E_ABRUPT_DOCTYPE_PUBLIC_IDENTIFIER);
			} else {
				tokenizer_error(t, E_ABRUPT_DOCTYPE_SYSTEM_IDENTIFIER);
			}

			/* An identifier cut short forces quirks mode. */
			t->current.force_quirks = 1;
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_append(t, identifier, (uint32_t)c);
		}

		break;
	case S_AFTER_DOCTYPE_PUBLIC_ID:
	case S_BETWEEN_DOCTYPE_IDS:
		if ((kind & KIND_SPACE) != 0) {
			t->state = S_BETWEEN_DOCTYPE_IDS;
		} else if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == '"' || c == '\'') {
			if (t->state == S_AFTER_DOCTYPE_PUBLIC_ID)
				tokenizer_error(t, E_MISSING_WHITESPACE_BETWEEN_DOCTYPE_IDS);
			t->current.has_system_id = 1;
			wb_units_clear(&t->current.system_id);
			t->state = S_DOCTYPE_SYSTEM_ID_SINGLE;
			if (c == '"')
				t->state = S_DOCTYPE_SYSTEM_ID_DOUBLE;
		} else {
			tokenizer_error(t, E_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
			t->current.force_quirks = 1;
			tokenizer_reconsume(t, S_BOGUS_DOCTYPE);
		}

		break;
	case S_AFTER_DOCTYPE_SYSTEM_ID:
		if ((kind & KIND_SPACE) != 0) {
			break;
		} else if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else {
			tokenizer_error(t, E_UNEXPECTED_CHARACTER_AFTER_DOCTYPE_SYSTEM_IDENTIFIER);
			tokenizer_reconsume(t, S_BOGUS_DOCTYPE);
		}

		break;
	case S_BOGUS_DOCTYPE:
		if (c == '>') {
			t->state = S_DATA;
			tokenizer_emit_current(t);
		} else if (c == CHAR_NULL) {
			tokenizer_error(t, E_UNEXPECTED_NULL_CHARACTER);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The CDATA section states. */
static int
tokenizer_cdata_states(
	struct html_tokenizer *t)
{
	int32_t c;

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Runs the state. */
	switch (t->state) {
	case S_CDATA_SECTION:
		if (c == ']') {
			t->state = S_CDATA_SECTION_BRACKET;
		} else if (c == TOKENIZER_EOF) {
			tokenizer_error(t, E_EOF_IN_CDATA);
			tokenizer_emit_eof(t);
		} else {
			tokenizer_emit_char(t, (uint32_t)c);
		}

		break;
	case S_CDATA_SECTION_BRACKET:
		if (c == ']') {
			t->state = S_CDATA_SECTION_END;
		} else {
			tokenizer_emit_char(t, ']');
			tokenizer_reconsume(t, S_CDATA_SECTION);
		}

		break;
	case S_CDATA_SECTION_END:
		if (c == ']') {
			tokenizer_emit_char(t, ']');
		} else if (c == '>') {
			t->state = S_DATA;
		} else {
			tokenizer_emit_char(t, ']');
			tokenizer_emit_char(t, ']');
			tokenizer_reconsume(t, S_CDATA_SECTION);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The character reference states. */
static int
tokenizer_reference_states(
	struct html_tokenizer *t)
{
	struct html_token_attribute *attribute;
	int32_t c;
	unsigned kind;
	uint32_t digit;
	int in_attribute;

	/* The named reference looks ahead itself, and the numeric end consumes nothing. */
	if (t->state == S_NAMED_CHARACTER_REFERENCE)
		return tokenizer_named_reference(t);
	if (t->state == S_NUMERIC_REFERENCE_END) {
		tokenizer_numeric_end(t);
		return STEP_CONTINUE;
	}

	/* Takes the next code point, or stops for more input. */
	c = tokenizer_consume(t);
	if (c == TOKENIZER_WAIT)
		return STEP_WAIT;

	/* Classifies the code point once for the decisions below. */
	kind = char_kind(c);

	/* Runs the state. */
	switch (t->state) {
	case S_CHARACTER_REFERENCE:
		wb_units_clear(&t->temporary);
		tokenizer_append(t, &t->temporary, '&');
		if ((kind & KIND_ALNUM) != 0) {
			tokenizer_reconsume(t, S_NAMED_CHARACTER_REFERENCE);
		} else if (c == '#') {
			tokenizer_append(t, &t->temporary, '#');
			t->state = S_NUMERIC_CHARACTER_REFERENCE;
		} else {
			tokenizer_flush_reference(t);
			tokenizer_reconsume(t, t->return_state);
		}

		break;
	case S_AMBIGUOUS_AMPERSAND:
		if ((kind & KIND_ALNUM) != 0) {
			in_attribute = tokenizer_in_attribute(t->return_state);
			attribute = NULL;
			if (in_attribute)
				attribute = tokenizer_attribute(t);
			if (attribute != NULL) {
				tokenizer_append(t, &attribute->value, (uint32_t)c);
			} else if (!in_attribute) {
				tokenizer_emit_char(t, (uint32_t)c);
			}
		} else {
			if (c == ';')
				tokenizer_error(t, E_UNKNOWN_NAMED_CHARACTER_REFERENCE);
			tokenizer_reconsume(t, t->return_state);
		}

		break;
	case S_NUMERIC_CHARACTER_REFERENCE:
		t->reference_code = 0;
		if (c == 'x' || c == 'X') {
			tokenizer_append(t, &t->temporary, (uint32_t)c);
			t->state = S_HEX_REFERENCE_START;
		} else {
			tokenizer_reconsume(t, S_DECIMAL_REFERENCE_START);
		}

		break;
	case S_HEX_REFERENCE_START:
	case S_DECIMAL_REFERENCE_START:
		if ((t->state == S_HEX_REFERENCE_START && (kind & KIND_HEX) != 0) ||
		    (t->state == S_DECIMAL_REFERENCE_START && (kind & KIND_DIGIT) != 0)) {
			tokenizer_reconsume(t, t->state + 2);
		} else {
			tokenizer_error(t, E_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE);
			tokenizer_flush_reference(t);
			tokenizer_reconsume(t, t->return_state);
		}

		break;
	case S_HEX_REFERENCE:
	case S_DECIMAL_REFERENCE:
		if ((t->state == S_HEX_REFERENCE && (kind & KIND_HEX) != 0) ||
		    (t->state == S_DECIMAL_REFERENCE && (kind & KIND_DIGIT) != 0)) {
			/* Adds the digit, stopping just past the last code point so the value cannot overflow. */
			if ((kind & KIND_DIGIT) != 0) {
				digit = (uint32_t)(c - '0');
			} else if ((kind & KIND_UPPER) != 0) {
				digit = (uint32_t)(c - 'A' + 10);
			} else {
				digit = (uint32_t)(c - 'a' + 10);
			}

			/* Values past the last code point stop growing; they are replaced at the end anyway. */
			if (t->reference_code <= WB_CODE_POINT_MAX) {
				if (t->state == S_HEX_REFERENCE) {
					t->reference_code = t->reference_code * 16U + digit;
				} else {
					t->reference_code = t->reference_code * 10U + digit;
				}
			}
		} else if (c == ';') {
			t->state = S_NUMERIC_REFERENCE_END;
		} else {
			tokenizer_error(t, E_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE);
			tokenizer_reconsume(t, S_NUMERIC_REFERENCE_END);
		}

		break;
	}

	/* The step is done. */
	return STEP_CONTINUE;
}

/*
 * The named character reference state: finds the longest name in the table
 * that the input starts with.
 */
static int
tokenizer_named_reference(
	struct html_tokenizer *t)
{
	const struct html_entity *entity;
	struct html_input *input;
	uint16_t unit;
	uint16_t next;
	size_t low;
	size_t high;
	size_t middle;
	size_t length;
	size_t matched_length;
	size_t matched;
	size_t available;
	unsigned next_kind;
	int in_attribute;
	int found;

	/*
	 * Narrows the range of table names that share the prefix read so far, one
	 * character at a time; a name exactly as long as the prefix sorts first in
	 * the range, so the longest match is remembered as the range shrinks.
	 */
	input = t->input;
	low = 0;
	high = html_entity_count;
	length = 0;
	matched_length = 0;
	matched = 0;
	found = 0;
	for (;;) {
		/* Waits for more input while a longer name could still match. */
		available = input->units.length - input->position;
		if (length >= available) {
			if (!input->closed)
				return STEP_WAIT;
			break;
		}

		/* Names are ASCII; anything else ends the name. */
		unit = input->units.data[input->position + length];
		if (unit > 0x7fU)
			break;

		/* Skips the names that end before this character, then those whose character is smaller. */
		while (low < high && html_entities[low].name[length] == '\0')
			low++;
		middle = low;
		while (middle < high && (unsigned char)html_entities[middle].name[length] < unit)
			middle++;
		low = middle;
		while (middle < high && (unsigned char)html_entities[middle].name[length] == unit)
			middle++;
		high = middle;
		if (low >= high)
			break;
		length++;

		/* A name that ends here is the longest match so far. */
		if (html_entities[low].name[length] == '\0') {
			matched_length = length;
			matched = low;
			found = 1;
		}
	}

	/* No name matched: the ampersand and what follows are text. */
	if (!found) {
		tokenizer_flush_reference(t);
		t->state = S_AMBIGUOUS_AMPERSAND;
		return STEP_CONTINUE;
	}

	/* Consumes the name and keeps it in the temporary buffer. */
	entity = &html_entities[matched];
	for (middle = 0; middle < matched_length; middle++)
		tokenizer_append(t, &t->temporary, input->units.data[input->position + middle]);
	input->position += matched_length;

	/*
	 * In an attribute, a name without its semicolon followed by '=' or an
	 * alphanumeric is left as text, for URLs like ?a=1&copy=2.
	 */
	in_attribute = tokenizer_in_attribute(t->return_state);
	if (entity->name[matched_length - 1U] != ';' && in_attribute) {
		if (input->position < input->units.length) {
			next = input->units.data[input->position];
			next_kind = char_kind(next);
			if (next == '=' || (next_kind & KIND_ALNUM) != 0) {
				tokenizer_flush_reference(t);
				t->state = t->return_state;
				return STEP_CONTINUE;
			}
		} else if (!input->closed) {
			input->position -= matched_length;
			t->temporary.length -= matched_length;
			return STEP_WAIT;
		}
	}

	/* A name without its semicolon is an error, but still stands for its characters. */
	if (entity->name[matched_length - 1U] != ';')
		tokenizer_error(t, E_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE);
	wb_units_clear(&t->temporary);
	tokenizer_append(t, &t->temporary, entity->first);
	if (entity->second != 0)
		tokenizer_append(t, &t->temporary, entity->second);
	tokenizer_flush_reference(t);
	t->state = t->return_state;

	/* The step is done. */
	return STEP_CONTINUE;
}

/* The numeric character reference end state: checks the value and hands it out. */
static void
tokenizer_numeric_end(
	struct html_tokenizer *t)
{
	uint32_t code;
	int noncharacter;
	int control;
	int space;

	/* Replaces the values that may not appear, reporting each. */
	code = t->reference_code;
	noncharacter = is_noncharacter(code);
	control = is_control(code);
	space = 0;
	if (code <= 0x7fU)
		space = (char_kind((int32_t)code) & KIND_SPACE) != 0;
	if (code == 0) {
		tokenizer_error(t, E_NULL_CHARACTER_REFERENCE);
		code = WB_REPLACEMENT;
	} else if (code > WB_CODE_POINT_MAX) {
		tokenizer_error(t, E_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE);
		code = WB_REPLACEMENT;
	} else if (code >= 0xd800U && code <= 0xdfffU) {
		tokenizer_error(t, E_SURROGATE_CHARACTER_REFERENCE);
		code = WB_REPLACEMENT;
	} else if (noncharacter) {
		tokenizer_error(t, E_NONCHARACTER_CHARACTER_REFERENCE);
	} else if (code == CHAR_CR ||
	    (control && !space)) {
		tokenizer_error(t, E_CONTROL_CHARACTER_REFERENCE);
		if (code >= 0x80U && code <= 0x9fU && tokenizer_c1_replacements[code - 0x80U] != 0)
			code = tokenizer_c1_replacements[code - 0x80U];
	}

	/* Hands out the one code point and returns to where the reference began. */
	wb_units_clear(&t->temporary);
	tokenizer_append(t, &t->temporary, code);
	tokenizer_flush_reference(t);
	t->state = t->return_state;
}

/*
 * Consumes the next code point of the input.
 *
 * Returns TOKENIZER_WAIT without consuming when the input has run out (or
 * ends in half a surrogate pair) before it was closed, and TOKENIZER_EOF
 * when it is closed and read to the end.
 */
static int32_t
tokenizer_consume(
	struct html_tokenizer *t)
{
	struct html_input *input;
	uint32_t code_point;
	size_t available;
	size_t width;

	/* Nothing left: end of file, or wait for more. */
	input = t->input;
	available = input->units.length - input->position;

	/* A raw script-written CR already emitted LF; swallow its following LF. */
	if (input->position == input->inserted_cr && available != 0) {
		input->inserted_cr = (size_t)-1;

		/* The LF completes the already emitted CR line break. */
		if (input->units.data[input->position] == 0x0aU) {
			input->position++;
			available--;
		}
	}

	/* The insertion boundary may leave no readable code point yet. */
	if (available == 0) {
		t->last_width = 0;
		if (input->closed)
			return TOKENIZER_EOF;
		return TOKENIZER_WAIT;
	}

	/* A high surrogate at the very end may still get its low half. */
	if (available == 1 && !input->closed) {
		if (input->units.data[input->position] >= 0xd800U && input->units.data[input->position] <= 0xdbffU)
			return TOKENIZER_WAIT;
	}

	/* Decodes the code point and moves past it, checking it the first time it is read. */
	width = wb_utf16_decode(input->units.data + input->position, available, &code_point);

	/* Network input is preprocessed; raw inserted text is folded when read. */
	if (code_point == 0x0dU) {
		code_point = 0x0aU;
		input->inserted_cr = input->position + width;
	}

	/* Checks and advances past the normalized code point. */
	tokenizer_check_input(t, code_point, input->position);
	input->position += width;
	t->last_width = width;

	/* Reports the code point. */
	return (int32_t)code_point;
}

/* Steps back over the code point just consumed and switches state, so the new state reads it again. */
static void
tokenizer_reconsume(
	struct html_tokenizer *t,
	int state)
{
	/* Moves back by the width of the last code point (nothing for end of file). */
	t->input->position -= t->last_width;
	t->last_width = 0;
	t->state = state;
}

/*
 * Reports the input stream errors for a code point read at position for
 * the first time: surrogates, noncharacters and controls.
 */
static void
tokenizer_check_input(
	struct html_tokenizer *t,
	uint32_t code_point,
	size_t position)
{
	int noncharacter;
	int control;
	int space;

	/* A position read before was checked then (reconsuming reads it again). */
	if (position < t->checked_position)
		return;
	t->checked_position = position + 1U;

	/* Checks the kinds the standard names. */
	noncharacter = is_noncharacter(code_point);
	control = is_control(code_point);
	space = 0;
	if (code_point <= 0x7fU)
		space = (char_kind((int32_t)code_point) & KIND_SPACE) != 0;
	if (code_point >= 0xd800U && code_point <= 0xdfffU) {
		tokenizer_error(t, E_SURROGATE_IN_INPUT_STREAM);
	} else if (noncharacter) {
		tokenizer_error(t, E_NONCHARACTER_IN_INPUT_STREAM);
	} else if (control &&
	    code_point != CHAR_NULL &&
	    !space) {
		tokenizer_error(t, E_CONTROL_CHARACTER_IN_INPUT_STREAM);
	}
}

/*
 * Tells whether the input continues with text (ASCII), ignoring ASCII case
 * when fold_case is set, without consuming anything.
 */
static int
tokenizer_lookahead(
	struct html_tokenizer *t,
	const char *text,
	int fold_case)
{
	struct html_input *input;
	uint16_t unit;
	size_t length;
	size_t index;

	/* Compares as many characters as are there. */
	input = t->input;
	length = strlen(text);
	for (index = 0; index < length; index++) {
		/* Input that ends early: a mismatch at end of file, else wait. */
		if (input->position + index >= input->units.length) {
			if (input->closed)
				return LOOKAHEAD_MISMATCH;
			return LOOKAHEAD_WAIT;
		}

		/* Folds the case of an ASCII letter when asked to. */
		unit = input->units.data[input->position + index];
		if (fold_case && unit >= 'a' && unit <= 'z')
			unit = (uint16_t)(unit - 0x20U);
		if (unit != (unsigned char)text[index])
			return LOOKAHEAD_MISMATCH;
	}

	/* Every character matched. */
	return LOOKAHEAD_MATCH;
}

/* Counts a parse error and remembers its kind while there is room. */
static void
tokenizer_error(
	struct html_tokenizer *t,
	int error)
{
	/* Keeps the kind of the first errors for tests and diagnostics. */
	if (t->error_count < HTML_ERRORS_KEPT)
		t->errors[t->error_count] = error;
	t->error_count++;
}

/* Adds one code point to the characters being gathered. */
static void
tokenizer_emit_char(
	struct html_tokenizer *t,
	uint32_t code_point)
{
	/* Appends the code point to the run. */
	tokenizer_append(t, &t->characters.data, code_point);
}

/* Adds the code units of a buffer to the characters being gathered. */
static void
tokenizer_emit_units(
	struct html_tokenizer *t,
	const struct wb_units *units)
{
	int error;

	/* Appends the units to the run; a failure stops the tokenizer. */
	error = wb_units_append(&t->characters.data, units->data, units->length);
	if (error != 0)
		t->failed = 1;
}

/* Marks the tag, comment or DOCTYPE complete, applying what emitting a tag does. */
static void
tokenizer_emit_current(
	struct html_tokenizer *t)
{
	int error;

	/* A start tag becomes the tag end tags are compared with. */
	if (t->current.type == HTML_TOKEN_START_TAG) {
		wb_units_clear(&t->last_start_tag);
		error = wb_units_append(&t->last_start_tag, t->current.name.data, t->current.name.length);
		if (error != 0)
			t->failed = 1;
	}

	/* An end tag may carry neither attributes nor a closing slash. */
	if (t->current.type == HTML_TOKEN_END_TAG) {
		if (t->current.attribute_count != 0)
			tokenizer_error(t, E_END_TAG_WITH_ATTRIBUTES);
		if (t->current.self_closing)
			tokenizer_error(t, E_END_TAG_WITH_TRAILING_SOLIDUS);
	}

	/* The token goes out after the characters gathered before it. */
	t->current_ready = 1;
}

/* Emits end of file: the current token becomes the end-of-file token once what is pending is out. */
static void
tokenizer_emit_eof(
	struct html_tokenizer *t)
{
	/*
	 * A comment or DOCTYPE emitted in the same step is handed out first; the
	 * end-of-file flag makes every later call return end of file.
	 */
	t->eof_emitted = 1;
}

/* Appends a code point to one of the token's buffers; a failure stops the tokenizer. */
static void
tokenizer_append(
	struct html_tokenizer *t,
	struct wb_units *units,
	uint32_t code_point)
{
	int error;

	/* Appends the code point as UTF-16. */
	error = wb_units_append_code_point(units, code_point);
	if (error != 0)
		t->failed = 1;
}

/* Starts a new tag, comment or DOCTYPE, reusing the current token's buffers. */
static void
tokenizer_begin(
	struct html_tokenizer *t,
	enum html_token_type type)
{
	/* Empties the token and gives it its type. */
	tokenizer_clear_token(&t->current);
	t->current.type = type;
}

/* Starts a new attribute on the current tag. */
static void
tokenizer_begin_attribute(
	struct html_tokenizer *t)
{
	struct html_token_attribute *attributes;
	struct html_token_attribute *attribute;
	size_t capacity;

	/* Grows the attribute array when it is full. */
	if (t->current.attribute_count == t->current.attribute_capacity) {
		capacity = t->current.attribute_capacity * 2U;
		if (capacity < 8U)
			capacity = 8U;

		/* Moves the attributes to larger storage and prepares the new places. */
		attributes = realloc(t->current.attributes, capacity * sizeof(*attributes));
		if (attributes == NULL) {
			t->failed = 1;
			return;
		}

		/* Every new place starts with empty buffers. */
		memset(attributes + t->current.attribute_capacity, 0,
		    (capacity - t->current.attribute_capacity) * sizeof(*attributes));
		t->current.attributes = attributes;
		t->current.attribute_capacity = capacity;
	}

	/* The next place becomes the attribute, empty. */
	attribute = &t->current.attributes[t->current.attribute_count];
	wb_units_clear(&attribute->name);
	wb_units_clear(&attribute->value);
	attribute->dropped = 0;
	t->current.attribute_count++;
}

/* Checks the attribute name just finished against the tag's earlier ones and drops a repeat. */
static void
tokenizer_finish_attribute_name(
	struct html_tokenizer *t)
{
	struct html_token_attribute *attribute;
	struct html_token_attribute *earlier;
	size_t index;
	int differs;

	/* Only a tag with an attribute being built has anything to check. */
	attribute = tokenizer_attribute(t);
	if (attribute == NULL)
		return;

	/* Compares with every earlier attribute that was kept. */
	for (index = 0; index + 1U < t->current.attribute_count; index++) {
		earlier = &t->current.attributes[index];

		/* Skips the dropped ones and those of another length. */
		if (earlier->dropped || earlier->name.length != attribute->name.length)
			continue;

		/* The same name is a duplicate, which the standard drops. */
		differs = memcmp(earlier->name.data, attribute->name.data, attribute->name.length * sizeof(uint16_t));
		if (differs == 0) {
			tokenizer_error(t, E_DUPLICATE_ATTRIBUTE);
			attribute->dropped = 1;
			return;
		}
	}
}

/* Finds the attribute being built, or NULL when the tag has none. */
static struct html_token_attribute *
tokenizer_attribute(
	struct html_tokenizer *t)
{
	/* A tag without attributes has none being built. */
	if (t->current.attribute_count == 0)
		return NULL;

	/* The attribute being built is the last one. */
	return &t->current.attributes[t->current.attribute_count - 1U];
}

/* Tells whether the end tag being read closes the last start tag. */
static int
tokenizer_appropriate_end_tag(
	const struct html_tokenizer *t)
{
	int differs;

	/* The names must have the same length. */
	if (t->last_start_tag.length == 0 || t->last_start_tag.length != t->current.name.length)
		return 0;

	/* And the same units. */
	differs = memcmp(t->last_start_tag.data, t->current.name.data, t->current.name.length * sizeof(uint16_t));
	if (differs != 0)
		return 0;

	/* The end tag closes the last start tag. */
	return 1;
}

/* Tells whether a return state is one of the attribute value states. */
static int
tokenizer_in_attribute(
	int state)
{
	/* The three attribute value states. */
	if (state == S_ATTRIBUTE_VALUE_DOUBLE)
		return 1;
	if (state == S_ATTRIBUTE_VALUE_SINGLE)
		return 1;
	if (state == S_ATTRIBUTE_VALUE_UNQUOTED)
		return 1;

	/* Every other state is text. */
	return 0;
}

/*
 * Flushes the code points consumed as a character reference: into the
 * attribute value when the reference is in one, as characters otherwise.
 */
static void
tokenizer_flush_reference(
	struct html_tokenizer *t)
{
	struct html_token_attribute *attribute;
	int in_attribute;
	int error;

	/* Text references become characters. */
	in_attribute = tokenizer_in_attribute(t->return_state);
	if (!in_attribute) {
		tokenizer_emit_units(t, &t->temporary);
		return;
	}

	/* Attribute references extend the value (of an attribute that exists). */
	attribute = tokenizer_attribute(t);
	if (attribute == NULL)
		return;
	error = wb_units_append(&attribute->value, t->temporary.data, t->temporary.length);
	if (error != 0)
		t->failed = 1;
}

/* Tells whether the temporary buffer holds exactly "script". */
static int
tokenizer_temporary_is_script(
	const struct html_tokenizer *t)
{
	static const uint16_t script[6] = { 's', 'c', 'r', 'i', 'p', 't' };
	int differs;

	/* The length must be that of "script". */
	if (t->temporary.length != 6)
		return 0;

	/* And the units the same. */
	differs = memcmp(t->temporary.data, script, sizeof(script));
	if (differs != 0)
		return 0;

	/* The buffer holds "script". */
	return 1;
}

/* Empties a token and keeps its buffers for the next one. */
static void
tokenizer_clear_token(
	struct html_token *token)
{
	/* Empties the buffers and resets the flags; the attribute places stay allocated. */
	wb_units_clear(&token->name);
	wb_units_clear(&token->data);
	wb_units_clear(&token->public_id);
	wb_units_clear(&token->system_id);
	token->has_name = 0;
	token->has_public_id = 0;
	token->has_system_id = 0;
	token->force_quirks = 0;
	token->self_closing = 0;
	token->attribute_count = 0;
}

/* Frees a token's buffers. */
static void
tokenizer_release_token(
	struct html_token *token)
{
	size_t index;

	/* Frees each attribute place's buffers and the array. */
	for (index = 0; index < token->attribute_capacity; index++) {
		wb_units_release(&token->attributes[index].name);
		wb_units_release(&token->attributes[index].value);
	}

	/* Frees the array of places. */
	free(token->attributes);

	/* Frees the token's own buffers. */
	wb_units_release(&token->name);
	wb_units_release(&token->data);
	wb_units_release(&token->public_id);
	wb_units_release(&token->system_id);
	memset(token, 0, sizeof(*token));
}

/* Classifies a code point: an ASCII letter (and its case), digit, hex digit, or whitespace. */
static unsigned
char_kind(
	int32_t c)
{
	/* Upper case letters; A to F are also hex digits. */
	if (c >= 'A' && c <= 'Z') {
		if (c <= 'F')
			return KIND_ALPHA | KIND_UPPER | KIND_ALNUM | KIND_HEX;
		return KIND_ALPHA | KIND_UPPER | KIND_ALNUM;
	}

	/* Lower case letters; a to f are also hex digits. */
	if (c >= 'a' && c <= 'z') {
		if (c <= 'f')
			return KIND_ALPHA | KIND_ALNUM | KIND_HEX;
		return KIND_ALPHA | KIND_ALNUM;
	}

	/* Digits. */
	if (c >= '0' && c <= '9')
		return KIND_DIGIT | KIND_HEX | KIND_ALNUM;

	/* Whitespace to the tokenizer: tab, line feed, form feed and space (carriage returns are gone). */
	if (c == CHAR_TAB || c == CHAR_LF)
		return KIND_SPACE;
	if (c == CHAR_FF || c == CHAR_SPACE)
		return KIND_SPACE;

	/* Anything else is none of these. */
	return 0;
}

/* Tells whether a code point is a Unicode noncharacter. */
static int
is_noncharacter(
	uint32_t c)
{
	/* FDD0 to FDEF, and the last two code points of every plane. */
	if (c >= 0xfdd0U && c <= 0xfdefU)
		return 1;
	if (c <= WB_CODE_POINT_MAX && (c & 0xfffeU) == 0xfffeU)
		return 1;

	/* Anything else is a character. */
	return 0;
}

/* Tells whether a code point is a control: C0 controls, DELETE and the C1 controls. */
static int
is_control(
	uint32_t c)
{
	/* The C0 controls. */
	if (c <= 0x1fU)
		return 1;

	/* DELETE and the C1 controls. */
	if (c >= 0x7fU && c <= 0x9fU)
		return 1;

	/* Anything else is not a control. */
	return 0;
}
