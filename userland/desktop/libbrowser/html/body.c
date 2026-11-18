/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The "in body" insertion mode, the largest of the tree construction: the
 * token cases in the standard's order, with select content parsed here as
 * the current standard does (the customizable <select>).
 */

#include "html/parser.h"

#include <string.h>

static void body_characters(struct html_parser *p, const struct tb_token *token);
static void body_start_tag(struct html_parser *p, const struct tb_token *token);
static void body_end_tag(struct html_parser *p, const struct tb_token *token);
static void body_eof(struct html_parser *p, const struct tb_token *token);
static void body_list_item(struct html_parser *p, const struct tb_token *token);
static void body_close_block(struct html_parser *p, const struct tb_token *token);
static void body_any_other_end_tag(struct html_parser *p, const struct tb_token *token);
static int body_is_block_start(int tag);
static int body_is_block_end(int tag);
static int body_is_formatting(int tag);
static int body_is_heading(int tag);
static int body_select_context(const struct html_parser *p);

/*
 * The "in body" insertion mode.
 */
void
tb_mode_in_body(
	struct html_parser *p,
	const struct tb_token *token)
{
	/* Picks the case by the kind of token. */
	switch (token->type) {
	case HTML_TOKEN_CHARACTERS:
		body_characters(p, token);
		break;
	case HTML_TOKEN_COMMENT:
		tb_insert_comment(p, token, NULL);
		break;
	case HTML_TOKEN_DOCTYPE:
		tb_error(p);
		break;
	case HTML_TOKEN_START_TAG:
		body_start_tag(p, token);
		break;
	case HTML_TOKEN_END_TAG:
		body_end_tag(p, token);
		break;
	case HTML_TOKEN_EOF:
		body_eof(p, token);
		break;
	default:
		break;
	}
}

/* Characters in body: NULs are dropped, the rest reopens formatting and is inserted. */
static void
body_characters(
	struct html_parser *p,
	const struct tb_token *token)
{
	/* NULs are errors and dropped. */
	if (token->tag == TB_TEXT_NULL) {
		tb_error(p);
		return;
	}

	/* Text reopens the formatting elements and is inserted. */
	tb_reconstruct_formatting(p);
	tb_insert_characters(p, token->text, token->length);

	/* Text that is not whitespace rules out a frameset. */
	if (token->tag == TB_TEXT_OTHER)
		p->frameset_ok = 0;
}

