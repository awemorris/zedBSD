/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The insertion modes of the tree construction, but "in body" (body.c):
 * each function is one mode's list of token cases in the standard's order.
 */

#include "html/parser.h"

#include <errno.h>
#include <string.h>

static void modes_doctype(struct html_parser *p, const struct tb_token *token);
static int modes_quirks(const struct html_token *doctype);
static int modes_limited_quirks(const struct html_token *doctype);
static int modes_prefix(const struct wb_units *units, const char *prefix);
static int modes_is_start(const struct tb_token *token, int tag);
static int modes_is_end(const struct tb_token *token, int tag);
static int modes_is_whitespace(const struct tb_token *token);
static void modes_clear_to_table_context(struct html_parser *p);
static void modes_clear_to_table_body_context(struct html_parser *p);
static void modes_clear_to_row_context(struct html_parser *p);
static void modes_close_cell(struct html_parser *p);
static void modes_flush_table_text(struct html_parser *p);
static void modes_close_section(struct html_parser *p, const struct tb_token *token);
static void modes_close_row(struct html_parser *p, const struct tb_token *token);
static int modes_current_tag(const struct html_parser *p);
static int modes_text_kind(uint16_t unit);
static void modes_declarative_shadow(struct html_parser *p, struct dom_element *template);

/*
 * The public identifiers whose prefixes put a document in quirks mode
 * (compared ignoring ASCII case).
 */
static const char *const modes_quirks_prefixes[] = {
	"+//silmaril//dtd html pro v0r11 19970101//",
	"-//as//dtd html 3.0 aswedit + extensions//",
	"-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
	"-//ietf//dtd html 2.0 level 1//",
	"-//ietf//dtd html 2.0 level 2//",
	"-//ietf//dtd html 2.0 strict level 1//",
	"-//ietf//dtd html 2.0 strict level 2//",
	"-//ietf//dtd html 2.0 strict//",
	"-//ietf//dtd html 2.0//",
	"-//ietf//dtd html 2.1e//",
	"-//ietf//dtd html 3.0//",
	"-//ietf//dtd html 3.2 final//",
	"-//ietf//dtd html 3.2//",
	"-//ietf//dtd html 3//",
	"-//ietf//dtd html level 0//",
	"-//ietf//dtd html level 1//",
	"-//ietf//dtd html level 2//",
	"-//ietf//dtd html level 3//",
	"-//ietf//dtd html strict level 0//",
	"-//ietf//dtd html strict level 1//",
	"-//ietf//dtd html strict level 2//",
	"-//ietf//dtd html strict level 3//",
	"-//ietf//dtd html strict//",
	"-//ietf//dtd html//",
	"-//metrius//dtd metrius presentational//",
	"-//microsoft//dtd internet explorer 2.0 html strict//",
	"-//microsoft//dtd internet explorer 2.0 html//",
	"-//microsoft//dtd internet explorer 2.0 tables//",
	"-//microsoft//dtd internet explorer 3.0 html strict//",
	"-//microsoft//dtd internet explorer 3.0 html//",
	"-//microsoft//dtd internet explorer 3.0 tables//",
	"-//netscape comm. corp.//dtd html//",
	"-//netscape comm. corp.//dtd strict html//",
	"-//o'reilly and associates//dtd html 2.0//",
	"-//o'reilly and associates//dtd html extended 1.0//",
	"-//o'reilly and associates//dtd html extended relaxed 1.0//",
	"-//sq//dtd html 2.0 hotmetal + extensions//",
	"-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
	"-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
	"-//spyglass//dtd html 2.0 extended//",
	"-//sun microsystems corp.//dtd hotjava html//",
	"-//sun microsystems corp.//dtd hotjava strict html//",
	"-//w3c//dtd html 3 1995-03-24//",
	"-//w3c//dtd html 3.2 draft//",
	"-//w3c//dtd html 3.2 final//",
	"-//w3c//dtd html 3.2//",
	"-//w3c//dtd html 3.2s draft//",
	"-//w3c//dtd html 4.0 frameset//",
	"-//w3c//dtd html 4.0 transitional//",
	"-//w3c//dtd html experimental 19960712//",
	"-//w3c//dtd html experimental 970421//",
	"-//w3c//dtd w3 html//",
	"-//w3o//dtd w3 html 3.0//",
	"-//webtechs//dtd mozilla html 2.0//",
	"-//webtechs//dtd mozilla html//",
	NULL
};

/*
 * The initial insertion mode: the DOCTYPE and the document's mode.
 */
void
tb_mode_initial(
	struct html_parser *p,
	const struct tb_token *token)
{
	int whitespace;

	/* Whitespace is ignored. */
	whitespace = modes_is_whitespace(token);
	if (whitespace)
		return;

	/* A comment goes to the document. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, &p->document->node);
		return;
	}

	/* A DOCTYPE is appended and sets the mode. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		modes_doctype(p, token);
		p->mode = TB_BEFORE_HTML;
		return;
	}

	/* Anything else: no DOCTYPE, so quirks mode. */
	tb_error(p);
	p->document->quirks = DOM_QUIRKS;
	p->mode = TB_BEFORE_HTML;
	tb_process(p, token);
}

/*
 * The "before html" insertion mode.
 */
void
tb_mode_before_html(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_start;
	struct dom_element *html;
	int whitespace;

	/* A DOCTYPE is an error; comments go to the document; whitespace is ignored. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* Comments go to the document. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, &p->document->node);
		return;
	}

	/* Whitespace is ignored. */
	whitespace = modes_is_whitespace(token);
	if (whitespace)
		return;

	/* An <html> start tag makes the root element. */
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (is_start) {
		html = tb_create_element(p, token, DOM_NS_HTML);
		if (html == NULL)
			return;
		dom_append_child(&p->document->node, &html->node);
		tb_push(p, html);
		p->mode = TB_BEFORE_HEAD;
		return;
	}

	/* Other end tags than head, body, html and br are ignored. */
	if (token->type == HTML_TOKEN_END_TAG &&
	    token->tag != DOM_TAG_HEAD &&
	    token->tag != DOM_TAG_BODY &&
	    token->tag != DOM_TAG_HTML &&
	    token->tag != DOM_TAG_BR) {
		tb_error(p);
		return;
	}

	/* Anything else makes an implied root element and is reprocessed. */
	html = tb_create_element_for_tag(p, DOM_TAG_HTML);
	if (html == NULL)
		return;
	dom_append_child(&p->document->node, &html->node);
	tb_push(p, html);
	p->mode = TB_BEFORE_HEAD;
	tb_process(p, token);
}

/*
 * The "before head" insertion mode.
 */
