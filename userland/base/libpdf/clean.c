/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clean copy of a document (ws175-p009, plan/ws175/phase001/design.md
 * D1 and [M11]): Notes' "Save Clean Copy", which writes the document as it
 * reads now into a new file without what its earlier revisions left
 * behind.
 *
 * The copy holds the objects the catalog and the information dictionary
 * reach, numbered again from 1: the catalog is 1, one page tree node that
 * lists every page is 2, and the pages follow in order.  Each page carries
 * the boxes, rotation and resources it inherited, so the old page tree
 * nodes are left out (a reference to one is written as the new node).  A
 * page's resources keep, of the categories its content names things in
 * (XObject, Font, ExtGState, Pattern, Shading, ColorSpace, Properties),
 * only the entries its content names: an image the editor deleted, or a
 * font the page no longer uses, is then reached by nothing and left out
 * with its stream.  A form, Type 3 font or tiling pattern the content
 * names that takes the page's resources for want of its own adds the
 * names its own content uses; one with resources of its own has them
 * kept the same way, by the names its content (a Type 3 font's glyph
 * procedures) uses (ws177-p011).  Resources whose content cannot be read
 * are kept whole.
 *
 * Objects are written as the reader read them: references as the new
 * numbers, strings in hexadecimal, streams with their own encoded bytes
 * and a direct /Length.  Objects that were in object streams become
 * objects of their own; the object streams and cross-reference streams
 * are reached by nothing and go.  One attached file may be left out by
 * name: its file specification is dropped from the EmbeddedFiles name tree
 * and the catalog's /AF (Notes leaves out its edit data, so the copy opens
 * as another program's PDF, its edits part of its pages), whether it is an
 * object of its own or written in the tree or the list; the name tree's
 * /Limits are written again from the names it keeps (ws177-p011).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"
#include "writer.h"

/* The numbers of the catalog and the page tree node of a clean copy, and of its first page. */
#define CLEAN_CATALOG 1UL
#define CLEAN_PAGES 2UL
#define CLEAN_FIRST_PAGE 3UL

/* What the copy does with an object of the document (clean_state.kinds). */
#define CLEAN_KIND_UNSEEN 0
#define CLEAN_KIND_COPIED 1
#define CLEAN_KIND_DROPPED 2
#define CLEAN_KIND_NULL 3

/* The categories of a page's resources whose entries its content names. */
#define CLEAN_CATEGORIES 7

/* The categories whose entries draw with resources: XObject, Font and Pattern (their places in the names). */
#define CLEAN_XOBJECT 0
#define CLEAN_FONT 1
#define CLEAN_PATTERN 3

/* A category entry's mark: not named, named by the content, named and its own content followed. */
#define CLEAN_UNUSED 0
#define CLEAN_NAMED 1
#define CLEAN_FOLLOWED 2

/* What holds the content a resource dictionary is named in (clean_source.kind). */
#define CLEAN_SOURCE_PAGE 0
#define CLEAN_SOURCE_STREAM 1
#define CLEAN_SOURCE_TYPE3 2

/* The most direct objects of the document the copy leaves out (file specifications written in place). */
#define CLEAN_DIRECT_MAX 64

/*
 * The state of one clean copy.
 *
 * numbers maps each object number of the document (below count) to its
 * number in the copy (0: none yet), and kinds tells what the copy does
 * with it (CLEAN_KIND_*).  queue holds the references of the objects
 * numbered but not yet written, in the order of their numbers; offsets
 * holds where each of the copy's objects begins in the file.  direct
 * holds the objects written in place (not referenced) that the copy
 * leaves out, and names_dropped tells that a name tree lost entries, so
 * its /Limits are written again.
 */
struct clean_state {
	struct pdf_document *document;
	struct pdf_buffer *file;
	unsigned long count;
	unsigned long *numbers;
	unsigned char *kinds;
	struct pdf_object **queue;
	size_t queue_head;
	size_t queue_length;
	size_t *offsets;
	unsigned long next;
	size_t dropped;
	const struct pdf_object *direct[CLEAN_DIRECT_MAX];
	size_t direct_count;
	int names_dropped;
};

/*
 * Where the content a resource dictionary serves is: a page's content
 * streams (page is its place), a form's or tiling pattern's own stream,
 * or a Type 3 font's glyph procedures (holder is the stream or the font).
 */
struct clean_source {
	int kind;
	size_t page;
	struct pdf_object *holder;
};

/*
 * One category of a page's resources: its dictionary (NULL: the page has
 * none) and, for each of its entries, whether the page's content names it
 * (CLEAN_UNUSED, CLEAN_NAMED, CLEAN_FOLLOWED).
 */
struct clean_category {
	const char *name;
	struct pdf_object *dictionary;
	unsigned char *used;
};

static int clean_prepare(struct clean_state *state, size_t pages);
static int clean_drop_attachment(struct clean_state *state, struct pdf_object *catalog, const char *attachment);
static int clean_drop_names(struct clean_state *state, struct pdf_object *node, const char *attachment, int depth);
static int clean_drop_associated(struct clean_state *state, struct pdf_object *catalog, const char *attachment);
static int clean_string_is(const struct pdf_object *object, const char *text);
static void clean_write_catalog(struct clean_state *state, struct pdf_object *catalog);
static void clean_write_tree(struct clean_state *state, size_t pages);
static void clean_write_page(struct clean_state *state, size_t index);
static void clean_write_resources(struct clean_state *state, const struct clean_source *source, struct pdf_object *resources);
static int clean_find_used(struct clean_state *state, const struct clean_source *source, struct clean_category *categories);
static int clean_find_page(struct clean_state *state, size_t index, struct clean_category *categories);
static int clean_mark_glyphs(struct clean_state *state, struct pdf_object *font, struct clean_category *categories);
static int clean_own_resources(struct clean_state *state, struct pdf_object *object, struct clean_source *source, struct pdf_object **resources);
static int clean_drop_direct(struct clean_state *state, const struct pdf_object *object);
static void clean_write_limits(struct clean_state *state, const struct pdf_object *node, const struct pdf_object *limits, int depth);
static int clean_tree_bounds(struct clean_state *state, const struct pdf_object *node, const struct pdf_object **low, const struct pdf_object **high, int depth);
static int clean_string_before(const struct pdf_object *left, const struct pdf_object *right);
static int clean_mark_stream(struct clean_state *state, struct pdf_object *stream, struct clean_category *categories);
static int clean_skip_inline(struct pdf_lexer *lexer);
static int clean_is_space(unsigned char byte);
static void clean_mark_name(struct clean_category *categories, const struct pdf_token *token);
static int clean_follow_borrowed(struct clean_state *state, struct clean_category *categories);
static void clean_write_queued(struct clean_state *state, struct pdf_object *reference);
static void clean_write_trailer(struct clean_state *state, struct pdf_object *trailer, size_t xref);
static void clean_write_value(struct clean_state *state, const struct pdf_object *object, int pairs, int depth);
static void clean_write_entries(struct clean_state *state, const struct pdf_object *dictionary, const char *const *skipped, size_t skipped_count, int depth);
static unsigned long clean_number(struct clean_state *state, const struct pdf_object *reference);
static int clean_is_dropped(const struct clean_state *state, const struct pdf_object *object);
static int clean_is_tree_node(const struct pdf_object *object);
static int clean_key_is(const struct pdf_object *key, const char *name);
static int clean_save_file(const struct pdf_buffer *file, const char *path);
static void clean_free(struct clean_state *state, struct clean_category *categories);