/* The start tag cases of "in body". */
static void
body_start_tag(
	struct html_parser *p,
	const struct tb_token *token)
{
	int current_is;
	int is_element;
	int select_context;
	int template_open;
	const struct html_token_attribute *attribute;
	struct dom_element *element;
	struct dom_element *body;
	int in_scope;
	int hidden;
	int more;
	int open_count;
	int heading;
	int option_open;
	int optgroup_open;
	int ruby_current;

	/* The tag's case. */
	switch (token->tag) {
	case DOM_TAG_HTML:
		/* A second <html> lends its new attributes to the root. */
		tb_error(p);
		template_open = tb_template_on_stack(p);
		if (template_open)
			return;
		tb_add_missing_attributes(p, tb_open_at(p, 0), token);
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
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	case DOM_TAG_BODY:
		/* A second <body> lends its new attributes to the body. */
		tb_error(p);
		open_count = tb_open_count(p);
		if (open_count < 2)
			return;
		template_open = tb_template_on_stack(p);
		if (template_open)
			return;
		body = tb_open_at(p, 1);
		is_element = dom_element_is(&body->node, DOM_NS_HTML, DOM_TAG_BODY);
		if (!is_element)
			return;
		p->frameset_ok = 0;
		tb_add_missing_attributes(p, body, token);
		return;
	case DOM_TAG_FRAMESET:
		/* A frameset replaces a body that has had no content yet. */
		tb_error(p);
		open_count = tb_open_count(p);
		if (open_count < 2)
			return;
		body = tb_open_at(p, 1);
		is_element = dom_element_is(&body->node, DOM_NS_HTML, DOM_TAG_BODY);
		if (!is_element || !p->frameset_ok)
			return;
		dom_remove(&body->node);
		while (p->open.count > 1)
			tb_pop(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		p->mode = TB_IN_FRAMESET;
		return;
	case DOM_TAG_H1:
	case DOM_TAG_H2:
	case DOM_TAG_H3:
	case DOM_TAG_H4:
	case DOM_TAG_H5:
	case DOM_TAG_H6:
		/* A heading closes a p and a heading that is the current node. */
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (in_scope)
			tb_close_p(p);
		element = tb_current(p);
		heading = 0;
		if (element != NULL && element->ns == DOM_NS_HTML)
			heading = body_is_heading(element->tag);
		if (heading) {
			tb_error(p);
			tb_pop(p);
		}

		/* Then the heading. */
		tb_insert_element(p, token, DOM_NS_HTML);
		return;
	case DOM_TAG_PRE:
	case DOM_TAG_LISTING:
		/* A leading line feed of their text is dropped. */
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (in_scope)
			tb_close_p(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		p->skip_newline = 1;
		p->frameset_ok = 0;
		return;
	case DOM_TAG_FORM:
		/* One form at a time outside templates. */
		template_open = tb_template_contents(p);
		if (p->form != NULL && !template_open) {
			tb_error(p);
			return;
		}

		/* Closes a p before the form. */
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (in_scope)
			tb_close_p(p);
		element = tb_insert_element(p, token, DOM_NS_HTML);
		template_open = tb_template_contents(p);
		if (!template_open)
			p->form = element;
		return;
	case DOM_TAG_LI:
	case DOM_TAG_DD:
	case DOM_TAG_DT:
		body_list_item(p, token);
		return;
	case DOM_TAG_PLAINTEXT:
		/* Everything after it is its text. */
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (in_scope)
			tb_close_p(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		html_tokenizer_set_state(&p->tokenizer, HTML_TOKENIZE_PLAINTEXT);
		return;
	case DOM_TAG_BUTTON:
		/* A button closes an open one. */
		in_scope = tb_in_scope(p, DOM_TAG_BUTTON, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_error(p);
			tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
			tb_pop_until_tag(p, DOM_TAG_BUTTON);
		}

		/* Then the button. */
		tb_reconstruct_formatting(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		p->frameset_ok = 0;
		return;
	case DOM_TAG_A:
		/* An <a> closes an earlier open one through the adoption agency. */
		element = tb_formatting_after_marker(p, DOM_TAG_A);
		if (element != NULL) {
			tb_error(p);
			tb_adoption_agency(p, token);
			tb_remove_formatting(p, element);
			tb_remove_open(p, element);
		}

		/* Then the new a. */
		tb_reconstruct_formatting(p);
		element = tb_insert_element(p, token, DOM_NS_HTML);
		if (element != NULL)
			tb_push_formatting(p, element);
		return;
	case DOM_TAG_NOBR:
		/* A <nobr> closes an open one. */
		tb_reconstruct_formatting(p);
		in_scope = tb_in_scope(p, DOM_TAG_NOBR, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_error(p);
			tb_adoption_agency(p, token);
			tb_reconstruct_formatting(p);
		}

		/* Then the new nobr. */
		element = tb_insert_element(p, token, DOM_NS_HTML);
		if (element != NULL)
			tb_push_formatting(p, element);
		return;
	case DOM_TAG_APPLET:
	case DOM_TAG_MARQUEE:
	case DOM_TAG_OBJECT:
		tb_reconstruct_formatting(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		tb_push_marker(p);
		p->frameset_ok = 0;
		return;
	case DOM_TAG_TABLE:
		/* A table closes a p except in quirks mode. */
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (p->document->quirks != DOM_QUIRKS && in_scope)
			tb_close_p(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		p->frameset_ok = 0;
		p->mode = TB_IN_TABLE;
		return;
	case DOM_TAG_AREA:
	case DOM_TAG_BR:
	case DOM_TAG_EMBED:
	case DOM_TAG_IMG:
	case DOM_TAG_KEYGEN:
	case DOM_TAG_WBR:
		tb_reconstruct_formatting(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		tb_pop(p);
		p->frameset_ok = 0;
		return;
	case DOM_TAG_INPUT:
		/* An input closes an open select (and is ignored in a select fragment). */
		select_context = body_select_context(p);
		if (select_context) {
			tb_error(p);
			return;
		}

		/* Closes an open select. */
		in_scope = tb_in_scope(p, DOM_TAG_SELECT, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_error(p);
			tb_pop_until_tag(p, DOM_TAG_SELECT);
		}

		/* Then the input, which is void. */
		tb_reconstruct_formatting(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		tb_pop(p);
		attribute = tb_token_attribute(token, "type");
		hidden = 0;
		if (attribute != NULL)
			hidden = tb_units_equal_ascii(attribute->value.data, attribute->value.length, "hidden", 1);
		if (!hidden)
			p->frameset_ok = 0;
		return;
	case DOM_TAG_PARAM:
	case DOM_TAG_SOURCE:
	case DOM_TAG_TRACK:
		tb_insert_element(p, token, DOM_NS_HTML);
		tb_pop(p);
		return;
	case DOM_TAG_HR:
		/* A rule closes a p, and inside a select the open options. */
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (in_scope)
			tb_close_p(p);
		in_scope = tb_in_scope(p, DOM_TAG_SELECT, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
			option_open = tb_in_scope(p, DOM_TAG_OPTION, TB_SCOPE_DEFAULT);
			optgroup_open = tb_in_scope(p, DOM_TAG_OPTGROUP, TB_SCOPE_DEFAULT);
			if (option_open || optgroup_open)
				tb_error(p);
		}

		/* Then the rule, which is void. */
		tb_insert_element(p, token, DOM_NS_HTML);
		tb_pop(p);
		p->frameset_ok = 0;
		return;
	case DOM_TAG_IMAGE:
		/* <image> is an error for <img>: an img element with the token's attributes. */
		tb_error(p);
		tb_reconstruct_formatting(p);
		element = tb_create_element_for_tag(p, DOM_TAG_IMG);
		if (element == NULL)
			return;
		tb_add_missing_attributes(p, element, token);
		tb_insert_node(p, &element->node, NULL);
		p->frameset_ok = 0;
		return;
	case DOM_TAG_TEXTAREA:
		/* Its text is RCDATA, without a leading line feed. */
		tb_insert_element(p, token, DOM_NS_HTML);
		p->skip_newline = 1;
		html_tokenizer_set_state(&p->tokenizer, HTML_TOKENIZE_RCDATA);
		p->original_mode = p->mode;
		p->frameset_ok = 0;
		p->mode = TB_TEXT;
		return;
	case DOM_TAG_XMP:
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (in_scope)
			tb_close_p(p);
		tb_reconstruct_formatting(p);
		p->frameset_ok = 0;
		tb_generic_text(p, token, HTML_TOKENIZE_RAWTEXT);
		return;
	case DOM_TAG_IFRAME:
		p->frameset_ok = 0;
		tb_generic_text(p, token, HTML_TOKENIZE_RAWTEXT);
		return;
	case DOM_TAG_NOEMBED:
		tb_generic_text(p, token, HTML_TOKENIZE_RAWTEXT);
		return;
	case DOM_TAG_NOSCRIPT:
		if (p->scripting) {
			tb_generic_text(p, token, HTML_TOKENIZE_RAWTEXT);
			return;
		}

		break;
	case DOM_TAG_SELECT:
		/* A select inside a select closes the outer one (and is ignored in a select fragment). */
		select_context = body_select_context(p);
		if (select_context) {
			tb_error(p);
			return;
		}

		/* Closes an open select. */
		in_scope = tb_in_scope(p, DOM_TAG_SELECT, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_error(p);
			tb_pop_until_tag(p, DOM_TAG_SELECT);
			return;
		}

		/* Then the new select. */
		tb_reconstruct_formatting(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		p->frameset_ok = 0;
		return;
	case DOM_TAG_OPTION:
		/* An option closes an open option. */
		in_scope = tb_in_scope(p, DOM_TAG_SELECT, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_generate_implied_end_tags(p, DOM_TAG_OPTGROUP);
			in_scope = tb_in_scope(p, DOM_TAG_OPTION, TB_SCOPE_DEFAULT);
			if (in_scope)
				tb_error(p);
		} else {
			current_is = tb_current_is(p, DOM_TAG_OPTION);
			if (current_is)
				tb_pop(p);
		}

		/* Then the option. */
		tb_reconstruct_formatting(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		return;
	case DOM_TAG_OPTGROUP:
		/* An optgroup closes an open option and optgroup. */
		in_scope = tb_in_scope(p, DOM_TAG_SELECT, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
			option_open = tb_in_scope(p, DOM_TAG_OPTION, TB_SCOPE_DEFAULT);
			optgroup_open = tb_in_scope(p, DOM_TAG_OPTGROUP, TB_SCOPE_DEFAULT);
			if (option_open || optgroup_open)
				tb_error(p);
		} else {
			current_is = tb_current_is(p, DOM_TAG_OPTION);
			if (current_is)
				tb_pop(p);
		}

		/* Then the optgroup. */
		tb_reconstruct_formatting(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		return;
	case DOM_TAG_RB:
	case DOM_TAG_RTC:
		in_scope = tb_in_scope(p, DOM_TAG_RUBY, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
			current_is = tb_current_is(p, DOM_TAG_RUBY);
			if (!current_is)
				tb_error(p);
		}

		/* Then the ruby base or container. */
		tb_insert_element(p, token, DOM_NS_HTML);
		return;
	case DOM_TAG_RP:
	case DOM_TAG_RT:
		in_scope = tb_in_scope(p, DOM_TAG_RUBY, TB_SCOPE_DEFAULT);
		if (in_scope) {
			tb_generate_implied_end_tags(p, DOM_TAG_RTC);
			current_is = tb_current_is(p, DOM_TAG_RTC);
			ruby_current = tb_current_is(p, DOM_TAG_RUBY);
			if (!current_is && !ruby_current)
				tb_error(p);
		}

		/* Then the ruby text or parenthesis. */
		tb_insert_element(p, token, DOM_NS_HTML);
		return;
	case DOM_TAG_MATH:
		tb_reconstruct_formatting(p);
		tb_insert_foreign(p, token, DOM_NS_MATHML);
		return;
	case DOM_TAG_SVG:
		tb_reconstruct_formatting(p);
		tb_insert_foreign(p, token, DOM_NS_SVG);
		return;
	case DOM_TAG_CAPTION:
	case DOM_TAG_COL:
	case DOM_TAG_COLGROUP:
	case DOM_TAG_FRAME:
	case DOM_TAG_HEAD:
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

	/* The block elements close a p. */
	more = body_is_block_start(token->tag);
	if (more) {
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (in_scope)
			tb_close_p(p);
		tb_insert_element(p, token, DOM_NS_HTML);
		return;
	}

	/* The formatting elements join the list of active formatting elements. */
	more = body_is_formatting(token->tag);
	if (more) {
		tb_reconstruct_formatting(p);
		element = tb_insert_element(p, token, DOM_NS_HTML);
		if (element != NULL)
			tb_push_formatting(p, element);
		return;
	}

	/* Any other start tag is an ordinary element. */
	tb_reconstruct_formatting(p);
	tb_insert_element(p, token, DOM_NS_HTML);
}

/* The end tag cases of "in body". */
static void
body_end_tag(
	struct html_parser *p,
	const struct tb_token *token)
{
	int block_end;
	int current_is;
	int template_open;
	struct tb_token br;
	struct dom_element *node;
	struct dom_element *current;
	int in_scope;
	int agency;
	int index;
	int found;
	int heading;
	int formatting;

	/* The tag's case. */
	switch (token->tag) {
	case DOM_TAG_TEMPLATE:
		tb_process_in_mode(p, TB_IN_HEAD, token);
		return;
	case DOM_TAG_BODY:
	case DOM_TAG_HTML:
		/* The body ends; </html> is reprocessed after it. */
		in_scope = tb_in_scope(p, DOM_TAG_BODY, TB_SCOPE_DEFAULT);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* The body is done. */
		p->mode = TB_AFTER_BODY;
		if (token->tag == DOM_TAG_HTML)
			tb_process(p, token);
		return;
	case DOM_TAG_FORM:
		template_open = tb_template_contents(p);
		if (!template_open) {
			/* The form pointer's form is closed wherever it is. */
			node = p->form;
			p->form = NULL;
			in_scope = 0;
			if (node != NULL)
				in_scope = tb_element_in_scope(p, node, TB_SCOPE_DEFAULT);
			if (!in_scope) {
				tb_error(p);
				return;
			}

			/* Closes what is implied; the form itself leaves the stack wherever it is. */
			tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
			current = tb_current(p);
			if (current != node)
				tb_error(p);
			tb_remove_open(p, node);
			return;
		}

		/* Inside a template the form is closed by name. */
		in_scope = tb_in_scope(p, DOM_TAG_FORM, TB_SCOPE_DEFAULT);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then the form. */
		tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
		current_is = tb_current_is(p, DOM_TAG_FORM);
		if (!current_is)
			tb_error(p);
		tb_pop_until_tag(p, DOM_TAG_FORM);
		return;
	case DOM_TAG_P:
		/* </p> without a p makes an empty one. */
		in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
		if (!in_scope) {
			tb_error(p);
			tb_insert_html_tag(p, DOM_TAG_P);
		}

		/* Closes the p. */
		tb_close_p(p);
		return;
	case DOM_TAG_LI:
		in_scope = tb_in_scope(p, DOM_TAG_LI, TB_SCOPE_LIST_ITEM);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then the item. */
		tb_generate_implied_end_tags(p, DOM_TAG_LI);
		current_is = tb_current_is(p, DOM_TAG_LI);
		if (!current_is)
			tb_error(p);
		tb_pop_until_tag(p, DOM_TAG_LI);
		return;
	case DOM_TAG_DD:
	case DOM_TAG_DT:
		in_scope = tb_in_scope(p, token->tag, TB_SCOPE_DEFAULT);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then the item. */
		tb_generate_implied_end_tags(p, token->tag);
		current_is = tb_current_is(p, token->tag);
		if (!current_is)
			tb_error(p);
		tb_pop_until_tag(p, token->tag);
		return;
	case DOM_TAG_H1:
	case DOM_TAG_H2:
	case DOM_TAG_H3:
	case DOM_TAG_H4:
	case DOM_TAG_H5:
	case DOM_TAG_H6:
		/* Any heading closes any open heading. */
		found = 0;
		for (index = DOM_TAG_H1; index <= DOM_TAG_H6; index++) {
			in_scope = tb_in_scope(p, index, TB_SCOPE_DEFAULT);
			if (in_scope)
				found = 1;
		}

		/* Only an open heading can close. */
		if (!found) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then up to a heading. */
		tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
		current_is = tb_current_is(p, token->tag);
		if (!current_is)
			tb_error(p);
		while (p->open.count > 0) {
			node = tb_current(p);
			tb_pop(p);
			heading = 0;
			if (node->ns == DOM_NS_HTML)
				heading = body_is_heading(node->tag);
			if (heading)
				break;
		}

		/* The heading is closed. */
		return;
	case DOM_TAG_APPLET:
	case DOM_TAG_MARQUEE:
	case DOM_TAG_OBJECT:
		in_scope = tb_in_scope(p, token->tag, TB_SCOPE_DEFAULT);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then the element and its marker. */
		tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
		current_is = tb_current_is(p, token->tag);
		if (!current_is)
			tb_error(p);
		tb_pop_until_tag(p, token->tag);
		tb_clear_formatting_to_marker(p);
		return;
	case DOM_TAG_BR:
		/* </br> is an error for <br>. */
		tb_error(p);
		memset(&br, 0, sizeof(br));
		br.type = HTML_TOKEN_START_TAG;
		br.tag = DOM_TAG_BR;
		body_start_tag(p, &br);
		return;
	case DOM_TAG_SELECT:
		in_scope = tb_in_scope(p, DOM_TAG_SELECT, TB_SCOPE_DEFAULT);
		if (!in_scope) {
			tb_error(p);
			return;
		}

		/* Closes what is implied, then the select. */
		tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
		tb_pop_until_tag(p, DOM_TAG_SELECT);
		return;
	default:
		break;
	}

	/* The block elements' end tags. */
	block_end = body_is_block_end(token->tag);
	if (block_end) {
		body_close_block(p, token);
		return;
	}

	/* The formatting elements' end tags run the adoption agency. */
	formatting = body_is_formatting(token->tag);
	if (formatting ||
	    token->tag == DOM_TAG_A ||
	    token->tag == DOM_TAG_NOBR) {
		agency = tb_adoption_agency(p, token);
		if (agency)
			body_any_other_end_tag(p, token);
		return;
	}

	/* Any other end tag. */
	body_any_other_end_tag(p, token);
}

/* End of file in body: templates are closed, otherwise parsing stops. */
static void
body_eof(
	struct html_parser *p,
	const struct tb_token *token)
{
	/* Open templates are closed by the template mode's rules. */
	if (p->template_modes.count > 0) {
		tb_process_in_mode(p, TB_IN_TEMPLATE, token);
		return;
	}

	/* Parsing stops. */
	tb_stop(p);
}

/* <li>, <dd> and <dt>: close the open list item of the same kind first. */
static void
body_list_item(
	struct html_parser *p,
	const struct tb_token *token)
{
	int current_is;
	struct dom_element *node;
	size_t index;
	int in_scope;
	int matches;
	int special;

	/* Walks up from the current node for an item to close. */
	p->frameset_ok = 0;
	index = tb_open_count(p);
	while (index > 0) {
		index--;
		node = tb_open_at(p, index);

		/* An open item of the same kind is closed. */
		matches = 0;
		if (node->ns == DOM_NS_HTML) {
			if (token->tag == DOM_TAG_LI && node->tag == DOM_TAG_LI)
				matches = 1;
			if (token->tag != DOM_TAG_LI && (node->tag == DOM_TAG_DD || node->tag == DOM_TAG_DT))
				matches = 1;
		}

		/* A matching item is closed with what is below it. */
		if (matches) {
			tb_generate_implied_end_tags(p, node->tag);
			current_is = tb_current_is(p, node->tag);
			if (!current_is)
				tb_error(p);
			tb_pop_until_tag(p, node->tag);
			break;
		}

		/* A special element other than address, div and p ends the search. */
		special = tb_is_special(node);
		if (special && node->ns == DOM_NS_HTML) {
			if (node->tag == DOM_TAG_ADDRESS || node->tag == DOM_TAG_DIV || node->tag == DOM_TAG_P)
				special = 0;
		}

		/* The search stops at it. */
		if (special)
			break;
	}

	/* Closes a p, then inserts the item. */
	in_scope = tb_in_scope(p, DOM_TAG_P, TB_SCOPE_BUTTON);
	if (in_scope)
		tb_close_p(p);
	tb_insert_element(p, token, DOM_NS_HTML);
}

/* The end tag of a block element: closes it when it is in scope. */
static void
body_close_block(
	struct html_parser *p,
	const struct tb_token *token)
{
	int current_is;
	int in_scope;

	/* Only an open block can close. */
	in_scope = tb_in_scope(p, token->tag, TB_SCOPE_DEFAULT);
	if (!in_scope) {
		tb_error(p);
		return;
	}

	/* Closes what is implied, then the block. */
	tb_generate_implied_end_tags(p, DOM_TAG_UNKNOWN);
	current_is = tb_current_is(p, token->tag);
	if (!current_is)
		tb_error(p);
	tb_pop_until_tag(p, token->tag);
}

/* "Any other end tag": closes the nearest open element of that name, unless a special one is in the way. */
static void
body_any_other_end_tag(
	struct html_parser *p,
	const struct tb_token *token)
{
	struct dom_element *node;
	struct dom_element *current;
	size_t index;
	int special;
	int same;

	/* Walks up from the current node. */
	index = tb_open_count(p);
	while (index > 0) {
		index--;
		node = tb_open_at(p, index);

		/* An HTML element of the token's name is closed with what is below it. */
		same = 0;
		if (node->ns == DOM_NS_HTML && token->raw != NULL)
			same = vm_string_equal_units(node->local_name, token->raw->name.data, token->raw->name.length);
		if (same) {
			tb_generate_implied_end_tags(p, node->tag);
			current = tb_current(p);
			if (current != node)
				tb_error(p);
			tb_pop_until_element(p, node);
			return;
		}

		/* A special element in the way makes the end tag an error to ignore. */
		special = tb_is_special(node);
		if (special) {
			tb_error(p);
			return;
		}
	}
}

/* Tells whether a start tag is one of the blocks that close a p. */
static int
body_is_block_start(
	int tag)
{
	/* The standard's list. */
	switch (tag) {
	case DOM_TAG_ADDRESS:
	case DOM_TAG_ARTICLE:
	case DOM_TAG_ASIDE:
	case DOM_TAG_BLOCKQUOTE:
	case DOM_TAG_CENTER:
	case DOM_TAG_DETAILS:
	case DOM_TAG_DIALOG:
	case DOM_TAG_DIR:
	case DOM_TAG_DIV:
	case DOM_TAG_DL:
	case DOM_TAG_FIELDSET:
	case DOM_TAG_FIGCAPTION:
	case DOM_TAG_FIGURE:
	case DOM_TAG_FOOTER:
	case DOM_TAG_HEADER:
	case DOM_TAG_HGROUP:
	case DOM_TAG_MAIN:
	case DOM_TAG_MENU:
	case DOM_TAG_NAV:
	case DOM_TAG_OL:
	case DOM_TAG_P:
	case DOM_TAG_SEARCH:
	case DOM_TAG_SECTION:
	case DOM_TAG_SUMMARY:
	case DOM_TAG_UL:
		return 1;
	default:
		return 0;
	}
}

/* Tells whether an end tag is one of the blocks closed when in scope. */
static int
body_is_block_end(
	int tag)
{
	/* The standard's list. */
	switch (tag) {
	case DOM_TAG_ADDRESS:
	case DOM_TAG_ARTICLE:
	case DOM_TAG_ASIDE:
	case DOM_TAG_BLOCKQUOTE:
	case DOM_TAG_BUTTON:
	case DOM_TAG_CENTER:
	case DOM_TAG_DETAILS:
	case DOM_TAG_DIALOG:
	case DOM_TAG_DIR:
	case DOM_TAG_DIV:
	case DOM_TAG_DL:
	case DOM_TAG_FIELDSET:
	case DOM_TAG_FIGCAPTION:
	case DOM_TAG_FIGURE:
	case DOM_TAG_FOOTER:
	case DOM_TAG_HEADER:
	case DOM_TAG_HGROUP:
	case DOM_TAG_LISTING:
	case DOM_TAG_MAIN:
	case DOM_TAG_MENU:
	case DOM_TAG_NAV:
	case DOM_TAG_OL:
	case DOM_TAG_PRE:
	case DOM_TAG_SEARCH:
	case DOM_TAG_SECTION:
	case DOM_TAG_SUMMARY:
	case DOM_TAG_UL:
		return 1;
	default:
		return 0;
	}
}

/* Tells whether a tag is one of the formatting elements (but a and nobr). */
static int
body_is_formatting(
	int tag)
{
	/* The standard's list. */
	switch (tag) {
	case DOM_TAG_B:
	case DOM_TAG_BIG:
	case DOM_TAG_CODE:
	case DOM_TAG_EM:
	case DOM_TAG_FONT:
	case DOM_TAG_I:
	case DOM_TAG_S:
	case DOM_TAG_SMALL:
	case DOM_TAG_STRIKE:
	case DOM_TAG_STRONG:
	case DOM_TAG_TT:
	case DOM_TAG_U:
		return 1;
	default:
		return 0;
	}
}

/* Tells whether a tag is a heading, h1 to h6. */
static int
body_is_heading(
	int tag)
{
	/* The six headings are consecutive tags. */
	if (tag >= DOM_TAG_H1 && tag <= DOM_TAG_H6)
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether the parser is parsing a fragment whose context is a select. */
static int
body_select_context(
	const struct html_parser *p)
{
	int is_element;

	/* Only the fragment case has a context. */
	if (p->context == NULL)
		return 0;

	/* The context must be an HTML select. */
	is_element = dom_element_is(&p->context->node, DOM_NS_HTML, DOM_TAG_SELECT);
	if (!is_element)
		return 0;

	/* The fragment is a select's. */
	return 1;
}
