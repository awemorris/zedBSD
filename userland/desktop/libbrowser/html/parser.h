/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of the HTML tree builder, shared by its files: parser.c (the
 * token loop, the stacks and the algorithms the modes share), modes.c (the
 * insertion modes but "in body"), body.c ("in body") and foreign.c
 * (foreign content).
 */

#ifndef KEILAND_BROWSER_HTML_PARSER_H
#define KEILAND_BROWSER_HTML_PARSER_H

#include "html/html.h"
#include "dom/dom.h"

/* What a run of characters is, for the modes' decisions. */
#define TB_TEXT_WHITESPACE 0
#define TB_TEXT_NULL 1
#define TB_TEXT_OTHER 2

/*
 * The insertion modes of the standard's tree construction.
 */
enum tb_mode {
	TB_INITIAL,
	TB_BEFORE_HTML,
	TB_BEFORE_HEAD,
	TB_IN_HEAD,
	TB_IN_HEAD_NOSCRIPT,
	TB_AFTER_HEAD,
	TB_IN_BODY,
	TB_TEXT,
	TB_IN_TABLE,
	TB_IN_TABLE_TEXT,
	TB_IN_CAPTION,
	TB_IN_COLUMN_GROUP,
	TB_IN_TABLE_BODY,
	TB_IN_ROW,
	TB_IN_CELL,
	TB_IN_SELECT,
	TB_IN_SELECT_IN_TABLE,
	TB_IN_TEMPLATE,
	TB_AFTER_BODY,
	TB_IN_FRAMESET,
	TB_AFTER_FRAMESET,
	TB_AFTER_AFTER_BODY,
	TB_AFTER_AFTER_FRAMESET
};

/*
 * The scopes "has an element in scope" can be asked in.
 */
enum tb_scope {
	TB_SCOPE_DEFAULT,
	TB_SCOPE_LIST_ITEM,
	TB_SCOPE_BUTTON,
	TB_SCOPE_TABLE,
	TB_SCOPE_SELECT
};

/*
 * A token as the tree builder handles it.
 *
 * Tags carry their tag number; a tag the parser makes itself (an implied
 * <tbody>, say) has no raw token and no attributes.  A character token is
 * one run of characters of one kind: all whitespace, all NULs or all
 * others, because the modes treat the three differently.
 */
struct tb_token {
	int type;
	int tag;
	const struct html_token *raw;
	const uint16_t *text;
	size_t length;
};

/*
 * One entry of the list of active formatting elements: an element, or a
 * marker (element NULL).
 */
struct tb_formatting {
	struct dom_element *element;
};

/*
 * The HTML parser: the tokenizer it drives and the tree construction's
 * state.
 *
 * The stacks hold cells in malloc'd memory, so the parser marks them from a
 * tracer registered on the heap for as long as it lives.
 */
struct html_parser {
	/* The document built, its heap and the tokenizer's input. */
	struct dom_document *document;
	struct vm_heap *heap;
	struct html_input input;
	struct html_tokenizer tokenizer;

	/* The insertion mode, the one to return to after text, and the template modes. */
	int mode;
	int original_mode;
	struct wb_vector template_modes;

	/* The stack of open elements (the current node last) and the active formatting elements. */
	struct wb_vector open;
	struct wb_vector formatting;

	/* The head and form element pointers. */
	struct dom_element *head;
	struct dom_element *form;

	/* The flags of the standard: scripting, frameset-ok, foster parenting. */
	int scripting;
	int frameset_ok;
	int foster_parenting;

	/* Whether a line feed at the start of the next characters is dropped (after <pre>, <textarea>). */
	int skip_newline;

	/* The pending table character tokens and whether any of them is not whitespace. */
	struct wb_units table_text;
	int table_text_other;

	/* The fragment case: the context element (NULL for a whole document) and the root html element the fragment is built in (ws074-p081). */
	struct dom_element *context;
	struct dom_element *fragment_root;

	/* What runs a script element when its end tag is parsed (NULL: nothing runs). */
	html_script_hook script_hook;
	void *script_context;
	/* The current script's input boundary, or SIZE_MAX outside a script hook. */
	size_t insertion_point;
	/* Reentrant writes on the C stack; the parser refuses excessive nesting. */
	unsigned write_depth;

	/* Whether parsing stopped, and how many parse errors the tree builder saw. */
	int stopped;
	size_t error_count;

	/* Whether memory ran out; the parse then stops. */
	int failed;
};