/*
 * Writes a clean copy of a document to a path (ws175-p009).
 *
 * attachment names an attached file the copy leaves out (NULL: none);
 * counts, when not NULL, tells how many objects the copy has and how many
 * page resources it left out.  A signed document is refused with
 * PDF_ESIGNED and an encrypted one with EACCES, as an update refuses them.
 * Returns 0, or an errno value (the path may then hold part of a file).
 */
int
pdf_document_save_clean(
	struct pdf_document *document,
	const char *path,
	const char *attachment,
	struct pdf_clean_counts *counts)
{
	struct clean_state state;
	struct pdf_buffer file;
	struct pdf_object *trailer;
	struct pdf_object *catalog;
	struct pdf_crypt *crypt;
	size_t pages;
	size_t index;
	size_t xref;
	int is_signed;
	int error;

	/* Refuses a missing document or path. */
	if (document == NULL)
		return EINVAL;
	if (path == NULL)
		return EINVAL;

	/* Refuses a signed document, whose signatures the copy would break, and an encrypted one. */
	error = pdf_document_signed(document, &is_signed);
	if (error != 0)
		return error;
	if (is_signed)
		return PDF_ESIGNED;
	crypt = pdf_reader_crypt(document);
	if (crypt != NULL)
		return EACCES;

	/* Refuses a document without pages, which is not a valid PDF. */
	pages = pdf_document_page_count(document);
	if (pages == 0)
		return PDF_EFORMAT;

	/* The numbering: the catalog, the page tree node and the pages first. */
	memset(&state, 0, sizeof(state));
	memset(&file, 0, sizeof(file));
	state.document = document;
	state.file = &file;
	error = clean_prepare(&state, pages);
	if (error != 0) {
		clean_free(&state, NULL);
		return error;
	}

	/* The attached file left out, by its file specification. */
	pdf_reader_roots(document, &trailer, &catalog);
	if (attachment != NULL) {
		error = clean_drop_attachment(&state, catalog, attachment);
		if (error != 0) {
			clean_free(&state, NULL);
			return error;
		}
	}

	/* The information dictionary is numbered before anything is written, so the trailer can name it. */
	(void)clean_number(&state, pdf_object_get(trailer, "Info"));

	/* The header, with the bytes that mark the file as binary. */
	pdf_buffer_printf(&file, "%%PDF-1.7\n%%\xE2\xE3\xCF\xD3\n");

	/* The catalog, the page tree node and each page. */
	clean_write_catalog(&state, catalog);
	clean_write_tree(&state, pages);
	for (index = 0; index < pages; index++)
		clean_write_page(&state, index);

	/* Every object they reach, and those reach, in the order they were numbered. */
	while (state.queue_head < state.queue_length && file.error == 0) {
		clean_write_queued(&state, state.queue[state.queue_head]);
		state.queue_head++;
	}

	/* The cross-reference table and the trailer. */
	xref = file.length;
	clean_write_trailer(&state, trailer, xref);

	/* A file that could not be laid out is not written. */
	error = file.error;
	if (error == 0)
		error = clean_save_file(&file, path);

	/* The counts. */
	if (error == 0 && counts != NULL) {
		counts->objects = (size_t)(state.next - 1UL);
		counts->dropped = state.dropped;
	}

	/* Frees the layout and the file's bytes. */
	free(file.data);
	clean_free(&state, NULL);
	return error;
}

/*
 * Makes the tables of a copy and numbers the pages: each page's object
 * takes the number of its place, so references to it (an outline's, a
 * link's) reach it.
 */
static int
clean_prepare(
	struct clean_state *state,
	size_t pages)
{
	struct pdf_object *reference;
	struct pdf_object *trailer;
	struct pdf_object *catalog;
	struct pdf_object *root;
	size_t index;
	int error;

	/* One entry for each object number of the document. */
	state->count = pdf_reader_next_number(state->document);
	state->numbers = calloc((size_t)state->count, sizeof(*state->numbers));
	state->kinds = calloc((size_t)state->count, sizeof(*state->kinds));
	state->queue = calloc((size_t)state->count, sizeof(*state->queue));
	state->offsets = calloc((size_t)state->count + pages + CLEAN_FIRST_PAGE, sizeof(*state->offsets));
	if (state->numbers == NULL || state->kinds == NULL || state->queue == NULL || state->offsets == NULL)
		return ENOMEM;

	/* The catalog's number, when the trailer names it by reference. */
	pdf_reader_roots(state->document, &trailer, &catalog);
	root = pdf_object_get(trailer, "Root");
	if (root != NULL && root->type == PDF_OBJECT_REFERENCE && root->number < state->count) {
		state->numbers[root->number] = CLEAN_CATALOG;
		state->kinds[root->number] = CLEAN_KIND_COPIED;
	}

	/* Each page's, in order; a page written in its parent has none. */
	for (index = 0; index < pages; index++) {
		/* The page's own number, when it is an object of its own. */
		error = pdf_reader_page_reference(state->document, index, &reference);
		if (error == ENOTSUP)
			continue;
		if (error != 0)
			return error;
		if (reference->number >= state->count)
			return PDF_EFORMAT;

		/* A page listed twice is refused: its one object cannot be two pages of the copy. */
		if (state->numbers[reference->number] != 0)
			return PDF_EFORMAT;
		state->numbers[reference->number] = CLEAN_FIRST_PAGE + (unsigned long)index;
		state->kinds[reference->number] = CLEAN_KIND_COPIED;
	}

	/* The other objects are numbered after the pages. */
	state->next = CLEAN_FIRST_PAGE + (unsigned long)pages;
	return 0;
}

/*
 * Marks the file specifications of an attached file to be left out: the
 * EmbeddedFiles name tree's entry of its name, and the catalog's /AF items
 * that name it.  A document without the file is copied whole.
 */
static int
clean_drop_attachment(
	struct clean_state *state,
	struct pdf_object *catalog,
	const char *attachment)
{
	struct pdf_object *names;
	struct pdf_object *tree;
	int error;

	/* The catalog's name dictionary and its EmbeddedFiles tree. */
	error = pdf_reader_resolve_key(state->document, catalog, "Names", &names);
	if (error != 0)
		return error;
	error = pdf_reader_resolve_key(state->document, names, "EmbeddedFiles", &tree);
	if (error != 0)
		return error;

	/* The tree's entries of the name. */
	if (tree->type == PDF_OBJECT_DICTIONARY) {
		error = clean_drop_names(state, tree, attachment, 0);
		if (error != 0)
			return error;
	}

	/* The catalog's associated files of the name. */
	error = clean_drop_associated(state, catalog, attachment);
	if (error != 0)
		return error;

	/* Succeeded: the file's specifications are marked. */
	return 0;
}

