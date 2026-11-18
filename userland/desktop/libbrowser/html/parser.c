/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HTML tree builder: the loop that pulls tokens from the tokenizer and
 * hands them to the insertion modes, and the algorithms the modes share
 * (the stack of open elements and its scopes, the list of active
 * formatting elements, inserting nodes with foster parenting, the adoption
 * agency, resetting the insertion mode).
 */

#include "html/parser.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most iterations of the adoption agency's outer and inner loops. */
#define ADOPTION_OUTER_LIMIT	8
#define ADOPTION_INNER_LIMIT	3

static void parser_trace(struct vm_heap *heap, void *context);
static int parser_run(struct html_parser *p);
static void parser_dispatch_characters(struct html_parser *p, const struct html_token *raw);
static int parser_text_kind(uint16_t unit);
static int parser_scope_boundary(const struct dom_element *element, int scope);
static int parser_is_implied(const struct dom_element *element, int thorough);
static struct dom_node *parser_insertion_parent(struct html_parser *p, struct dom_element *override, struct dom_node **before);
static struct dom_node *parser_template_contents(struct dom_node *parent);
static void parser_set_open(struct html_parser *p, size_t index, struct dom_element *element);
static void parser_insert_open(struct html_parser *p, size_t index, struct dom_element *element);
static void parser_set_formatting(struct html_parser *p, size_t index, struct dom_element *element);
static void parser_insert_formatting(struct html_parser *p, size_t index, struct dom_element *element);
static struct dom_element *parser_clone_element(struct html_parser *p, const struct dom_element *element);
static void parser_nomem(struct html_parser *p);
static int parser_fragment_state(const struct dom_element *context, int scripting);

/*
 * Makes a parser that builds a whole document.
 *
 * scripting says whether scripting is enabled (it changes how <noscript>
 * parses).  The parser marks what it holds for the document's heap until
 * it is destroyed; the caller keeps the document alive.
 */
int
html_parser_create(
	struct html_parser **parser,
	struct dom_document *document,
	int scripting)
{
	struct html_parser *p;
	int error;

	/* Allocates the parser's record. */
	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return ENOMEM;

	/* Starts in the initial mode with empty stacks. */
	p->document = document;
	p->heap = document->heap;
	p->scripting = scripting;
	p->frameset_ok = 1;
	p->mode = TB_INITIAL;
	p->insertion_point = (size_t)-1;
	html_input_init(&p->input);
	html_tokenizer_init(&p->tokenizer, &p->input);
	wb_vector_init(&p->template_modes, sizeof(int));
	wb_vector_init(&p->open, sizeof(struct dom_element *));
	wb_vector_init(&p->formatting, sizeof(struct tb_formatting));
	wb_units_init(&p->table_text);

	/* Lets the heap see the elements the stacks hold. */
	error = vm_heap_add_tracer(p->heap, parser_trace, p);
	if (error != 0) {
		html_tokenizer_release(&p->tokenizer);
		free(p);
		return error;
	}

	/* Succeeded: the parser is ready for input. */
	*parser = p;
	return 0;
}

/*
 * Makes a parser that builds a fragment in the context of an element (the
 * HTML Standard's HTML fragment parsing algorithm; ws074-p081): the
 * tokenizer starts in the state the context's content is read in, the
 * elements are built in a root html element of the context's document
 * (not inserted in it; html_parser_fragment_root finds it), the insertion
 * mode is the one the context element gives, and the form element pointer
 * is the context's nearest form.  No script runs.
 */
int
html_parser_create_fragment(
	struct html_parser **parser,
	struct dom_element *context,
	int scripting)
{
	struct html_parser *p;
	struct dom_element *root;
	struct dom_node *walk;
	int state;
	int form;
	int error;

	/* A parser of the context's document. */
	error = html_parser_create(&p, context->node.document, scripting);
	if (error != 0)
		return error;
	p->context = context;

	/* The tokenizer's state is the one the context's content is read in. */
	state = parser_fragment_state(context, scripting);
	html_tokenizer_set_state(&p->tokenizer, state);

	/*
	 * The tokenizer has seen no start tag: an end tag of the context's name
	 * in its raw text is text (the html5lib fragment cases, and other
	 * browsers).
	 */

	/* The root html element, the only element open. */
	root = tb_create_element_for_tag(p, DOM_TAG_HTML);
	if (root == NULL) {
		html_parser_destroy(p);
		return ENOMEM;
	}

	/* It is the fragment's root and the stack's bottom. */
	p->fragment_root = root;
	tb_push(p, root);

	/* A template context reads in template contents. */
	if (context->ns == DOM_NS_HTML && context->tag == DOM_TAG_TEMPLATE)
		tb_push_template_mode(p, TB_IN_TEMPLATE);

	/* The mode the context gives. */
	tb_reset_insertion_mode(p);

	/* The form element pointer: the context or its nearest ancestor that is a form. */
	for (walk = &context->node; walk != NULL; walk = walk->parent) {
		if (walk->type != DOM_ELEMENT)
			continue;
		form = dom_element_is(walk, DOM_NS_HTML, DOM_TAG_FORM);
		if (form) {
			p->form = (struct dom_element *)walk;
			break;
		}
	}

	/* Succeeded: the parser takes the fragment's text. */
	*parser = p;
	return 0;
}

/*
 * Finds the root html element a fragment parser built the fragment in
 * (its children are the fragment), or NULL for a document's parser.
 */
struct dom_element *
html_parser_fragment_root(
	const struct html_parser *p)
{
	/* The root, kept apart from the stack, which a stopped parse empties. */
	return p->fragment_root;
}

/*
 * Sets what runs each script element when the parser reaches its end tag.
 */
void
html_parser_set_script_hook(
	struct html_parser *p,
	html_script_hook hook,
	void *context)
{
	/* The hook and what it is given. */
	p->script_hook = hook;
	p->script_context = context;
}

/* Transfers stack tracing to an owner that participates in the GC graph. */
void
html_parser_transfer_ownership(
	struct html_parser *p)
{
	/* A published owner must trace the parser before its independent root disappears. */
	vm_heap_remove_tracer(p->heap, parser_trace, p);

	/* Succeeded: the owner now determines parser reachability. */
	return;
}

/* Marks native parser stacks whenever their managed owner is reachable. */
void
html_parser_trace_owned(
	struct vm_heap *heap,
	struct html_parser *p)
{
	/* Windows without a script-created parser have no parser stack to mark. */
	if (p == NULL)
		return;

	/* Reuses the complete stack traversal of ordinary standalone parsers. */
	parser_trace(heap, p);

	/* Succeeded: every parser-held DOM edge is visible to this collection. */
	return;
}

/*
 * Destroys a parser (the document stays).
 */
void
html_parser_destroy(
	struct html_parser *p)
{
	/* A NULL parser is nothing to destroy. */
	if (p == NULL)
		return;

	/* Stops marking and frees the parser's memory. */
	vm_heap_remove_tracer(p->heap, parser_trace, p);
	html_tokenizer_release(&p->tokenizer);
	html_input_release(&p->input);
	wb_vector_release(&p->template_modes);
	wb_vector_release(&p->open);
	wb_vector_release(&p->formatting);
	wb_units_release(&p->table_text);
	free(p);
}

/*
 * Feeds decoded text to the parser and builds as much of the tree as the
 * text allows.
 */
int
html_parser_feed(
	struct html_parser *p,
	const uint16_t *units,
	size_t length)
{
	int error;

	/* Appends the text to the input. */
	error = html_input_append(&p->input, units, length);
	if (error != 0)
		return error;

	/* Runs the tokenizer and the tree construction until the input runs out. */
	error = parser_run(p);
	if (error != 0)
		return error;

	/* Succeeded: the tree holds what the text described. */
	return 0;
}

/*
 * Inserts script-written text before the current script's input boundary.
 *
 * The unread suffix stays owned while the tokenizer runs the inserted prefix.
 * Partial tokens remain in the tokenizer for the next write or source bytes.
 */
