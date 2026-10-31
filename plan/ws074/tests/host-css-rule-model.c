/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Verifies immutable top-level CSS sources and transactional mutation through the real cascade. */

#include "css/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Count independent grammar, ownership and real cascade observations until teardown. */
static unsigned checks;
/* Preserve every failing observation while independent fixture cases continue. */
static unsigned failures;

static void model_check(int condition, const char *name);
static int model_input(struct css_rule_model **model, struct vm_heap *heap, const char *text);
static int model_insert_text(struct css_rule_model *model, const char *text, size_t index);
static int model_case(struct vm_heap *heap);
static int model_height(struct css_rule_model *model, struct dom_element *image, float *height);
static int model_margins(struct dom_element *image, const char *source, float margins[4]);

/*
 * Runs native structural CSS source and cascade observations.
 */
int
main(
	void)
{
	struct vm_heap *heap;
	int status;
	int printed;

	/* Ordinary native CSS and DOM APIs share a genuine production heap. */
	status = vm_heap_create(&heap, 0);
	if (status != 0)
		return 2;
	vm_heap_set_stack_base(heap, __builtin_frame_address(0));
	status = model_case(heap);
	if (status != 0) {
		vm_heap_destroy(heap);
		return 2;
	}

	/* Releases the heap only after the fixture outcome is checked. */
	vm_heap_destroy(heap);

	/* Publish complete observations after production resources were released. */
	printed = printf("native CSS rule model: %u/%u passed\n", checks - failures, checks);
	if (printed < 0)
		return 2;
	if (failures != 0)
		return 1;

	/* Succeeded: structural sources and their real cascade effects were verified. */
	return 0;
}

/* Records one independent source or rendering observation. */
static void
model_check(
	int condition,
	const char *name)
{
	int printed;

	/* Keep named failures while remaining independent cases execute. */
	checks++;
	if (!condition) {
		failures++;
		printed = fprintf(stderr, "FAIL %s\n", name);
		if (printed < 0)
			failures++;
	}

	/* Succeeded: this source or cascade observation is recorded. */
	return;
}

