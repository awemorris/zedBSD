/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the record of DOM changes (dom/change.c): the bits a
 * change leaves on the nodes, the summary it leaves in the document, the
 * order the style pass takes them in, and what the record leaves alone.
 * The expected values come from the contract of dom_note_change.
 *
 *   build/ws074-host/<variant>/host-change
 */

#include "dom/dom.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;
static struct vm_heap *heap;
static struct dom_document *document;

/* The nodes of the test tree, which is  html > (head > style, body > (div#a > (span, p), div#b)). */
static struct dom_element *html;
static struct dom_element *head;
static struct dom_element *style;
static struct dom_element *body;
static struct dom_element *div_a;
static struct dom_element *span;
static struct dom_element *paragraph;
static struct dom_element *div_b;

/* What the visitor was given. */
static struct dom_node *visited[16];
static unsigned visited_bits[16];
static int visited_count;

static void check(int condition, const char *what);
static struct vm_string *text(const char *ascii);
static struct dom_element *make(const char *name);
static void build(void);
static void settle(void);
static int visit_record(void *context, struct dom_node *node, unsigned bits);
static int visit_stop(void *context, struct dom_node *node, unsigned bits);
static int styled_never(void *context, const struct vm_string *name);
static void set_attribute(struct dom_element *element, const char *name, const char *value);
static unsigned all_bits(void);
static void test_attribute(void);
static void test_children(void);
static void test_text(void);
static void test_sheets(void);
static void test_detached(void);
static void test_quiet(void);
static void test_kinds(void);
static void test_shadow(void);
static void test_visit(void);
static void test_limit(void);

int
main(
	void)
{
	int status;

	/* A heap and a document, with the tree built and settled. */
	status = vm_heap_create(&heap, 0);
	if (status != 0)
		return 2;
	vm_heap_set_stack_base(heap, __builtin_frame_address(0));
	document = dom_document_create(heap);
	if (document == NULL)
		return 2;
	build();

	test_attribute();
	test_children();
	test_text();
	test_sheets();
	test_detached();
	test_quiet();
	test_kinds();
	test_shadow();
	test_visit();
	test_limit();

	printf("change: %d/%d passed\n", checks - failures, checks);
	vm_heap_destroy(heap);
	return failures == 0 ? 0 : 1;
}

/* Counts a check and tells a failed one. */
static void
check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		fprintf(stderr, "FAIL %s\n", what);
	}
}

/* Makes a string. */
static struct vm_string *
text(
	const char *ascii)
{
	return vm_string_from_utf8(heap, ascii, strlen(ascii));
}

/* Makes an HTML element. */
static struct dom_element *
make(
	const char *name)
{
	return dom_element_create(document, DOM_NS_HTML, vm_atom_from_ascii(heap, name), NULL);
}

/* Builds the tree and settles it: no bit set, no summary. */
static void
build(void)
{
	html = make("html");
	head = make("head");
	style = make("style");
	body = make("body");
	div_a = make("div");
	span = make("span");
	paragraph = make("p");
	div_b = make("div");
	dom_append_child(&document->node, &html->node);
	dom_append_child(&html->node, &head->node);
	dom_append_child(&head->node, &style->node);
	dom_append_child(&html->node, &body->node);
	dom_append_child(&body->node, &div_a->node);
	dom_append_child(&div_a->node, &span->node);
	dom_append_child(&div_a->node, &paragraph->node);
	dom_append_child(&body->node, &div_b->node);
	settle();
}

/* Takes every record: the style bits and the layout bits, and the summary. */
static void
settle(void)
{
	(void)dom_changes_take_style(document, NULL, NULL);
	dom_changes_clear_layout(document);
}

/* Records the node and bits a visit was given. */
static int
visit_record(
	void *context,
	struct dom_node *node,
	unsigned bits)
{
	(void)context;
	if (visited_count < 16) {
		visited[visited_count] = node;
		visited_bits[visited_count] = bits;
	}
	visited_count++;
	return 0;
}