int
html_parser_write(
	struct html_parser *p,
	const uint16_t *units,
	size_t length)
{
	struct wb_units suffix;
	size_t suffix_length;
	int was_closed;
	int restore_error;
	int error;

	/* A failed parser cannot safely expose another input boundary. */
	if (p->failed)
		return ENOMEM;

	/* Only a parser executing a script has a supported insertion point. */
	if (p->insertion_point == (size_t)-1 || p->stopped)
		return ENOTSUP;

	/* Refuses a recursive writer before it consumes the host's C stack. */
	if (p->write_depth >= 32U)
		return ELOOP;

	/* Protects the unread source, which this write must not consume. */
	wb_units_init(&suffix);
	suffix_length = p->input.units.length - p->insertion_point;
	if (suffix_length != 0) {
		error = wb_units_append(&suffix, p->input.units.data + p->insertion_point, suffix_length);
		if (error != 0) {
			p->failed = 1;
			wb_units_release(&suffix);
			return error;
		}
	}

	/* Reserves the inserted text before hiding any original source bytes. */
	error = wb_units_reserve(&p->input.units, length);
	if (error != 0) {
		p->failed = 1;
		wb_units_release(&suffix);
		return error;
	}

	/* Makes only the script's prefix available, with no synthetic EOF. */
	was_closed = p->input.closed;
	p->input.closed = 0;
	p->input.units.length = p->insertion_point;
	error = wb_units_append(&p->input.units, units, length);
	if (error == 0) {
		/* Keeps the boundary after partial tokens that await further writes. */
		p->insertion_point = p->input.units.length;
		p->write_depth++;
		error = parser_run(p);
		p->write_depth--;
	}

	/* Restores the source even when parsing the inserted prefix failed. */
	restore_error = wb_units_append(&p->input.units, suffix.data, suffix.length);
	p->input.closed = was_closed;
	wb_units_release(&suffix);
	if (restore_error != 0) {
		p->failed = 1;
		return restore_error;
	}

	/* Propagates a failed insertion instead of claiming a complete parse. */
	if (error != 0) {
		p->failed = 1;
		return error;
	}

	/* Succeeded: the written prefix is parsed and the source is readable again. */
	return 0;
}

/*
 * Gives a parser script its insertion point and restores the enclosing writer.
 */
void
tb_run_script(
	struct html_parser *p,
	struct dom_element *script)
{
	size_t previous_point;
	size_t previous_length;
	int error;

	/* A parser without script execution needs no insertion scope. */
	if (p->script_hook == NULL)
		return;

	/* Publishes the point immediately after this script's end tag. */
	previous_point = p->insertion_point;
	previous_length = p->input.units.length;
	p->insertion_point = p->input.position;
	error = p->script_hook(p->script_context, script);
	if (error != 0)
		p->failed = 1;

	/* Nested text shifted the outer script's still-unread boundary. */
	if (previous_point != (size_t)-1 && !p->failed)
		previous_point += p->input.units.length - previous_length;

	/* The outer writer, or normal source parser, owns the input again. */
	p->insertion_point = previous_point;
}

/*
 * Ends the input and finishes the tree.
 */
int
html_parser_finish(
	struct html_parser *p)
{
	int error;

	/* Closes the input and runs to end of file. */
	html_input_close(&p->input);
	error = parser_run(p);
	if (error != 0)
		return error;

	/* Succeeded: parsing stopped. */
	return 0;
}

/*
 * Reports how many parse errors the tokenizer and the tree builder found.
 */
size_t
html_parser_errors(
	const struct html_parser *p)
{
	/* Adds the two counts. */
	return p->error_count + p->tokenizer.error_count;
}

/*
 * Processes a token by the rules the standard's dispatcher picks: the
 * insertion mode's, or those for foreign content.
 */
void
tb_process(
	struct html_parser *p,
	const struct tb_token *token)
{
	int foreign;

	/* A stopped parser takes no more tokens. */
	if (p->stopped)
		return;

	/* Picks the rules. */
	foreign = tb_use_foreign_rules(p, token);
	if (foreign) {
		tb_foreign_content(p, token);
		return;
	}

	/* The insertion mode's rules. */
	tb_process_in_mode(p, p->mode, token);
}

/*
 * Processes a token by the rules of an insertion mode (the current one, or
 * another as "using the rules for" asks).
 */
void
tb_process_in_mode(
	struct html_parser *p,
	int mode,
	const struct tb_token *token)
{
	/* Runs the mode's rules. */
	switch (mode) {
	case TB_INITIAL:
		tb_mode_initial(p, token);
		break;
	case TB_BEFORE_HTML:
		tb_mode_before_html(p, token);
		break;
	case TB_BEFORE_HEAD:
		tb_mode_before_head(p, token);
		break;
	case TB_IN_HEAD:
		tb_mode_in_head(p, token);
		break;
	case TB_IN_HEAD_NOSCRIPT:
		tb_mode_in_head_noscript(p, token);
		break;
	case TB_AFTER_HEAD:
		tb_mode_after_head(p, token);
		break;
	case TB_IN_BODY:
		tb_mode_in_body(p, token);
		break;
	case TB_TEXT:
		tb_mode_text(p, token);
		break;
	case TB_IN_TABLE:
		tb_mode_in_table(p, token);
		break;
	case TB_IN_TABLE_TEXT:
		tb_mode_in_table_text(p, token);
		break;
	case TB_IN_CAPTION:
		tb_mode_in_caption(p, token);
		break;
	case TB_IN_COLUMN_GROUP:
		tb_mode_in_column_group(p, token);
		break;
	case TB_IN_TABLE_BODY:
		tb_mode_in_table_body(p, token);
		break;
	case TB_IN_ROW:
		tb_mode_in_row(p, token);
		break;
	case TB_IN_CELL:
		tb_mode_in_cell(p, token);
		break;
	case TB_IN_SELECT:
		tb_mode_in_select(p, token);
		break;
	case TB_IN_SELECT_IN_TABLE:
		tb_mode_in_select_in_table(p, token);
		break;
	case TB_IN_TEMPLATE:
		tb_mode_in_template(p, token);
		break;
	case TB_AFTER_BODY:
		tb_mode_after_body(p, token);
		break;
	case TB_IN_FRAMESET:
		tb_mode_in_frameset(p, token);
		break;
	case TB_AFTER_FRAMESET:
		tb_mode_after_frameset(p, token);
		break;
	case TB_AFTER_AFTER_BODY:
		tb_mode_after_after_body(p, token);
		break;
	case TB_AFTER_AFTER_FRAMESET:
		tb_mode_after_after_frameset(p, token);
		break;
	}
}

/*
 * Counts a parse error of the tree construction.
 */
void
tb_error(
	struct html_parser *p)
{
	/* The tree builder only counts its errors. */
	p->error_count++;
}

/*
 * Finds the current node (the bottom of the stack), or NULL when the stack
 * is empty.
 */
struct dom_element *
tb_current(
	const struct html_parser *p)
{
	/* An empty stack has no current node. */
	if (p->open.count == 0)
		return NULL;

	/* The current node is the last one pushed. */
	return tb_open_at(p, p->open.count - 1U);
}

/*
 * Finds the adjusted current node: the context element when only the root
 * is open in the fragment case, the current node otherwise.
 */
struct dom_element *
tb_adjusted_current(
	const struct html_parser *p)
{
	/* The fragment case with just the root open adjusts to the context element. */
	if (p->context != NULL && p->open.count == 1)
		return p->context;

	/* Otherwise it is the current node. */
	return tb_current(p);
}

/*
 * Finds the element at a place of the stack (0 is the top, the html
 * element).
 */
struct dom_element *
tb_open_at(
	const struct html_parser *p,
	size_t index)
{
	/* Reports the element stored there. */
	return *(struct dom_element **)wb_vector_at(&p->open, index);
}

/*
 * Reports how many elements are open.
 */
size_t
tb_open_count(
	const struct html_parser *p)
{
	/* Reports the stack's size. */
	return p->open.count;
}

/*
 * Pushes an element onto the stack of open elements.
 */
void
tb_push(
	struct html_parser *p,
	struct dom_element *element)
{
	int error;

	/* Pushes it; running out of memory stops the parse. */
	error = wb_vector_push(&p->open, &element);
	if (error != 0)
		parser_nomem(p);
}

/*
 * Pops the current node (nothing happens when the stack is empty).
 */
void
tb_pop(
	struct html_parser *p)
{
	/* An empty stack has nothing to pop. */
	if (p->open.count == 0)
		return;

	/* Forgets the current node. */
	wb_vector_pop(&p->open);
}

/*
 * Pops elements until an HTML element with a tag has been popped.
 */
void
tb_pop_until_tag(
	struct html_parser *p,
	int tag)
{
	struct dom_element *element;

	/* Pops, stopping after the element with the tag. */
	while (p->open.count > 0) {
		element = tb_current(p);
		tb_pop(p);
		if (element->ns == DOM_NS_HTML && element->tag == tag)
			return;
	}
}

/*
 * Pops elements until a given element has been popped.
 */
void
tb_pop_until_element(
	struct html_parser *p,
	struct dom_element *element)
{
	struct dom_element *popped;