void
tb_mode_before_head(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_start;
	int whitespace;

	/* Whitespace is ignored; comments are inserted; a DOCTYPE is an error. */
	whitespace = modes_is_whitespace(token);
	if (whitespace)
		return;
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* <html> is handled as in body. */
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* <head> makes the head element. */
	is_start = modes_is_start(token, DOM_TAG_HEAD);
	if (is_start) {
		p->head = tb_insert_element(p, token, DOM_NS_HTML);
		p->mode = TB_IN_HEAD;
		return;
	}

	/* Other end tags than head, body, html and br are ignored. */
	if (token->type == HTML_TOKEN_END_TAG &&
	    token->tag != DOM_TAG_HEAD &&
	    token->tag != DOM_TAG_BODY &&
	    token->tag != DOM_TAG_HTML &&
	    token->tag != DOM_TAG_BR) {
		tb_error(p);
		return;
	}

	/* Anything else implies the head and is reprocessed. */
	p->head = tb_insert_html_tag(p, DOM_TAG_HEAD);
	p->mode = TB_IN_HEAD;
	tb_process(p, token);
}

/*
 * The "in head" insertion mode.
 */
void
tb_mode_in_head(
	struct html_parser *p,
	const struct tb_token *token)
{
	struct dom_element *template_element;
	int template_open;
	int whitespace;
	int is_template;

	/* Whitespace is inserted; comments are inserted; a DOCTYPE is an error. */
	whitespace = modes_is_whitespace(token);
	if (whitespace) {
		tb_insert_characters(p, token->text, token->length);
		return;
	}

	/* Comments are inserted. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* The start tags of the head. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_HTML:
			tb_process_in_mode(p, TB_IN_BODY, token);
			return;
		case DOM_TAG_BASE:
		case DOM_TAG_BASEFONT:
		case DOM_TAG_BGSOUND:
		case DOM_TAG_LINK:
		case DOM_TAG_META:
			tb_insert_element(p, token, DOM_NS_HTML);
			tb_pop(p);
			return;
		case DOM_TAG_TITLE:
			tb_generic_text(p, token, HTML_TOKENIZE_RCDATA);
			return;
		case DOM_TAG_NOSCRIPT:
			if (p->scripting) {
				tb_generic_text(p, token, HTML_TOKENIZE_RAWTEXT);
				return;
			}

			/* Then the template, with a marker and its own mode. */
			tb_insert_element(p, token, DOM_NS_HTML);
			p->mode = TB_IN_HEAD_NOSCRIPT;
			return;
		case DOM_TAG_NOFRAMES:
		case DOM_TAG_STYLE:
			tb_generic_text(p, token, HTML_TOKENIZE_RAWTEXT);
			return;
		case DOM_TAG_SCRIPT:
			tb_insert_element(p, token, DOM_NS_HTML);
			html_tokenizer_set_state(&p->tokenizer, HTML_TOKENIZE_SCRIPT_DATA);
			p->original_mode = p->mode;
			p->mode = TB_TEXT;
			return;
		case DOM_TAG_TEMPLATE:
			tb_insert_element(p, token, DOM_NS_HTML);
			tb_push_marker(p);
			p->frameset_ok = 0;
			p->mode = TB_IN_TEMPLATE;
			tb_push_template_mode(p, TB_IN_TEMPLATE);
			return;
		case DOM_TAG_HEAD:
			tb_error(p);
			return;
		default:
			break;
		}
	}

	/* The end tags of the head. */
	if (token->type == HTML_TOKEN_END_TAG) {
		switch (token->tag) {
		case DOM_TAG_HEAD:
			tb_pop(p);
			p->mode = TB_AFTER_HEAD;
			return;
		case DOM_TAG_BODY:
		case DOM_TAG_HTML:
		case DOM_TAG_BR:
			break;
		case DOM_TAG_TEMPLATE:
			template_open = tb_template_on_stack(p);
			if (!template_open) {
				tb_error(p);
				return;
			}

			/* Closes what is implied, then the element. */
			tb_generate_all_implied_end_tags(p);
			is_template = tb_current_is(p, DOM_TAG_TEMPLATE);
			if (!is_template)
				tb_error(p);
			template_element = is_template ? tb_current(p) : NULL;
			tb_pop_until_tag(p, DOM_TAG_TEMPLATE);
			tb_clear_formatting_to_marker(p);
			tb_pop_template_mode(p);
			tb_reset_insertion_mode(p);
			if (template_element != NULL)
				modes_declarative_shadow(p, template_element);
			return;
		default:
			tb_error(p);
			return;
		}
	}

	/* Anything else closes the head and is reprocessed. */
	tb_pop(p);
	p->mode = TB_AFTER_HEAD;
	tb_process(p, token);
}

/*
 * A closed template with a shadowrootmode of open or closed, directly in an
 * element that can host a shadow tree and has none, is that element's
 * shadow root: its content moves there and the template goes.
 * https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead
 */
static void
modes_declarative_shadow(
	struct html_parser *p,
	struct dom_element *template)
{
	struct dom_attribute *attribute;
	struct dom_shadow_root *root;
	struct dom_element *host;
	struct dom_node *child;
	struct vm_string *name;
	int closed;

	/* Only a whole document's parse makes declarative shadow roots, and only from a template with contents. */
	if (p->context != NULL || template->content == NULL || template->node.parent == NULL || template->node.parent->type != DOM_ELEMENT)
		return;
	name = vm_atom_from_ascii(p->heap, "shadowrootmode");
	if (name == NULL)
		return;
	attribute = dom_element_find_attribute(template, DOM_NS_NONE, name);
	if (attribute == NULL)
		return;
	if (vm_string_equal_ascii(attribute->value, "open"))
		closed = 0;
	else if (vm_string_equal_ascii(attribute->value, "closed"))
		closed = 1;
	else
		return;

	/* The host must be able to have a shadow tree. */
	host = (struct dom_element *)template->node.parent;
	if (!dom_can_attach_shadow(host))
		return;
	root = dom_shadow_attach(host, closed);
	if (root == NULL) {
		p->failed = 1;
		return;
	}

	/* The template's content is the shadow tree. */
	while (template->content->first_child != NULL) {
		child = template->content->first_child;
		dom_append_child(&root->node, child);
	}
	dom_remove(&template->node);
}

/*
 * The "in head noscript" insertion mode (scripting disabled).
 */