/* The token loop and shared algorithms (parser.c). */
void tb_process(struct html_parser *p, const struct tb_token *token);
void tb_run_script(struct html_parser *p, struct dom_element *script);
void tb_process_in_mode(struct html_parser *p, int mode, const struct tb_token *token);
void tb_error(struct html_parser *p);
struct dom_element *tb_current(const struct html_parser *p);
struct dom_element *tb_adjusted_current(const struct html_parser *p);
struct dom_element *tb_open_at(const struct html_parser *p, size_t index);
size_t tb_open_count(const struct html_parser *p);
void tb_push(struct html_parser *p, struct dom_element *element);
void tb_pop(struct html_parser *p);
void tb_pop_until_tag(struct html_parser *p, int tag);
void tb_pop_until_element(struct html_parser *p, struct dom_element *element);
void tb_remove_open(struct html_parser *p, struct dom_element *element);
int tb_open_index(const struct html_parser *p, const struct dom_element *element, size_t *index);
int tb_current_is(const struct html_parser *p, int tag);
int tb_in_scope(const struct html_parser *p, int tag, int scope);
int tb_element_in_scope(const struct html_parser *p, const struct dom_element *target, int scope);
int tb_template_on_stack(const struct html_parser *p);
int tb_template_contents(const struct html_parser *p);
int tb_is_special(const struct dom_element *element);
void tb_generate_implied_end_tags(struct html_parser *p, int except);
void tb_generate_all_implied_end_tags(struct html_parser *p);
void tb_close_p(struct html_parser *p);
struct dom_element *tb_create_element(struct html_parser *p, const struct tb_token *token, int ns);
struct dom_element *tb_create_element_for_tag(struct html_parser *p, int tag);
struct dom_element *tb_insert_element(struct html_parser *p, const struct tb_token *token, int ns);
struct dom_element *tb_insert_html_tag(struct html_parser *p, int tag);
void tb_insert_node(struct html_parser *p, struct dom_node *node, struct dom_element *override);
void tb_insert_characters(struct html_parser *p, const uint16_t *text, size_t length);
void tb_insert_comment(struct html_parser *p, const struct tb_token *token, struct dom_node *parent);
void tb_add_missing_attributes(struct html_parser *p, struct dom_element *element, const struct tb_token *token);
const struct html_token_attribute *tb_token_attribute(const struct tb_token *token, const char *name);
void tb_push_formatting(struct html_parser *p, struct dom_element *element);
void tb_push_marker(struct html_parser *p);
void tb_clear_formatting_to_marker(struct html_parser *p);
void tb_reconstruct_formatting(struct html_parser *p);
void tb_remove_formatting(struct html_parser *p, const struct dom_element *element);
int tb_formatting_index(const struct html_parser *p, const struct dom_element *element, size_t *index);
struct dom_element *tb_formatting_after_marker(const struct html_parser *p, int tag);
int tb_adoption_agency(struct html_parser *p, const struct tb_token *token);
void tb_reset_insertion_mode(struct html_parser *p);
void tb_push_template_mode(struct html_parser *p, int mode);
void tb_pop_template_mode(struct html_parser *p);
int tb_current_template_mode(const struct html_parser *p);
void tb_generic_text(struct html_parser *p, const struct tb_token *token, enum html_tokenizer_start state);
void tb_stop(struct html_parser *p);
int tb_units_equal_ascii(const uint16_t *units, size_t length, const char *ascii, int fold_case);

/* The insertion modes (modes.c, body.c). */
void tb_mode_initial(struct html_parser *p, const struct tb_token *token);
void tb_mode_before_html(struct html_parser *p, const struct tb_token *token);
void tb_mode_before_head(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_head(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_head_noscript(struct html_parser *p, const struct tb_token *token);
void tb_mode_after_head(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_body(struct html_parser *p, const struct tb_token *token);
void tb_mode_text(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_table(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_table_text(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_caption(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_column_group(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_table_body(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_row(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_cell(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_select(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_select_in_table(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_template(struct html_parser *p, const struct tb_token *token);
void tb_mode_after_body(struct html_parser *p, const struct tb_token *token);
void tb_mode_in_frameset(struct html_parser *p, const struct tb_token *token);
void tb_mode_after_frameset(struct html_parser *p, const struct tb_token *token);
void tb_mode_after_after_body(struct html_parser *p, const struct tb_token *token);
void tb_mode_after_after_frameset(struct html_parser *p, const struct tb_token *token);

/* Foreign content (foreign.c). */
int tb_use_foreign_rules(const struct html_parser *p, const struct tb_token *token);
void tb_foreign_content(struct html_parser *p, const struct tb_token *token);
void tb_insert_foreign(struct html_parser *p, const struct tb_token *token, int ns);
int tb_is_html_integration_point(const struct dom_element *element);
int tb_is_mathml_text_integration_point(const struct dom_element *element);

#endif