	/* Pops, stopping after the element. */
	while (p->open.count > 0) {
		popped = tb_current(p);
		tb_pop(p);
		if (popped == element)
			return;
	}
}

/*
 * Removes an element from wherever it is in the stack.
 */
void
tb_remove_open(
	struct html_parser *p,
	struct dom_element *element)
{
	struct dom_element **items;
	size_t index;
	int found;

	/* Finds the element and closes the gap it leaves. */
	found = tb_open_index(p, element, &index);
	if (!found)
		return;
	items = p->open.items;
	memmove(&items[index], &items[index + 1U], (p->open.count - index - 1U) * sizeof(*items));
	p->open.count--;
}

/*
 * Finds the place of an element in the stack; returns 0 when it is not
 * there.
 */
int
tb_open_index(
	const struct html_parser *p,
	const struct dom_element *element,
	size_t *index)
{
	const struct dom_element *candidate;
	size_t position;

	/* Searches from the current node up. */
	position = p->open.count;
	while (position > 0) {
		position--;
		candidate = tb_open_at(p, position);
		if (candidate == element) {
			*index = position;
			return 1;
		}
	}

	/* The element is not open. */
	return 0;
}

/*
 * Tells whether the current node is an HTML element with a tag.
 */
int
tb_current_is(
	const struct html_parser *p,
	int tag)
{
	struct dom_element *current;
	int matches;

	/* Compares the current node, if any. */
	current = tb_current(p);
	if (current == NULL)
		return 0;
	matches = dom_element_is(&current->node, DOM_NS_HTML, tag);

	/* Reports the answer. */
	return matches;
}

/*
 * Tells whether the stack has an HTML element with a tag in a scope.
 */
int
tb_in_scope(
	const struct html_parser *p,
	int tag,
	int scope)
{
	struct dom_element *element;
	size_t index;
	int boundary;

	/* Walks from the current node up until the element or a boundary of the scope. */
	index = p->open.count;
	while (index > 0) {
		index--;
		element = tb_open_at(p, index);
		if (element->ns == DOM_NS_HTML && element->tag == tag)
			return 1;

		/* A boundary of the scope ends the search. */
		boundary = parser_scope_boundary(element, scope);
		if (boundary)
			return 0;
	}

	/* The html element is always a boundary, so this is not reached. */
	return 0;
}

/*
 * Tells whether the stack has a given element in a scope.
 */
int
tb_element_in_scope(
	const struct html_parser *p,
	const struct dom_element *target,
	int scope)
{
	struct dom_element *element;
	size_t index;
	int boundary;

	/* Walks from the current node up until the element or a boundary. */
	index = p->open.count;
	while (index > 0) {
		index--;
		element = tb_open_at(p, index);
		if (element == target)
			return 1;

		/* A boundary of the scope ends the search. */
		boundary = parser_scope_boundary(element, scope);
		if (boundary)
			return 0;
	}

	/* The element is not in scope. */
	return 0;
}

/*
 * Tells whether a template element is open.
 */
int
tb_template_on_stack(
	const struct html_parser *p)
{
	int is_element;
	size_t index;

	/* Looks at every open element. */
	for (index = 0; index < p->open.count; index++) {
		/* A template anywhere answers yes. */
		is_element = dom_element_is(&tb_open_at(p, index)->node, DOM_NS_HTML, DOM_TAG_TEMPLATE);
		if (is_element)
			return 1;
	}

	/* No template is open. */
	return 0;
}

/*
 * Tells whether the parser builds template contents, for the form element
 * pointer: a template is open, or a fragment's context is a template (the
 * context is not on the stack, but its contents are what is built;
 * ws074-p081, as the html5lib fragment cases have it).
 */
int
tb_template_contents(
	const struct html_parser *p)
{
	int open;
	int context_template;

	/* A template on the stack. */
	open = tb_template_on_stack(p);
	if (open)
		return 1;

	/* A fragment built in a template. */
	if (p->context == NULL)
		return 0;
	context_template = dom_element_is(&p->context->node, DOM_NS_HTML, DOM_TAG_TEMPLATE);
	if (context_template)
		return 1;

	/* Succeeded: no template contents are built. */
	return 0;
}

/*
 * Tells whether an element is in the standard's special category.
 */
int
tb_is_special(
	const struct dom_element *element)
{
	/* MathML's text elements and annotation-xml are special. */
	if (element->ns == DOM_NS_MATHML) {
		switch (element->tag) {
		case DOM_TAG_MI:
		case DOM_TAG_MO:
		case DOM_TAG_MN:
		case DOM_TAG_MS:
		case DOM_TAG_MTEXT:
		case DOM_TAG_ANNOTATION_XML:
			return 1;
		default:
			return 0;
		}
	}

	/* SVG's foreignObject, desc and title are special. */
	if (element->ns == DOM_NS_SVG) {
		switch (element->tag) {
		case DOM_TAG_FOREIGNOBJECT:
		case DOM_TAG_DESC:
		case DOM_TAG_TITLE:
			return 1;
		default:
			return 0;
		}
	}

	/* Only HTML elements are left. */
	if (element->ns != DOM_NS_HTML)
		return 0;

	/* The HTML elements the standard lists. */
	switch (element->tag) {
	case DOM_TAG_ADDRESS:
	case DOM_TAG_APPLET:
	case DOM_TAG_AREA:
	case DOM_TAG_ARTICLE:
	case DOM_TAG_ASIDE:
	case DOM_TAG_BASE:
	case DOM_TAG_BASEFONT:
	case DOM_TAG_BGSOUND:
	case DOM_TAG_BLOCKQUOTE:
	case DOM_TAG_BODY:
	case DOM_TAG_BR:
	case DOM_TAG_BUTTON:
	case DOM_TAG_CAPTION:
	case DOM_TAG_CENTER:
	case DOM_TAG_COL:
	case DOM_TAG_COLGROUP:
	case DOM_TAG_DD:
	case DOM_TAG_DETAILS:
	case DOM_TAG_DIR:
	case DOM_TAG_DIV:
	case DOM_TAG_DL:
	case DOM_TAG_DT:
	case DOM_TAG_EMBED:
	case DOM_TAG_FIELDSET:
	case DOM_TAG_FIGCAPTION:
	case DOM_TAG_FIGURE:
	case DOM_TAG_FOOTER:
	case DOM_TAG_FORM:
	case DOM_TAG_FRAME:
	case DOM_TAG_FRAMESET:
	case DOM_TAG_H1:
	case DOM_TAG_H2:
	case DOM_TAG_H3:
	case DOM_TAG_H4:
	case DOM_TAG_H5:
	case DOM_TAG_H6:
	case DOM_TAG_HEAD:
	case DOM_TAG_HEADER:
	case DOM_TAG_HGROUP:
	case DOM_TAG_HR:
	case DOM_TAG_HTML:
	case DOM_TAG_IFRAME:
	case DOM_TAG_IMG:
	case DOM_TAG_INPUT:
	case DOM_TAG_KEYGEN:
	case DOM_TAG_LI:
	case DOM_TAG_LINK:
	case DOM_TAG_LISTING:
	case DOM_TAG_MAIN:
	case DOM_TAG_MARQUEE:
	case DOM_TAG_MENU:
	case DOM_TAG_META:
	case DOM_TAG_NAV:
	case DOM_TAG_NOEMBED:
	case DOM_TAG_NOFRAMES:
	case DOM_TAG_NOSCRIPT:
	case DOM_TAG_OBJECT:
	case DOM_TAG_OL:
	case DOM_TAG_P:
	case DOM_TAG_PARAM:
	case DOM_TAG_PLAINTEXT:
	case DOM_TAG_PRE:
	case DOM_TAG_SCRIPT:
	case DOM_TAG_SEARCH:
	case DOM_TAG_SECTION:
	case DOM_TAG_SELECT:
	case DOM_TAG_SOURCE:
	case DOM_TAG_STYLE:
	case DOM_TAG_SUMMARY:
	case DOM_TAG_TABLE:
	case DOM_TAG_TBODY:
	case DOM_TAG_TD:
	case DOM_TAG_TEMPLATE:
	case DOM_TAG_TEXTAREA:
	case DOM_TAG_TFOOT:
	case DOM_TAG_TH:
	case DOM_TAG_THEAD:
	case DOM_TAG_TITLE:
	case DOM_TAG_TR:
	case DOM_TAG_TRACK:
	case DOM_TAG_UL:
	case DOM_TAG_WBR:
	case DOM_TAG_XMP:
		return 1;
	default:
		return 0;
	}
}

/*
 * Pops the elements whose end tags are implied (dd, dt, li, optgroup,
 * option, p, rb, rp, rt, rtc), except one with the tag except (or none when
 * except is DOM_TAG_UNKNOWN).
 */