void
tb_mode_in_head_noscript(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_end;
	int is_start;
	int whitespace;

	/* A DOCTYPE is an error; <html> is handled as in body. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* A <html> start tag. */
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* </noscript> returns to the head. */
	is_end = modes_is_end(token, DOM_TAG_NOSCRIPT);
	if (is_end) {
		tb_pop(p);
		p->mode = TB_IN_HEAD;
		return;
	}

	/* Whitespace, comments and the head's own elements are handled as in head. */
	whitespace = modes_is_whitespace(token);
	if (whitespace || token->type == HTML_TOKEN_COMMENT) {
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	}

	/* The start tags of this mode. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_BASEFONT:
		case DOM_TAG_BGSOUND:
		case DOM_TAG_LINK:
		case DOM_TAG_META:
		case DOM_TAG_NOFRAMES:
		case DOM_TAG_STYLE:
			tb_process_in_mode(p, TB_IN_HEAD, token);
			return;
		case DOM_TAG_HEAD:
		case DOM_TAG_NOSCRIPT:
			tb_error(p);
			return;
		default:
			break;
		}
	}

	/* End tags but </br> are ignored. */
	if (token->type == HTML_TOKEN_END_TAG && token->tag != DOM_TAG_BR) {
		tb_error(p);
		return;
	}

	/* Anything else closes the noscript and is reprocessed in head. */
	tb_error(p);
	tb_pop(p);
	p->mode = TB_IN_HEAD;
	tb_process(p, token);
}

/*
 * The "after head" insertion mode.
 */
void
tb_mode_after_head(
	struct html_parser *p,
	const struct tb_token *token)
{
	int whitespace;

	/* Whitespace is inserted; comments are inserted; a DOCTYPE is an error. */
	whitespace = modes_is_whitespace(token);
	if (whitespace) {
		tb_insert_characters(p, token->text, token->length);
		return;
	}

	/* Comments are inserted. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* The start tags after the head. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_HTML:
			tb_process_in_mode(p, TB_IN_BODY, token);
			return;
		case DOM_TAG_BODY:
			tb_insert_element(p, token, DOM_NS_HTML);
			p->frameset_ok = 0;
			p->mode = TB_IN_BODY;
			return;
		case DOM_TAG_FRAMESET:
			tb_insert_element(p, token, DOM_NS_HTML);
			p->mode = TB_IN_FRAMESET;
			return;
		case DOM_TAG_BASE:
		case DOM_TAG_BASEFONT:
		case DOM_TAG_BGSOUND:
		case DOM_TAG_LINK:
		case DOM_TAG_META:
		case DOM_TAG_NOFRAMES:
		case DOM_TAG_SCRIPT:
		case DOM_TAG_STYLE:
		case DOM_TAG_TEMPLATE:
		case DOM_TAG_TITLE:
			/* The head's elements after the head go back into it. */
			tb_error(p);
			if (p->head == NULL)
				return;
			tb_push(p, p->head);
			tb_process_in_mode(p, TB_IN_HEAD, token);
			tb_remove_open(p, p->head);
			return;
		case DOM_TAG_HEAD:
			tb_error(p);
			return;
		default:
			break;
		}
	}

	/* The end tags after the head. */
	if (token->type == HTML_TOKEN_END_TAG) {
		switch (token->tag) {
		case DOM_TAG_TEMPLATE:
			tb_process_in_mode(p, TB_IN_HEAD, token);
			return;
		case DOM_TAG_BODY:
		case DOM_TAG_HTML:
		case DOM_TAG_BR:
			break;
		default:
			tb_error(p);
			return;
		}
	}

	/* Anything else implies the body and is reprocessed. */
	tb_insert_html_tag(p, DOM_TAG_BODY);
	p->mode = TB_IN_BODY;
	tb_process(p, token);
}

/*
 * The "text" insertion mode: the contents of script, style, title and the
 * like.
 */
void
tb_mode_text(
	struct html_parser *p,
	const struct tb_token *token)
{
	struct dom_element *element;
	int is_script;

	/* Characters are the element's text. */
	if (token->type == HTML_TOKEN_CHARACTERS) {
		tb_insert_characters(p, token->text, token->length);
		return;
	}

	/* End of file cuts the element short and is reprocessed. */
	if (token->type == HTML_TOKEN_EOF) {
		tb_error(p);
		tb_pop(p);
		p->mode = p->original_mode;
		tb_process(p, token);
		return;
	}

	/* An end tag (the tokenizer only lets the right one through) closes the element. */
	if (token->type != HTML_TOKEN_END_TAG)
		return;
	element = tb_current(p);
	tb_pop(p);
	p->mode = p->original_mode;

	/* A script element's end runs the script, while the parser waits. */
	if (element == NULL)
		return;
	is_script = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_SCRIPT);
	if (is_script)
		tb_run_script(p, element);
}

/*
 * The "in table" insertion mode.
 */
void
tb_mode_in_table(
	struct html_parser *p,
	const struct tb_token *token)
{
	const struct html_token_attribute *type;
	struct dom_element *current;
	int hidden;
	int in_scope;
	int template_open;

	/* Characters in a table element's place are gathered as table text. */
	current = tb_current(p);
	if (token->type == HTML_TOKEN_CHARACTERS && current != NULL && current->ns == DOM_NS_HTML) {
		switch (current->tag) {
		case DOM_TAG_TABLE:
		case DOM_TAG_TBODY:
		case DOM_TAG_TEMPLATE:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			wb_units_clear(&p->table_text);
			p->table_text_other = 0;
			p->original_mode = p->mode;
			p->mode = TB_IN_TABLE_TEXT;
			tb_process(p, token);
			return;
		default:
			break;
		}
	}

	/* Comments are inserted; a DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* The start tags of a table. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_CAPTION:
			modes_clear_to_table_context(p);
			tb_push_marker(p);
			tb_insert_element(p, token, DOM_NS_HTML);
			p->mode = TB_IN_CAPTION;
			return;
		case DOM_TAG_COLGROUP:
			modes_clear_to_table_context(p);
			tb_insert_element(p, token, DOM_NS_HTML);
			p->mode = TB_IN_COLUMN_GROUP;
			return;
		case DOM_TAG_COL:
			modes_clear_to_table_context(p);
			tb_insert_html_tag(p, DOM_TAG_COLGROUP);
			p->mode = TB_IN_COLUMN_GROUP;
			tb_process(p, token);
			return;
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
			modes_clear_to_table_context(p);
			tb_insert_element(p, token, DOM_NS_HTML);
			p->mode = TB_IN_TABLE_BODY;
			return;
		case DOM_TAG_TD:
		case DOM_TAG_TH:
		case DOM_TAG_TR:
			modes_clear_to_table_context(p);
			tb_insert_html_tag(p, DOM_TAG_TBODY);
			p->mode = TB_IN_TABLE_BODY;
			tb_process(p, token);
			return;
		case DOM_TAG_TABLE:
			tb_error(p);
			in_scope = tb_in_scope(p, DOM_TAG_TABLE, TB_SCOPE_TABLE);
			if (!in_scope)
				return;
			tb_pop_until_tag(p, DOM_TAG_TABLE);
			tb_reset_insertion_mode(p);
			tb_process(p, token);
			return;
		case DOM_TAG_STYLE:
		case DOM_TAG_SCRIPT:
		case DOM_TAG_TEMPLATE:
			tb_process_in_mode(p, TB_IN_HEAD, token);
			return;
		case DOM_TAG_INPUT:
			/* Only a hidden input stays in the table. */
			type = tb_token_attribute(token, "type");
			hidden = 0;
			if (type != NULL)
				hidden = tb_units_equal_ascii(type->value.data, type->value.length, "hidden", 1);
			if (!hidden)
				break;
			tb_error(p);
			tb_insert_element(p, token, DOM_NS_HTML);
			tb_pop(p);
			return;
		case DOM_TAG_FORM:
			tb_error(p);
			template_open = tb_template_on_stack(p);
			if (template_open || p->form != NULL)
				return;
			p->form = tb_insert_element(p, token, DOM_NS_HTML);
			tb_pop(p);
			return;
		default:
			break;
		}
	}

	/* The end tags of a table. */
	if (token->type == HTML_TOKEN_END_TAG) {
		switch (token->tag) {
		case DOM_TAG_TABLE:
			in_scope = tb_in_scope(p, DOM_TAG_TABLE, TB_SCOPE_TABLE);
			if (!in_scope) {
				tb_error(p);
				return;
			}

			/* Closes the table and reprocesses the tag. */
			tb_pop_until_tag(p, DOM_TAG_TABLE);
			tb_reset_insertion_mode(p);
			return;
		case DOM_TAG_BODY:
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_HTML:
		case DOM_TAG_TBODY:
		case DOM_TAG_TD:
		case DOM_TAG_TFOOT:
		case DOM_TAG_TH:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			tb_error(p);
			return;
		case DOM_TAG_TEMPLATE:
			tb_process_in_mode(p, TB_IN_HEAD, token);
			return;
		default:
			break;
		}
	}

	/* End of file is handled as in body. */
	if (token->type == HTML_TOKEN_EOF) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* Anything else goes in body, with foster parenting. */
	tb_error(p);
	p->foster_parenting = 1;
	tb_process_in_mode(p, TB_IN_BODY, token);
	p->foster_parenting = 0;
}