/* Stops at the first node. */
static int
visit_stop(
	void *context,
	struct dom_node *node,
	unsigned bits)
{
	(void)context;
	(void)node;
	(void)bits;
	visited_count++;
	return 7;
}

/* Says no style sheet names any attribute. */
static int
styled_never(
	void *context,
	const struct vm_string *name)
{
	(void)context;
	(void)name;
	return 0;
}

/* Sets an attribute of an element. */
static void
set_attribute(
	struct dom_element *element,
	const char *name,
	const char *value)
{
	(void)dom_element_set_attribute(element, vm_atom_from_ascii(heap, name), text(value));
}

/* The union of the change bits of every node of the test tree and the document. */
static unsigned
all_bits(void)
{
	return document->node.change | html->node.change | head->node.change | style->node.change | body->node.change |
	    div_a->node.change | span->node.change | paragraph->node.change | div_b->node.change;
}

/* An attribute: the element's subtree, siblings and layout, and the way down to it. */
static void
test_attribute(void)
{
	uint32_t generation;

	check(all_bits() == 0U, "settled tree has no bits");
	generation = document->generation;
	set_attribute(span, "class", "x");
	check(document->generation == generation + 1U, "attribute grows generation by one");
	check((span->node.change & (DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_STYLE_SIBLINGS | DOM_CHANGED_LAYOUT)) ==
	    (DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_STYLE_SIBLINGS | DOM_CHANGED_LAYOUT), "attribute: element bits");
	check((div_a->node.change & (DOM_CHANGED_STYLE_BELOW | DOM_CHANGED_LAYOUT_BELOW)) ==
	    (DOM_CHANGED_STYLE_BELOW | DOM_CHANGED_LAYOUT_BELOW), "attribute: parent below bits");
	check((document->node.change & DOM_CHANGED_STYLE_BELOW) != 0U, "attribute: document below bit");
	check((body->node.change & DOM_CHANGED_STYLE_BITS) == 0U && (paragraph->node.change & DOM_CHANGED_STYLE_BITS) == 0U &&
	    (div_b->node.change & DOM_CHANGED_STYLE_BITS) == 0U, "attribute: no bits elsewhere");
	check(document->change_sheets == 0U && document->change_full_style == 0U && document->change_full_layout == 0U,
	    "attribute: no summary flags");
	check(!dom_change_needs_sheets(document), "attribute: sheets not needed");
	settle();
	check(all_bits() == 0U, "settle clears every bit");
}

/* Children: the parent and the node that came in; a node that goes out loses its bits. */
static void
test_children(void)
{
	struct dom_element *em;
	uint32_t generation;
	unsigned both;

	both = DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_LAYOUT;
	em = make("em");
	generation = document->generation;
	dom_append_child(&div_b->node, &em->node);
	check(document->generation == generation + 1U, "insertion grows generation by one");
	check((div_b->node.change & both) == both, "insertion: parent bits");
	check((em->node.change & both) == both, "insertion: child bits");
	check((body->node.change & DOM_CHANGED_STYLE_BELOW) != 0U, "insertion: below bits");
	check(!dom_change_needs_sheets(document), "insertion of an em needs no sheets");

	/* The child's own subtree bits go when it leaves; the old parent is marked. */
	settle();
	set_attribute(em, "id", "e");
	generation = document->generation;
	dom_remove(&em->node);
	check(document->generation == generation + 1U, "removal grows generation by one");
	check((div_b->node.change & both) == both, "removal: parent bits");
	check(em->node.change == 0U, "removal: removed node's bits are gone");
	settle();

	/* A moved node carries no stale bits to its new place, and is marked there. */
	dom_append_child(&div_a->node, &em->node);
	check((div_a->node.change & both) == both && (em->node.change & both) == both, "move: marked at the new place");
	settle();
	dom_remove(&em->node);
	settle();
}