void
tb_generate_implied_end_tags(
	struct html_parser *p,
	int except)
{
	struct dom_element *current;
	int implied;

	/* Pops while the current node's end tag is implied. */
	for (;;) {
		current = tb_current(p);
		if (current == NULL)
			return;
		implied = parser_is_implied(current, 0);
		if (!implied)
			return;
		if (except != DOM_TAG_UNKNOWN && current->tag == except)
			return;

		/* The element is closed. */
		tb_pop(p);
	}
}

/*
 * Pops the elements whose end tags are implied thoroughly (the table
 * elements as well).
 */
void
tb_generate_all_implied_end_tags(
	struct html_parser *p)
{
	struct dom_element *current;
	int implied;

	/* Pops while the current node's end tag is implied. */
	for (;;) {
		current = tb_current(p);
		if (current == NULL)
			return;
		implied = parser_is_implied(current, 1);
		if (!implied)
			return;

		/* The element is closed. */
		tb_pop(p);
	}
}

/*
 * Closes a p element: implied end tags but p's, then pops up to the p.
 */
void
tb_close_p(
	struct html_parser *p)
{
	int is_p;

	/* Generates the implied end tags except p's. */
	tb_generate_implied_end_tags(p, DOM_TAG_P);

	/* A current node other than the p is an error. */
	is_p = tb_current_is(p, DOM_TAG_P);
	if (!is_p)
		tb_error(p);

	/* Pops up to and including the p. */
	tb_pop_until_tag(p, DOM_TAG_P);
}

/*
 * Creates an element for a tag token in a namespace, with the token's
 * attributes (not foreign-adjusted; foreign.c adjusts its own).
 */
struct dom_element *
tb_create_element(
	struct html_parser *p,
	const struct tb_token *token,
	int ns)
{
	const struct html_token_attribute *attribute;
	struct dom_element *element;
	struct vm_string *local_name;
	struct vm_string *name;
	struct vm_string *value;
	struct dom_attribute *existing;
	struct vm_cell *element_root;
	size_t index;
	int error;

	/* A tag the parser made itself has only its tag number. */
	if (token->raw == NULL) {
		element = tb_create_element_for_tag(p, token->tag);
		return element;
	}

	/* Names the element after the token. */
	local_name = vm_atom_from_units(p->heap, token->raw->name.data, token->raw->name.length);
	if (local_name == NULL) {
		parser_nomem(p);
		return NULL;
	}

	/* Makes the element (a document's parser's own). */
	element = dom_element_create(p->document, ns, local_name, NULL);
	if (element == NULL) {
		parser_nomem(p);
		return NULL;
	}
	if (p->context == NULL)
		element->node.flags |= DOM_NODE_PARSER_CREATED;

	/* The element is detached while its attributes and contents are allocated. */
	element_root = &element->node.cell;
	error = vm_heap_add_root(p->heap, &element_root);
	if (error != 0) {
		parser_nomem(p);
		return NULL;
	}

	/* Keeps the detached element alive while allocating its attribute values. */
	for (index = 0; index < token->raw->attribute_count; index++) {
		attribute = &token->raw->attributes[index];
		if (attribute->dropped)
			continue;

		/* Interns the name and makes the value. */
		name = vm_atom_from_units(p->heap, attribute->name.data, attribute->name.length);
		if (name == NULL) {
			parser_nomem(p);
			goto cleanup;
		}

		/* Makes the value. */
		value = vm_string_from_units(p->heap, attribute->value.data, attribute->value.length);
		if (value == NULL) {
			parser_nomem(p);
			goto cleanup;
		}

		/* A name repeated after the tokenizer's check (it compares only kept names) is skipped. */
		existing = dom_element_find_attribute(element, DOM_NS_NONE, name);
		if (existing != NULL)
			continue;

		/* Adds the attribute. */
		error = dom_element_add_attribute(element, DOM_NS_NONE, NULL, name, value);
		if (error != 0) {
			parser_nomem(p);
			goto cleanup;
		}
	}

	/* A template gets its contents, a fragment of its own. */
	if (ns == DOM_NS_HTML && element->tag == DOM_TAG_TEMPLATE) {
		element->content = dom_fragment_create(p->document);
		if (element->content == NULL)
			parser_nomem(p);
	}

cleanup:
	/* The caller takes the detached element; an attempted parse failure stays recorded. */
	vm_heap_remove_root(p->heap, &element_root);

	/* Succeeded: the element is detached. */
	return element;
}

/*
 * Creates an HTML element with a tag number and no attributes.
 */
struct dom_element *
tb_create_element_for_tag(
	struct html_parser *p,
	int tag)
{
	struct dom_element *element;
	struct vm_string *local_name;
	struct vm_cell *element_root;
	int error;

	/* Interns the tag's name. */
	local_name = vm_atom_from_ascii(p->heap, dom_tag_name(tag));
	if (local_name == NULL) {
		parser_nomem(p);
		return NULL;
	}

	/* Makes the element (a document's parser's own). */
	element = dom_element_create(p->document, DOM_NS_HTML, local_name, NULL);
	if (element == NULL) {
		parser_nomem(p);
		return NULL;
	}
	if (p->context == NULL)
		element->node.flags |= DOM_NODE_PARSER_CREATED;

	/* The element stays live until its template fragment has been allocated. */
	element_root = &element->node.cell;
	error = vm_heap_add_root(p->heap, &element_root);
	if (error != 0) {
		parser_nomem(p);
		return NULL;
	}

	/* A template gets its contents while its detached element remains live. */
	if (tag == DOM_TAG_TEMPLATE) {
		element->content = dom_fragment_create(p->document);
		if (element->content == NULL)
			parser_nomem(p);
	}

	/* The caller takes the detached element. */
	vm_heap_remove_root(p->heap, &element_root);

	/* Succeeded: the element is detached. */
	return element;
}

/*
 * Inserts an element for a tag token at the appropriate place and pushes
 * it (the standard's "insert an HTML element", or a foreign one).
 */
struct dom_element *
tb_insert_element(
	struct html_parser *p,
	const struct tb_token *token,
	int ns)
{
	struct dom_element *element;

	/* Creates the element. */
	element = tb_create_element(p, token, ns);
	if (element == NULL)
		return NULL;

	/* Inserts it and makes it the current node. */
	tb_insert_node(p, &element->node, NULL);
	tb_push(p, element);

	/* Succeeded: the element is the current node. */
	return element;
}

/*
 * Inserts an HTML element for a tag the parser makes itself (an implied
 * <html>, <head>, <body>, <tbody>...).
 */
struct dom_element *
tb_insert_html_tag(
	struct html_parser *p,
	int tag)
{
	struct tb_token token;
	struct dom_element *element;

	/* A tag token without a raw token stands for the bare tag. */
	memset(&token, 0, sizeof(token));
	token.type = HTML_TOKEN_START_TAG;
	token.tag = tag;
	element = tb_insert_element(p, &token, DOM_NS_HTML);

	/* Reports the element (NULL when memory ran out). */
	return element;
}

/*
 * Inserts a node at the appropriate place for inserting a node, with an
 * optional override target, applying foster parenting.
 */
void
tb_insert_node(
	struct html_parser *p,
	struct dom_node *node,
	struct dom_element *override)
{
	struct dom_node *parent;
	struct dom_node *before;

	/* Finds the place and inserts there. */
	parent = parser_insertion_parent(p, override, &before);
	if (parent == NULL)
		return;
	dom_insert_before(parent, node, before);
}

/*
 * Inserts characters at the appropriate place, joining a text node that
 * is already there.
 */
void
tb_insert_characters(
	struct html_parser *p,
	const uint16_t *text,
	size_t length)
{
	struct dom_node *parent;
	struct dom_node *before;
	struct dom_node *previous;
	struct dom_node *node;
	int error;

	/* Nothing to insert is nothing to do. */
	if (length == 0)
		return;

	/* Finds the place; a document takes no text. */
	parent = parser_insertion_parent(p, NULL, &before);
	if (parent == NULL || parent->type == DOM_DOCUMENT)
		return;

	/* Text right before the place grows. */
	previous = parent->last_child;
	if (before != NULL)
		previous = before->previous;
	if (previous != NULL && previous->type == DOM_TEXT) {
		error = dom_text_append(previous, text, length);
		if (error != 0)
			parser_nomem(p);
		return;
	}

	/* Otherwise a new text node goes there. */
	node = dom_text_create(p->document, text, length);
	if (node == NULL) {
		parser_nomem(p);
		return;
	}

	/* Puts the new text node at the place. */
	dom_insert_before(parent, node, before);
}