/*
 * The "in table text" insertion mode: characters inside a table, gathered
 * until the next other token.
 */
void
tb_mode_in_table_text(
	struct html_parser *p,
	const struct tb_token *token)
{
	int error;

	/* NULs are errors and dropped. */
	if (token->type == HTML_TOKEN_CHARACTERS && token->tag == TB_TEXT_NULL) {
		tb_error(p);
		return;
	}

	/* Other characters are gathered. */
	if (token->type == HTML_TOKEN_CHARACTERS) {
		if (token->tag == TB_TEXT_OTHER)
			p->table_text_other = 1;
		error = wb_units_append(&p->table_text, token->text, token->length);
		if (error != 0)
			p->failed = 1;
		return;
	}

	/* Anything else flushes the gathered text and is reprocessed. */
	modes_flush_table_text(p);
	p->mode = p->original_mode;
	tb_process(p, token);
}

/*
 * The "in caption" insertion mode.
 */
void
tb_mode_in_caption(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_end;
	int in_scope;
	int is_caption;
	int closes;

	/* </caption>, and the table tags that close the caption first. */
	closes = 0;
	if (token->type == HTML_TOKEN_END_TAG) {
		if (token->tag == DOM_TAG_CAPTION || token->tag == DOM_TAG_TABLE)
			closes = 1;
	}

	/* And the start tags of table parts. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_TBODY:
		case DOM_TAG_TD:
		case DOM_TAG_TFOOT:
		case DOM_TAG_TH:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			closes = 1;
			break;
		default:
			break;
		}
	}

	/* Closes the caption. */
	if (closes) {
		in_scope = tb_in_scope(p, DOM_TAG_CAPTION, TB_SCOPE_TABLE);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then the element. */
		tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
		is_caption = tb_current_is(p, DOM_TAG_CAPTION);
		if (!is_caption)
			tb_error(p);
		tb_pop_until_tag(p, DOM_TAG_CAPTION);
		tb_clear_formatting_to_marker(p);
		p->mode = TB_IN_TABLE;

		/* Only </caption> itself is done; the others are reprocessed in the table. */
		is_end = modes_is_end(token, DOM_TAG_CAPTION);
		if (!is_end)
			tb_process(p, token);
		return;
	}

	/* End tags that belong elsewhere are ignored. */
	if (token->type == HTML_TOKEN_END_TAG) {
		switch (token->tag) {
		case DOM_TAG_BODY:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_HTML:
		case DOM_TAG_TBODY:
		case DOM_TAG_TD:
		case DOM_TAG_TFOOT:
		case DOM_TAG_TH:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			tb_error(p);
			return;
		default:
			break;
		}
	}

	/* Anything else is handled as in body. */
	tb_process_in_mode(p, TB_IN_BODY, token);
}

/*
 * The "in column group" insertion mode.
 */
void
tb_mode_in_column_group(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_end;
	int is_start;
	int whitespace;
	int is_colgroup;

	/* Whitespace is inserted; comments are inserted; a DOCTYPE is an error. */
	whitespace = modes_is_whitespace(token);
	if (whitespace) {
		tb_insert_characters(p, token->text, token->length);
		return;
	}

	/* Comments are inserted. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* The tags of a column group. */
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* A <col> start tag. */
	is_start = modes_is_start(token, DOM_TAG_COL);
	if (is_start) {
		tb_insert_element(p, token, DOM_NS_HTML);
		tb_pop(p);
		return;
	}

	/* A </colgroup> end tag. */
	is_end = modes_is_end(token, DOM_TAG_COLGROUP);
	if (is_end) {
		is_colgroup = tb_current_is(p, DOM_TAG_COLGROUP);
		if (!is_colgroup) {
			tb_error(p);
			return;
		}

		/* Closes the current node. */
		tb_pop(p);
		p->mode = TB_IN_TABLE;
		return;
	}

	/* A </col> end tag. */
	is_end = modes_is_end(token, DOM_TAG_COL);
	if (is_end) {
		tb_error(p);
		return;
	}

	/* Template tags are handled as in head. */
	if ((token->type == HTML_TOKEN_START_TAG || token->type == HTML_TOKEN_END_TAG) &&
	    token->tag == DOM_TAG_TEMPLATE) {
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	}

	/* End of file. */
	if (token->type == HTML_TOKEN_EOF) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* Anything else closes the column group and is reprocessed. */
	is_colgroup = tb_current_is(p, DOM_TAG_COLGROUP);
	if (!is_colgroup) {
		tb_error(p);
		return;
	}

	/* Closes the current node. */
	tb_pop(p);
	p->mode = TB_IN_TABLE;
	tb_process(p, token);
}

/*
 * The "in table body" insertion mode.
 */