/* Text: the text's own box and its parent's style (:empty) and layout; a comment leaves nothing. */
static void
test_text(void)
{
	static const uint16_t units[2] = { 'h', 'i' };
	struct dom_node *node;
	struct dom_node *comment;
	uint32_t generation;
	int error;

	node = dom_text_create(document, units, 2);
	dom_append_child(&span->node, node);
	settle();
	generation = document->generation;
	error = dom_text_append(node, units, 2);
	check(error == 0, "text append succeeds");
	check(document->generation == generation + 1U, "text grows generation by one");
	check((node->change & DOM_CHANGED_LAYOUT) != 0U, "text: its own layout bit");
	check((span->node.change & (DOM_CHANGED_STYLE | DOM_CHANGED_LAYOUT)) == (DOM_CHANGED_STYLE | DOM_CHANGED_LAYOUT),
	    "text: parent style and layout bits");
	check((span->node.change & DOM_CHANGED_STYLE_SUBTREE) == 0U, "text: not the parent's whole subtree");
	settle();

	/* A comment's characters change the generation and nothing else. */
	comment = dom_comment_create(document, units, 2);
	dom_append_child(&span->node, comment);
	settle();
	generation = document->generation;
	error = dom_text_append(comment, units, 2);
	check(error == 0 && document->generation == generation + 1U, "comment grows generation by one");
	check(all_bits() == 0U && comment->change == 0U, "comment leaves no bits");
	dom_remove(comment);
	dom_remove(node);
	settle();
}

/* The sheets: a style or link element changing, going in or out, or its text. */
static void
test_sheets(void)
{
	static const uint16_t units[1] = { 'a' };
	struct dom_element *link;
	struct dom_element *wrap;
	struct dom_node *node;

	/* An attribute of the style element. */
	set_attribute(style, "media", "print");
	check(dom_change_needs_sheets(document), "style attribute needs sheets");
	check((style->node.change & DOM_CHANGED_STYLE_SUBTREE) != 0U, "style attribute: bits too");
	settle();
	check(!dom_change_needs_sheets(document), "settle clears the sheets flag");

	/* Its text. */
	node = dom_text_create(document, units, 1);
	dom_append_child(&style->node, node);
	check(dom_change_needs_sheets(document), "text child of style needs sheets");
	settle();
	(void)dom_text_append(node, units, 1);
	check(dom_change_needs_sheets(document), "style text change needs sheets");
	settle();
	dom_remove(node);
	check(dom_change_needs_sheets(document), "removal from style needs sheets");
	settle();

	/* A link element, alone and inside a subtree. */
	link = make("link");
	dom_append_child(&head->node, &link->node);
	check(dom_change_needs_sheets(document), "link insertion needs sheets");
	settle();
	set_attribute(link, "href", "a.css");
	check(dom_change_needs_sheets(document), "link attribute needs sheets");
	settle();
	dom_remove(&link->node);
	check(dom_change_needs_sheets(document), "link removal needs sheets");
	settle();
	wrap = make("div");
	dom_append_child(&wrap->node, &link->node);
	settle();
	dom_append_child(&body->node, &wrap->node);
	check(dom_change_needs_sheets(document), "a subtree with a link needs sheets");
	settle();
	dom_remove(&wrap->node);
	check(dom_change_needs_sheets(document), "removing a subtree with a link needs sheets");
	settle();

	/* An ordinary subtree does not. */
	wrap = make("div");
	dom_append_child(&wrap->node, &make("span")->node);
	dom_append_child(&body->node, &wrap->node);
	check(!dom_change_needs_sheets(document), "a subtree without sheets needs none");
	settle();
	dom_remove(&wrap->node);
	settle();
}