/*
 * Inserts a comment: as the last child of parent when given, at the
 * appropriate place otherwise.
 */
void
tb_insert_comment(
	struct html_parser *p,
	const struct tb_token *token,
	struct dom_node *parent)
{
	struct dom_node *comment;

	/* Makes the comment node. */
	comment = dom_comment_create(p->document, token->raw->data.data, token->raw->data.length);
	if (comment == NULL) {
		parser_nomem(p);
		return;
	}

	/* Puts it where it goes. */
	if (parent != NULL) {
		dom_append_child(parent, comment);
	} else {
		tb_insert_node(p, comment, NULL);
	}
}

/*
 * Adds to an element the token's attributes it does not have yet (for a
 * second <html> or <body> start tag).
 */
void
tb_add_missing_attributes(
	struct html_parser *p,
	struct dom_element *element,
	const struct tb_token *token)
{
	const struct html_token_attribute *attribute;
	struct dom_attribute *existing;
	struct vm_string *name;
	struct vm_string *value;
	size_t index;
	int error;

	/* A tag the parser made has no attributes. */
	if (token->raw == NULL)
		return;

	/* Adds each attribute the element lacks. */
	for (index = 0; index < token->raw->attribute_count; index++) {
		attribute = &token->raw->attributes[index];
		if (attribute->dropped)
			continue;

		/* Skips a name the element already has. */
		name = vm_atom_from_units(p->heap, attribute->name.data, attribute->name.length);
		if (name == NULL) {
			parser_nomem(p);
			return;
		}

		/* Looks for the name on the element. */
		existing = dom_element_find_attribute(element, DOM_NS_NONE, name);
		if (existing != NULL)
			continue;

		/* Adds it. */
		value = vm_string_from_units(p->heap, attribute->value.data, attribute->value.length);
		if (value == NULL) {
			parser_nomem(p);
			return;
		}

		/* Adds the attribute. */
		error = dom_element_add_attribute(element, DOM_NS_NONE, NULL, name, value);
		if (error != 0) {
			parser_nomem(p);
			return;
		}
	}
}

/*
 * Finds a token's attribute by its (lower case ASCII) name.
 */
const struct html_token_attribute *
tb_token_attribute(
	const struct tb_token *token,
	const char *name)
{
	const struct html_token_attribute *attribute;
	size_t index;
	int same;

	/* A tag the parser made has none. */
	if (token->raw == NULL)
		return NULL;

	/* Compares each kept attribute's name. */
	for (index = 0; index < token->raw->attribute_count; index++) {
		attribute = &token->raw->attributes[index];
		if (attribute->dropped)
			continue;

		/* The first with the name is the attribute. */
		same = tb_units_equal_ascii(attribute->name.data, attribute->name.length, name, 0);
		if (same)
			return attribute;
	}

	/* The token has no such attribute. */
	return NULL;
}

/*
 * Pushes an element onto the list of active formatting elements, keeping
 * at most three equal ones after the last marker (the Noah's Ark clause).
 */
void
tb_push_formatting(
	struct html_parser *p,
	struct dom_element *element)
{
	struct tb_formatting *entries;
	struct tb_formatting entry;
	const struct dom_element *other;
	const struct dom_attribute *found;
	int value_same;
	size_t index;
	size_t earliest;
	size_t attribute;
	int equal_count;
	int same;
	int error;

	/* Counts the equal elements after the last marker, remembering the earliest. */
	entries = p->formatting.items;
	equal_count = 0;
	earliest = 0;
	index = p->formatting.count;
	while (index > 0) {
		index--;
		other = entries[index].element;
		if (other == NULL)
			break;

		/* Equal means the same name, namespace and attributes. */
		if (other->local_name != element->local_name || other->ns != element->ns)
			continue;
		if (other->attribute_count != element->attribute_count)
			continue;
		same = 1;
		for (attribute = 0; attribute < element->attribute_count; attribute++) {
			/* Every attribute must be on the other element with the same value. */
			found = dom_element_find_attribute(other, element->attributes[attribute].ns, element->attributes[attribute].name);
			if (found == NULL) {
				same = 0;
				break;
			}

			/* The values must be equal too. */
			value_same = vm_string_equal(found->value, element->attributes[attribute].value);
			if (!value_same) {
				same = 0;
				break;
			}
		}

		/* Only an equal element counts. */
		if (!same)
			continue;

		/* One more equal element; the earliest so far moves up. */
		equal_count++;
		earliest = index;
	}

	/* A fourth equal element pushes out the earliest. */
	if (equal_count >= 3) {
		memmove(&entries[earliest], &entries[earliest + 1U], (p->formatting.count - earliest - 1U) * sizeof(*entries));
		p->formatting.count--;
	}

	/* Adds the element at the end. */
	entry.element = element;
	error = wb_vector_push(&p->formatting, &entry);
	if (error != 0)
		parser_nomem(p);
}

/*
 * Pushes a marker onto the list of active formatting elements.
 */
void
tb_push_marker(
	struct html_parser *p)
{
	struct tb_formatting entry;
	int error;

	/* A marker is an entry without an element. */
	entry.element = NULL;
	error = wb_vector_push(&p->formatting, &entry);
	if (error != 0)
		parser_nomem(p);
}

/*
 * Clears the list of active formatting elements up to the last marker.
 */
void
tb_clear_formatting_to_marker(
	struct html_parser *p)
{
	struct tb_formatting *entries;
	struct dom_element *element;

	/* Removes entries until a marker has been removed. */
	entries = p->formatting.items;
	while (p->formatting.count > 0) {
		element = entries[p->formatting.count - 1U].element;
		p->formatting.count--;
		if (element == NULL)
			return;
	}
}

/*
 * Reconstructs the active formatting elements: reopens the formatting
 * elements after the last marker that were closed.
 */
void
tb_reconstruct_formatting(
	struct html_parser *p)
{
	struct tb_formatting *entries;
	struct dom_element *clone;
	size_t index;
	size_t unused;
	int open;

	/* Nothing to do for an empty list, or when the last entry is a marker or open. */
	if (p->formatting.count == 0)
		return;
	entries = p->formatting.items;
	index = p->formatting.count - 1U;
	if (entries[index].element == NULL)
		return;
	open = tb_open_index(p, entries[index].element, &unused);
	if (open)
		return;

	/* Rewinds to the entry after the last marker or open element. */
	while (index > 0) {
		if (entries[index - 1U].element == NULL)
			break;
		open = tb_open_index(p, entries[index - 1U].element, &unused);
		if (open)
			break;
		index--;
	}

	/* Advances, reopening each entry as a new element that replaces it. */
	for (; index < p->formatting.count; index++) {
		clone = parser_clone_element(p, entries[index].element);
		if (clone == NULL)
			return;
		tb_insert_node(p, &clone->node, NULL);
		tb_push(p, clone);
		entries = p->formatting.items;
		entries[index].element = clone;
	}
}

/*
 * Removes an element from the list of active formatting elements.
 */
void
tb_remove_formatting(
	struct html_parser *p,
	const struct dom_element *element)
{
	struct tb_formatting *entries;
	size_t index;
	int found;

	/* Finds the element and closes the gap. */
	found = tb_formatting_index(p, element, &index);
	if (!found)
		return;
	entries = p->formatting.items;
	memmove(&entries[index], &entries[index + 1U], (p->formatting.count - index - 1U) * sizeof(*entries));
	p->formatting.count--;
}

/*
 * Finds the place of an element in the list of active formatting
 * elements; returns 0 when it is not there.
 */
int
tb_formatting_index(
	const struct html_parser *p,
	const struct dom_element *element,
	size_t *index)
{
	const struct tb_formatting *entries;
	size_t position;

	/* Searches from the end. */
	entries = p->formatting.items;
	position = p->formatting.count;
	while (position > 0) {
		position--;
		if (entries[position].element == element) {
			*index = position;
			return 1;
		}
	}

	/* The element is not in the list. */
	return 0;
}

/*
 * Finds the last HTML element with a tag after the last marker of the list
 * of active formatting elements, or NULL.
 */
struct dom_element *
tb_formatting_after_marker(
	const struct html_parser *p,
	int tag)
{
	int is_element;
	const struct tb_formatting *entries;
	size_t position;

	/* Searches back to the last marker. */
	entries = p->formatting.items;
	position = p->formatting.count;
	while (position > 0) {
		position--;
		if (entries[position].element == NULL)
			return NULL;
		is_element = dom_element_is(&entries[position].element->node, DOM_NS_HTML, tag);
		if (is_element)
			return entries[position].element;
	}

	/* No such element. */
	return NULL;
}