/* Creates an ordinary source model from UTF8 fixture input. */
static int
model_input(
	struct css_rule_model **model,
	struct vm_heap *heap,
	const char *text)
{
	struct wb_units units;
	uint32_t before;
	uint32_t after;
	int status;

	/* Input remains caller-owned and immutable through native model construction. */
	wb_units_init(&units);
	status = wb_utf8_to_units((const unsigned char *)text, strlen(text), &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Compare caller input before and after the model copies its source. */
	before = wb_hash_units(units.data, units.length);
	status = css_rule_model_create(model, heap, units.data, units.length);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Successful construction leaves the original caller input untouched. */
	after = wb_hash_units(units.data, units.length);
	model_check(before == after, "model construction leaves original source units unchanged");
	wb_units_release(&units);

	/* Succeeded: the model owns copied sources after caller input is released. */
	return 0;
}

/* Inserts fixture source through the production transactional model API. */
static int
model_insert_text(
	struct css_rule_model *model,
	const char *text,
	size_t index)
{
	struct wb_units units;
	int status;

	/* Ordinary Unicode conversion supplies the same rule source as DOM bindings will. */
	wb_units_init(&units);
	status = wb_utf8_to_units((const unsigned char *)text, strlen(text), &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Copy and validate the single rule before publishing its insertion. */
	status = css_rule_model_insert(model, units.data, units.length, index);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* The inserted rule now owns its independently copied source. */
	wb_units_release(&units);

	/* Succeeded: the model owns a new immutable current rule. */
	return 0;
}

/* Parses actual native source grammar and checks stable rule identities and rendering effects. */
static int
model_case(
	struct vm_heap *heap)
{
	struct css_rule_model *model;
	struct css_sheet *sheet;
	unsigned depth;
	struct wb_units text;
	struct dom_document *document;
	struct dom_element *image;
	struct vm_string *name;
	const uint16_t *source;
	const uint16_t *saved_source;
	size_t length;
	size_t count;
	uint32_t original;
	uint32_t inserted;
	uint32_t generation;
	float height;
	float margins[4];
	int type;
	int present;
	int status;

	/* Strings and escaped selector braces must not imitate structural block boundaries. */
	status = model_input(&model, heap,
			     "<!-- /*lead*/ .a\\7d {height:10px;content:'}';} @media print{a{height:1px}b{height:2px}}"
			     "@supports (unknown: value){img{height:3px}} @import url('x.css');"
			     "@font-face{font-family:X;src:url(x)} # {height:9px} @unknown{x{height:8px}} -->");
	if (status != 0)
		return status;
	count = css_rule_model_count(model);
	model_check(count == 5U, "recognized groups count once and invalid selectors or unknown rules are discarded");
	original = css_rule_model_id(model, 0);
	status = css_rule_model_source(model, original, &source, &length, &type, &present);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* A successful borrowed source has live storage until model destruction. */
	if (source == NULL || length == 0) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Inspect copied source only after successful stable identity lookup. */
	model_check(length != 0 &&
	    source[0] == '.' &&
	    type == 1 &&
	    present == 1, "source begins after skipped comment and preserves escaped ordinary rule");
	status = css_rule_model_source(model, 0, &source, &length, &type, &present);
	if (status != ENOENT) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Absent stable identity has no borrowed source. */
	model_check(status == ENOENT &&
	    source == NULL &&
	    length == 0, "absent stable identity has no borrowed source");
	original = css_rule_model_id(model, 1);
	status = css_rule_model_source(model, original, &source, &length, &type, &present);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* A successful borrowed source has live storage until model destruction. */
	if (source == NULL || length == 0) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Reads only validated borrowed source storage. */
	model_check(status == 0 &&
	    type == 4 &&
	    source[0] == '@', "media group retains one real top-level source");

	/* Reparsed cascade rules flatten independently of the structural native source list. */
	wb_units_init(&text);
	status = css_rule_model_text(model, &text);
	if (status != 0) {
		wb_units_release(&text);
		css_rule_model_destroy(model);
		return status;
	}

	/* Reparse native model sources with the ordinary flattened rendering parser. */
	sheet = NULL;
	status = css_sheet_create(&sheet, heap, text.data, text.length);
	if (status != 0) {
		wb_units_release(&text);
		css_rule_model_destroy(model);
		return status;
	}

	/* The parsed sheet owns every source copied from the temporary join. */
	wb_units_release(&text);

	/* Structural source length and flattened cascade rule count are independent. */
	count = css_sheet_rule_count(sheet);
	model_check(count == 3U, "ordinary rendering parser still flattens media contents independently");
	css_sheet_destroy(sheet);
	css_rule_model_destroy(model);

	/* A real image verifies ordinary cascade results from current sources without DOM text writes. */
	document = dom_document_create(heap);
	if (document == NULL)
		return ENOMEM;
	name = vm_atom_from_ascii(heap, "img");
	if (name == NULL)
		return ENOMEM;
	image = dom_element_create(document, DOM_NS_HTML, name, NULL);
	if (image == NULL)
		return ENOMEM;
	dom_append_child(&document->node, &image->node);
	generation = document->generation;
	status = model_input(&model, heap, "img {height:10px}");
	if (status != 0)
		return status;
	original = css_rule_model_id(model, 0);
	status = css_rule_model_source(model, original, &saved_source, &length, &type, &present);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* A successful borrowed source has live storage until model destruction. */
	if (saved_source == NULL || length == 0) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Observe the initial source through the actual native cascade. */
	status = model_height(model, image, &height);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Initial model source supplies actual cascade height10. */
	model_check(status == 0 && height == 10, "initial model source supplies actual cascade height10");

	/* Appending and then inserting another rule preserves order and actual last-rule precedence. */
	status = model_insert_text(model, "img {height:20px}", 1);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Append insertion succeeds. */
	model_check(status == 0, "append insertion succeeds");
	status = model_height(model, image, &height);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Appended model source supplies actual cascade height20. */
	model_check(status == 0 && height == 20, "appended model source supplies actual cascade height20");
	status = model_insert_text(model, "img {height:40px}", 2);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* End insertion succeeds. */
	model_check(status == 0, "end insertion succeeds");
	inserted = css_rule_model_id(model, 2);
	status = model_height(model, image, &height);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Inserted source supplies actual cascade height40. */
	model_check(status == 0 && height == 40, "inserted source supplies actual cascade height40");

	/* Failed insertion must preserve live count and every already published rule identity. */
	status = model_insert_text(model, "img{height:1px}", 4);
	if (status != ERANGE) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Invalid insertion index leaves live list unchanged. */
	count = css_rule_model_count(model);
	model_check(status == ERANGE && count == 3U, "invalid insertion index leaves live list unchanged");
	status = model_insert_text(model, "# {height:1px}", 0);
	if (status != EINVAL) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Invalid selector cannot silently produce a rule. */
	count = css_rule_model_count(model);
	model_check(status == EINVAL && count == 3U, "invalid selector cannot silently produce a rule");
	status = model_insert_text(model, "img{height:1px} img{height:2px}", 0);
	if (status != EINVAL) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Multiple top-level rules are not a single insertion. */
	count = css_rule_model_count(model);
	model_check(status == EINVAL && count == 3U, "multiple top-level rules are not a single insertion");
	status = model_insert_text(model, "img{height:1px} trailing", 0);
	if (status != EINVAL) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Unparsed trailing input rejects single-rule insertion. */
	count = css_rule_model_count(model);
	model_check(status == EINVAL && count == 3U, "unparsed trailing input rejects single-rule insertion");
	status = model_insert_text(model, "@import url(x.css);", 0);
	if (status != ENOTSUP) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* At-rule insertion is explicitly unsupported without mutation. */
	count = css_rule_model_count(model);
	model_check(status == ENOTSUP && count == 3U, "at-rule insertion is explicitly unsupported without mutation");
	status = model_insert_text(model, " ", 0);
	if (status != EINVAL) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Empty insertion does not publish a rule. */
	count = css_rule_model_count(model);
	model_check(status == EINVAL && count == 3U, "empty insertion does not publish a rule");

	/* Removing a current rule retires its immutable source and restores earlier cascade precedence. */
	status = css_rule_model_delete(model, 2);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Delete removes one current rule. */
	count = css_rule_model_count(model);
	model_check(status == 0 && count == 2U, "delete removes one current rule");
	status = css_rule_model_source(model, inserted, &source, &length, &type, &present);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* A successful borrowed source has live storage until model destruction. */
	if (source == NULL || length == 0) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Retired source remains available by stable identity. */
	model_check(status == 0 &&
	    present == 0 &&
	    type == 1 &&
	    length != 0, "retired source remains available by stable identity");
	status = model_height(model, image, &height);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Deletion restores actual cascade height20. */
	model_check(status == 0 && height == 20, "deletion restores actual cascade height20");

	/* Beginning and middle insertion shift current indexes without changing existing handles. */
	status = model_insert_text(model, "img{height:1px}", 0);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Beginning insertion succeeds. */
	model_check(status == 0, "beginning insertion succeeds");
	status = model_insert_text(model, "img{height:2px}", 2);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Middle insertion succeeds. */
	model_check(status == 0, "middle insertion succeeds");
	status = css_rule_model_source(model, original, &source, &length, &type, &present);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* A successful borrowed source has live storage until model destruction. */
	if (source == NULL || length == 0) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Insertion preserves original immutable source address and membership. */
	model_check(status == 0 &&
	    source == saved_source &&
	    present == 1, "insertion preserves original immutable source address and membership");
	count = css_rule_model_count(model);
	status = css_rule_model_delete(model, count);
	if (status != ERANGE) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Invalid deletion index refuses mutation. */
	model_check(status == ERANGE, "invalid deletion index refuses mutation");
	model_check(document->generation == generation, "CSS source mutations never change native DOM generation");
	css_rule_model_destroy(model);

	/* CSS Syntax permits an otherwise valid open block to close at end of input. */
	status = model_input(&model, heap, "img{height:10px");
	if (status != 0)
		return status;
	count = css_rule_model_count(model);
	model_check(count == 1U, "end-of-input closes a valid ordinary source block");
	original = css_rule_model_id(model, 0);
	status = css_rule_model_source(model, original, &source, &length, &type, &present);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* A successful borrowed source has live storage until model destruction. */
	if (source == NULL || length == 0) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Reads only validated borrowed source storage. */
	model_check(status == 0 &&
	    length != 0 &&
	    source[length - 1U] == 'x', "EOF closure metadata leaves original source units unchanged");
	status = model_insert_text(model, "img{height:20px}", 1);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Insertion after an implicitly closed block succeeds. */
	model_check(status == 0, "insertion after an implicitly closed block succeeds");
	status = model_height(model, image, &height);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Insertion after EOF block changes actual cascade height20. */
	model_check(status == 0 && height == 20, "insertion after EOF block changes actual cascade height20");
	css_rule_model_destroy(model);

	/* Nested grouping closures remain separate from a subsequently inserted top-level rule. */
	status = model_input(&model, heap, "@media all{img{height:10px");
	if (status != 0)
		return status;
	status = model_insert_text(model, "img{height:20px}", 1);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Insertion after nested EOF block succeeds. */
	model_check(status == 0, "insertion after nested EOF block succeeds");
	status = model_height(model, image, &height);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Nested EOF closures restore independent top-level cascade order. */
	model_check(status == 0 && height == 20, "nested EOF closures restore independent top-level cascade order");
	css_rule_model_destroy(model);

	/* An otherwise valid EOF import retains one top-level source and its implicit terminator. */
	status = model_input(&model, heap, "@import url('x.css')");
	if (status != 0)
		return status;
	count = css_rule_model_count(model);
	model_check(count == 1U, "import without final semicolon is retained at EOF");
	status = model_insert_text(model, "img{height:20px}", 1);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* Ordinary insertion after EOF import succeeds. */
	model_check(status == 0, "ordinary insertion after EOF import succeeds");
	wb_units_init(&text);
	status = css_rule_model_text(model, &text);
	if (status != 0) {
		wb_units_release(&text);
		css_rule_model_destroy(model);
		return status;
	}

	/* Reparse through the native import and qualified-rule parser without performing network IO. */
	sheet = NULL;
	status = css_sheet_create(&sheet, heap, text.data, text.length);
	if (status != 0) {
		wb_units_release(&text);
		css_rule_model_destroy(model);
		return status;
	}

	/* The parsed sheet owns every source copied from the temporary join. */
	wb_units_release(&text);

	/* Both import metadata and the subsequent style rule survive the completed source join. */
	count = css_sheet_import_count(sheet);
	model_check(count == 1U, "EOF import source remains an actual native import after insertion");
	count = css_sheet_rule_count(sheet);
	model_check(count == 1U, "inserted style remains independent of preceding EOF import");
	css_sheet_destroy(sheet);
	css_rule_model_destroy(model);

	/* Copied source retains original UTF16 units, including non-ASCII and supplementary characters. */
	status = model_input(&model, heap, ".é😀{height:3px;content:'é'}");
	if (status != 0)
		return status;
	original = css_rule_model_id(model, 0);
	status = css_rule_model_source(model, original, &source, &length, &type, &present);
	if (status != 0) {
		css_rule_model_destroy(model);
		return status;
	}

	/* A successful borrowed source has live storage until model destruction. */
	if (source == NULL || length == 0) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Original UTF16 source survives supplementary-name tokenization. */
	if (source == NULL || length <= 4) {
		css_rule_model_destroy(model);
		return EIO;
	}

	/* Reads only validated borrowed source storage. */
	model_check(status == 0 &&
	    length > 4 &&
	    source[1] == 0xe9 &&
	    source[2] == 0xd83d &&
	    source[3] == 0xde00, "original UTF16 source survives supplementary-name tokenization");
	css_rule_model_destroy(model);

	/* Recognized at-rule names do not make an invalid block-versus-semicolon grammar valid. */
	status = model_input(&model, heap, "@media screen;@font-face;@supports (display:block);@import {img{height:1px}}");
	if (status != 0)
		return status;
	count = css_rule_model_count(model);
	model_check(count == 0, "invalid recognized at-rule boundary forms are discarded");
	css_rule_model_destroy(model);

	/* Bounded grammar depth is checked without recursive source parsing or native stack growth. */
	wb_units_init(&text);
	for (depth = 0; depth < 129U; depth++) {
		status = wb_units_append_code_point(&text, '{');
		if (status != 0) {
			wb_units_release(&text);
			return status;
		}
	}

	/* Excessive native source nesting cannot publish a partially constructed model. */
	status = css_rule_model_create(&model, heap, text.data, text.length);
	if (status != EOVERFLOW) {
		css_rule_model_destroy(model);
		wb_units_release(&text);
		return EIO;
	}

	/* The expected grammar bound refusal has no retained source input. */
	wb_units_release(&text);
	model_check(status == EOVERFLOW && model == NULL, "excessive curly nesting refuses partial model publication");

	/* Logical margins map with the final flow, even when its declaration comes later. */
	status = model_margins(image, "img{margin-inline-start:7px;writing-mode:vertical-lr}", margins);
	if (status != 0)
		return status;
	model_check(margins[CSS_TOP] == 7 && margins[CSS_LEFT] == 0, "vertical-lr inline start maps to top");
	status = model_margins(image, "img{direction:rtl;margin-inline-start:7px;writing-mode:vertical-lr}", margins);
	if (status != 0)
		return status;
	model_check(margins[CSS_BOTTOM] == 7 && margins[CSS_TOP] == 0, "vertical-lr RTL inline start maps to bottom");
	status = model_margins(image, "img{margin-block:3px 4px;writing-mode:vertical-lr}", margins);
	if (status != 0)
		return status;
	model_check(margins[CSS_LEFT] == 3 && margins[CSS_RIGHT] == 4, "vertical-lr block shorthand maps to left and right");
	status = model_margins(image, "img{margin-inline:3px 4px;writing-mode:vertical-lr}", margins);
	if (status != 0)
		return status;
	model_check(margins[CSS_TOP] == 3 && margins[CSS_BOTTOM] == 4, "vertical-lr inline shorthand maps to top and bottom");
	status = model_margins(image, "img{writing-mode:vertical-lr;margin-left:2px;margin-block-start:9px}", margins);
	if (status != 0)
		return status;
	model_check(margins[CSS_LEFT] == 9, "later logical margin wins physical side");
	status = model_margins(image, "img{writing-mode:vertical-lr;margin-block-start:9px;margin-left:2px}", margins);
	if (status != 0)
		return status;
	model_check(margins[CSS_LEFT] == 2, "later physical margin wins logical side");
	status = model_margins(image, "img{direction:rtl;margin-inline-start:5px}", margins);
	if (status != 0)
		return status;
	model_check(margins[CSS_RIGHT] == 5 && margins[CSS_LEFT] == 0, "horizontal RTL inline start maps to right");

	/* Succeeded: structural grammar, immutable handles and actual cascade mutation were observed. */
	return 0;
}

/* Computes a real native image cascade from the model's current source order. */
static int
model_height(
	struct css_rule_model *model,
	struct dom_element *image,
	float *height)
{
	struct css_engine *engine;
	struct css_style style;
	struct wb_units units;
	int status;

	/* Ordinary source reparsing uses the existing engine rather than a model-only expected result. */
	wb_units_init(&units);
	status = css_rule_model_text(model, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Create the same ordinary cascade engine used by production styling. */
	status = css_engine_create(&engine, image->node.document->heap);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Parse current sources before asking for a real native element style. */
	status = css_engine_add_sheet(engine, units.data, units.length);
	if (status != 0) {
		wb_units_release(&units);
		css_engine_destroy(engine);
		return status;
	}

	/* Copied sheet sources no longer borrow the temporary joined text. */
	wb_units_release(&units);
	status = css_engine_compute(engine, image, NULL, &style);
	if (status != 0) {
		css_engine_destroy(engine);
		return status;
	}

	/* Publishes only the completed real cascade height. */
	*height = style.height.value;
	css_engine_destroy(engine);

	/* Succeeded: the real cascade supplied the current image height. */
	return 0;
}

/* Computes the four actual physical margins from one ordinary rule source. */
static int
model_margins(
	struct dom_element *image,
	const char *source,
	float margins[4])
{
	struct css_engine *engine;
	struct css_style style;
	struct wb_units units;
	int status;
	int side;

	wb_units_init(&units);
	status = wb_utf8_to_units((const unsigned char *)source, strlen(source), &units);
	if (status != 0)
		goto cleanup_units;
	status = css_engine_create(&engine, image->node.document->heap);
	if (status != 0)
		goto cleanup_units;
	status = css_engine_add_sheet(engine, units.data, units.length);
	if (status != 0)
		goto cleanup_engine;
	status = css_engine_compute(engine, image, NULL, &style);
	if (status != 0)
		goto cleanup_engine;
	for (side = 0; side < 4; side++)
		margins[side] = style.margin[side].value;

cleanup_engine:
	css_engine_destroy(engine);
cleanup_units:
	wb_units_release(&units);
	return status;
}
