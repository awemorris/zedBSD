/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of what a change can restyle (css_engine_change_scope) and of
 * dropping the styles an engine kept (css_engine_invalidate).  The expected
 * scopes are decided from the contract: which selectors look at what.
 *
 *   build/ws074-host/<variant>/host-restyle-scope
 */

#include "css/css.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
static struct vm_heap *heap;
static struct dom_document *document;

static void check(int condition, const char *what);
static struct vm_string *name(const char *ascii);
static struct dom_element *make(const char *tag);
static struct css_engine *engine_with(const char *sheet);
static void add(struct css_engine *engine, const char *sheet);
static void expect_scope(const char *sheet, int kind, const char *attribute, unsigned expected, const char *what);
static void test_scopes(void);
static void test_invalidate(void);
static void test_collisions(void);

int
main(
	void)
{
	int status;

	status = vm_heap_create(&heap, 0);
	if (status != 0)
		return 2;
	vm_heap_set_stack_base(heap, __builtin_frame_address(0));
	document = dom_document_create(heap);
	if (document == NULL)
		return 2;

	test_scopes();
	test_invalidate();
	test_collisions();

	printf("restyle-scope: %d/%d passed\n", checks - failures, checks);
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

/* Makes an atom. */
static struct vm_string *
name(
	const char *ascii)
{
	return vm_atom_from_ascii(heap, ascii);
}

/* Makes an HTML element. */
static struct dom_element *
make(
	const char *tag)
{
	return dom_element_create(document, DOM_NS_HTML, name(tag), NULL);
}

/* Adds a style sheet given as ASCII text to an engine. */
static void
add(
	struct css_engine *engine,
	const char *sheet)
{
	uint16_t *units;
	size_t length;
	size_t index;

	length = strlen(sheet);
	units = malloc((length + 1U) * sizeof(*units));
	for (index = 0; index < length; index++)
		units[index] = (uint16_t)(unsigned char)sheet[index];
	(void)css_engine_add_sheet(engine, units, length);
	free(units);
}

/* Makes an engine (with the user agent's sheet) and one author sheet. */
static struct css_engine *
engine_with(
	const char *sheet)
{
	struct css_engine *engine;

	engine = NULL;
	if (css_engine_create(&engine, heap) != 0)
		return NULL;
	if (sheet != NULL)
		add(engine, sheet);
	return engine;
}

/* Checks the scope of a change under a sheet. */
static void
expect_scope(
	const char *sheet,
	int kind,
	const char *attribute,
	unsigned expected,
	const char *what)
{
	struct css_engine *engine;
	struct dom_element *element;
	unsigned got;

	engine = engine_with(sheet);
	if (engine == NULL) {
		check(0, "engine made");
		return;
	}
	element = make("div");
	got = css_engine_change_scope(engine, element, kind, attribute == NULL ? NULL : name(attribute));
	if (got != expected)
		fprintf(stderr, "  %s: expected %u, got %u\n", what, expected, got);
	check(got == expected, what);
	css_engine_destroy(engine);
}

/* What each kind of selector makes a change restyle. */
static void
test_scopes(void)
{
	struct css_engine *engine;
	struct dom_element *element;
	unsigned sub;
	unsigned subsib;

	sub = CSS_RESTYLE_SUBTREE;
	subsib = CSS_RESTYLE_SUBTREE | CSS_RESTYLE_SIBLINGS;

	/* A sheet with plain selectors: classes, ids and type selectors. */
	expect_scope(".a { color: red } #b { color: blue }", DOM_CHANGE_ATTRIBUTE, "class", sub, "plain: class");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "id", sub, "plain: id");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "style", sub, "plain: style");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "width", sub, "plain: width (a presentational hint)");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "href", sub, "plain: href (the link state)");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "lang", sub, "plain: lang");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, NULL, sub, "plain: any attribute");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "aria-label", CSS_RESTYLE_NONE, "plain: aria-label restyles nothing");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "aria-hidden", CSS_RESTYLE_NONE, "plain: aria-hidden restyles nothing");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "role", CSS_RESTYLE_NONE, "plain: role restyles nothing");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "tabindex", CSS_RESTYLE_NONE, "plain: tabindex restyles nothing");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "data-x", CSS_RESTYLE_NONE, "plain: an unnamed data-x restyles nothing");
	expect_scope(".a { color: red }", DOM_CHANGE_ATTRIBUTE, "aria", sub, "plain: aria alone is no aria-* name");
	expect_scope(".a { color: red }", DOM_CHANGE_STATE, NULL, sub, "plain: a state (the user agent's sheet looks at some)");
	expect_scope(".a { color: red }", DOM_CHANGE_STATE, "aria-label", sub, "plain: a state is not an attribute");

	/* A sheet that names such an attribute: in a selector, or in attr(). */
	expect_scope("[aria-label] { color: red }", DOM_CHANGE_ATTRIBUTE, "aria-label", sub, "named aria-label in a selector");
	expect_scope("[aria-label] { color: red }", DOM_CHANGE_ATTRIBUTE, "aria-hidden", CSS_RESTYLE_NONE, "another aria attribute still restyles nothing");
	expect_scope("[role=button] { color: red }", DOM_CHANGE_ATTRIBUTE, "role", sub, "named role");
	expect_scope("[data-state=on] { color: red }", DOM_CHANGE_ATTRIBUTE, "data-state", sub, "named data-state");
	expect_scope(".x::before { content: attr(aria-label) }", DOM_CHANGE_ATTRIBUTE, "aria-label", sub, "aria-label named by attr()");
	expect_scope(".x::before { content: attr(aria-label) }", DOM_CHANGE_ATTRIBUTE, "tabindex", CSS_RESTYLE_NONE, "attr() of another name");

	/* Selectors that join siblings add the following siblings. */
	expect_scope(".a + .b { color: red }", DOM_CHANGE_ATTRIBUTE, "class", subsib, "next-sibling combinator");
	expect_scope(".a ~ .b { color: red }", DOM_CHANGE_ATTRIBUTE, "class", subsib, "subsequent-sibling combinator");
	expect_scope(":is(.a + .b) { color: red }", DOM_CHANGE_ATTRIBUTE, "class", subsib, "a sibling combinator inside :is");
	expect_scope(".a > .b { color: red }", DOM_CHANGE_ATTRIBUTE, "class", sub, "a child combinator joins no siblings");
	expect_scope(".a + .b { color: red }", DOM_CHANGE_ATTRIBUTE, "aria-label", CSS_RESTYLE_NONE, "a sibling combinator and an unnamed aria attribute");

	/* Selectors that look below an element or count siblings by a selector: everything. */
	expect_scope("div:has(.x) { color: red }", DOM_CHANGE_ATTRIBUTE, "class", CSS_RESTYLE_ALL, ":has: class");
	expect_scope("div:has(.x) { color: red }", DOM_CHANGE_STATE, NULL, CSS_RESTYLE_ALL, ":has: state");
	expect_scope("div:has(.x) { color: red }", DOM_CHANGE_ATTRIBUTE, "aria-label", CSS_RESTYLE_ALL, ":has: even aria-label");
	expect_scope(":not(:has(p)) { color: red }", DOM_CHANGE_ATTRIBUTE, "class", CSS_RESTYLE_ALL, ":has inside :not");
	expect_scope("li:nth-child(2 of .a) { color: red }", DOM_CHANGE_ATTRIBUTE, "class", CSS_RESTYLE_ALL, ":nth-child(of S)");
	expect_scope("li:nth-child(2) { color: red }", DOM_CHANGE_ATTRIBUTE, "class", sub, ":nth-child without of S looks at no attribute");

	/* States: only when a selector looks at one. */
	expect_scope("input:checked { color: red }", DOM_CHANGE_STATE, NULL, sub, ":checked: state");
	expect_scope("input:disabled + span { color: red }", DOM_CHANGE_STATE, NULL, subsib, ":disabled with a sibling combinator");
	expect_scope("input:placeholder-shown { color: red }", DOM_CHANGE_STATE, NULL, sub, ":placeholder-shown: state");

	/* A sheet added after the question changes the answer. */
	engine = engine_with(".a { color: red }");
	element = make("div");
	check(css_engine_change_scope(engine, element, DOM_CHANGE_ATTRIBUTE, name("class")) == sub, "before the sheet: class");
	check(css_engine_change_scope(engine, element, DOM_CHANGE_STATE, NULL) == sub, "before the sheet: state");
	add(engine, "div:has(p) { color: red }");
	check(css_engine_change_scope(engine, element, DOM_CHANGE_ATTRIBUTE, name("class")) == CSS_RESTYLE_ALL, "after a :has sheet: class");
	css_engine_destroy(engine);
}