/*
 * Runs the adoption agency algorithm for an end tag (or a start tag that
 * closes an open one).
 *
 * Returns 1 when the token must be handled as "any other end tag" instead.
 */
int
tb_adoption_agency(
	struct html_parser *p,
	const struct tb_token *token)
{
	struct dom_element *formatting_element;
	struct dom_element *furthest_block;
	struct dom_element *common_ancestor;
	struct dom_element *node;
	struct dom_element *last_node;
	struct dom_element *clone;
	struct dom_element *current;
	struct dom_node *child;
	size_t formatting_open;
	size_t node_index;
	size_t bookmark;
	size_t list_index;
	size_t index;
	int outer;
	int inner;
	int found;
	int in_list;
	int is_element;

	/* The current node with the tag and not in the list is simply popped. */
	current = tb_current(p);
	is_element = 0;
	if (current != NULL)
		is_element = dom_element_is(&current->node, DOM_NS_HTML, token->tag);
	if (is_element) {
		found = tb_formatting_index(p, current, &list_index);
		if (!found) {
			tb_pop(p);
			return 0;
		}
	}

	/* The outer loop runs at most eight times. */
	for (outer = 0; outer < ADOPTION_OUTER_LIMIT; outer++) {
		/* The formatting element is the last with the tag after the last marker. */
		formatting_element = tb_formatting_after_marker(p, token->tag);
		if (formatting_element == NULL)
			return 1;

		/* It must be open, and in scope. */
		found = tb_open_index(p, formatting_element, &formatting_open);
		if (!found) {
			tb_error(p);
			tb_remove_formatting(p, formatting_element);
			return 0;
		}

		/* And in scope. */
		found = tb_element_in_scope(p, formatting_element, TB_SCOPE_DEFAULT);
		if (!found) {
			tb_error(p);
			return 0;
		}

		/* It should be the current node. */
		current = tb_current(p);
		if (formatting_element != current)
			tb_error(p);

		/* The furthest block is the topmost special element below the formatting element. */
		furthest_block = NULL;
		for (index = formatting_open + 1U; index < p->open.count; index++) {
			node = tb_open_at(p, index);
			found = tb_is_special(node);
			if (found) {
				furthest_block = node;
				break;
			}
		}

		/* Without one, the formatting element and what is below it are simply closed. */
		if (furthest_block == NULL) {
			tb_pop_until_element(p, formatting_element);
			tb_remove_formatting(p, formatting_element);
			return 0;
		}

		/* The common ancestor is right above the formatting element; the bookmark is its place. */
		common_ancestor = tb_open_at(p, formatting_open - 1U);
		tb_formatting_index(p, formatting_element, &bookmark);

		/* The inner loop walks up from the furthest block to the formatting element. */
		node = furthest_block;
		last_node = furthest_block;
		tb_open_index(p, furthest_block, &node_index);
		for (inner = 1;; inner++) {
			/* The node above (the stack may have lost the one below since). */
			node_index--;
			node = tb_open_at(p, node_index);
			if (node == formatting_element)
				break;

			/* Past three rounds a formatting node leaves the list. */
			in_list = tb_formatting_index(p, node, &list_index);
			if (inner > ADOPTION_INNER_LIMIT && in_list) {
				tb_remove_formatting(p, node);
				if (list_index < bookmark)
					bookmark--;
				in_list = 0;
			}

			/* A node not in the list leaves the stack. */
			if (!in_list) {
				tb_remove_open(p, node);
				continue;
			}

			/* The node is replaced by a new element in the list and the stack. */
			clone = parser_clone_element(p, node);
			if (clone == NULL)
				return 0;
			parser_set_formatting(p, list_index, clone);
			parser_set_open(p, node_index, clone);
			node = clone;

			/* The first replacement moves the bookmark after the node. */
			if (last_node == furthest_block)
				bookmark = list_index + 1U;

			/* The last node moves into the node. */
			dom_append_child(&node->node, &last_node->node);
			last_node = node;
		}

		/* The last node goes where the common ancestor takes children. */
		tb_insert_node(p, &last_node->node, common_ancestor);

		/* A new formatting element takes the furthest block's children and goes into it. */
		clone = parser_clone_element(p, formatting_element);
		if (clone == NULL)
			return 0;
		while (furthest_block->node.first_child != NULL) {
			child = furthest_block->node.first_child;
			dom_append_child(&clone->node, child);
		}

		/* The new element becomes the furthest block's only child. */
		dom_append_child(&furthest_block->node, &clone->node);

		/* The new element replaces the formatting element in the list, at the bookmark. */
		tb_formatting_index(p, formatting_element, &list_index);
		tb_remove_formatting(p, formatting_element);
		if (list_index < bookmark)
			bookmark--;
		parser_insert_formatting(p, bookmark, clone);

		/* And in the stack, right below the furthest block. */
		tb_remove_open(p, formatting_element);
		tb_open_index(p, furthest_block, &index);
		parser_insert_open(p, index + 1U, clone);
	}

	/* The outer loop ran out. */
	return 0;
}

/*
 * Resets the insertion mode appropriately, from the stack of open
 * elements.
 */
void
tb_reset_insertion_mode(
	struct html_parser *p)
{
	struct dom_element *node;
	size_t index;
	int last;

	/* Walks up from the current node. */
	index = p->open.count;
	while (index > 0) {
		index--;
		node = tb_open_at(p, index);
		last = 0;
		if (index == 0) {
			last = 1;
			if (p->context != NULL)
				node = p->context;
		}

		/* Only HTML elements choose a mode. */
		if (node->ns != DOM_NS_HTML) {
			if (last) {
				p->mode = TB_IN_BODY;
				return;
			}

			continue;
		}

		/* The element's mode. */
		switch (node->tag) {
		case DOM_TAG_TD:
		case DOM_TAG_TH:
			if (!last) {
				p->mode = TB_IN_CELL;
				return;
			}

			break;
		case DOM_TAG_TR:
			p->mode = TB_IN_ROW;
			return;
		case DOM_TAG_TBODY:
		case DOM_TAG_THEAD:
		case DOM_TAG_TFOOT:
			p->mode = TB_IN_TABLE_BODY;
			return;
		case DOM_TAG_CAPTION:
			p->mode = TB_IN_CAPTION;
			return;
		case DOM_TAG_COLGROUP:
			p->mode = TB_IN_COLUMN_GROUP;
			return;
		case DOM_TAG_TABLE:
			p->mode = TB_IN_TABLE;
			return;
		case DOM_TAG_TEMPLATE:
			p->mode = tb_current_template_mode(p);
			return;
		case DOM_TAG_HEAD:
			if (!last) {
				p->mode = TB_IN_HEAD;
				return;
			}

			break;
		case DOM_TAG_BODY:
			p->mode = TB_IN_BODY;
			return;
		case DOM_TAG_FRAMESET:
			p->mode = TB_IN_FRAMESET;
			return;
		case DOM_TAG_HTML:
			if (p->head == NULL) {
				p->mode = TB_BEFORE_HEAD;
			} else {
				p->mode = TB_AFTER_HEAD;
			}

			/* The mode of the root is chosen. */
			return;
		default:
			break;
		}

		/* The top of the stack without a mode of its own means in body. */
		if (last) {
			p->mode = TB_IN_BODY;
			return;
		}
	}

	/* An empty stack (not reached while parsing) is in body. */
	p->mode = TB_IN_BODY;
}

/*
 * Pushes a mode onto the stack of template insertion modes.
 */
void
tb_push_template_mode(
	struct html_parser *p,
	int mode)
{
	int error;

	/* Pushes it; running out of memory stops the parse. */
	error = wb_vector_push(&p->template_modes, &mode);
	if (error != 0)
		parser_nomem(p);
}

/*
 * Pops the current template insertion mode.
 */
void
tb_pop_template_mode(
	struct html_parser *p)
{
	/* An empty stack has nothing to pop. */
	if (p->template_modes.count == 0)
		return;

	/* Forgets the current template mode. */
	wb_vector_pop(&p->template_modes);
}

/*
 * Reports the current template insertion mode (in body when none).
 */
int
tb_current_template_mode(
	const struct html_parser *p)
{
	/* Without template modes, in body. */
	if (p->template_modes.count == 0)
		return TB_IN_BODY;

	/* The last pushed mode. */
	return *(int *)wb_vector_at(&p->template_modes, p->template_modes.count - 1U);
}

/*
 * Runs the generic raw text or RCDATA element parsing algorithm: inserts
 * the element and reads its contents as text.
 */