/* A tree that is not in the document records nothing but the generation. */
static void
test_detached(void)
{
	struct dom_element *loose;
	struct dom_element *child;
	uint32_t generation;
	uint32_t count;

	loose = make("div");
	child = make("span");
	dom_append_child(&loose->node, &child->node);
	settle();
	generation = document->generation;
	count = document->change_count;
	set_attribute(child, "class", "y");
	check(document->generation == generation + 1U, "detached attribute grows generation");
	check(loose->node.change == 0U && child->node.change == 0U && all_bits() == 0U, "detached change leaves no bits");
	check(document->change_count == count, "detached change is not counted");
	check(!dom_change_needs_sheets(document), "detached change needs no sheets");
}

/* A data-* attribute no sheet names is quiet: the generation, and nothing recorded. */
static void
test_quiet(void)
{
	uint32_t generation;
	uint32_t quiet;

	document->attribute_styled = styled_never;
	document->attribute_styled_context = NULL;
	generation = document->generation;
	quiet = document->quiet_generation;
	set_attribute(span, "data-x", "1");
	check(document->generation == generation + 1U && document->quiet_generation == quiet + 1U, "quiet attribute counts as before");
	check(all_bits() == 0U, "quiet attribute leaves no bits");
	set_attribute(span, "title", "t");
	check((span->node.change & DOM_CHANGED_STYLE_SUBTREE) != 0U, "a non-data attribute is recorded");
	check(document->quiet_generation == quiet + 1U, "a non-data attribute is not quiet");
	settle();
	document->attribute_styled = NULL;
}

/* The other kinds. */
static void
test_kinds(void)
{
	uint32_t generation;

	/* A state of the element: its subtree, siblings and layout. */
	generation = document->generation;
	dom_note_change(&span->node, DOM_CHANGE_STATE, NULL, NULL);
	check(document->generation == generation + 1U, "state grows generation");
	check((span->node.change & (DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_STYLE_SIBLINGS | DOM_CHANGED_LAYOUT)) ==
	    (DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_STYLE_SIBLINGS | DOM_CHANGED_LAYOUT), "state bits");
	settle();

	/* A scroll: layout only. */
	dom_note_change(&div_a->node, DOM_CHANGE_SCROLL, NULL, NULL);
	check((div_a->node.change & DOM_CHANGED_LAYOUT) != 0U && (div_a->node.change & DOM_CHANGED_STYLE_BITS) == 0U, "scroll: layout only");
	check((document->node.change & DOM_CHANGED_STYLE_BELOW) == 0U && (document->node.change & DOM_CHANGED_LAYOUT_BELOW) != 0U,
	    "scroll: only layout below bits");
	settle();

	/* A resource: layout only, and the generation does not grow. */
	generation = document->generation;
	dom_note_change(&span->node, DOM_CHANGE_RESOURCE, NULL, NULL);
	check(document->generation == generation, "resource does not grow generation");
	check((span->node.change & DOM_CHANGED_LAYOUT) != 0U && (span->node.change & DOM_CHANGED_STYLE_BITS) == 0U, "resource: layout only");
	settle();

	/* A resource no element is named for: every layout, no style. */
	dom_note_change(&document->node, DOM_CHANGE_RESOURCE, NULL, NULL);
	check(document->change_full_layout == 1U && document->change_full_style == 0U, "document resource: full layout only");
	check(document->generation == generation, "document resource does not grow generation");
	settle();
	check(document->change_full_layout == 0U, "clear_layout clears the full layout flag");

	/* The whole document. */
	dom_note_change(&document->node, DOM_CHANGE_DOCUMENT, NULL, NULL);
	check(document->change_full_style == 1U && document->change_full_layout == 1U && document->change_sheets == 0U, "document: both full flags");
	settle();
	check(document->change_full_style == 0U && document->change_full_layout == 0U, "settle clears both full flags");

	/* The sheets of the document. */
	dom_note_change(&document->node, DOM_CHANGE_SHEETS, NULL, NULL);
	check(document->change_sheets == 1U && document->change_full_style == 1U && document->change_full_layout == 1U, "sheets: all flags");
	settle();
	check(document->change_sheets == 0U, "settle clears sheets");
}