void
tb_mode_in_table_body(
	struct html_parser *p,
	const struct tb_token *token)
{
	int in_scope;

	/* The start tags of a table body. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_TR:
			modes_clear_to_table_body_context(p);
			tb_insert_element(p, token, DOM_NS_HTML);
			p->mode = TB_IN_ROW;
			return;
		case DOM_TAG_TH:
		case DOM_TAG_TD:
			tb_error(p);
			modes_clear_to_table_body_context(p);
			tb_insert_html_tag(p, DOM_TAG_TR);
			p->mode = TB_IN_ROW;
			tb_process(p, token);
			return;
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
			modes_close_section(p, token);
			return;
		default:
			break;
		}
	}

	/* The end tags of a table body. */
	if (token->type == HTML_TOKEN_END_TAG) {
		switch (token->tag) {
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
			in_scope = tb_in_scope(p, token->tag, TB_SCOPE_TABLE);
			if (!in_scope) {
				tb_error(p);
				return;
			}

			/* Closes the table section. */
			modes_clear_to_table_body_context(p);
			tb_pop(p);
			p->mode = TB_IN_TABLE;
			return;
		case DOM_TAG_TABLE:
			modes_close_section(p, token);
			return;
		case DOM_TAG_BODY:
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_HTML:
		case DOM_TAG_TD:
		case DOM_TAG_TH:
		case DOM_TAG_TR:
			tb_error(p);
			return;
		default:
			break;
		}
	}

	/* Anything else is handled as in table. */
	tb_process_in_mode(p, TB_IN_TABLE, token);
}

/*
 * The "in row" insertion mode.
 */
void
tb_mode_in_row(
	struct html_parser *p,
	const struct tb_token *token)
{
	int in_scope;

	/* The start tags of a row. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_TH:
		case DOM_TAG_TD:
			modes_clear_to_row_context(p);
			tb_insert_element(p, token, DOM_NS_HTML);
			p->mode = TB_IN_CELL;
			tb_push_marker(p);
			return;
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			modes_close_row(p, token);
			return;
		default:
			break;
		}
	}

	/* The end tags of a row. */
	if (token->type == HTML_TOKEN_END_TAG) {
		switch (token->tag) {
		case DOM_TAG_TR:
			in_scope = tb_in_scope(p, DOM_TAG_TR, TB_SCOPE_TABLE);
			if (!in_scope) {
				tb_error(p);
				return;
			}

			/* Closes the row. */
			modes_clear_to_row_context(p);
			tb_pop(p);
			p->mode = TB_IN_TABLE_BODY;
			return;
		case DOM_TAG_TABLE:
			modes_close_row(p, token);
			return;
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
			in_scope = tb_in_scope(p, token->tag, TB_SCOPE_TABLE);
			if (!in_scope) {
				tb_error(p);
				return;
			}

			/* Closes the row. */
			in_scope = tb_in_scope(p, DOM_TAG_TR, TB_SCOPE_TABLE);
			if (!in_scope)
				return;
			modes_clear_to_row_context(p);
			tb_pop(p);
			p->mode = TB_IN_TABLE_BODY;
			tb_process(p, token);
			return;
		case DOM_TAG_BODY:
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_HTML:
		case DOM_TAG_TD:
		case DOM_TAG_TH:
			tb_error(p);
			return;
		default:
			break;
		}
	}

	/* Anything else is handled as in table. */
	tb_process_in_mode(p, TB_IN_TABLE, token);
}

/*
 * The "in cell" insertion mode.
 */
void
tb_mode_in_cell(
	struct html_parser *p,
	const struct tb_token *token)
{
	int in_scope;
	int same;
	int td_open;
	int th_open;

	/* </td> and </th> close their cell. */
	if (token->type == HTML_TOKEN_END_TAG &&
	    (token->tag == DOM_TAG_TD ||
	     token->tag == DOM_TAG_TH)) {
		in_scope = tb_in_scope(p, token->tag, TB_SCOPE_TABLE);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then the element. */
		tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
		same = tb_current_is(p, token->tag);
		if (!same)
			tb_error(p);
		tb_pop_until_tag(p, token->tag);
		tb_clear_formatting_to_marker(p);
		p->mode = TB_IN_ROW;
		return;
	}

	/* Table start tags close the cell and are reprocessed. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_TBODY:
		case DOM_TAG_TD:
		case DOM_TAG_TFOOT:
		case DOM_TAG_TH:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			td_open = tb_in_scope(p, DOM_TAG_TD, TB_SCOPE_TABLE);
			th_open = tb_in_scope(p, DOM_TAG_TH, TB_SCOPE_TABLE);
			if (!td_open && !th_open) {
				tb_error(p);
				return;
			}

			/* Closes the cell and reprocesses the tag in the row. */
			modes_close_cell(p);
			tb_process(p, token);
			return;
		default:
			break;
		}
	}

	/* End tags that belong elsewhere are ignored, or close the cell. */
	if (token->type == HTML_TOKEN_END_TAG) {
		switch (token->tag) {
		case DOM_TAG_BODY:
		case DOM_TAG_CAPTION:
		case DOM_TAG_COL:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_HTML:
			tb_error(p);
			return;
		case DOM_TAG_TABLE:
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			in_scope = tb_in_scope(p, token->tag, TB_SCOPE_TABLE);
			if (!in_scope) {
				tb_error(p);
				return;
			}

			/* Closes the cell and reprocesses the tag in the row. */
			modes_close_cell(p);
			tb_process(p, token);
			return;
		default:
			break;
		}
	}

	/* Anything else is handled as in body. */
	tb_process_in_mode(p, TB_IN_BODY, token);
}

/*
 * The "in select" insertion mode (kept for documents parsed by older
 * rules; the current standard parses select content in body).
 */
void
tb_mode_in_select(
	struct html_parser *p,
	const struct tb_token *token)
{
	/* The standard no longer enters this mode; everything is handled in body. */
	tb_process_in_mode(p, TB_IN_BODY, token);
}

/*
 * The "in select in table" insertion mode (likewise retired).
 */
void
tb_mode_in_select_in_table(
	struct html_parser *p,
	const struct tb_token *token)
{
	/* Everything is handled in body. */
	tb_process_in_mode(p, TB_IN_BODY, token);
}

/*
 * The "in template" insertion mode.
 */