/* Computes an element's style under its parent's (NULL for the root). */
static void
compute(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_style *parent,
	struct css_style *style)
{
	(void)css_engine_compute(engine, element, parent, style);
}

/* Counts what the engine computed and gave from its kept styles. */
static void
counts(
	const struct css_engine *engine,
	uint64_t *computed,
	uint64_t *hits,
	uint64_t *dropped)
{
	css_engine_counts(engine, computed, hits, dropped);
}

/* Dropping: only the styles the scope reaches are computed again. */
static void
test_invalidate(void)
{
	struct css_engine *engine;
	struct dom_element *html;
	struct dom_element *body;
	struct dom_element *d1;
	struct dom_element *s1;
	struct dom_element *s2;
	struct dom_element *d2;
	struct dom_element *t1;
	struct dom_element *host;
	struct dom_element *inner;
	struct dom_shadow_root *root;
	struct css_style style_html;
	struct css_style style_body;
	struct css_style style_d1;
	struct css_style style_d2;
	struct css_style style_host;
	struct css_style scratch;
	uint64_t computed;
	uint64_t hits;
	uint64_t dropped;
	uint64_t before;

	engine = engine_with(".a { color: red }");
	html = make("html");
	body = make("body");
	d1 = make("div");
	s1 = make("span");
	s2 = make("span");
	d2 = make("div");
	t1 = make("p");
	host = make("section");
	inner = make("b");
	dom_append_child(&document->node, &html->node);
	dom_append_child(&html->node, &body->node);
	dom_append_child(&body->node, &d1->node);
	dom_append_child(&d1->node, &s1->node);
	dom_append_child(&d1->node, &s2->node);
	dom_append_child(&body->node, &d2->node);
	dom_append_child(&d2->node, &t1->node);
	dom_append_child(&body->node, &host->node);
	root = dom_shadow_attach(host, 0);
	if (root != NULL)
		dom_append_child(&root->node, &inner->node);

	/* Computes all nine (the shadow tree's element under its host's style). */
	compute(engine, html, NULL, &style_html);
	compute(engine, body, &style_html, &style_body);
	compute(engine, d1, &style_body, &style_d1);
	compute(engine, s1, &style_d1, &scratch);
	compute(engine, s2, &style_d1, &scratch);
	compute(engine, d2, &style_body, &style_d2);
	compute(engine, t1, &style_d2, &scratch);
	compute(engine, host, &style_body, &style_host);
	compute(engine, inner, &style_host, &scratch);
	counts(engine, &computed, &hits, &dropped);
	check(computed == 9 && hits == 0 && dropped == 0, "nine computed, none given, none dropped");

	/* Asking again gives the kept ones. */
	compute(engine, d1, &style_body, &scratch);
	counts(engine, &computed, &hits, &dropped);
	check(computed == 9 && hits == 1, "a kept style is given");

	/* NONE drops nothing. */
	css_engine_invalidate(engine, d1, CSS_RESTYLE_NONE);
	counts(engine, &computed, &hits, &dropped);
	check(dropped == 0, "none drops nothing");

	/* The subtree of d1: d1, s1, s2. */
	css_engine_invalidate(engine, d1, CSS_RESTYLE_SUBTREE);
	counts(engine, &computed, &hits, &dropped);
	check(dropped == 3, "subtree drops the element and its two children");
	before = computed;
	compute(engine, d1, &style_body, &style_d1);
	compute(engine, s1, &style_d1, &scratch);
	compute(engine, s2, &style_d1, &scratch);
	compute(engine, d2, &style_body, &style_d2);
	compute(engine, t1, &style_d2, &scratch);
	counts(engine, &computed, &hits, &dropped);
	check(computed == before + 3, "only the three are computed again");

	/* A leaf: only itself. */
	css_engine_invalidate(engine, s1, CSS_RESTYLE_SUBTREE);
	counts(engine, &computed, &hits, &dropped);
	check(dropped == 4, "a leaf drops itself only");
	compute(engine, s1, &style_d1, &scratch);

	/* The following siblings with what is below them (and the element itself). */
	css_engine_invalidate(engine, d1, CSS_RESTYLE_SIBLINGS);
	counts(engine, &computed, &hits, &dropped);
	check(dropped == 4 + 1 + 2 + (root != NULL ? 2 : 1), "siblings alone drops the element, d2 and p, the host and its shadow tree");
	compute(engine, d1, &style_body, &style_d1);
	compute(engine, d2, &style_body, &style_d2);
	compute(engine, t1, &style_d2, &scratch);
	compute(engine, host, &style_body, &style_host);
	compute(engine, inner, &style_host, &scratch);

	/* The subtree of a host reaches its shadow tree. */
	before = dropped;
	css_engine_invalidate(engine, host, CSS_RESTYLE_SUBTREE);
	counts(engine, &computed, &hits, &dropped);
	check(dropped == before + (root != NULL ? 2 : 1), "a host's subtree reaches its shadow tree");
	compute(engine, host, &style_body, &style_host);
	compute(engine, inner, &style_host, &scratch);

	/* A style that was never kept drops nothing. */
	before = dropped;
	css_engine_invalidate(engine, make("em"), CSS_RESTYLE_SUBTREE);
	counts(engine, &computed, &hits, &dropped);
	check(dropped == before, "an element with no kept style drops nothing");

	/* Everything. */
	css_engine_invalidate(engine, d1, CSS_RESTYLE_ALL);
	before = computed;
	compute(engine, html, NULL, &style_html);
	compute(engine, body, &style_html, &style_body);
	counts(engine, &computed, &hits, &dropped);
	check(computed == before + 2, "after everything is dropped the next ones are computed");
	css_engine_destroy(engine);
}