void
tb_generic_text(
	struct html_parser *p,
	const struct tb_token *token,
	enum html_tokenizer_start state)
{
	/* Inserts the element, switches the tokenizer and remembers the mode to return to. */
	tb_insert_element(p, token, DOM_NS_HTML);
	html_tokenizer_set_state(&p->tokenizer, state);
	p->original_mode = p->mode;
	p->mode = TB_TEXT;
}

/*
 * Stops parsing: the stack is emptied and no token is taken any more.
 */
void
tb_stop(
	struct html_parser *p)
{
	/* Pops everything and stops. */
	wb_vector_clear(&p->open);
	p->stopped = 1;
}

/*
 * Tells whether UTF-16 units spell an ASCII string, folding ASCII case
 * when asked.
 */
int
tb_units_equal_ascii(
	const uint16_t *units,
	size_t length,
	const char *ascii,
	int fold_case)
{
	uint16_t unit;
	size_t index;

	/* Compares unit by unit, including the end. */
	for (index = 0; index < length; index++) {
		if (ascii[index] == '\0')
			return 0;
		unit = units[index];
		if (fold_case && unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (unit != (unsigned char)ascii[index])
			return 0;
	}

	/* The ASCII string must end here too. */
	if (ascii[length] != '\0')
		return 0;

	/* The two are equal. */
	return 1;
}

/* Marks the elements the parser's stacks and pointers hold. */
static void
parser_trace(
	struct vm_heap *heap,
	void *context)
{
	struct html_parser *p;
	struct tb_formatting *entries;
	size_t index;

	/* Marks the document, the pointers and the fragment's context. */
	p = context;
	vm_heap_mark(heap, &p->document->node.cell);
	if (p->head != NULL)
		vm_heap_mark(heap, &p->head->node.cell);
	if (p->form != NULL)
		vm_heap_mark(heap, &p->form->node.cell);
	if (p->context != NULL)
		vm_heap_mark(heap, &p->context->node.cell);
	if (p->fragment_root != NULL)
		vm_heap_mark(heap, &p->fragment_root->node.cell);

	/* Marks the open elements and the formatting elements. */
	for (index = 0; index < p->open.count; index++)
		vm_heap_mark(heap, &tb_open_at(p, index)->node.cell);
	entries = p->formatting.items;
	for (index = 0; index < p->formatting.count; index++) {
		if (entries[index].element != NULL)
			vm_heap_mark(heap, &entries[index].element->node.cell);
	}
}

/* Pulls tokens and processes them until the tokenizer waits for input or parsing stops. */
static int
parser_run(
	struct html_parser *p)
{
	const struct html_token *raw;
	struct tb_token token;
	struct dom_element *adjusted;
	enum html_token_type type;

	/* Takes one token after another. */
	while (!p->stopped && !p->failed) {
		/* A CDATA section may only open in foreign content. */
		adjusted = tb_adjusted_current(p);
		p->tokenizer.allow_cdata = 0;
		if (adjusted != NULL && adjusted->ns != DOM_NS_HTML)
			p->tokenizer.allow_cdata = 1;

		/* Takes the next token; none means the input ran out for now. */
		type = html_tokenizer_next(&p->tokenizer, &raw);
		if (type == HTML_TOKEN_NONE)
			break;

		/* Character runs are split by kind. */
		if (type == HTML_TOKEN_CHARACTERS) {
			parser_dispatch_characters(p, raw);
			continue;
		}

		/* Any other token loses the pending line feed skip. */
		p->skip_newline = 0;
		memset(&token, 0, sizeof(token));
		token.type = type;
		token.raw = raw;
		token.tag = DOM_TAG_UNKNOWN;
		if (type == HTML_TOKEN_START_TAG || type == HTML_TOKEN_END_TAG)
			token.tag = dom_tag_lookup(raw->name.data, raw->name.length);
		tb_process(p, &token);

		/* End of file ends the run. */
		if (type == HTML_TOKEN_EOF)
			break;
	}

	/* Running out of memory is the one failure. */
	if (p->failed)
		return ENOMEM;

	/* Succeeded: the input is used up. */
	return 0;
}

/* Splits a character run into runs of one kind (whitespace, NULs, others) and processes each. */
static void
parser_dispatch_characters(
	struct html_parser *p,
	const struct html_token *raw)
{
	struct tb_token token;
	const uint16_t *text;
	size_t length;
	size_t start;
	size_t end;
	int next_kind;
	int kind;

	/* A line feed right after <pre>, <listing> or <textarea> is dropped. */
	text = raw->data.data;
	length = raw->data.length;
	if (p->skip_newline) {
		p->skip_newline = 0;
		if (length > 0 && text[0] == 0x0aU) {
			text++;
			length--;
		}
	}

	/* Processes each run of characters of one kind. */
	start = 0;
	while (start < length && !p->stopped) {
		kind = parser_text_kind(text[start]);
		end = start + 1U;
		while (end < length) {
			/* The run ends at the first character of another kind. */
			next_kind = parser_text_kind(text[end]);
			if (next_kind != kind)
				break;

			/* The character belongs to the run. */
			end++;
		}

		/* The run goes through the dispatcher as one token. */
		memset(&token, 0, sizeof(token));
		token.type = HTML_TOKEN_CHARACTERS;
		token.tag = kind;
		token.raw = raw;
		token.text = text + start;
		token.length = end - start;
		tb_process(p, &token);
		start = end;
	}
}

/* Classifies a character for the modes: whitespace, NUL or other. */
static int
parser_text_kind(
	uint16_t unit)
{
	/* The tree builder's whitespace: tab, line feed, form feed, carriage return and space. */
	if (unit == 0x09U || unit == 0x0aU || unit == 0x0cU || unit == 0x0dU || unit == 0x20U)
		return TB_TEXT_WHITESPACE;

	/* The NUL character. */
	if (unit == 0)
		return TB_TEXT_NULL;

	/* Anything else. */
	return TB_TEXT_OTHER;
}

/* Tells whether an element bounds a scope (ends the search for an element in it). */
static int
parser_scope_boundary(
	const struct dom_element *element,
	int scope)
{
	/* The select scope is bounded by everything but optgroup and option. */
	if (scope == TB_SCOPE_SELECT) {
		if (element->ns == DOM_NS_HTML && (element->tag == DOM_TAG_OPTGROUP || element->tag == DOM_TAG_OPTION))
			return 0;
		return 1;
	}

	/* The table scope is bounded by html, table and template. */
	if (scope == TB_SCOPE_TABLE) {
		if (element->ns != DOM_NS_HTML)
			return 0;
		if (element->tag == DOM_TAG_HTML || element->tag == DOM_TAG_TABLE || element->tag == DOM_TAG_TEMPLATE)
			return 1;
		return 0;
	}

	/* The list item and button scopes add their elements to the default ones. */
	if (element->ns == DOM_NS_HTML) {
		if (scope == TB_SCOPE_LIST_ITEM && (element->tag == DOM_TAG_OL || element->tag == DOM_TAG_UL))
			return 1;
		if (scope == TB_SCOPE_BUTTON && element->tag == DOM_TAG_BUTTON)
			return 1;
	}

	/* The default scope's boundaries. */
	if (element->ns == DOM_NS_HTML) {
		switch (element->tag) {
		case DOM_TAG_APPLET:
		case DOM_TAG_CAPTION:
		case DOM_TAG_HTML:
		case DOM_TAG_TABLE:
		case DOM_TAG_TD:
		case DOM_TAG_TH:
		case DOM_TAG_MARQUEE:
		case DOM_TAG_OBJECT:
		case DOM_TAG_TEMPLATE:
			return 1;
		default:
			return 0;
		}
	}

	/* MathML's text elements and annotation-xml bound the default scopes. */
	if (element->ns == DOM_NS_MATHML) {
		switch (element->tag) {
		case DOM_TAG_MI:
		case DOM_TAG_MO:
		case DOM_TAG_MN:
		case DOM_TAG_MS:
		case DOM_TAG_MTEXT:
		case DOM_TAG_ANNOTATION_XML:
			return 1;
		default:
			return 0;
		}
	}

	/* SVG's foreignObject, desc and title bound them too. */
	if (element->ns == DOM_NS_SVG) {
		switch (element->tag) {
		case DOM_TAG_FOREIGNOBJECT:
		case DOM_TAG_DESC:
		case DOM_TAG_TITLE:
			return 1;
		default:
			return 0;
		}
	}

	/* Anything else is inside the scope. */
	return 0;
}

/* Tells whether an element's end tag is implied (thoroughly: the table elements too). */
static int
parser_is_implied(
	const struct dom_element *element,
	int thorough)
{
	/* Only HTML elements have implied end tags. */
	if (element->ns != DOM_NS_HTML)
		return 0;

	/* The elements whose end tags are always implied. */
	switch (element->tag) {
	case DOM_TAG_DD:
	case DOM_TAG_DT:
	case DOM_TAG_LI:
	case DOM_TAG_OPTGROUP:
	case DOM_TAG_OPTION:
	case DOM_TAG_P:
	case DOM_TAG_RB:
	case DOM_TAG_RP:
	case DOM_TAG_RT:
	case DOM_TAG_RTC:
		return 1;
	case DOM_TAG_CAPTION:
	case DOM_TAG_COLGROUP:
	case DOM_TAG_TBODY:
	case DOM_TAG_TD:
	case DOM_TAG_TFOOT:
	case DOM_TAG_TH:
	case DOM_TAG_THEAD:
	case DOM_TAG_TR:
		return thorough;
	default:
		return 0;
	}
}

/*
 * Finds the appropriate place for inserting a node: the parent, and the
 * child to insert before (NULL for the end).  Applies foster parenting and
 * redirects a template to its contents.
 */
static struct dom_node *
parser_insertion_parent(
	struct html_parser *p,
	struct dom_element *override,
	struct dom_node **before)
{
	int is_element;
	struct dom_element *target;
	struct dom_element *last_template;
	struct dom_element *last_table;
	struct dom_element *element;
	struct dom_node *parent;
	size_t template_index;
	size_t table_index;
	size_t index;
	int foster;

	/* The target is the override, or the current node. */
	target = override;
	if (target == NULL)
		target = tb_current(p);
	*before = NULL;
	if (target == NULL)
		return &p->document->node;

	/* Foster parenting applies to table elements while it is on. */
	foster = 0;
	if (p->foster_parenting && target->ns == DOM_NS_HTML) {
		switch (target->tag) {
		case DOM_TAG_TABLE:
		case DOM_TAG_TBODY:
		case DOM_TAG_TFOOT:
		case DOM_TAG_THEAD:
		case DOM_TAG_TR:
			foster = 1;
			break;
		default:
			break;
		}
	}

	/* Without foster parenting the target takes the node at its end. */
	if (!foster) {
		parent = parser_template_contents(&target->node);
		return parent;
	}

	/* Finds the last template and the last table in the stack. */
	last_template = NULL;
	last_table = NULL;
	template_index = 0;
	table_index = 0;
	for (index = 0; index < p->open.count; index++) {
		element = tb_open_at(p, index);
		is_element = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TEMPLATE);
		if (is_element) {
			last_template = element;
			template_index = index;
		}

		/* And the last table. */
		is_element = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TABLE);
		if (is_element) {
			last_table = element;
			table_index = index;
		}
	}

	/* A template lower than the last table takes the node at the end of its contents. */
	if (last_template != NULL &&
	    (last_table == NULL ||
	     template_index > table_index)) {
		parent = parser_template_contents(&last_template->node);
		return parent;
	}

	/* Without a table (the fragment case) the html element takes it. */
	if (last_table == NULL) {
		parent = parser_template_contents(&tb_open_at(p, 0)->node);
		return parent;
	}

	/* A table with a parent: the node goes right before the table. */
	if (last_table->node.parent != NULL) {
		*before = &last_table->node;
		return last_table->node.parent;
	}

	/* Otherwise the element above the table takes it at its end. */
	parent = parser_template_contents(&tb_open_at(p, table_index - 1U)->node);

	/* Reports the place. */
	return parent;
}

