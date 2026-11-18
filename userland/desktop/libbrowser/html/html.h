/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * HTML parsing (plan/ws074/design.md §3): the input stream, the tokenizer
 * of the WHATWG HTML Standard and, later, the tree builder and the
 * serializer.
 *
 * The input is UTF-16 code units, already decoded from the document's
 * encoding, so script-inserted text (document.write) goes in as it is.
 * The tokenizer is pulled by its consumer one token at a time; when the
 * input runs out before the stream is closed it reports that it needs more
 * and resumes where it stopped.
 */

#ifndef KEILAND_BROWSER_HTML_H
#define KEILAND_BROWSER_HTML_H

#include "base/base.h"

/* How many parse errors a tokenizer remembers by kind (it counts them all). */
#define HTML_ERRORS_KEPT 64U

/*
 * The kinds of token the tokenizer hands out.
 *
 * HTML_TOKEN_NONE means the input ran out before the stream was closed:
 * append more and ask again.
 */
enum html_token_type {
	HTML_TOKEN_NONE,
	HTML_TOKEN_DOCTYPE,
	HTML_TOKEN_START_TAG,
	HTML_TOKEN_END_TAG,
	HTML_TOKEN_COMMENT,
	HTML_TOKEN_CHARACTERS,
	HTML_TOKEN_EOF
};

/*
 * The states the consumer of the tokenizer may switch it to (the tree
 * builder does after <title>, <script> and the like).
 */
enum html_tokenizer_start {
	HTML_TOKENIZE_DATA,
	HTML_TOKENIZE_RCDATA,
	HTML_TOKENIZE_RAWTEXT,
	HTML_TOKENIZE_SCRIPT_DATA,
	HTML_TOKENIZE_PLAINTEXT,
	HTML_TOKENIZE_CDATA_SECTION
};

/*
 * The text a parser reads, in UTF-16 code units.
 *
 * Network carriage returns are normalized as text is appended; raw inserted
 * text is normalized when the tokenizer reads it. CR LF and lone CR become LF.
 * position is how far the tokenizer has read; closed says the source has ended.
 * after_cr suppresses an LF following the last appended network CR.
 */
struct html_input {
	struct wb_units units;
	size_t position;
	int closed;
	int after_cr;
	/* The read position after a raw inserted CR, or SIZE_MAX when none waits. */
	size_t inserted_cr;
};

/*
 * One attribute of a tag token.
 *
 * dropped marks an attribute whose name repeated an earlier one's; the
 * standard drops it, and the consumer must skip it.
 */
struct html_token_attribute {
	struct wb_units name;
	struct wb_units value;
	int dropped;
};

/*
 * A token, owned by the tokenizer and good until the next call.
 *
 * Tags carry name (lowercase), attributes and self_closing; DOCTYPEs carry
 * name, the identifiers and force_quirks, each with a flag saying whether
 * it was present at all; comments and character runs carry data.
 */
struct html_token {
	enum html_token_type type;
	struct wb_units name;
	struct wb_units data;
	struct wb_units public_id;
	struct wb_units system_id;
	int has_name;
	int has_public_id;
	int has_system_id;
	int force_quirks;
	int self_closing;
	struct html_token_attribute *attributes;
	size_t attribute_count;
	size_t attribute_capacity;
};

/*
 * The tokenizer: its state machine, the token it is building and the run
 * of characters it has not handed out yet.
 */
struct html_tokenizer {
	/* The text read and where the state machine stands. */
	struct html_input *input;
	int state;
	int return_state;

	/* The tag, comment or DOCTYPE being built, and whether it is complete. */
	struct html_token current;
	int current_ready;

	/* The characters not yet handed out, as a token of their own. */
	struct html_token characters;

	/* The standard's temporary buffer and the numeric reference's value. */
	struct wb_units temporary;
	uint32_t reference_code;

	/* The last start tag's name, which an end tag in RCDATA and the like must match. */
	struct wb_units last_start_tag;

	/* Whether a CDATA section may open (the tree builder is in foreign content). */
	int allow_cdata;

	/* The size of the last code point read, for reconsuming it. */
	size_t last_width;

	/* Whether end of file was reached, and whether the characters were handed out (to be emptied next call). */
	int eof_emitted;
	int characters_out;

	/* The first input position whose code point has not been checked for input stream errors. */
	size_t checked_position;

	/* How many parse errors there were, and the kinds of the first ones. */
	size_t error_count;
	int errors[HTML_ERRORS_KEPT];

	/* Whether memory ran out; the tokenizer then only reports end of file. */
	int failed;
};

struct dom_document;
struct dom_element;
struct html_parser;
struct vm_heap;

/*
 * What the parser calls when a script element's end tag is parsed (the
 * standard's "prepare the script element" from the parser): the page runs
 * the script there, while the parser waits. Returns zero or ENOMEM; a fatal
 * allocation failure stops the parser instead of silently losing the write.
 */
typedef int (*html_script_hook)(void *context, struct dom_element *script);

/* The input stream (input.c). */
void html_input_init(struct html_input *input);
int html_input_append(struct html_input *input, const uint16_t *units, size_t length);
void html_input_close(struct html_input *input);
void html_input_release(struct html_input *input);

/* The tokenizer (tokenizer.c). */
void html_tokenizer_init(struct html_tokenizer *tokenizer, struct html_input *input);
void html_tokenizer_release(struct html_tokenizer *tokenizer);
void html_tokenizer_set_state(struct html_tokenizer *tokenizer, enum html_tokenizer_start state);
int html_tokenizer_set_last_start_tag(struct html_tokenizer *tokenizer, const uint16_t *name, size_t length);
enum html_token_type html_tokenizer_next(struct html_tokenizer *tokenizer, const struct html_token **token);
const char *html_error_name(int error);

/* The tree builder (parser.c). */
int html_parser_create(struct html_parser **parser, struct dom_document *document, int scripting);
int html_parser_create_fragment(struct html_parser **parser, struct dom_element *context, int scripting);
struct dom_element *html_parser_fragment_root(const struct html_parser *parser);
void html_parser_set_script_hook(struct html_parser *parser, html_script_hook hook, void *context);
/* Owned parsers are traced by their Window, rather than permanently rooting it. */
void html_parser_transfer_ownership(struct html_parser *parser);
void html_parser_trace_owned(struct vm_heap *heap, struct html_parser *parser);
void html_parser_destroy(struct html_parser *parser);
int html_parser_feed(struct html_parser *parser, const uint16_t *units, size_t length);
int html_parser_write(struct html_parser *parser, const uint16_t *units, size_t length);
int html_parser_finish(struct html_parser *parser);
size_t html_parser_errors(const struct html_parser *parser);

#endif