/* Dropping among many kept styles loses none of the others (the table's runs of colliding probes are closed up). */
static void
test_collisions(void)
{
	enum { COUNT = 900 };
	struct css_engine *engine;
	struct dom_element *html;
	struct dom_element *body;
	struct dom_element **items;
	struct css_style style_html;
	struct css_style style_body;
	struct css_style scratch;
	uint64_t computed;
	uint64_t hits;
	uint64_t dropped;
	uint64_t before;
	int index;
	int removed;

	engine = engine_with(NULL);
	html = make("html");
	body = make("body");
	items = calloc(COUNT, sizeof(*items));
	dom_append_child(&document->node, &html->node);
	dom_append_child(&html->node, &body->node);
	for (index = 0; index < COUNT; index++) {
		items[index] = make("div");
		dom_append_child(&body->node, &items[index]->node);
	}
	compute(engine, html, NULL, &style_html);
	compute(engine, body, &style_html, &style_body);
	for (index = 0; index < COUNT; index++)
		compute(engine, items[index], &style_body, &scratch);
	counts(engine, &computed, &hits, &dropped);
	check(computed == COUNT + 2, "all are computed");

	/* Every third, and then every fifth of the rest, is dropped. */
	removed = 0;
	for (index = 0; index < COUNT; index++) {
		if (index % 3 == 0) {
			css_engine_invalidate(engine, items[index], CSS_RESTYLE_SUBTREE);
			removed++;
		}
	}
	for (index = 0; index < COUNT; index++) {
		if (index % 3 != 0 && index % 5 == 0) {
			css_engine_invalidate(engine, items[index], CSS_RESTYLE_SUBTREE);
			removed++;
		}
	}
	counts(engine, &computed, &hits, &dropped);
	check(dropped == (uint64_t)removed, "each dropped one is counted once");

	/* Asking for all of them again computes exactly the dropped ones. */
	before = computed;
	for (index = 0; index < COUNT; index++)
		compute(engine, items[index], &style_body, &scratch);
	counts(engine, &computed, &hits, &dropped);
	check(computed == before + (uint64_t)removed, "exactly the dropped ones are computed again");
	check(hits == (uint64_t)(COUNT - removed), "none of the others was lost");
	free(items);
	css_engine_destroy(engine);
}