/* A change inside a shadow tree is a change of its host's whole subtree. */
static void
test_shadow(void)
{
	struct dom_shadow_root *root;
	struct dom_element *inner;
	unsigned both;

	both = DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_LAYOUT;
	root = dom_shadow_attach(div_b, 0);
	check(root != NULL, "shadow root attached");
	if (root == NULL)
		return;
	check((div_b->node.change & both) == both, "shadow attach: host bits");
	settle();
	inner = make("b");
	dom_append_child(&root->node, &inner->node);
	check((div_b->node.change & both) == both, "shadow child: host bits");
	check(root->node.change == 0U && inner->node.change == 0U, "shadow child: no bits inside the shadow tree");
	check((body->node.change & DOM_CHANGED_STYLE_BELOW) != 0U, "shadow child: below bits above the host");
	settle();
	set_attribute(inner, "class", "z");
	check((div_b->node.change & both) == both && inner->node.change == 0U, "shadow attribute: host bits");
	settle();
	dom_remove(&inner->node);
	check((div_b->node.change & both) == both && inner->node.change == 0U, "shadow removal: host bits");
	settle();
}

/* The style pass: tree order, the style bits only, pruned by the below bits, stoppable. */
static void
test_visit(void)
{
	unsigned layout;

	set_attribute(paragraph, "class", "a");
	set_attribute(span, "class", "b");
	set_attribute(div_b, "class", "c");
	visited_count = 0;
	check(dom_changes_take_style(document, visit_record, NULL) == 0, "take_style returns 0");
	check(visited_count == 3, "take_style visits three nodes");
	check(visited_count == 3 && visited[0] == &span->node && visited[1] == &paragraph->node && visited[2] == &div_b->node, "take_style visits in tree order");
	check(visited_count == 3 && (visited_bits[0] & DOM_CHANGED_STYLE_SUBTREE) != 0U && (visited_bits[0] & DOM_CHANGED_LAYOUT) == 0U,
	    "take_style gives style bits only");
	layout = span->node.change & DOM_CHANGED_LAYOUT;
	check(layout != 0U, "take_style leaves the layout bits");
	check((document->node.change & DOM_CHANGED_STYLE_BELOW) == 0U && (div_a->node.change & DOM_CHANGED_STYLE_BELOW) == 0U,
	    "take_style clears the below bits");
	check((document->node.change & DOM_CHANGED_LAYOUT_BELOW) != 0U, "take_style leaves the layout below bits");
	settle();

	/* A visit that stops is the last one called, and the bits are all cleared. */
	set_attribute(span, "class", "d");
	set_attribute(div_b, "class", "e");
	visited_count = 0;
	check(dom_changes_take_style(document, visit_stop, NULL) == 7, "take_style returns the visit's value");
	check(visited_count == 1, "a stopped take_style visits one node");
	check((span->node.change | div_b->node.change | document->node.change) & (DOM_CHANGED_STYLE_BITS | DOM_CHANGED_STYLE_BELOW) ? 0 : 1,
	    "a stopped take_style still clears the style bits");
	settle();

	/* Nothing to take. */
	visited_count = 0;
	check(dom_changes_take_style(document, visit_record, NULL) == 0 && visited_count == 0, "take_style with nothing visits nothing");
}

/* Past the limit the record stops telling changes apart and asks for everything. */
static void
test_limit(void)
{
	unsigned index;

	settle();
	for (index = 0; index < DOM_CHANGE_LIMIT; index++)
		dom_note_change(&span->node, DOM_CHANGE_STATE, NULL, NULL);
	check(document->change_full_style == 0U && document->change_full_layout == 0U, "at the limit nothing is asked for yet");
	dom_note_change(&span->node, DOM_CHANGE_STATE, NULL, NULL);
	check(document->change_full_style == 1U && document->change_full_layout == 1U, "past the limit everything is asked for");
	settle();
	check(document->change_count == 0U && document->change_full_style == 0U, "settle starts counting again");
}