/* Marks the file specifications of a name in a node of the EmbeddedFiles name tree and under it. */
static int
clean_drop_names(
	struct clean_state *state,
	struct pdf_object *node,
	const char *attachment,
	int depth)
{
	struct pdf_object *names;
	struct pdf_object *kids;
	struct pdf_object *key;
	struct pdf_object *value;
	struct pdf_object *kid;
	size_t index;
	int matches;
	int error;

	/* Refuses a tree deeper than the reader reads. */
	if (depth > PDF_READER_DEPTH_MAX)
		return PDF_EFORMAT;

	/* A leaf's pairs: the value of a key that is the name is dropped. */
	error = pdf_reader_resolve_key(state->document, node, "Names", &names);
	if (error != 0)
		return error;
	if (names->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index + 1 < names->count; index += 2) {
			/* The key, which may be a reference. */
			error = pdf_reader_resolve(state->document, names->values[index], &key);
			if (error != 0)
				return error;
			matches = clean_string_is(key, attachment);
			if (!matches)
				continue;

			/* The specification is left out, an object of its own or one written in the tree (ws177-p011). */
			value = names->values[index + 1];
			state->names_dropped = 1;
			if (value->type != PDF_OBJECT_REFERENCE) {
				error = clean_drop_direct(state, value);
				if (error != 0)
					return error;
				continue;
			}

			/* An object of its own. */
			if (value->number < state->count)
				state->kinds[value->number] = CLEAN_KIND_DROPPED;
		}
	}

	/* The kids of an intermediate node. */
	error = pdf_reader_resolve_key(state->document, node, "Kids", &kids);
	if (error != 0)
		return error;
	if (kids->type != PDF_OBJECT_ARRAY)
		return 0;
	for (index = 0; index < kids->count; index++) {
		/* Each kid that is a dictionary. */
		error = pdf_reader_resolve(state->document, kids->values[index], &kid);
		if (error != 0)
			return error;
		if (kid->type != PDF_OBJECT_DICTIONARY)
			continue;
		error = clean_drop_names(state, kid, attachment, depth + 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the node's entries of the name are marked. */
	return 0;
}

/* Marks the catalog's associated files (/AF) whose specification names the attached file. */
static int
clean_drop_associated(
	struct clean_state *state,
	struct pdf_object *catalog,
	const char *attachment)
{
	struct pdf_object *associated;
	struct pdf_object *item;
	struct pdf_object *specification;
	struct pdf_object *name;
	size_t index;
	int matches;
	int error;

	/* The catalog's list, when it has one. */
	error = pdf_reader_resolve_key(state->document, catalog, "AF", &associated);
	if (error != 0)
		return error;
	if (associated->type != PDF_OBJECT_ARRAY)
		return 0;

	/* Each item that is a specification of the name, an object of its own or written in the list (ws177-p011). */
	for (index = 0; index < associated->count; index++) {
		/* A reference past the document's objects names nothing. */
		item = associated->values[index];
		if (item->type == PDF_OBJECT_REFERENCE && item->number >= state->count)
			continue;
		error = pdf_reader_resolve(state->document, item, &specification);
		if (error != 0)
			return error;
		if (specification->type != PDF_OBJECT_DICTIONARY)
			continue;

		/* The specification's Unicode name, or its plain one. */
		error = pdf_reader_resolve_key(state->document, specification, "UF", &name);
		if (error != 0)
			return error;
		if (name->type != PDF_OBJECT_STRING) {
			error = pdf_reader_resolve_key(state->document, specification, "F", &name);
			if (error != 0)
				return error;
		}

		/* A specification that names another file stays. */
		matches = clean_string_is(name, attachment);
		if (!matches)
			continue;

		/* The specification is left out: one written in the list by itself, else its object. */
		if (item->type != PDF_OBJECT_REFERENCE) {
			error = clean_drop_direct(state, item);
			if (error != 0)
				return error;
			continue;
		}

		/* An object of its own. */
		state->kinds[item->number] = CLEAN_KIND_DROPPED;
	}

	/* Succeeded: the list's specifications of the name are marked. */
	return 0;
}

/* Tells whether an object is a string of exactly a text's bytes. */
static int
clean_string_is(
	const struct pdf_object *object,
	const char *text)
{
	size_t length;
	int difference;

	/* A string of the same length and bytes. */
	if (object->type != PDF_OBJECT_STRING)
		return 0;
	length = strlen(text);
	if (object->length != length)
		return 0;
	difference = memcmp(object->bytes, text, length);
	if (difference != 0)
		return 0;

	/* The string is the text. */
	return 1;
}

/* Writes the catalog: its entries as they are, its page tree the copy's one node. */
static void
clean_write_catalog(
	struct clean_state *state,
	struct pdf_object *catalog)
{
	static const char *const skipped[] = { "Pages" };

	/* The object, its entries but /Pages, then the copy's node. */
	state->offsets[CLEAN_CATALOG] = state->file->length;
	pdf_buffer_printf(state->file, "%lu 0 obj\n<<", CLEAN_CATALOG);
	clean_write_entries(state, catalog, skipped, sizeof(skipped) / sizeof(skipped[0]), 0);
	pdf_buffer_printf(state->file, " /Pages %lu 0 R >>\nendobj\n", CLEAN_PAGES);
}

/* Writes the copy's page tree: one node whose kids are all the pages, in order. */
static void
clean_write_tree(
	struct clean_state *state,
	size_t pages)
{
	size_t index;

	/* The node, then each page's reference. */
	state->offsets[CLEAN_PAGES] = state->file->length;
	pdf_buffer_printf(state->file, "%lu 0 obj\n<< /Type /Pages /Kids [", CLEAN_PAGES);
	for (index = 0; index < pages; index++) {
		if (index != 0)
			pdf_buffer_append(state->file, " ", 1);
		pdf_buffer_printf(state->file, "%lu 0 R", CLEAN_FIRST_PAGE + (unsigned long)index);
	}

	/* The count, and the end of the object. */
	pdf_buffer_printf(state->file, "] /Count %lu >>\nendobj\n", (unsigned long)pages);
}

/*
 * Writes a page: its entries as they are, its parent the copy's node, and
 * the boxes, rotation and resources it has or inherited written on it, the
 * resources but the entries its content does not name.
 */
static void
clean_write_page(
	struct clean_state *state,
	size_t index)
{
	static const char *const skipped[] = { "Parent", "MediaBox", "CropBox", "Rotate", "Resources" };
	struct clean_source source;
	struct pdf_object *page;
	struct pdf_object *resources;
	struct pdf_object *media_box;
	struct pdf_object *crop_box;
	struct pdf_object *rotate;
	unsigned long number;
	int error;

	/* The page and what it inherits. */
	error = pdf_reader_page(state->document, index, &page, &resources);
	if (error == 0)
		error = pdf_reader_page_inherited(state->document, index, &media_box, &crop_box, &rotate);
	if (error != 0) {
		pdf_buffer_fail(state->file, error);
		return;
	}

	/* The object, its own entries but those written below. */
	number = CLEAN_FIRST_PAGE + (unsigned long)index;
	state->offsets[number] = state->file->length;
	pdf_buffer_printf(state->file, "%lu 0 obj\n<<", number);
	clean_write_entries(state, page, skipped, sizeof(skipped) / sizeof(skipped[0]), 0);
	pdf_buffer_printf(state->file, " /Parent %lu 0 R", CLEAN_PAGES);

	/* The boxes and the rotation, where the page has or inherits them. */
	if (media_box != NULL) {
		pdf_buffer_printf(state->file, " /MediaBox ");
		clean_write_value(state, media_box, 0, 1);
	}

	/* The crop box. */
	if (crop_box != NULL) {
		pdf_buffer_printf(state->file, " /CropBox ");
		clean_write_value(state, crop_box, 0, 1);
	}

	/* The rotation. */
	if (rotate != NULL) {
		pdf_buffer_printf(state->file, " /Rotate ");
		clean_write_value(state, rotate, 0, 1);
	}

	/* The resources, but those the content does not name. */
	if (resources->type == PDF_OBJECT_DICTIONARY) {
		source.kind = CLEAN_SOURCE_PAGE;
		source.page = index;
		source.holder = page;
		clean_write_resources(state, &source, resources);
	}

	/* The end of the object. */
	pdf_buffer_printf(state->file, " >>\nendobj\n");
}

/*
 * Writes the resources of a page, a form, a tiling pattern or a Type 3
 * font (source) as a direct dictionary: each category whose entries the
 * content names keeps only the entries named (design.md [M11]), those a
 * resource that borrows them names too (ws177-p011); the other entries are
 * written as they are.  Resources whose content cannot be read keep them
 * all.
 */
static void
clean_write_resources(
	struct clean_state *state,
	const struct clean_source *source,
	struct pdf_object *resources)
{
	static const char *const names[CLEAN_CATEGORIES] = { "XObject", "Font", "ExtGState", "Pattern", "Shading", "ColorSpace", "Properties" };
	struct clean_category categories[CLEAN_CATEGORIES];
	struct clean_category *category;
	struct pdf_object *key;
	size_t entry;
	size_t which;
	int is_category;
	int pruned;
	int error;

	/* Each category's dictionary, and a mark for each of its entries. */
	memset(categories, 0, sizeof(categories));
	for (which = 0; which < CLEAN_CATEGORIES; which++) {
		/* The category's direct dictionary, when the resources have one. */
		categories[which].name = names[which];
		error = pdf_reader_resolve_key(state->document, resources, names[which], &categories[which].dictionary);
		if (error != 0) {
			pdf_buffer_fail(state->file, error);
			clean_free(NULL, categories);
			return;
		}

		/* A category that is not a dictionary is written as it is. */
		if (categories[which].dictionary->type != PDF_OBJECT_DICTIONARY) {
			categories[which].dictionary = NULL;
			continue;
		}

		/* No entry is named yet. */
		categories[which].used = calloc(categories[which].dictionary->count + 1, 1);
		if (categories[which].used == NULL) {
			pdf_buffer_fail(state->file, ENOMEM);
			clean_free(NULL, categories);
			return;
		}
	}

	/* The names the content uses; a content that cannot be read keeps everything. */
	pruned = 0;
	error = clean_find_used(state, source, categories);
	if (error == 0)
		error = clean_follow_borrowed(state, categories);

	/* Memory running out fails the copy. */
	if (error == ENOMEM) {
		pdf_buffer_fail(state->file, error);
		clean_free(NULL, categories);
		return;
	}

	/* A content read, with what borrows the resources, prunes them. */
	if (error == 0)
		pruned = 1;

	/* The dictionary: each key, a pruned category with only its entries named. */
	pdf_buffer_printf(state->file, " /Resources <<");
	for (entry = 0; entry < resources->count; entry++) {
		/* The category the key is, if any, with its dictionary. */
		key = resources->keys[entry];
		category = NULL;
		for (which = 0; which < CLEAN_CATEGORIES; which++) {
			if (categories[which].dictionary == NULL)
				continue;
			is_category = clean_key_is(key, categories[which].name);
			if (is_category)
				category = &categories[which];
		}

		/* A key written as it is: not a category, or nothing pruned. */
		pdf_buffer_append(state->file, " ", 1);
		pdf_writer_write_name(state->file, key->bytes, key->length);
		pdf_buffer_append(state->file, " ", 1);
		if (category == NULL || !pruned) {
			clean_write_value(state, resources->values[entry], 0, 1);
			continue;
		}

		/* The category's entries the content names; the others are counted as left out. */
		pdf_buffer_printf(state->file, "<<");
		for (which = 0; which < category->dictionary->count; which++) {
			if (!category->used[which]) {
				state->dropped++;
				continue;
			}

			/* An entry named, as it is. */
			pdf_buffer_append(state->file, " ", 1);
			pdf_writer_write_name(state->file, category->dictionary->keys[which]->bytes, category->dictionary->keys[which]->length);
			pdf_buffer_append(state->file, " ", 1);
			clean_write_value(state, category->dictionary->values[which], 0, 2);
		}

		/* The category ends. */
		pdf_buffer_printf(state->file, " >>");
	}

	/* The dictionary ends; the marks go. */
	pdf_buffer_printf(state->file, " >>");
	clean_free(NULL, categories);
}

/*
 * Marks the resource entries a content names: every name token of its
 * streams (a name in an operand, a marked content's properties or an
 * inline image's dictionary counts), which keeps a resource whenever the
 * content could use it.  The content is a page's streams, a form's or
 * pattern's stream, or a Type 3 font's glyph procedures.  Returns 0, or an
 * errno value when a stream cannot be read (PDF_EFORMAT, ENOTSUP) or
 * memory runs out (ENOMEM).
 */
static int
clean_find_used(
	struct clean_state *state,
	const struct clean_source *source,
	struct clean_category *categories)
{
	int error;

	/* The streams of each kind of holder. */
	switch (source->kind) {
	case CLEAN_SOURCE_PAGE:
		error = clean_find_page(state, source->page, categories);
		break;
	case CLEAN_SOURCE_STREAM:
		error = clean_mark_stream(state, source->holder, categories);
		break;
	default:
		error = clean_mark_glyphs(state, source->holder, categories);
		break;
	}

	/* Reports a content that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the names the content uses are marked. */
	return 0;
}

/* Marks the resource entries a page's content streams name (clean_find_used). */
static int
clean_find_page(
	struct clean_state *state,
	size_t index,
	struct clean_category *categories)
{
	struct pdf_object *page;
	struct pdf_object *resources;
	struct pdf_object *contents;
	struct pdf_object *stream;
	size_t item;
	int error;

	/* The page's content: one stream, an array of them, or nothing. */
	error = pdf_reader_page(state->document, index, &page, &resources);
	if (error != 0)
		return error;
	error = pdf_reader_resolve_key(state->document, page, "Contents", &contents);
	if (error != 0)
		return error;

	/* One stream. */
	if (contents->type == PDF_OBJECT_STREAM)
		return clean_mark_stream(state, contents, categories);

	/* Each stream of an array. */
	if (contents->type == PDF_OBJECT_ARRAY) {
		for (item = 0; item < contents->count; item++) {
			/* Each item must be a stream. */
			error = pdf_reader_resolve(state->document, contents->values[item], &stream);
			if (error != 0)
				return error;
			if (stream->type != PDF_OBJECT_STREAM)
				return PDF_EFORMAT;
			error = clean_mark_stream(state, stream, categories);
			if (error != 0)
				return error;
		}

		/* Every stream was read. */
		return 0;
	}

	/* A page without content names nothing; anything else is not content. */
	if (contents->type == PDF_OBJECT_NULL)
		return 0;
	return PDF_EFORMAT;
}

/* Marks the resource entries a Type 3 font's glyph procedures (/CharProcs) name. */
static int
clean_mark_glyphs(
	struct clean_state *state,
	struct pdf_object *font,
	struct clean_category *categories)
{
	struct pdf_object *procedures;
	struct pdf_object *stream;
	size_t item;
	int error;

	/* The procedures; a font without them names nothing. */
	error = pdf_reader_resolve_key(state->document, font, "CharProcs", &procedures);
	if (error != 0)
		return error;
	if (procedures->type == PDF_OBJECT_NULL)
		return 0;
	if (procedures->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* Each glyph's stream. */
	for (item = 0; item < procedures->count; item++) {
		/* Each value must be a stream. */
		error = pdf_reader_resolve(state->document, procedures->values[item], &stream);
		if (error != 0)
			return error;
		if (stream->type != PDF_OBJECT_STREAM)
			return PDF_EFORMAT;
		error = clean_mark_stream(state, stream, categories);
		if (error != 0)
			return error;
	}

	/* Succeeded: every glyph's names are marked. */
	return 0;
}

/* Marks the resource entries one content stream names. */
static int
clean_mark_stream(
	struct clean_state *state,
	struct pdf_object *stream,
	struct clean_category *categories)
{
	const unsigned char *data;
	struct pdf_arena arena;
	struct pdf_lexer lexer;
	struct pdf_token token;
	unsigned char *owned;
	size_t size;
	int dct;
	int is_inline;
	int error;

	/* The decoded bytes. */
	error = pdf_filter_decode(state->document, stream, 0, &data, &size, &owned, &dct);
	if (error != 0)
		return error;

	/* Each token: a name is marked, an inline image's data is skipped. */
	memset(&arena, 0, sizeof(arena));
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = data;
	lexer.size = size;
	lexer.arena = &arena;
	for (;;) {
		/* The next token; the end of the bytes ends the stream. */
		error = pdf_lexer_next(&lexer, &token);
		if (error != 0)
			break;
		if (token.type == PDF_TOKEN_END)
			break;

		/* A name. */
		if (token.type == PDF_TOKEN_NAME) {
			clean_mark_name(categories, &token);
			continue;
		}

		/* The data of an inline image, which is not tokens. */
		is_inline = pdf_token_is_keyword(&token, "ID");
		if (is_inline) {
			error = clean_skip_inline(&lexer);
			if (error != 0)
				break;
		}
	}

	/* Frees the tokens and the decoded bytes. */
	pdf_arena_free(&arena);
	free(owned);
	return error;
}

/*
 * Skips an inline image's data after its ID: past the white space byte
 * that follows ID, to after the first EI between white space (or at the
 * end).  Returns 0, or PDF_EFORMAT when no EI ends the data.
 */
static int
clean_skip_inline(
	struct pdf_lexer *lexer)
{
	const unsigned char *data;
	size_t position;
	int before;
	int after;

	/* The single white space byte after ID (EI needs one before it, so the data starts one further). */
	data = lexer->data;
	position = lexer->position + 1;

	/* The first EI with white space before it and white space or the end after it. */
	for (; position + 2 <= lexer->size; position++) {
		/* EI at this place. */
		if (data[position] != 'E' || data[position + 1] != 'I')
			continue;

		/* White space around it. */
		before = clean_is_space(data[position - 1]);
		after = position + 2 == lexer->size;
		if (!after)
			after = clean_is_space(data[position + 2]);
		if (before && after) {
			lexer->position = position + 2;
			return 0;
		}
	}

	/* The data has no end. */
	return PDF_EFORMAT;
}

/* Tells whether a byte is PDF's white space. */
static int
clean_is_space(
	unsigned char byte)
{
	/* NUL, tab, line feed, form feed, carriage return and space. */
	switch (byte) {
	case 0x00:
	case 0x09:
	case 0x0a:
	case 0x0c:
	case 0x0d:
	case 0x20:
		return 1;
	default:
		return 0;
	}
}

/* Marks the resource entries of a name, in every category. */
static void
clean_mark_name(
	struct clean_category *categories,
	const struct pdf_token *token)
{
	const struct pdf_object *key;
	size_t which;
	size_t entry;
	int difference;

	/* Each category's entries of the same bytes. */
	for (which = 0; which < CLEAN_CATEGORIES; which++) {
		/* A category the resources do not have. */
		if (categories[which].dictionary == NULL)
			continue;

		/* Its keys, compared by length and bytes. */
		for (entry = 0; entry < categories[which].dictionary->count; entry++) {
			key = categories[which].dictionary->keys[entry];
			if (key->length != token->length)
				continue;
			difference = memcmp(key->bytes, token->bytes, token->length);
			if (difference == 0 && categories[which].used[entry] == CLEAN_UNUSED)
				categories[which].used[entry] = CLEAN_NAMED;
		}
	}
}

/*
 * Follows the resources the content names that draw with the resources
 * written for want of their own (ws177-p011): a form XObject, a Type 3
 * font or a tiling pattern without /Resources.  Their content's names are
 * marked too, and the resources they name in turn, until no named entry
 * is left to follow.  Returns 0, or an errno value when one's content
 * cannot be read (the resources are then kept whole).
 */
static int
clean_follow_borrowed(
	struct clean_state *state,
	struct clean_category *categories)
{
	struct pdf_object *value;
	struct pdf_object *own;
	struct pdf_object *subtype;
	struct pdf_object *pattern;
	size_t which;
	size_t entry;
	int followed;
	int is_form;
	int is_type3;
	int error;

	/* Each round follows the entries named since the last; a round that follows none ends it. */
	do {
		followed = 0;
		for (which = 0; which < CLEAN_CATEGORIES; which++) {
			/* The categories whose entries can draw. */
			if (which != CLEAN_XOBJECT && which != CLEAN_FONT && which != CLEAN_PATTERN)
				continue;
			if (categories[which].dictionary == NULL)
				continue;

			/* Each entry named and not followed yet. */
			for (entry = 0; entry < categories[which].dictionary->count; entry++) {
				/* An entry not named, or followed already. */
				if (categories[which].used[entry] != CLEAN_NAMED)
					continue;
				categories[which].used[entry] = CLEAN_FOLLOWED;
				followed = 1;

				/* The entry, which must be readable. */
				error = pdf_reader_resolve(state->document, categories[which].dictionary->values[entry], &value);
				if (error != 0)
					return error;
				if (value->type != PDF_OBJECT_DICTIONARY && value->type != PDF_OBJECT_STREAM)
					continue;

				/* One that has resources of its own uses them, and they are kept by its own content. */
				own = pdf_object_get(value, "Resources");
				if (own != NULL)
					continue;

				/* A form, a Type 3 font or a tiling pattern without them names entries of these. */
				subtype = pdf_object_get(value, "Subtype");
				is_form = pdf_object_is_name(subtype, "Form");
				is_type3 = pdf_object_is_name(subtype, "Type3");
				pattern = pdf_object_get(value, "PatternType");
				error = 0;
				if (is_type3) {
					error = clean_mark_glyphs(state, value, categories);
				} else if (value->type == PDF_OBJECT_STREAM && (is_form || pattern != NULL)) {
					error = clean_mark_stream(state, value, categories);
				}

				/* Reports a content that could not be read. */
				if (error != 0)
					return error;
			}
		}
	} while (followed);

	/* Succeeded: every named entry is followed. */
	return 0;
}

/*
 * Tells whether an object the copy writes has resources of its own to
 * keep by its content's names: a form or a tiling pattern (its stream),
 * or a Type 3 font (its glyph procedures), with a /Resources dictionary.
 * Returns 1 with the source and the resources, or 0.
 */
static int
clean_own_resources(
	struct clean_state *state,
	struct pdf_object *object,
	struct clean_source *source,
	struct pdf_object **resources)
{
	struct pdf_object *subtype;
	struct pdf_object *pattern;
	int is_form;
	int is_type3;
	int error;

	/* Its resources, which must be a dictionary. */
	if (object->type != PDF_OBJECT_DICTIONARY && object->type != PDF_OBJECT_STREAM)
		return 0;
	error = pdf_reader_resolve_key(state->document, object, "Resources", resources);
	if (error != 0)
		return 0;
	if ((*resources)->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* What it is. */
	subtype = pdf_object_get(object, "Subtype");
	is_form = pdf_object_is_name(subtype, "Form");
	is_type3 = pdf_object_is_name(subtype, "Type3");
	pattern = pdf_object_get(object, "PatternType");
	source->page = 0;
	source->holder = object;

	/* A form's or a tiling pattern's stream. */
	if (object->type == PDF_OBJECT_STREAM && (is_form || pattern != NULL)) {
		source->kind = CLEAN_SOURCE_STREAM;
		return 1;
	}

	/* A Type 3 font's glyph procedures. */
	if (object->type == PDF_OBJECT_DICTIONARY && is_type3) {
		source->kind = CLEAN_SOURCE_TYPE3;
		return 1;
	}

	/* Anything else keeps its resources as they are. */
	return 0;
}

/* Writes an object the copy numbered: a dictionary, a stream with its encoded bytes, or any other object. */
static void
clean_write_queued(
	struct clean_state *state,
	struct pdf_object *reference)
{
	static const char *const skipped[] = { "Length" };
	static const char *const own_skipped[] = { "Resources", "Length" };
	const unsigned char *bytes;
	struct clean_source source;
	struct pdf_object *object;
	struct pdf_object *resources;
	unsigned long number;
	int own;
	int error;

	/* The object, which clean_number() read before. */
	error = pdf_reader_resolve(state->document, reference, &object);
	if (error != 0) {
		pdf_buffer_fail(state->file, error);
		return;
	}

	/* Its number in the copy, and where it begins. */
	number = state->numbers[reference->number];
	state->offsets[number] = state->file->length;
	pdf_buffer_printf(state->file, "%lu 0 obj\n", number);

	/* A form, a tiling pattern or a Type 3 font keeps its own resources by its content's names (ws177-p011). */
	own = clean_own_resources(state, object, &source, &resources);

	/* A Type 3 font: its entries, its resources kept so. */
	if (own && object->type == PDF_OBJECT_DICTIONARY) {
		pdf_buffer_append(state->file, "<<", 2);
		clean_write_entries(state, object, own_skipped, 1, 0);
		clean_write_resources(state, &source, resources);
		pdf_buffer_printf(state->file, " >>\nendobj\n");
		return;
	}

	/* Anything else but a stream is written as its value. */
	if (object->type != PDF_OBJECT_STREAM) {
		clean_write_value(state, object, 0, 0);
		pdf_buffer_printf(state->file, "\nendobj\n");
		return;
	}

	/* A stream: its dictionary (a form's or pattern's resources kept so) with a direct length, then its bytes as the file has them. */
	pdf_buffer_append(state->file, "<<", 2);
	if (own) {
		clean_write_entries(state, object, own_skipped, sizeof(own_skipped) / sizeof(own_skipped[0]), 0);
		clean_write_resources(state, &source, resources);
	} else {
		clean_write_entries(state, object, skipped, sizeof(skipped) / sizeof(skipped[0]), 0);
	}

	/* The direct length, then the bytes. */
	pdf_buffer_printf(state->file, " /Length %lu >>\nstream\n", (unsigned long)object->data_length);
	bytes = pdf_reader_bytes(state->document);
	pdf_buffer_append(state->file, bytes + object->data_offset, object->data_length);
	pdf_buffer_printf(state->file, "\nendstream\nendobj\n");
}

/*
 * Writes the cross-reference table of the copy's objects and its trailer:
 * the size, the catalog, the information dictionary when there is one,
 * and an identifier that keeps the document's permanent one with a new
 * version.
 */
static void
clean_write_trailer(
	struct clean_state *state,
	struct pdf_object *trailer,
	size_t xref)
{
	unsigned char version[PDF_WRITER_ID_SIZE];
	unsigned char permanent[PDF_WRITER_ID_SIZE];
	struct pdf_object *info;
	unsigned long number;
	unsigned long info_number;
	int error;

	/* The table: the free head, then each object's offset. */
	pdf_buffer_printf(state->file, "xref\n0 %lu\n0000000000 65535 f \n", state->next);
	for (number = 1; number < state->next; number++)
		pdf_buffer_printf(state->file, "%010lu 00000 n \n", (unsigned long)state->offsets[number]);

	/* The permanent identifier the document has (a new one when it has none), and a new version. */
	error = pdf_document_get_id(state->document, permanent);
	if (error != 0)
		arc4random_buf(permanent, sizeof(permanent));
	arc4random_buf(version, sizeof(version));

	/* The trailer. */
	pdf_buffer_printf(state->file, "trailer\n<< /Size %lu /Root %lu 0 R", state->next, CLEAN_CATALOG);
	info = pdf_object_get(trailer, "Info");
	info_number = 0;
	if (info != NULL && info->type == PDF_OBJECT_REFERENCE && info->number < state->count)
		info_number = state->numbers[info->number];
	if (info_number != 0)
		pdf_buffer_printf(state->file, " /Info %lu 0 R", info_number);
	pdf_buffer_printf(state->file, " /ID [");
	pdf_buffer_append_hex_string(state->file, permanent, sizeof(permanent));
	pdf_buffer_append(state->file, " ", 1);
	pdf_buffer_append_hex_string(state->file, version, sizeof(version));
	pdf_buffer_printf(state->file, "] >>\nstartxref\n%lu\n%%%%EOF\n", (unsigned long)xref);
}

/*
 * Writes a value as the reader read it, with references numbered for the
 * copy: a reference to an object left out is dropped from an array or a
 * dictionary, and with pairs (a name tree's /Names) its key with it.
 */
static void
clean_write_value(
	struct clean_state *state,
	const struct pdf_object *object,
	int pairs,
	int depth)
{
	unsigned long number;
	size_t index;
	int dropped;
	int first;

	/* Refuses nesting deeper than the reader reads. */
	if (depth > PDF_READER_DEPTH_MAX) {
		pdf_buffer_fail(state->file, PDF_EFORMAT);
		return;
	}

	/* A missing object is null. */
	if (object == NULL) {
		pdf_buffer_printf(state->file, "null");
		return;
	}

	/* Writes the object by its kind. */
	switch (object->type) {
	case PDF_OBJECT_NULL:
		pdf_buffer_printf(state->file, "null");
		break;
	case PDF_OBJECT_BOOLEAN:
		if (object->boolean)
			pdf_buffer_printf(state->file, "true");
		else
			pdf_buffer_printf(state->file, "false");
		break;
	case PDF_OBJECT_INTEGER:
		pdf_buffer_printf(state->file, "%ld", object->integer);
		break;
	case PDF_OBJECT_REAL:
		pdf_writer_write_real(state->file, object->real);
		break;
	case PDF_OBJECT_NAME:
		pdf_writer_write_name(state->file, object->bytes, object->length);
		break;
	case PDF_OBJECT_STRING:
		pdf_buffer_append_hex_string(state->file, object->bytes, object->length);
		break;
	case PDF_OBJECT_ARRAY:
		/* Each item but those left out, a space between two. */
		pdf_buffer_append(state->file, "[", 1);
		first = 1;
		for (index = 0; index < object->count; index++) {
			/* A pair whose value is left out goes whole; a single item left out goes. */
			if (pairs && index + 1 < object->count && index % 2 == 0) {
				dropped = clean_is_dropped(state, object->values[index + 1]);
				if (dropped) {
					index++;
					continue;
				}
			}

			/* A single item left out goes. */
			dropped = clean_is_dropped(state, object->values[index]);
			if (dropped)
				continue;

			/* The item. */
			if (!first)
				pdf_buffer_append(state->file, " ", 1);
			first = 0;
			clean_write_value(state, object->values[index], 0, depth + 1);
		}

		/* The array ends. */
		pdf_buffer_append(state->file, "]", 1);
		break;
	case PDF_OBJECT_DICTIONARY:
		pdf_buffer_append(state->file, "<<", 2);
		clean_write_entries(state, object, NULL, 0, depth);
		pdf_buffer_append(state->file, " >>", 3);
		break;
	case PDF_OBJECT_STREAM:
		pdf_buffer_fail(state->file, PDF_EFORMAT);
		break;
	case PDF_OBJECT_REFERENCE:
		/* The object's number in the copy, or null for one that is not there. */
		number = clean_number(state, object);
		if (number == 0) {
			pdf_buffer_printf(state->file, "null");
			break;
		}

		/* The reference. */
		pdf_buffer_printf(state->file, "%lu 0 R", number);
		break;
	}
}

/*
 * Writes the entries of a dictionary (" /key value" each) but those of the
 * skipped keys, and those whose value is a reference left out.
 */
static void
clean_write_entries(
	struct clean_state *state,
	const struct pdf_object *dictionary,
	const char *const *skipped,
	size_t skipped_count,
	int depth)
{
	const struct pdf_object *key;
	const struct pdf_object *value;
	size_t index;
	size_t which;
	int skip;
	int pairs;
	int limits;

	/* Each entry the caller does not write itself. */
	for (index = 0; index < dictionary->count; index++) {
		/* Leaves out the skipped keys. */
		key = dictionary->keys[index];
		value = dictionary->values[index];
		skip = 0;
		for (which = 0; which < skipped_count && !skip; which++)
			skip = clean_key_is(key, skipped[which]);
		if (skip)
			continue;

		/* Leaves out an entry whose value is left out. */
		skip = clean_is_dropped(state, value);
		if (skip)
			continue;

		/* A name tree's /Limits, once the tree lost a name, from the names it keeps (ws177-p011). */
		limits = clean_key_is(key, "Limits");
		if (limits && state->names_dropped) {
			clean_write_limits(state, dictionary, value, depth);
			continue;
		}

		/* A name tree's leaf (/Names, an array) drops its pairs whole. */
		pairs = 0;
		if (value->type == PDF_OBJECT_ARRAY)
			pairs = clean_key_is(key, "Names");

		/* The key and its value. */
		pdf_buffer_append(state->file, " ", 1);
		pdf_writer_write_name(state->file, key->bytes, key->length);
		pdf_buffer_append(state->file, " ", 1);
		clean_write_value(state, value, pairs, depth + 1);
	}
}

/*
 * Gives the number in the copy of the object a reference names, numbering
 * it and queueing it to be written when it is met first: a page tree node
 * becomes the copy's one node, and a missing or unreadable object, or one
 * left out, has none (0).
 */
static unsigned long
clean_number(
	struct clean_state *state,
	const struct pdf_object *reference)
{
	struct pdf_object *object;
	int is_node;
	int error;

	/* Only references are numbered; one past the document's numbers is missing. */
	if (reference == NULL || reference->type != PDF_OBJECT_REFERENCE)
		return 0;
	if (reference->number >= state->count)
		return 0;

	/* An object met before. */
	if (state->kinds[reference->number] == CLEAN_KIND_COPIED)
		return state->numbers[reference->number];
	if (state->kinds[reference->number] != CLEAN_KIND_UNSEEN)
		return 0;

	/* The object; one that cannot be read is null in the copy (memory running out fails the copy). */
	error = pdf_reader_resolve(state->document, (struct pdf_object *)reference, &object);
	if (error == ENOMEM)
		pdf_buffer_fail(state->file, error);
	if (error != 0 || object->type == PDF_OBJECT_NULL) {
		state->kinds[reference->number] = CLEAN_KIND_NULL;
		return 0;
	}

	/* A node of the page tree is the copy's node. */
	state->kinds[reference->number] = CLEAN_KIND_COPIED;
	is_node = clean_is_tree_node(object);
	if (is_node) {
		state->numbers[reference->number] = CLEAN_PAGES;
		return CLEAN_PAGES;
	}

	/* Any other object takes the next number and waits to be written. */
	state->numbers[reference->number] = state->next;
	state->next++;
	state->queue[state->queue_length] = (struct pdf_object *)reference;
	state->queue_length++;
	return state->numbers[reference->number];
}

/* Tells whether an object is a reference to an object the copy leaves out. */
static int
clean_is_dropped(
	const struct clean_state *state,
	const struct pdf_object *object)
{
	size_t index;

	/* An object written in place that the copy leaves out (ws177-p011). */
	if (object == NULL)
		return 0;
	for (index = 0; index < state->direct_count; index++) {
		if (state->direct[index] == object)
			return 1;
	}

	/* A reference to a number marked left out. */
	if (object->type != PDF_OBJECT_REFERENCE)
		return 0;
	if (object->number >= state->count)
		return 0;
	if (state->kinds[object->number] != CLEAN_KIND_DROPPED)
		return 0;

	/* The object is left out. */
	return 1;
}

/*
 * Leaves out an object written in place (a file specification in a name
 * tree's leaf or in /AF).  Returns 0, or E2BIG when CLEAN_DIRECT_MAX are
 * left out already.
 */
static int
clean_drop_direct(
	struct clean_state *state,
	const struct pdf_object *object)
{
	/* Refuses one more than the list holds. */
	if (state->direct_count == CLEAN_DIRECT_MAX)
		return E2BIG;

	/* Remembers it; clean_is_dropped finds it. */
	state->direct[state->direct_count] = object;
	state->direct_count++;

	/* Succeeded: the object is left out. */
	return 0;
}

/*
 * Writes a name tree node's /Limits from the least and greatest names its
 * leaves keep; a node that keeps none has no /Limits, and a node of
 * another kind of tree (a number tree's) keeps its own as they are.
 */
static void
clean_write_limits(
	struct clean_state *state,
	const struct pdf_object *node,
	const struct pdf_object *limits,
	int depth)
{
	const struct pdf_object *low;
	const struct pdf_object *high;
	int found;

	/* The names kept under the node. */
	low = NULL;
	high = NULL;
	found = clean_tree_bounds(state, node, &low, &high, 0);

	/* A node of another tree, or one that cannot be read, keeps its limits. */
	if (found < 0) {
		pdf_buffer_printf(state->file, " /Limits ");
		clean_write_value(state, limits, 0, depth + 1);
		return;
	}

	/* A node that keeps no name has none. */
	if (low == NULL)
		return;

	/* The least and the greatest. */
	pdf_buffer_printf(state->file, " /Limits [");
	pdf_buffer_append_hex_string(state->file, low->bytes, low->length);
	pdf_buffer_append(state->file, " ", 1);
	pdf_buffer_append_hex_string(state->file, high->bytes, high->length);
	pdf_buffer_append(state->file, "]", 1);
}

/*
 * Finds the least and the greatest names a name tree node keeps, in its
 * own pairs (/Names) and its kids'.  Returns 0 (low and high NULL when
 * none is kept), or -1 for a node that is not a name tree's or cannot be
 * read.
 */
static int
clean_tree_bounds(
	struct clean_state *state,
	const struct pdf_object *node,
	const struct pdf_object **low,
	const struct pdf_object **high,
	int depth)
{
	struct pdf_object *names;
	struct pdf_object *kids;
	struct pdf_object *kid;
	struct pdf_object *key;
	size_t index;
	int dropped;
	int before;
	int found;
	int error;

	/* Refuses a tree deeper than the reader reads. */
	if (depth > PDF_READER_DEPTH_MAX)
		return -1;

	/* The node's own pairs and its kids. */
	error = pdf_reader_resolve_key(state->document, node, "Names", &names);
	if (error != 0)
		return -1;
	error = pdf_reader_resolve_key(state->document, node, "Kids", &kids);
	if (error != 0)
		return -1;

	/* A node with neither is not a name tree's (a number tree's has /Nums). */
	if (names->type != PDF_OBJECT_ARRAY && kids->type != PDF_OBJECT_ARRAY)
		return -1;

	/* Each pair kept: its name widens the bounds. */
	if (names->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index + 1 < names->count; index += 2) {
			/* A pair whose value is left out. */
			dropped = clean_is_dropped(state, names->values[index + 1]);
			if (dropped)
				continue;

			/* Its name, which must be a string. */
			error = pdf_reader_resolve(state->document, names->values[index], &key);
			if (error != 0)
				return -1;
			if (key->type != PDF_OBJECT_STRING)
				return -1;

			/* The least so far. */
			before = 0;
			if (*low != NULL)
				before = clean_string_before(key, *low);
			if (*low == NULL || before)
				*low = key;

			/* The greatest so far. */
			before = 0;
			if (*high != NULL)
				before = clean_string_before(*high, key);
			if (*high == NULL || before)
				*high = key;
		}
	}

	/* Each kid's names. */
	if (kids->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index < kids->count; index++) {
			/* The kid, which must be a node. */
			error = pdf_reader_resolve(state->document, kids->values[index], &kid);
			if (error != 0)
				return -1;
			if (kid->type != PDF_OBJECT_DICTIONARY)
				return -1;
			found = clean_tree_bounds(state, kid, low, high, depth + 1);
			if (found != 0)
				return -1;
		}
	}

	/* Succeeded: the bounds of the names kept. */
	return 0;
}

/* Tells whether a string comes before another in byte order (a prefix first). */
static int
clean_string_before(
	const struct pdf_object *left,
	const struct pdf_object *right)
{
	size_t shorter;
	int difference;

	/* The bytes both have. */
	shorter = left->length;
	if (right->length < shorter)
		shorter = right->length;
	difference = memcmp(left->bytes, right->bytes, shorter);
	if (difference < 0)
		return 1;
	if (difference > 0)
		return 0;

	/* The same start: the shorter first. */
	if (left->length < right->length)
		return 1;

	/* Not before. */
	return 0;
}

/*
 * Tells whether an object is a node of a page tree: /Type /Pages, or no
 * type, /Kids and /Count (a name tree's node has /Kids but no /Count; it
 * was taken for a page tree's before ws177-p011).
 */
static int
clean_is_tree_node(
	const struct pdf_object *object)
{
	struct pdf_object *type;
	struct pdf_object *kids;
	struct pdf_object *count;

	/* Only a dictionary. */
	if (object->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* By its type, or by its kids when it has none. */
	type = pdf_object_get(object, "Type");
	if (type != NULL)
		return pdf_object_is_name(type, "Pages");
	kids = pdf_object_get(object, "Kids");
	count = pdf_object_get(object, "Count");
	if (kids != NULL && count != NULL)
		return 1;

	/* Not a node. */
	return 0;
}

/* Tells whether a dictionary key is a name. */
static int
clean_key_is(
	const struct pdf_object *key,
	const char *name)
{
	size_t length;
	int difference;

	/* Compares the length, then the bytes. */
	length = strlen(name);
	if (key->length != length)
		return 0;
	difference = memcmp(key->bytes, name, length);
	if (difference != 0)
		return 0;

	/* The key is the name. */
	return 1;
}

/* Writes a laid-out file to a path in one pass. */
static int
clean_save_file(
	const struct pdf_buffer *file,
	const char *path)
{
	FILE *stream;
	size_t written;
	int closed;
	int error;

	/* Opens the destination. */
	stream = fopen(path, "wb");
	if (stream == NULL) {
		error = errno;
		return error;
	}

	/* Writes the bytes and closes it, which reports a late write failure. */
	written = fwrite(file->data, 1, file->length, stream);
	closed = fclose(stream);
	if (written != file->length)
		return EIO;
	if (closed != 0)
		return EIO;

	/* Succeeded: the file is written. */
	return 0;
}

/* Frees what a copy allocated (state) or a page's marks (categories); either may be NULL. */
static void
clean_free(
	struct clean_state *state,
	struct clean_category *categories)
{
	size_t which;

	/* The tables. */
	if (state != NULL) {
		free(state->numbers);
		free(state->kinds);
		free(state->queue);
		free(state->offsets);
	}

	/* The marks. */
	if (categories != NULL) {
		for (which = 0; which < CLEAN_CATEGORIES; which++)
			free(categories[which].used);
	}
}