void
tb_mode_in_template(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_end;
	int template_open;
	int mode;

	/* Characters, comments and DOCTYPEs are handled as in body. */
	if (token->type == HTML_TOKEN_CHARACTERS ||
	    token->type == HTML_TOKEN_COMMENT ||
	    token->type == HTML_TOKEN_DOCTYPE) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* The head's elements and </template> are handled as in head. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_BASE:
		case DOM_TAG_BASEFONT:
		case DOM_TAG_BGSOUND:
		case DOM_TAG_LINK:
		case DOM_TAG_META:
		case DOM_TAG_NOFRAMES:
		case DOM_TAG_SCRIPT:
		case DOM_TAG_STYLE:
		case DOM_TAG_TEMPLATE:
		case DOM_TAG_TITLE:
			tb_process_in_mode(p, TB_IN_HEAD, token);
			return;
		default:
			break;
		}
	}

	/* A </template> end tag. */
	is_end = modes_is_end(token, DOM_TAG_TEMPLATE);
	if (is_end) {
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	}

	/* A start tag picks the template's content mode and is reprocessed in it. */
	if (token->type == HTML_TOKEN_START_TAG) {
		switch (token->tag) {
		case DOM_TAG_CAPTION:
		case DOM_TAG_COLGROUP:
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
			mode = TB_IN_TABLE;
			break;
		case DOM_TAG_COL:
			mode = TB_IN_COLUMN_GROUP;
			break;
		case DOM_TAG_TR:
			mode = TB_IN_TABLE_BODY;
			break;
		case DOM_TAG_TD:
		case DOM_TAG_TH:
			mode = TB_IN_ROW;
			break;
		default:
			mode = TB_IN_BODY;
			break;
		}

		/* Switches to the mode the tag belongs in. */
		tb_pop_template_mode(p);
		tb_push_template_mode(p, mode);
		p->mode = mode;
		tb_process(p, token);
		return;
	}

	/* Other end tags are ignored. */
	if (token->type == HTML_TOKEN_END_TAG) {
		tb_error(p);
		return;
	}

	/* End of file closes the templates one at a time, or stops. */
	if (token->type == HTML_TOKEN_EOF) {
		template_open = tb_template_on_stack(p);
		if (!template_open) {
			tb_stop(p);
			return;
		}

		/* Closes the innermost template and reprocesses end of file. */
		tb_error(p);
		tb_pop_until_tag(p, DOM_TAG_TEMPLATE);
		tb_clear_formatting_to_marker(p);
		tb_pop_template_mode(p);
		tb_reset_insertion_mode(p);
		tb_process(p, token);
	}
}

/*
 * The "after body" insertion mode.
 */
void
tb_mode_after_body(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_end;
	int is_start;
	int whitespace;

	/* Whitespace is handled as in body; comments go to the html element. */
	whitespace = modes_is_whitespace(token);
	if (whitespace) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* Comments are inserted. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, &tb_open_at(p, 0)->node);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* A <html> start tag. */
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* </html> ends the body for good (not in the fragment case). */
	is_end = modes_is_end(token, DOM_TAG_HTML);
	if (is_end) {
		if (p->context != NULL) {
			tb_error(p);
			return;
		}

		/* The body ends for good. */
		p->mode = TB_AFTER_AFTER_BODY;
		return;
	}

	/* End of file. */
	if (token->type == HTML_TOKEN_EOF) {
		tb_stop(p);
		return;
	}

	/* Anything else goes back into the body. */
	tb_error(p);
	p->mode = TB_IN_BODY;
	tb_process(p, token);
}

/*
 * The "in frameset" insertion mode.
 */
void
tb_mode_in_frameset(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_end;
	int is_start;
	int whitespace;
	int is_html;
	int is_frameset;

	/* Whitespace is inserted; comments are inserted; a DOCTYPE is an error. */
	whitespace = modes_is_whitespace(token);
	if (whitespace) {
		tb_insert_characters(p, token->text, token->length);
		return;
	}

	/* Comments are inserted. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* The tags of a frameset. */
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* A <frameset> start tag. */
	is_start = modes_is_start(token, DOM_TAG_FRAMESET);
	if (is_start) {
		tb_insert_element(p, token, DOM_NS_HTML);
		return;
	}

	/* A </frameset> end tag. */
	is_end = modes_is_end(token, DOM_TAG_FRAMESET);
	if (is_end) {
		is_html = tb_current_is(p, DOM_TAG_HTML);
		if (is_html) {
			tb_error(p);
			return;
		}

		/* Closes the current node. */
		tb_pop(p);
		is_frameset = tb_current_is(p, DOM_TAG_FRAMESET);
		if (p->context == NULL && !is_frameset)
			p->mode = TB_AFTER_FRAMESET;
		return;
	}

	/* A <frame> start tag. */
	is_start = modes_is_start(token, DOM_TAG_FRAME);
	if (is_start) {
		tb_insert_element(p, token, DOM_NS_HTML);
		tb_pop(p);
		return;
	}

	/* A <noframes> start tag. */
	is_start = modes_is_start(token, DOM_TAG_NOFRAMES);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	}

	/* End of file. */
	if (token->type == HTML_TOKEN_EOF) {
		is_html = tb_current_is(p, DOM_TAG_HTML);
		if (!is_html)
			tb_error(p);
		tb_stop(p);
		return;
	}

	/* Anything else is ignored (non-whitespace text included). */
	tb_error(p);
}

/*
 * The "after frameset" insertion mode.
 */
void
tb_mode_after_frameset(
	struct html_parser *p,
	const struct tb_token *token)
{
	int is_end;
	int is_start;
	int whitespace;

	/* Whitespace is inserted; comments are inserted; a DOCTYPE is an error. */
	whitespace = modes_is_whitespace(token);
	if (whitespace) {
		tb_insert_characters(p, token->text, token->length);
		return;
	}

	/* Comments are inserted. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* The tags after a frameset. */
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* A </html> end tag. */
	is_end = modes_is_end(token, DOM_TAG_HTML);
	if (is_end) {
		p->mode = TB_AFTER_AFTER_FRAMESET;
		return;
	}

	/* A <noframes> start tag. */
	is_start = modes_is_start(token, DOM_TAG_NOFRAMES);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	}

	/* End of file. */
	if (token->type == HTML_TOKEN_EOF) {
		tb_stop(p);
		return;
	}

	/* Anything else is ignored. */
	tb_error(p);
}

/*
 * The "after after body" insertion mode.
 */