/* Redirects an insertion into a template to the template's contents. */
static struct dom_node *
parser_template_contents(
	struct dom_node *parent)
{
	struct dom_element *element;
	int is_template;

	/* A template's children go into its contents. */
	is_template = dom_element_is(parent, DOM_NS_HTML, DOM_TAG_TEMPLATE);
	if (is_template) {
		element = (struct dom_element *)parent;
		if (element->content != NULL)
			return element->content;
	}

	/* Anything else takes the node itself. */
	return parent;
}

/* Replaces the element at a place of the stack. */
static void
parser_set_open(
	struct html_parser *p,
	size_t index,
	struct dom_element *element)
{
	/* Stores the element there. */
	*(struct dom_element **)wb_vector_at(&p->open, index) = element;
}

/* Inserts an element into the stack at a place, moving the ones below down. */
static void
parser_insert_open(
	struct html_parser *p,
	size_t index,
	struct dom_element *element)
{
	struct dom_element **items;
	int error;

	/* Grows the stack by one, then shifts and stores. */
	error = wb_vector_push(&p->open, &element);
	if (error != 0) {
		parser_nomem(p);
		return;
	}

	/* Shifts the elements below the place down and stores the element. */
	items = p->open.items;
	memmove(&items[index + 1U], &items[index], (p->open.count - index - 1U) * sizeof(*items));
	items[index] = element;
}

/* Replaces the element at a place of the list of active formatting elements. */
static void
parser_set_formatting(
	struct html_parser *p,
	size_t index,
	struct dom_element *element)
{
	struct tb_formatting *entries;

	/* Stores the element there. */
	entries = p->formatting.items;
	entries[index].element = element;
}

/* Inserts an element into the list of active formatting elements at a place. */
static void
parser_insert_formatting(
	struct html_parser *p,
	size_t index,
	struct dom_element *element)
{
	struct tb_formatting *entries;
	struct tb_formatting entry;
	int error;

	/* Grows the list by one, then shifts and stores. */
	entry.element = element;
	error = wb_vector_push(&p->formatting, &entry);
	if (error != 0) {
		parser_nomem(p);
		return;
	}

	/* Shifts the entries after the place and stores the new one. */
	entries = p->formatting.items;
	memmove(&entries[index + 1U], &entries[index], (p->formatting.count - index - 1U) * sizeof(*entries));
	entries[index] = entry;
}

/* Creates a new element like an existing one (for the token it was created for). */
static struct dom_element *
parser_clone_element(
	struct html_parser *p,
	const struct dom_element *element)
{
	struct dom_element *clone;
	size_t index;
	int error;

	/* Makes an element of the same name and namespace. */
	clone = dom_element_create(p->document, element->ns, element->local_name, element->prefix);
	if (clone == NULL) {
		parser_nomem(p);
		return NULL;
	}
	if (p->context == NULL)
		clone->node.flags |= DOM_NODE_PARSER_CREATED;

	/* Copies the attributes. */
	for (index = 0; index < element->attribute_count; index++) {
		error = dom_element_add_attribute(clone, element->attributes[index].ns, element->attributes[index].prefix,
		    element->attributes[index].name, element->attributes[index].value);
		if (error != 0) {
			parser_nomem(p);
			return NULL;
		}

		/* Attribute namespace identity survives copying before another parser allocation. */
		clone->attributes[index].namespace_uri = element->attributes[index].namespace_uri;
	}

	/* Succeeded: the clone is detached. */
	return clone;
}

/* Records that memory ran out: the parse stops. */
static void
parser_nomem(
	struct html_parser *p)
{
	/* The run loop reports the failure. */
	p->failed = 1;
	p->stopped = 1;
}

/*
 * Chooses the tokenizer's state for a fragment's context: RCDATA in title
 * and textarea, RAWTEXT in style, xmp, iframe, noembed and noframes (and
 * noscript when scripting), script data in script, PLAINTEXT in plaintext,
 * and data anywhere else (a context of another namespace too).
 */
static int
parser_fragment_state(
	const struct dom_element *context,
	int scripting)
{
	/* Only HTML elements change the state. */
	if (context->ns != DOM_NS_HTML)
		return HTML_TOKENIZE_DATA;

	/* Chooses by the element. */
	switch (context->tag) {
	case DOM_TAG_TITLE:
	case DOM_TAG_TEXTAREA:
		return HTML_TOKENIZE_RCDATA;
	case DOM_TAG_STYLE:
	case DOM_TAG_XMP:
	case DOM_TAG_IFRAME:
	case DOM_TAG_NOEMBED:
	case DOM_TAG_NOFRAMES:
		return HTML_TOKENIZE_RAWTEXT;
	case DOM_TAG_NOSCRIPT:
		if (scripting)
			return HTML_TOKENIZE_RAWTEXT;
		return HTML_TOKENIZE_DATA;
	case DOM_TAG_SCRIPT:
		return HTML_TOKENIZE_SCRIPT_DATA;
	case DOM_TAG_PLAINTEXT:
		return HTML_TOKENIZE_PLAINTEXT;
	default:
		break;
	}

	/* Any other element. */
	return HTML_TOKENIZE_DATA;
}