void
tb_mode_after_after_body(
	struct html_parser *p,
	const struct tb_token *token)
{
	int whitespace;
	int is_start;

	/* Comments go to the document. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, &p->document->node);
		return;
	}

	/* A DOCTYPE, whitespace and <html> are handled as in body. */
	whitespace = modes_is_whitespace(token);
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (token->type == HTML_TOKEN_DOCTYPE || whitespace || is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* End of file stops the parse. */
	if (token->type == HTML_TOKEN_EOF) {
		tb_stop(p);
		return;
	}

	/* Anything else goes back into the body. */
	tb_error(p);
	p->mode = TB_IN_BODY;
	tb_process(p, token);
}

/*
 * The "after after frameset" insertion mode.
 */
void
tb_mode_after_after_frameset(
	struct html_parser *p,
	const struct tb_token *token)
{
	int whitespace;
	int is_start;

	/* Comments go to the document. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, &p->document->node);
		return;
	}

	/* A DOCTYPE, whitespace and <html> are handled as in body. */
	whitespace = modes_is_whitespace(token);
	is_start = modes_is_start(token, DOM_TAG_HTML);
	if (token->type == HTML_TOKEN_DOCTYPE || whitespace || is_start) {
		tb_process_in_mode(p, TB_IN_BODY, token);
		return;
	}

	/* End of file stops the parse. */
	if (token->type == HTML_TOKEN_EOF) {
		tb_stop(p);
		return;
	}

	/* A <noframes> start tag. */
	is_start = modes_is_start(token, DOM_TAG_NOFRAMES);
	if (is_start) {
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	}

	/* Anything else is ignored. */
	tb_error(p);
}

/* Appends a DOCTYPE node for the token and sets the document's mode from it. */
static void
modes_doctype(
	struct html_parser *p,
	const struct tb_token *token)
{
	const struct html_token *raw;
	struct vm_string *name;
	struct vm_string *public_id;
	struct vm_string *system_id;
	struct dom_node *doctype;
	struct vm_cell *roots[3];
	unsigned index;
	unsigned registered;
	int html_name;
	int legacy;
	int quirks;
	int limited;
	int error;

	/* A DOCTYPE other than <!DOCTYPE html> (and a few legacy ones) is an error. */
	raw = token->raw;
	html_name = tb_units_equal_ascii(raw->name.data, raw->name.length, "html", 0);
	legacy = tb_units_equal_ascii(raw->system_id.data, raw->system_id.length, "about:legacy-compat", 0);
	if (!raw->has_name ||
	    !html_name ||
	    raw->has_public_id ||
	    (raw->has_system_id && !legacy))
		tb_error(p);

	/* Each uninterned identifier survives allocation of its successors. */
	roots[0] = NULL;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	error = 0;
	for (index = 0; index < 3U; index++) {
		error = vm_heap_add_root(p->heap, &roots[index]);
		if (error != 0)
			goto cleanup;
		registered++;
	}

	/* Makes the node with the name and identifiers (empty when missing). */
	name = vm_string_from_units(p->heap, raw->name.data, raw->name.length);
	if (name == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The name remains live during later identifier construction. */
	roots[0] = &name->cell;

	/* The public identifier stays live while creating the system identifier. */
	public_id = vm_string_from_units(p->heap, raw->public_id.data, raw->public_id.length);
	if (public_id == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The second identifier joins the registered temporary ownership. */
	roots[1] = &public_id->cell;

	/* All three identifiers remain live until the Document owns their node. */
	system_id = vm_string_from_units(p->heap, raw->system_id.data, raw->system_id.length);
	if (system_id == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The final identifier remains live through native node creation. */
	roots[2] = &system_id->cell;

	/* Makes the node. */
	doctype = dom_doctype_create(p->document, name, public_id, system_id);
	if (doctype == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Appends it to the document. */
	dom_append_child(&p->document->node, doctype);

cleanup:
	/* A connected node now traces the identifiers; failures leave no node. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(p->heap, &roots[registered]);
	}

	/* A failed allocation stops this parse without exposing an incomplete node. */
	if (error != 0) {
		p->failed = 1;
		return;
	}

	/* Picks the document's mode. */
	quirks = modes_quirks(raw);
	limited = modes_limited_quirks(raw);
	if (quirks) {
		p->document->quirks = DOM_QUIRKS;
	} else if (limited) {
		p->document->quirks = DOM_LIMITED_QUIRKS;
	}
}

/* Tells whether a DOCTYPE token puts the document in quirks mode. */
static int
modes_quirks(
	const struct html_token *doctype)
{
	int prefixed;
	int same;
	int index;
	int match;

	/* The flag, a name other than html, and the three whole identifiers. */
	if (doctype->force_quirks)
		return 1;
	match = tb_units_equal_ascii(doctype->name.data, doctype->name.length, "html", 0);
	if (!doctype->has_name || !match)
		return 1;
	if (doctype->has_public_id) {
		same = tb_units_equal_ascii(doctype->public_id.data, doctype->public_id.length, "-//w3o//dtd w3 html strict 3.0//en//", 1);
		if (same)
			return 1;
		same = tb_units_equal_ascii(doctype->public_id.data, doctype->public_id.length, "-/w3c/dtd html 4.0 transitional/en", 1);
		if (same)
			return 1;
		same = tb_units_equal_ascii(doctype->public_id.data, doctype->public_id.length, "html", 1);
		if (same)
			return 1;
	}

	/* A system identifier. */
	if (doctype->has_system_id) {
		match = tb_units_equal_ascii(doctype->system_id.data, doctype->system_id.length,
		    "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd", 1);
		if (match)
			return 1;
	}

	/* The public identifier prefixes. */
	if (!doctype->has_public_id)
		return 0;
	for (index = 0; modes_quirks_prefixes[index] != NULL; index++) {
		match = modes_prefix(&doctype->public_id, modes_quirks_prefixes[index]);
		if (match)
			return 1;
	}

	/* HTML 4.01 frameset and transitional without a system identifier. */
	if (!doctype->has_system_id) {
		prefixed = modes_prefix(&doctype->public_id, "-//w3c//dtd html 4.01 frameset//");
		if (prefixed)
			return 1;
		prefixed = modes_prefix(&doctype->public_id, "-//w3c//dtd html 4.01 transitional//");
		if (prefixed)
			return 1;
	}

	/* The document is not in quirks mode. */
	return 0;
}

/* Tells whether a DOCTYPE token puts the document in limited-quirks mode. */
static int
modes_limited_quirks(
	const struct html_token *doctype)
{
	int prefixed;

	/* Only a public identifier can. */
	if (!doctype->has_public_id)
		return 0;

	/* XHTML 1.0 frameset and transitional. */
	prefixed = modes_prefix(&doctype->public_id, "-//w3c//dtd xhtml 1.0 frameset//");
	if (prefixed)
		return 1;
	prefixed = modes_prefix(&doctype->public_id, "-//w3c//dtd xhtml 1.0 transitional//");
	if (prefixed)
		return 1;

	/* HTML 4.01 frameset and transitional with a system identifier. */
	if (doctype->has_system_id) {
		prefixed = modes_prefix(&doctype->public_id, "-//w3c//dtd html 4.01 frameset//");
		if (prefixed)
			return 1;
		prefixed = modes_prefix(&doctype->public_id, "-//w3c//dtd html 4.01 transitional//");
		if (prefixed)
			return 1;
	}

	/* The document is not in limited-quirks mode. */
	return 0;
}

/* Tells whether units start with a lower case ASCII prefix, ignoring ASCII case. */
static int
modes_prefix(
	const struct wb_units *units,
	const char *prefix)
{
	size_t length;
	int same;

	/* The units must be at least as long, and their start equal. */
	length = strlen(prefix);
	if (units->length < length)
		return 0;
	same = tb_units_equal_ascii(units->data, length, prefix, 1);

	/* Reports the answer. */
	return same;
}

/* Tells whether a token is a start tag with a tag. */
static int
modes_is_start(
	const struct tb_token *token,
	int tag)
{
	/* The type and the tag must both match. */
	if (token->type == HTML_TOKEN_START_TAG && token->tag == tag)
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether a token is an end tag with a tag. */
static int
modes_is_end(
	const struct tb_token *token,
	int tag)
{
	/* The type and the tag must both match. */
	if (token->type == HTML_TOKEN_END_TAG && token->tag == tag)
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether a token is a run of whitespace characters. */
static int
modes_is_whitespace(
	const struct tb_token *token)
{
	/* Character runs carry their kind in tag. */
	if (token->type == HTML_TOKEN_CHARACTERS && token->tag == TB_TEXT_WHITESPACE)
		return 1;

	/* Anything else is not whitespace. */
	return 0;
}

/* Closes the open table section for a tag that needs the table body closed first, and reprocesses it. */
static void
modes_close_section(
	struct html_parser *p,
	const struct tb_token *token)
{
	int tbody_open;
	int thead_open;
	int tfoot_open;

	/* A table section must be open to be closed by these. */
	tbody_open = tb_in_scope(p, DOM_TAG_TBODY, TB_SCOPE_TABLE);
	thead_open = tb_in_scope(p, DOM_TAG_THEAD, TB_SCOPE_TABLE);
	tfoot_open = tb_in_scope(p, DOM_TAG_TFOOT, TB_SCOPE_TABLE);
	if (!tbody_open && !thead_open && !tfoot_open) {
		tb_error(p);
		return;
	}

	/* Closes the section and hands the tag to the table. */
	modes_clear_to_table_body_context(p);
	tb_pop(p);
	p->mode = TB_IN_TABLE;
	tb_process(p, token);
}

/* Closes the open row for a tag that needs it closed first, and reprocesses it. */
static void
modes_close_row(
	struct html_parser *p,
	const struct tb_token *token)
{
	int in_scope;

	/* A row must be open to be closed by these. */
	in_scope = tb_in_scope(p, DOM_TAG_TR, TB_SCOPE_TABLE);
	if (!in_scope) {
		tb_error(p);
		return;
	}

	/* Closes the row and hands the tag to the table body. */
	modes_clear_to_row_context(p);
	tb_pop(p);
	p->mode = TB_IN_TABLE_BODY;
	tb_process(p, token);
}

/* Reports the tag of the current node when it is an HTML element, DOM_TAG_UNKNOWN otherwise. */
static int
modes_current_tag(
	const struct html_parser *p)
{
	struct dom_element *current;

	/* Only an HTML current node has a tag that counts here. */
	current = tb_current(p);
	if (current == NULL || current->ns != DOM_NS_HTML)
		return DOM_TAG_UNKNOWN;

	/* Reports its tag. */
	return current->tag;
}

/* Pops until the current node is a table, template or html element. */
static void
modes_clear_to_table_context(
	struct html_parser *p)
{
	int tag;

	/* Pops the rest. */
	while (p->open.count > 0) {
		tag = modes_current_tag(p);
		if (tag == DOM_TAG_TABLE || tag == DOM_TAG_TEMPLATE || tag == DOM_TAG_HTML)
			return;

		/* Closes the current node. */
		tb_pop(p);
	}
}

/* Pops until the current node is a table section, template or html element. */
static void
modes_clear_to_table_body_context(
	struct html_parser *p)
{
	int tag;

	/* Pops the rest. */
	while (p->open.count > 0) {
		tag = modes_current_tag(p);
		if (tag == DOM_TAG_TBODY ||
		    tag == DOM_TAG_TFOOT ||
		    tag == DOM_TAG_THEAD ||
		    tag == DOM_TAG_TEMPLATE ||
		    tag == DOM_TAG_HTML)
			return;

		/* Closes the current node. */
		tb_pop(p);
	}
}

/* Pops until the current node is a tr, template or html element. */
static void
modes_clear_to_row_context(
	struct html_parser *p)
{
	int tag;

	/* Pops the rest. */
	while (p->open.count > 0) {
		tag = modes_current_tag(p);
		if (tag == DOM_TAG_TR || tag == DOM_TAG_TEMPLATE || tag == DOM_TAG_HTML)
			return;

		/* Closes the current node. */
		tb_pop(p);
	}
}

/* Closes the cell that is open: implied end tags, then up to the td or th. */
static void
modes_close_cell(
	struct html_parser *p)
{
	int tag;

	/* Closes whatever is implied inside the cell. */
	tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
	tag = modes_current_tag(p);
	if (tag != DOM_TAG_TD && tag != DOM_TAG_TH)
		tb_error(p);

	/* Pops up to and including the cell. */
	while (p->open.count > 0) {
		tag = modes_current_tag(p);
		tb_pop(p);
		if (tag == DOM_TAG_TD || tag == DOM_TAG_TH)
			break;
	}

	/* Back in the row. */
	tb_clear_formatting_to_marker(p);
	p->mode = TB_IN_ROW;
}

/* Hands out the gathered table text: foster-parented when it has non-whitespace, inserted otherwise. */
static void
modes_flush_table_text(
	struct html_parser *p)
{
	struct tb_token token;
	size_t start;
	size_t end;
	int kind;
	int next_kind;

	/* Whitespace only is inserted where it is. */
	if (!p->table_text_other) {
		tb_insert_characters(p, p->table_text.data, p->table_text.length);
		wb_units_clear(&p->table_text);
		return;
	}

	/* Otherwise it goes through "in table"'s anything-else rule run by run. */
	tb_error(p);
	start = 0;
	while (start < p->table_text.length) {
		/* The run of one kind that starts here. */
		kind = modes_text_kind(p->table_text.data[start]);
		end = start + 1U;
		while (end < p->table_text.length) {
			next_kind = modes_text_kind(p->table_text.data[end]);
			if (next_kind != kind)
				break;

			/* The character belongs to the run. */
			end++;
		}

		/* Goes into the body with foster parenting. */
		memset(&token, 0, sizeof(token));
		token.type = HTML_TOKEN_CHARACTERS;
		token.tag = kind;
		token.text = p->table_text.data + start;
		token.length = end - start;
		p->foster_parenting = 1;
		tb_process_in_mode(p, TB_IN_BODY, &token);
		p->foster_parenting = 0;
		start = end;
	}

	/* The table text is used up. */
	wb_units_clear(&p->table_text);
}

/* Classifies a table text character: whitespace or other (NULs were dropped). */
static int
modes_text_kind(
	uint16_t unit)
{
	/* The tree builder's whitespace. */
	if (unit == 0x09U || unit == 0x0aU || unit == 0x0cU || unit == 0x0dU || unit == 0x20U)
		return TB_TEXT_WHITESPACE;

	/* Anything else. */
	return TB_TEXT_OTHER;
}
