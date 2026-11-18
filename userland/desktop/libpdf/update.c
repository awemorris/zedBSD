/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The update of libpdf's writer: a new revision added to a document being
 * read, as Notes saves its drawing on another program's PDF
 * (plan/ws079/design-pdf.md section 3).
 *
 * The file an update saves is the document's bytes, unchanged, followed by
 * the revision: the objects the revision changes, written again under their
 * own numbers, the objects it adds, a cross-reference section that lists
 * only those, and a trailer that links the document's newest section by
 * /Prev.  Every page of the document is listed once, in order: kept as it
 * is, drawn over (the page's own content streams follow one that saves the
 * graphics state, and the new content restores it before it draws, so
 * whatever state the page's content leaves does not reach the drawing; the
 * page's resources gain the new ones), or drawn anew in place of the
 * page's own content.  Pages the writer adds between them go into the page
 * tree next to their neighbours.  The attached file replaces any earlier
 * one of its name in the catalog's EmbeddedFiles name tree and associated
 * files, and the information dictionary gets the date of the save.
 *
 * The new content's resources are named with the prefix "ZedBSD", so they do
 * not meet the names the page's own content uses.  Objects of the document
 * that the revision copies are written back as the reader read them:
 * references stay references and strings are written in hexadecimal.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <compat/zlib/zlib.h>
#include <pdf.h>

#include "internal.h"
#include "writer.h"

/* The prefix of the names an update's resources take. */
#define PDF_UPDATE_NAME_PREFIX "ZedBSD"

/* The most nodes of the EmbeddedFiles name tree, and entries in it, an update carries over. */
#define PDF_UPDATE_TREE_NODES_MAX 4096
#define PDF_UPDATE_NAMES_MAX 65536

/* The offset of an object the revision does not write. */
#define PDF_UPDATE_NOT_WRITTEN ((size_t)-1)

/* The longest real number written, in characters. */
#define PDF_UPDATE_REAL_MAX 400

/*
 * One kid of a page tree node as an update writes it: a kid the node had
 * (original, a reference), or a page the update adds (original NULL, the
 * page's object number in number).
 */
struct update_kid {
	struct pdf_object *original;
	unsigned long number;
};

/*
 * One node of the page tree that an update writes again.
 *
 * A node is written again because pages are added under it: directly, into
 * its kids (kids_changed, with the kids as they will be written), or
 * further down, which only grows its /Count.  added counts both.  The node
 * keeps its object number and generation.
 */
struct update_node {
	unsigned long number;
	unsigned long generation;
	struct pdf_object *dictionary;
	struct update_kid *kids;
	size_t kid_count;
	size_t kid_capacity;
	int kids_changed;
	long added;
};

/*
 * One entry of the EmbeddedFiles name tree an update writes: the name and
 * the file specification (a reference, or NULL for the update's own).
 */
struct update_name {
	const unsigned char *key;
	size_t key_length;
	struct pdf_object *value;
};

/*
 * One object of the document that the revision writes again, with the
 * generation its new version keeps.
 */
struct update_changed {
	unsigned long number;
	unsigned long generation;
};

/*
 * The state of one update's layout, from the numbering of its objects to
 * its trailer.  It lives for the one call of lay_out_update().
 *
 * offsets holds, for each object number up to object_count, where the
 * revision writes the object (PDF_UPDATE_NOT_WRITTEN for the objects it
 * leaves as they are).  The page arrays hold, for each page of the writer,
 * the object numbers of the page (a page it adds), its content stream and
 * the stream that saves the graphics state (0 when it has none).  The
 * nodes are the page tree nodes written again; changed lists the objects of
 * the document written again, for their generations.
 */
struct update_layout {
	struct pdf_writer *writer;
	struct pdf_document *base;
	struct pdf_buffer *file;
	struct pdf_object *trailer;
	struct pdf_object *catalog;
	size_t section;
	size_t object_count;
	size_t *offsets;
	size_t *page_objects;
	size_t *content_objects;
	size_t *prefix_objects;
	size_t file_object;
	struct update_node *nodes;
	size_t node_count;
	size_t node_capacity;
	struct update_changed *changed;
	size_t changed_count;
	size_t changed_capacity;
	struct update_name *names;
	size_t name_count;
	size_t name_capacity;
	size_t tree_nodes;
	time_t now;
};

static int lay_out_update(struct pdf_writer *writer, struct pdf_buffer *file, time_t now);
static int check_listing(struct pdf_writer *writer, size_t index);
static int count_contents(struct pdf_document *base, size_t index, size_t *count);
static void shown_to_user(const struct pdf_page_box *box, double matrix[6]);
static int number_objects(struct update_layout *layout);
static int plan_page_tree(struct update_layout *layout);
static int anchor_page(struct update_layout *layout, size_t index, size_t *source, int *after);
static int find_node(struct update_layout *layout, struct pdf_object *reference, struct update_node **node);
static int load_kids(struct update_layout *layout, struct update_node *node);
static int insert_kid(struct update_node *node, size_t position, unsigned long number);
static int find_kid(const struct update_node *node, unsigned long number, size_t *position);
static int count_added(struct update_layout *layout, struct update_node *node);
static int note_changed(struct update_layout *layout, unsigned long number, unsigned long generation);
static int write_nodes(struct update_layout *layout);
static int write_page_over(struct update_layout *layout, size_t index);
static int write_contents_array(struct update_layout *layout, size_t index, struct pdf_object *page);
static int write_merged_resources(struct update_layout *layout, struct pdf_object *page, struct pdf_object *resources);
static int write_merged_category(struct update_layout *layout, struct pdf_object *resources, const char *category, int images);
static int write_new_page(struct update_layout *layout, size_t index);
static void write_content_streams(struct update_layout *layout, size_t index);
static int write_catalog_update(struct update_layout *layout);
static int gather_names(struct update_layout *layout, struct pdf_object *node, int depth);
static int add_name(struct update_layout *layout, const unsigned char *key, size_t key_length, struct pdf_object *value);
static void sort_names(struct update_layout *layout);
static int compare_names(const struct update_name *left, const struct update_name *right);
static int names_attachment(struct update_layout *layout, struct pdf_object *value, int *matches);
static int write_information_update(struct update_layout *layout);
static void write_cross_reference_update(struct update_layout *layout);
static unsigned long changed_generation(const struct update_layout *layout, unsigned long number);
static void write_entries_except(struct pdf_buffer *file, const struct pdf_object *dictionary, const char *first, const char *second, int depth);
static void write_object(struct pdf_buffer *file, const struct pdf_object *object, int depth);
static void write_entries_but(struct pdf_buffer *file, const struct pdf_object *dictionary, const char *first, const char *second, const char *third, const char *fourth);
static void place_page(struct pdf_writer_page *page, const double matrix[6]);
static void write_name(struct pdf_buffer *file, const unsigned char *bytes, size_t length);
static void write_real(struct pdf_buffer *file, double value);
static int key_is(const struct pdf_object *key, const char *name);
static void free_layout(struct update_layout *layout);
static void choose_prefix(struct pdf_writer *writer);
static int prefix_used(struct pdf_document *base, const char *prefix);

/*
 * Creates a writer that adds a revision to a document being read.
 *
 * Each page of the document is then listed once, in order, with
 * pdf_writer_keep_page() or pdf_writer_begin_page_over(); pages begun with
 * pdf_writer_begin_page() between them are added to the document there.
 * The document must stay open until the writer is destroyed.  A signed
 * document is refused with PDF_ESIGNED, an encrypted one (which the reader
 * opens with its empty user password) with EACCES: the revision would have
 * to be encrypted too.
 */
int
pdf_writer_create_update(
	struct pdf_document *base,
	struct pdf_writer **writer)
{
	struct pdf_writer *created;
	struct pdf_crypt *crypt;
	int is_signed;
	int error;

	/* Refuses a missing document or result. */
	if (base == NULL)
		return EINVAL;
	if (writer == NULL)
		return EINVAL;

	/* Refuses a signed document, whose signatures a new revision could invalidate. */
	error = pdf_document_signed(base, &is_signed);
	if (error != 0)
		return error;
	if (is_signed)
		return PDF_ESIGNED;

	/* Refuses an encrypted document. */
	crypt = pdf_reader_crypt(base);
	if (crypt != NULL)
		return EACCES;

	/* An ordinary writer. */
	error = pdf_writer_create(&created);
	if (error != 0)
		return error;

	/* Saves as a revision of the document, with names of its own for its resources (a prefix no page uses, design.md [M4]). */
	created->base = base;
	created->update_layout = lay_out_update;
	choose_prefix(created);

	/* Succeeded: the caller owns the writer. */
	*writer = created;
	return 0;
}

/*
 * Lists the next page of the document an update adds to, to be kept as it is.
 *
 * index counts the document's pages from 0; the pages are listed in order.
 */
int
pdf_writer_keep_page(
	struct pdf_writer *writer,
	size_t index)
{
	struct pdf_writer_page *page;
	struct pdf_page_box box;
	struct pdf_buffer prologue;
	int error;

	/* Refuses a page out of order, or a writer that is not an update. */
	error = check_listing(writer, index);
	if (error != 0)
		return error;

	/* The page's size as shown, for the record. */
	error = pdf_document_page_box(writer->base, index, &box);
	if (error != 0)
		return error;

	/* Adds the page with no content, and closes it at once: nothing is drawn on it. */
	memset(&prologue, 0, sizeof(prologue));
	error = pdf_writer_add_page(writer, box.width, box.height, &prologue, &page);
	if (error != 0)
		return error;
	writer->page_is_open = 0;

	/* The page stands for the document's page, as it is. */
	page->placement = PDF_WRITER_PLACE_KEEP;
	page->source = index;
	writer->last_source = index + 1;

	/* Succeeded: the page is listed. */
	return 0;
}

/*
 * Lists the next page of the document an update adds to, and opens it for
 * drawing over the page's own content (PDF_PAGE_OVERLAY) or instead of it
 * (PDF_PAGE_REPLACE).
 *
 * Drawing uses the page as shown: the origin at the top left of its crop
 * box, y growing downward, in points, with the page's rotation applied --
 * the space pdf_page_render() gives the page in.  index counts the
 * document's pages from 0; the pages are listed in order.
 */
int
pdf_writer_begin_page_over(
	struct pdf_writer *writer,
	size_t index,
	enum pdf_page_use use)
{
	struct pdf_writer_page *page;
	struct pdf_page_box box;
	struct pdf_buffer prologue;
	double matrix[6];
	size_t streams;
	size_t item;
	int prefixed;
	int error;

	/* Refuses a page out of order, or a writer that is not an update. */
	error = check_listing(writer, index);
	if (error != 0)
		return error;

	/* Refuses a use the writer does not know. */
	if (use != PDF_PAGE_OVERLAY && use != PDF_PAGE_REPLACE)
		return EINVAL;

	/* The page's boxes, which place the drawing on it. */
	error = pdf_document_page_box(writer->base, index, &box);
	if (error != 0)
		return error;

	/* How many content streams the page has; a page with some is drawn over after they restore the state. */
	error = count_contents(writer->base, index, &streams);
	if (error != 0)
		return error;
	prefixed = 0;
	if (use == PDF_PAGE_OVERLAY && streams != 0)
		prefixed = 1;

	/*
	 * The drawing's prologue: the restore of the state the prefix stream
	 * saved before the page's own content, then the matrix from the page as
	 * shown to its own space.
	 */
	memset(&prologue, 0, sizeof(prologue));
	if (prefixed)
		pdf_buffer_printf(&prologue, "Q\n");
	shown_to_user(&box, matrix);
	for (item = 0; item < 6; item++) {
		pdf_buffer_append_number(&prologue, matrix[item]);
		pdf_buffer_append(&prologue, " ", 1);
	}

	/* The operator that makes the matrix the drawing's. */
	pdf_buffer_printf(&prologue, "cm\n");
	if (prologue.error != 0) {
		free(prologue.data);
		return ENOMEM;
	}

	/* Adds the page, open for drawing, placed as the matrix places it (its patterns are, ws216-p002). */
	error = pdf_writer_add_page(writer, box.width, box.height, &prologue, &page);
	free(prologue.data);
	if (error != 0)
		return error;
	place_page(page, matrix);

	/* The page stands for the document's page, drawn over or replaced. */
	page->placement = PDF_WRITER_PLACE_OVERLAY;
	if (use == PDF_PAGE_REPLACE)
		page->placement = PDF_WRITER_PLACE_REPLACE;
	page->source = index;
	page->prefixed = prefixed;
	writer->last_source = index + 1;

	/* Succeeded: drawing now goes over the page. */
	return 0;
}

/*
 * Lists the next page of the document an update adds to with its content
 * changed (ws175-p003, pdf_writer_begin_page_edited in editor.c): edited is
 * the page's new content, which the page takes (freed on a failure too),
 * and the page opens for drawing over it, as pdf_writer_begin_page_over
 * draws (the page as shown).  Returns 0, EINVAL, ENOMEM, or the failure of
 * the page.
 */
int
pdf_update_begin_edited(
	struct pdf_writer *writer,
	size_t index,
	struct pdf_buffer *edited)
{
	struct pdf_writer_page *page;
	struct pdf_page_box box;
	struct pdf_buffer prologue;
	unsigned char *packed;
	size_t packed_size;
	double matrix[6];
	size_t item;
	int error;

	/* The page comes next. */
	error = check_listing(writer, index);
	if (error != 0) {
		free(edited->data);
		return error;
	}

	/* The new content compressed when that is shorter (ws175-p006, design.md [H6]). */
	error = pdf_writer_pack(edited->data, edited->length, &packed, &packed_size);
	if (error != 0) {
		free(edited->data);
		return error;
	}

	/* The compressed bytes take the place of the plain ones. */
	if (packed != NULL) {
		free(edited->data);
		edited->data = packed;
		edited->length = packed_size;
		edited->capacity = packed_size;
	}

	/* The page's boxes, which place the drawing on it. */
	error = pdf_document_page_box(writer->base, index, &box);
	if (error != 0) {
		free(edited->data);
		return error;
	}

	/* The six numbers from the page as shown to its own space (the new content leaves the state as it found it). */
	memset(&prologue, 0, sizeof(prologue));
	shown_to_user(&box, matrix);
	for (item = 0; item < 6; item++) {
		pdf_buffer_append_number(&prologue, matrix[item]);
		pdf_buffer_append(&prologue, " ", 1);
	}

	/* The operator that makes the matrix the drawing's. */
	pdf_buffer_printf(&prologue, "cm\n");
	if (prologue.error != 0) {
		free(prologue.data);
		free(edited->data);
		return ENOMEM;
	}

	/* Adds the page, open for drawing, placed as the matrix places it (its patterns are, ws216-p002). */
	error = pdf_writer_add_page(writer, box.width, box.height, &prologue, &page);
	free(prologue.data);
	if (error != 0) {
		free(edited->data);
		return error;
	}
	place_page(page, matrix);

	/* The page stands for the document's page, its content changed. */
	page->placement = PDF_WRITER_PLACE_EDIT;
	page->source = index;
	page->edited = *edited;
	if (packed != NULL)
		page->edited_flate = 1;
	writer->last_source = index + 1;

	/* Succeeded: drawing now goes over the changed page. */
	return 0;
}

/*
 * Lays out an update: the document's bytes, then the revision.
 *
 * It is the writer's update_layout.  Every page of the document must have
 * been listed.
 */
static int
lay_out_update(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	time_t now)
{
	struct update_layout layout;
	const unsigned char *bytes;
	size_t size;
	size_t index;
	size_t listed;
	size_t base_pages;
	int error;

	/* Counts the pages of the document the update listed. */
	listed = 0;
	for (index = 0; index < writer->pages_count; index++) {
		if (writer->pages[index]->placement != PDF_WRITER_PLACE_NEW)
			listed++;
	}

	/* The pages listed must be all of the document's (each was listed once and in order). */
	base_pages = pdf_document_page_count(writer->base);
	if (listed != base_pages)
		return EINVAL;

	/* The layout's state. */
	memset(&layout, 0, sizeof(layout));
	layout.writer = writer;
	layout.base = writer->base;
	layout.file = file;
	layout.now = now;
	pdf_reader_roots(layout.base, &layout.trailer, &layout.catalog);

	/* The document's bytes, unchanged, ended by a line end so the revision starts on a line of its own. */
	bytes = pdf_reader_bytes(layout.base);
	size = pdf_reader_size(layout.base);
	pdf_buffer_append(file, bytes, size);
	if (size != 0 &&
	    bytes[size - 1] != '\n' &&
	    bytes[size - 1] != '\r')
		pdf_buffer_append(file, "\n", 1);
	layout.section = file->length;

	/* Numbers the objects the revision adds. */
	error = number_objects(&layout);
	if (error != 0) {
		free_layout(&layout);
		return error;
	}

	/* Finds where the added pages go in the page tree. */
	error = plan_page_tree(&layout);
	if (error != 0) {
		free_layout(&layout);
		return error;
	}

	/* Writes the page tree nodes that gain pages. */
	error = write_nodes(&layout);
	if (error != 0) {
		free_layout(&layout);
		return error;
	}

	/* Writes each page drawn on: a page of the document drawn over or replaced, or an added one. */
	for (index = 0; index < writer->pages_count; index++) {
		error = 0;
		if (writer->pages[index]->placement == PDF_WRITER_PLACE_OVERLAY)
			error = write_page_over(&layout, index);
		else if (writer->pages[index]->placement == PDF_WRITER_PLACE_EDIT)
			error = write_page_over(&layout, index);
		else if (writer->pages[index]->placement == PDF_WRITER_PLACE_REPLACE)
			error = write_page_over(&layout, index);
		else if (writer->pages[index]->placement == PDF_WRITER_PLACE_NEW)
			error = write_new_page(&layout, index);
		if (error != 0) {
			free_layout(&layout);
			return error;
		}
	}

	/* Writes the opacities, the images, the grains with their patterns and the attached file. */
	pdf_writer_write_opacities(writer, file, layout.offsets);
	for (index = 0; index < writer->images_count; index++)
		pdf_writer_write_image_objects(&writer->images[index], file, layout.offsets);
	pdf_writer_write_paint_objects(writer, file, layout.offsets);
	if (writer->font_layout != NULL) {
		error = writer->font_layout(writer, file, layout.offsets);
		if (error != 0)
			pdf_buffer_fail(file, error);
	}

	/* The attachment. */
	if (writer->has_attachment)
		pdf_writer_write_attachment_objects(writer, file, layout.offsets, layout.file_object);

	/* Writes the catalog again when it must name the attached file. */
	if (writer->has_attachment) {
		error = write_catalog_update(&layout);
		if (error != 0) {
			free_layout(&layout);
			return error;
		}
	}

	/* Writes the information dictionary again with the date of the save. */
	error = write_information_update(&layout);
	if (error != 0) {
		free_layout(&layout);
		return error;
	}

	/* Writes the cross-reference section and the trailer. */
	write_cross_reference_update(&layout);
	free_layout(&layout);

	/* Reports why the revision could not be laid out. */
	if (file->error != 0)
		return file->error;

	/* Succeeded: file holds the document and its new revision. */
	return 0;
}

/* Checks that a page of the document may be listed next in an update. */
static int
check_listing(
	struct pdf_writer *writer,
	size_t index)
{
	size_t count;

	/* Refuses a writer that is not an update, or one with a page still open. */
	if (writer->base == NULL)
		return EINVAL;
	if (writer->page_is_open)
		return EINVAL;

	/* Refuses a page the document does not have. */
	count = pdf_document_page_count(writer->base);
	if (index >= count)
		return EINVAL;

	/* Refuses a page listed already, or one listed before a page that comes before it. */
	if (index < writer->last_source)
		return EINVAL;

	/* Refuses a page listed after skipping one, which would leave that one out. */
	if (index != writer->last_source)
		return EINVAL;

	/* The page comes next. */
	return 0;
}

/* Counts a page's content streams: none, one, or the items of an array. */
static int
count_contents(
	struct pdf_document *base,
	size_t index,
	size_t *count)
{
	struct pdf_object *page;
	struct pdf_object *resources;
	struct pdf_object *contents;
	int error;

	/* Finds the page. */
	error = pdf_reader_page(base, index, &page, &resources);
	if (error != 0)
		return error;

	/* Finds its content. */
	error = pdf_reader_resolve_key(base, page, "Contents", &contents);
	if (error != 0)
		return error;

	/* One stream, an array of them, or nothing. */
	*count = 0;
	if (contents->type == PDF_OBJECT_STREAM)
		*count = 1;
	if (contents->type == PDF_OBJECT_ARRAY)
		*count = contents->count;

	/* Succeeded: count is the number of streams. */
	return 0;
}

/*
 * Makes the matrix from a page as shown -- the crop box's top left at the
 * origin, y downward, the rotation applied -- to the page's own space: the
 * inverse of the matrix content.c draws a page with.
 */
static void
shown_to_user(
	const struct pdf_page_box *box,
	double matrix[6])
{
	/* Chooses the matrix by the rotation (clockwise, as shown). */
	switch (box->rotation) {
	case 90:
		matrix[0] = 0.0;
		matrix[1] = 1.0;
		matrix[2] = 1.0;
		matrix[3] = 0.0;
		matrix[4] = box->crop_left;
		matrix[5] = box->crop_bottom;
		break;
	case 180:
		matrix[0] = -1.0;
		matrix[1] = 0.0;
		matrix[2] = 0.0;
		matrix[3] = 1.0;
		matrix[4] = box->crop_right;
		matrix[5] = box->crop_bottom;
		break;
	case 270:
		matrix[0] = 0.0;
		matrix[1] = -1.0;
		matrix[2] = -1.0;
		matrix[3] = 0.0;
		matrix[4] = box->crop_right;
		matrix[5] = box->crop_top;
		break;
	default:
		matrix[0] = 1.0;
		matrix[1] = 0.0;
		matrix[2] = 0.0;
		matrix[3] = -1.0;
		matrix[4] = box->crop_left;
		matrix[5] = box->crop_top;
		break;
	}
}

/* Notes on a page of the update the matrix from the page as drawn to its own space. */
static void
place_page(
	struct pdf_writer_page *page,
	const double matrix[6])
{
	size_t item;

	/* Copies the six numbers. */
	for (item = 0; item < 6; item++)
		page->shown[item] = matrix[item];
}

/*
 * Numbers the objects the revision adds, from the document's first free
 * number: each added page and its content stream, each drawn-over page's
 * prefix and content streams, each replaced page's content stream, the
 * opacities, the images and the attached file with its specification.
 */
static int
number_objects(
	struct update_layout *layout)
{
	struct pdf_writer *writer;
	struct pdf_writer_page *page;
	size_t next;
	size_t index;

	/* The per-page object numbers, all zero at first. */
	writer = layout->writer;
	layout->page_objects = calloc(writer->pages_count, sizeof(size_t));
	if (layout->page_objects == NULL)
		return ENOMEM;
	layout->content_objects = calloc(writer->pages_count, sizeof(size_t));
	if (layout->content_objects == NULL)
		return ENOMEM;
	layout->prefix_objects = calloc(writer->pages_count, sizeof(size_t));
	if (layout->prefix_objects == NULL)
		return ENOMEM;

	/* Each page's new objects, in order. */
	next = pdf_reader_next_number(layout->base);
	for (index = 0; index < writer->pages_count; index++) {
		page = writer->pages[index];

		/* An added page is an object of its own. */
		if (page->placement == PDF_WRITER_PLACE_NEW) {
			layout->page_objects[index] = next;
			next++;
		}

		/* A drawn-over page with content first saves the state; an edited page first has its changed content. */
		if (page->prefixed || page->placement == PDF_WRITER_PLACE_EDIT) {
			layout->prefix_objects[index] = next;
			next++;
		}

		/* Every page drawn on has its new content stream. */
		if (page->placement != PDF_WRITER_PLACE_KEEP) {
			layout->content_objects[index] = next;
			next++;
		}
	}

	/* The opacities follow the pages. */
	writer->first_alpha_object = next;
	next += writer->alphas_count;

	/* Then each image and its mask. */
	for (index = 0; index < writer->images_count; index++) {
		writer->images[index].object = next;
		next++;
		if (writer->images[index].alpha != NULL)
			next++;
	}

	/* Then the grains, their colours and their patterns (ws216-p002), and the embedded fonts (ws175-p005). */
	next = pdf_writer_number_paint(writer, next);
	next = pdf_writer_number_fonts(writer, next);

	/* Then the attached file and its specification. */
	layout->file_object = next;
	if (writer->has_attachment)
		next += 2;

	/* Refuses more objects than a cross-reference table may list. */
	if (next > 8388608UL)
		return ENOMEM;
	layout->object_count = next;

	/* Where each object is written; none is, yet. */
	layout->offsets = malloc(next * sizeof(size_t));
	if (layout->offsets == NULL)
		return ENOMEM;
	for (index = 0; index < next; index++)
		layout->offsets[index] = PDF_UPDATE_NOT_WRITTEN;

	/* Succeeded: every added object has its number. */
	return 0;
}

/*
 * Finds where each added page goes in the page tree: after the page of the
 * document listed before it, or, for pages added before every page of the
 * document, before the first one; into the root when the document has no
 * pages.  The nodes that gain pages, and the nodes above them, are noted
 * for writing again.
 */
static int
plan_page_tree(
	struct update_layout *layout)
{
	struct pdf_writer *writer;
	struct pdf_object *reference;
	struct pdf_object *page;
	struct pdf_object *resources;
	struct pdf_object *parent;
	struct update_node *node;
	struct update_node *last_node;
	size_t last_position;
	size_t position;
	size_t source;
	size_t index;
	int after;
	int error;

	/* Walks the pages in order; consecutive added pages follow each other in one node. */
	writer = layout->writer;
	last_node = NULL;
	last_position = 0;
	for (index = 0; index < writer->pages_count; index++) {
		/* A page of the document breaks a run of added pages. */
		if (writer->pages[index]->placement != PDF_WRITER_PLACE_NEW) {
			last_node = NULL;
			continue;
		}

		/* An added page after another goes right after it. */
		if (last_node != NULL) {
			position = last_position + 1;
			error = insert_kid(last_node, position, (unsigned long)layout->page_objects[index]);
			if (error != 0)
				return error;
			last_position = position;
			continue;
		}

		/* The page of the document the added page is placed by, if the document has any. */
		error = anchor_page(layout, index, &source, &after);
		if (error == ENOENT) {
			/* A document without pages takes the page at the end of its root. */
			parent = pdf_object_get(layout->catalog, "Pages");
			error = find_node(layout, parent, &node);
			if (error != 0)
				return error;
			error = load_kids(layout, node);
			if (error != 0)
				return error;
			position = node->kid_count;
		} else if (error != 0) {
			return error;
		} else {
			/* The page's reference and its parent, which must be objects of their own. */
			error = pdf_reader_page_reference(layout->base, source, &reference);
			if (error != 0)
				return error;
			error = pdf_reader_page(layout->base, source, &page, &resources);
			if (error != 0)
				return error;
			parent = pdf_object_get(page, "Parent");
			error = find_node(layout, parent, &node);
			if (error != 0)
				return error;

			/* The page's place among its parent's kids. */
			error = load_kids(layout, node);
			if (error != 0)
				return error;
			error = find_kid(node, reference->number, &position);
			if (error != 0)
				return error;
			if (after)
				position++;
		}

		/* Puts the added page there, and counts it in the node and every node above. */
		error = insert_kid(node, position, (unsigned long)layout->page_objects[index]);
		if (error != 0)
			return error;
		last_node = node;
		last_position = position;
	}

	/* Counts the added pages in the nodes and in every node above them. */
	for (index = 0; index < layout->node_count; index++) {
		if (!layout->nodes[index].kids_changed)
			continue;
		error = count_added(layout, &layout->nodes[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: every added page has its place. */
	return 0;
}

/*
 * Finds the page of the document an added page is placed by: the one
 * listed before it (after is set), or else the one listed after it.
 * Reports ENOENT when the writer lists no page of the document.
 */
static int
anchor_page(
	struct update_layout *layout,
	size_t index,
	size_t *source,
	int *after)
{
	struct pdf_writer *writer;
	size_t other;

	/* The nearest page of the document listed before. */
	writer = layout->writer;
	for (other = index; other > 0; other--) {
		if (writer->pages[other - 1]->placement != PDF_WRITER_PLACE_NEW) {
			*source = writer->pages[other - 1]->source;
			*after = 1;
			return 0;
		}
	}

	/* Otherwise the nearest one listed after. */
	for (other = index + 1; other < writer->pages_count; other++) {
		if (writer->pages[other]->placement != PDF_WRITER_PLACE_NEW) {
			*source = writer->pages[other]->source;
			*after = 0;
			return 0;
		}
	}

	/* The document has no pages. */
	return ENOENT;
}

/*
 * Finds the page tree node a reference names among the nodes the update
 * writes again, adding it when it is not there yet.  The node must be an
 * object of its own.
 */
static int
find_node(
	struct update_layout *layout,
	struct pdf_object *reference,
	struct update_node **node)
{
	struct update_node *grown;
	struct update_node *added;
	struct pdf_object *dictionary;
	size_t capacity;
	size_t index;
	int error;

	/* Refuses a node that is not an object of its own: its new version could not replace it. */
	if (reference == NULL)
		return PDF_EFORMAT;
	if (reference->type != PDF_OBJECT_REFERENCE)
		return ENOTSUP;

	/* A node found before. */
	for (index = 0; index < layout->node_count; index++) {
		if (layout->nodes[index].number == reference->number) {
			*node = &layout->nodes[index];
			return 0;
		}
	}

	/* The node's dictionary. */
	error = pdf_reader_resolve(layout->base, reference, &dictionary);
	if (error != 0)
		return error;
	if (dictionary->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* Refuses more nodes than the tree may be deep, or a tree that loops. */
	if (layout->node_count > PDF_UPDATE_TREE_NODES_MAX)
		return PDF_EFORMAT;

	/* Grows the node array when it is full. */
	if (layout->node_count == layout->node_capacity) {
		capacity = layout->node_capacity * 2 + 8;
		grown = realloc(layout->nodes, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		layout->nodes = grown;
		layout->node_capacity = capacity;
	}

	/* Adds the node with its kids not loaded yet. */
	added = &layout->nodes[layout->node_count];
	memset(added, 0, sizeof(*added));
	added->number = reference->number;
	added->generation = reference->generation;
	added->dictionary = dictionary;
	layout->node_count++;

	/* The node's new version replaces the old one. */
	error = note_changed(layout, reference->number, reference->generation);
	if (error != 0)
		return error;

	/* Succeeded: node is the node's entry. */
	*node = added;
	return 0;
}

/* Loads the kids a node had, which it writes again with the added pages among them. */
static int
load_kids(
	struct update_layout *layout,
	struct update_node *node)
{
	struct pdf_object *kids;
	size_t index;
	int error;

	/* Kids loaded before. */
	if (node->kids_changed)
		return 0;

	/* The node's kids, which must be an array. */
	error = pdf_reader_resolve_key(layout->base, node->dictionary, "Kids", &kids);
	if (error != 0)
		return error;
	if (kids->type != PDF_OBJECT_ARRAY)
		return PDF_EFORMAT;

	/* Room for them and a few more. */
	node->kid_capacity = kids->count + 8;
	node->kids = calloc(node->kid_capacity, sizeof(*node->kids));
	if (node->kids == NULL)
		return ENOMEM;

	/* Each kid as the node had it. */
	for (index = 0; index < kids->count; index++)
		node->kids[index].original = kids->values[index];
	node->kid_count = kids->count;

	/* Succeeded: the node's kids are written again. */
	node->kids_changed = 1;
	return 0;
}

/* Inserts an added page into a node's kids at a position. */
static int
insert_kid(
	struct update_node *node,
	size_t position,
	unsigned long number)
{
	struct update_kid *grown;
	size_t capacity;

	/* Refuses a position past the end. */
	if (position > node->kid_count)
		return EINVAL;

	/* Grows the kids when they are full. */
	if (node->kid_count == node->kid_capacity) {
		capacity = node->kid_capacity * 2 + 8;
		grown = realloc(node->kids, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		node->kids = grown;
		node->kid_capacity = capacity;
	}

	/* Moves the later kids up and puts the page in the gap. */
	memmove(&node->kids[position + 1], &node->kids[position], (node->kid_count - position) * sizeof(*node->kids));
	node->kids[position].original = NULL;
	node->kids[position].number = number;
	node->kid_count++;
	node->added++;

	/* Succeeded: the page is among the kids. */
	return 0;
}

/* Finds the position of a page, by its object number, among a node's kids. */
static int
find_kid(
	const struct update_node *node,
	unsigned long number,
	size_t *position)
{
	const struct pdf_object *kid;
	size_t index;

	/* Looks at each kid the node had. */
	for (index = 0; index < node->kid_count; index++) {
		kid = node->kids[index].original;
		if (kid == NULL)
			continue;
		if (kid->type != PDF_OBJECT_REFERENCE)
			continue;
		if (kid->number == number) {
			*position = index;
			return 0;
		}
	}

	/* The page's parent does not list it. */
	return PDF_EFORMAT;
}

/*
 * Adds a node's added pages to the count of every node above it, up the
 * /Parent links to the root.
 */
static int
count_added(
	struct update_layout *layout,
	struct update_node *node)
{
	struct update_node *above;
	struct pdf_object *parent;
	struct pdf_object *dictionary;
	long added;
	int depth;
	int error;

	/* Climbs from the node to the root, a bounded number of levels. */
	added = node->added;
	dictionary = node->dictionary;
	for (depth = 0; depth <= PDF_READER_DEPTH_MAX; depth++) {
		/* The root has no parent. */
		parent = pdf_object_get(dictionary, "Parent");
		if (parent == NULL)
			return 0;

		/* The parent gains the pages too. */
		error = find_node(layout, parent, &above);
		if (error != 0)
			return error;
		above->added += added;
		dictionary = above->dictionary;
	}

	/* The tree is deeper than the limit, or loops. */
	return PDF_EFORMAT;
}

/* Notes an object of the document that the revision writes again, with its generation. */
static int
note_changed(
	struct update_layout *layout,
	unsigned long number,
	unsigned long generation)
{
	struct update_changed *grown;
	size_t capacity;
	size_t index;

	/* An object noted before. */
	for (index = 0; index < layout->changed_count; index++) {
		if (layout->changed[index].number == number)
			return 0;
	}

	/* Grows the list when it is full. */
	if (layout->changed_count == layout->changed_capacity) {
		capacity = layout->changed_capacity * 2 + 16;
		grown = realloc(layout->changed, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		layout->changed = grown;
		layout->changed_capacity = capacity;
	}

	/* Notes the object. */
	layout->changed[layout->changed_count].number = number;
	layout->changed[layout->changed_count].generation = generation;
	layout->changed_count++;

	/* Succeeded: the object's generation is kept. */
	return 0;
}

/* Writes the page tree nodes that gain pages, with their new kids and counts. */
static int
write_nodes(
	struct update_layout *layout)
{
	struct update_node *node;
	struct pdf_object *count;
	struct pdf_buffer *file;
	size_t index;
	size_t kid;
	int error;

	/* Writes each node under its own number. */
	file = layout->file;
	for (index = 0; index < layout->node_count; index++) {
		node = &layout->nodes[index];

		/* The count of pages under the node, which must be a number, grown by the added ones. */
		error = pdf_reader_resolve_key(layout->base, node->dictionary, "Count", &count);
		if (error != 0)
			return error;
		if (count->type != PDF_OBJECT_INTEGER)
			return PDF_EFORMAT;

		/* The node's entries but its kids (when they change) and its count. */
		layout->offsets[node->number] = file->length;
		pdf_buffer_printf(file, "%lu %lu obj\n<<", node->number, node->generation);
		if (node->kids_changed)
			write_entries_except(file, node->dictionary, "Kids", "Count", 0);
		else
			write_entries_except(file, node->dictionary, "Count", NULL, 0);

		/* The kids as they are now: the node's own, and the pages added among them. */
		if (node->kids_changed) {
			pdf_buffer_printf(file, " /Kids [");
			for (kid = 0; kid < node->kid_count; kid++) {
				pdf_buffer_append(file, " ", 1);
				if (node->kids[kid].original != NULL)
					write_object(file, node->kids[kid].original, 0);
				else
					pdf_buffer_printf(file, "%lu 0 R", node->kids[kid].number);
			}

			/* The kids end. */
			pdf_buffer_printf(file, " ]");
		}

		/* The new count closes the node. */
		pdf_buffer_printf(file, " /Count %ld >>\nendobj\n", count->integer + node->added);
	}

	/* Reports a node that could not be written. */
	if (file->error != 0)
		return file->error;

	/* Succeeded: every node that gains pages is written. */
	return 0;
}

/*
 * Writes a page of the document drawn over or replaced: the page again,
 * under its own number, with its new content streams and resources, and
 * the streams.
 */
static int
write_page_over(
	struct update_layout *layout,
	size_t index)
{
	struct pdf_writer_page *writer_page;
	struct pdf_object *reference;
	struct pdf_object *page;
	struct pdf_object *resources;
	struct pdf_buffer *file;
	int error;

	/* The page's reference, which its new version keeps. */
	writer_page = layout->writer->pages[index];
	error = pdf_reader_page_reference(layout->base, writer_page->source, &reference);
	if (error != 0)
		return error;
	error = note_changed(layout, reference->number, reference->generation);
	if (error != 0)
		return error;

	/* The page and the resources it has or inherits. */
	error = pdf_reader_page(layout->base, writer_page->source, &page, &resources);
	if (error != 0)
		return error;

	/* A page whose content failed to be recorded cannot be saved. */
	if (writer_page->content.error != 0)
		return writer_page->content.error;

	/* The page's entries but its content and its resources. */
	file = layout->file;
	layout->offsets[reference->number] = file->length;
	pdf_buffer_printf(file, "%lu %lu obj\n<<", reference->number, reference->generation);
	write_entries_except(file, page, "Contents", "Resources", 0);

	/* The content streams: the page's own after the one that saves the state, then the new one; or the new one alone. */
	error = write_contents_array(layout, index, page);
	if (error != 0)
		return error;

	/* The resources: the page's own with the new ones, or the new ones alone. */
	pdf_buffer_printf(file, " /Resources ");
	if (writer_page->placement == PDF_WRITER_PLACE_OVERLAY || writer_page->placement == PDF_WRITER_PLACE_EDIT) {
		error = write_merged_resources(layout, page, resources);
		if (error != 0)
			return error;
	} else {
		pdf_writer_write_resources(layout->writer, file);
	}

	/* The page's new version ends. */
	pdf_buffer_printf(file, " >>\nendobj\n");

	/* The streams the page now names. */
	write_content_streams(layout, index);

	/* Reports a page that could not be written. */
	if (file->error != 0)
		return file->error;

	/* Succeeded: the page's new version is written. */
	return 0;
}

/* Writes the /Contents entry of a page drawn over or replaced. */
static int
write_contents_array(
	struct update_layout *layout,
	size_t index,
	struct pdf_object *page)
{
	struct pdf_writer_page *writer_page;
	struct pdf_object *value;
	struct pdf_object *contents;
	struct pdf_buffer *file;
	size_t item;
	int error;

	/* Opens the array; a page drawn over starts with the stream that saves the state. */
	writer_page = layout->writer->pages[index];
	file = layout->file;
	pdf_buffer_printf(file, " /Contents [");
	if (writer_page->prefixed || writer_page->placement == PDF_WRITER_PLACE_EDIT)
		pdf_buffer_printf(file, " %lu 0 R", (unsigned long)layout->prefix_objects[index]);

	/* A page drawn over keeps its own streams, named as it names them. */
	if (writer_page->placement == PDF_WRITER_PLACE_OVERLAY) {
		value = pdf_object_get(page, "Contents");
		error = pdf_reader_resolve(layout->base, value, &contents);
		if (error != 0)
			return error;

		/* One stream is named by the page's own reference to it. */
		if (contents->type == PDF_OBJECT_STREAM) {
			pdf_buffer_append(file, " ", 1);
			write_object(file, value, 0);
		}

		/* An array names each stream in order. */
		if (contents->type == PDF_OBJECT_ARRAY) {
			for (item = 0; item < contents->count; item++) {
				pdf_buffer_append(file, " ", 1);
				write_object(file, contents->values[item], 0);
			}
		}
	}

	/* The new content ends the array. */
	pdf_buffer_printf(file, " %lu 0 R ]", (unsigned long)layout->content_objects[index]);

	/* Succeeded: the array is written. */
	return 0;
}

/*
 * Writes the resources of a page drawn over: the page's own, with the
 * update's opacities and images added to its ExtGState and XObject
 * dictionaries under the update's names.
 */
static int
write_merged_resources(
	struct update_layout *layout,
	struct pdf_object *page,
	struct pdf_object *resources)
{
	size_t fonts;
	struct pdf_writer *writer;
	struct pdf_object *own;
	struct pdf_buffer *file;
	int error;

	/* Nothing to add: the page keeps its own resources as it names them, or the ones it inherits. */
	writer = layout->writer;
	file = layout->file;
	fonts = pdf_writer_font_count(writer);
	if (writer->alphas_count == 0 &&
	    writer->images_count == 0 &&
	    writer->patterns_count == 0 &&
	    fonts == 0) {
		own = pdf_object_get(page, "Resources");
		if (own != NULL) {
			write_object(file, own, 0);
		} else if (resources->type == PDF_OBJECT_DICTIONARY) {
			write_object(file, resources, 0);
		} else {
			pdf_buffer_printf(file, "<< >>");
		}

		/* Succeeded: the resources are the page's. */
		return 0;
	}

	/* The page's entries but the three dictionaries the update adds to. */
	pdf_buffer_printf(file, "<<");
	if (resources->type == PDF_OBJECT_DICTIONARY)
		write_entries_but(file, resources, "ExtGState", "XObject", "Font", "Pattern");

	/* The graphics states: the page's own and the opacities. */
	error = write_merged_category(layout, resources, "ExtGState", 0);
	if (error != 0)
		return error;

	/* The external objects: the page's own and the images. */
	error = write_merged_category(layout, resources, "XObject", 1);
	if (error != 0)
		return error;

	/* The fonts: the page's own and the embedded ones (ws175-p005). */
	error = write_merged_category(layout, resources, "Font", 2);
	if (error != 0)
		return error;

	/* The patterns: the page's own and the grains' (ws216-p002). */
	error = write_merged_category(layout, resources, "Pattern", 3);
	if (error != 0)
		return error;

	/* Closes the resources. */
	pdf_buffer_printf(file, " >>");

	/* Succeeded: the resources are written. */
	return 0;
}

/*
 * Writes one category of a page's resources with the update's own entries
 * added: the ExtGState dictionary with the opacities, the XObject one with
 * the images (images 1), the Font one with the embedded fonts (images
 * 2, ws175-p005), or the Pattern one with the grains' patterns (images 3,
 * ws216-p002).  A category the update adds nothing to is written as
 * the page had it.  An update's name the page uses already is refused
 * (EEXIST).
 */
static int
write_merged_category(
	struct update_layout *layout,
	struct pdf_object *resources,
	const char *category,
	int images)
{
	struct pdf_writer *writer;
	struct pdf_object *value;
	struct pdf_object *dictionary;
	struct pdf_object *taken;
	struct pdf_buffer *file;
	char name[64];
	size_t count;
	size_t limit;
	size_t index;
	int error;

	/* How many entries the update adds to the category. */
	writer = layout->writer;
	file = layout->file;
	count = writer->alphas_count;
	if (images == 1)
		count = writer->images_count;
	if (images == 2)
		count = pdf_writer_font_count(writer);
	if (images == 3)
		count = writer->patterns_count;

	/* The page's own dictionary of the category (null when it has none). */
	dictionary = NULL;
	value = NULL;
	if (resources->type == PDF_OBJECT_DICTIONARY)
		value = pdf_object_get(resources, category);
	error = pdf_reader_resolve(layout->base, value, &dictionary);
	if (error != 0)
		return error;

	/* A category the update adds nothing to stays as the page had it. */
	if (count == 0) {
		if (value != NULL) {
			pdf_buffer_printf(file, " /%s ", category);
			write_object(file, value, 0);
		}

		/* Succeeded: the category is the page's. */
		return 0;
	}

	/* Refuses an update's name that the page already uses (a font's is its file's, F0 to F3). */
	limit = count;
	if (images == 2)
		limit = PDF_WRITER_FONT_FILES;
	for (index = 0; index < limit; index++) {
		if (images == 2 && !writer->fonts[index].any)
			continue;
		if (images == 1)
			(void)snprintf(name, sizeof(name), "%sIm%lu", writer->name_prefix, (unsigned long)index);
		else if (images == 2)
			(void)snprintf(name, sizeof(name), "%sF%lu", writer->name_prefix, (unsigned long)index);
		else if (images == 3)
			(void)snprintf(name, sizeof(name), "%sP%lu", writer->name_prefix, (unsigned long)index);
		else
			(void)snprintf(name, sizeof(name), "%sGS%lu", writer->name_prefix, (unsigned long)index);
		taken = pdf_object_get(dictionary, name);
		if (taken != NULL)
			return EEXIST;
	}

	/* The page's own entries, then the update's. */
	pdf_buffer_printf(file, " /%s <<", category);
	if (dictionary->type == PDF_OBJECT_DICTIONARY)
		write_entries_except(file, dictionary, NULL, NULL, 0);
	if (images == 1)
		pdf_writer_write_image_entries(writer, file);
	else if (images == 2)
		pdf_writer_write_font_entries(writer, file);
	else if (images == 3)
		pdf_writer_write_pattern_entries(writer, file);
	else
		pdf_writer_write_opacity_entries(writer, file);
	pdf_buffer_printf(file, " >>");

	/* Succeeded: the category is written. */
	return 0;
}

/*
 * Writes a page the update adds: the page, whose parent is the node it was
 * put in, with its own boxes, rotation and resources, and its content
 * stream.
 */
static int
write_new_page(
	struct update_layout *layout,
	size_t index)
{
	struct pdf_writer_page *writer_page;
	struct update_node *parent;
	struct pdf_buffer *file;
	unsigned long page_object;
	size_t node;
	size_t kid;

	/* A page whose content failed to be recorded cannot be saved. */
	writer_page = layout->writer->pages[index];
	if (writer_page->content.error != 0)
		return writer_page->content.error;

	/* The node the page was put in. */
	page_object = (unsigned long)layout->page_objects[index];
	parent = NULL;
	for (node = 0; node < layout->node_count && parent == NULL; node++) {
		if (!layout->nodes[node].kids_changed)
			continue;
		for (kid = 0; kid < layout->nodes[node].kid_count; kid++) {
			if (layout->nodes[node].kids[kid].original == NULL &&
			    layout->nodes[node].kids[kid].number == page_object) {
				parent = &layout->nodes[node];
				break;
			}
		}
	}

	/* A page the tree plan did not place cannot be written. */
	if (parent == NULL)
		return EINVAL;

	/*
	 * The page, with every attribute it would otherwise inherit from the
	 * node (the boxes and the rotation) given its own value.
	 */
	file = layout->file;
	layout->offsets[page_object] = file->length;
	pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /Page /Parent %lu %lu R /MediaBox [0 0 ", page_object, parent->number, parent->generation);
	pdf_buffer_append_number(file, writer_page->width);
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_number(file, writer_page->height);
	pdf_buffer_printf(file, "] /CropBox [0 0 ");
	pdf_buffer_append_number(file, writer_page->width);
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_number(file, writer_page->height);
	pdf_buffer_printf(file, "] /Rotate 0 /Resources ");
	pdf_writer_write_resources(layout->writer, file);
	pdf_buffer_printf(file, " /Contents %lu 0 R >>\nendobj\n", (unsigned long)layout->content_objects[index]);

	/* Its content stream. */
	write_content_streams(layout, index);

	/* Reports a page that could not be written. */
	if (file->error != 0)
		return file->error;

	/* Succeeded: the added page is written. */
	return 0;
}

/* Writes a page's new content stream, after the stream that saves the state when the page has one. */
static void
write_content_streams(
	struct update_layout *layout,
	size_t index)
{
	static const char save_state[] = "q\n";
	struct pdf_writer_page *writer_page;
	struct pdf_buffer *file;
	size_t object;

	/* The stream that saves the graphics state before the page's own content. */
	writer_page = layout->writer->pages[index];
	file = layout->file;
	if (writer_page->prefixed) {
		object = layout->prefix_objects[index];
		layout->offsets[object] = file->length;
		pdf_buffer_printf(file, "%lu 0 obj\n<< /Length %lu >>\nstream\n", (unsigned long)object, (unsigned long)(sizeof(save_state) - 1));
		pdf_buffer_append(file, save_state, sizeof(save_state) - 1);
		pdf_buffer_printf(file, "\nendstream\nendobj\n");
	}

	/* An edited page's changed content (ws175-p003). */
	if (writer_page->placement == PDF_WRITER_PLACE_EDIT) {
		object = layout->prefix_objects[index];
		layout->offsets[object] = file->length;
		pdf_buffer_printf(file, "%lu 0 obj\n<<", (unsigned long)object);
		if (writer_page->edited_flate)
			pdf_buffer_printf(file, " /Filter /FlateDecode");
		pdf_buffer_printf(file, " /Length %lu >>\nstream\n", (unsigned long)writer_page->edited.length);
		pdf_buffer_append(file, writer_page->edited.data, writer_page->edited.length);
		pdf_buffer_printf(file, "\nendstream\nendobj\n");
	}

	/* The new content, compressed when the writer compresses content (ws216-p002). */
	object = layout->content_objects[index];
	layout->offsets[object] = file->length;
	pdf_writer_write_content_stream(layout->writer, file, object, &writer_page->content);
}

/*
 * Writes the catalog again, under its own number, naming the attached file
 * in its EmbeddedFiles name tree and among its associated files, in place
 * of an earlier file of the same name.
 */
static int
write_catalog_update(
	struct update_layout *layout)
{
	struct pdf_writer *writer;
	struct pdf_object *root;
	struct pdf_object *names;
	struct pdf_object *associated;
	struct pdf_object *tree;
	struct pdf_buffer *file;
	size_t specification;
	size_t index;
	int matches;
	int error;

	/* The catalog must be an object of its own for its new version to replace it. */
	writer = layout->writer;
	file = layout->file;
	root = pdf_object_get(layout->trailer, "Root");
	if (root == NULL)
		return PDF_EFORMAT;
	if (root->type != PDF_OBJECT_REFERENCE)
		return ENOTSUP;
	error = note_changed(layout, root->number, root->generation);
	if (error != 0)
		return error;

	/* The name dictionary, and the entries of its EmbeddedFiles tree but the attached file's name. */
	error = pdf_reader_resolve_key(layout->base, layout->catalog, "Names", &names);
	if (error != 0)
		return error;
	tree = pdf_object_get(names, "EmbeddedFiles");
	if (tree != NULL) {
		error = gather_names(layout, tree, 0);
		if (error != 0)
			return error;
	}

	/* The attached file's own entry, then every entry in the order of their names. */
	error = add_name(layout, (const unsigned char *)writer->attachment.name, strlen(writer->attachment.name), NULL);
	if (error != 0)
		return error;
	sort_names(layout);

	/* The catalog's entries but the two it changes. */
	specification = layout->file_object + 1;
	layout->offsets[root->number] = file->length;
	pdf_buffer_printf(file, "%lu %lu obj\n<<", root->number, root->generation);
	write_entries_except(file, layout->catalog, "Names", "AF", 0);

	/* The name dictionary: its entries but the tree, and the tree as one node of every entry. */
	pdf_buffer_printf(file, " /Names <<");
	if (names->type == PDF_OBJECT_DICTIONARY)
		write_entries_except(file, names, "EmbeddedFiles", NULL, 0);
	pdf_buffer_printf(file, " /EmbeddedFiles << /Names [");
	for (index = 0; index < layout->name_count; index++) {
		pdf_buffer_append(file, " ", 1);
		pdf_buffer_append_hex_string(file, layout->names[index].key, layout->names[index].key_length);
		pdf_buffer_append(file, " ", 1);
		if (layout->names[index].value != NULL)
			write_object(file, layout->names[index].value, 0);
		else
			pdf_buffer_printf(file, "%lu 0 R", (unsigned long)specification);
	}

	/* The entries, the tree and the name dictionary end. */
	pdf_buffer_printf(file, " ] >> >>");

	/* The associated files: the catalog's own but an earlier file of the name, then the attached file. */
	error = pdf_reader_resolve_key(layout->base, layout->catalog, "AF", &associated);
	if (error != 0)
		return error;
	pdf_buffer_printf(file, " /AF [");
	if (associated->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index < associated->count; index++) {
			error = names_attachment(layout, associated->values[index], &matches);
			if (error != 0)
				return error;
			if (matches)
				continue;
			pdf_buffer_append(file, " ", 1);
			write_object(file, associated->values[index], 0);
		}
	}

	/* The attached file ends the associated files, and the catalog ends. */
	pdf_buffer_printf(file, " %lu 0 R ] >>\nendobj\n", (unsigned long)specification);

	/* Reports a catalog that could not be written. */
	if (file->error != 0)
		return file->error;

	/* Succeeded: the catalog names the attached file. */
	return 0;
}

/*
 * Gathers the entries of a name tree node and of the nodes under it, but
 * the attached file's name, to a bounded depth and number of nodes.
 */
static int
gather_names(
	struct update_layout *layout,
	struct pdf_object *node,
	int depth)
{
	struct pdf_object *resolved;
	struct pdf_object *names;
	struct pdf_object *kids;
	struct pdf_object *key;
	const char *attachment;
	size_t length;
	size_t index;
	int difference;
	int error;

	/* Refuses a tree deeper or larger than the limits, which also ends a tree that loops. */
	if (depth > PDF_READER_DEPTH_MAX)
		return PDF_EFORMAT;
	layout->tree_nodes++;
	if (layout->tree_nodes > PDF_UPDATE_TREE_NODES_MAX)
		return PDF_EFORMAT;

	/* The node; one that is not a dictionary holds nothing. */
	error = pdf_reader_resolve(layout->base, node, &resolved);
	if (error != 0)
		return error;
	if (resolved->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* The node's own pairs of name and value. */
	attachment = layout->writer->attachment.name;
	length = strlen(attachment);
	error = pdf_reader_resolve_key(layout->base, resolved, "Names", &names);
	if (error != 0)
		return error;
	if (names->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index + 1 < names->count; index += 2) {
			/* The name, which must be a string. */
			error = pdf_reader_resolve(layout->base, names->values[index], &key);
			if (error != 0)
				return error;
			if (key->type != PDF_OBJECT_STRING)
				continue;

			/* An earlier file of the attached file's name is left out. */
			if (key->length == length) {
				difference = memcmp(key->bytes, attachment, length);
				if (difference == 0)
					continue;
			}

			/* Keeps the entry. */
			error = add_name(layout, key->bytes, key->length, names->values[index + 1]);
			if (error != 0)
				return error;
		}
	}

	/* The node's kids. */
	error = pdf_reader_resolve_key(layout->base, resolved, "Kids", &kids);
	if (error != 0)
		return error;
	if (kids->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index < kids->count; index++) {
			error = gather_names(layout, kids->values[index], depth + 1);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the node's entries are gathered. */
	return 0;
}

/* Adds an entry to the name tree the catalog will name. */
static int
add_name(
	struct update_layout *layout,
	const unsigned char *key,
	size_t key_length,
	struct pdf_object *value)
{
	struct update_name *grown;
	size_t capacity;

	/* Refuses more entries than the limit. */
	if (layout->name_count >= PDF_UPDATE_NAMES_MAX)
		return PDF_EFORMAT;

	/* Grows the list when it is full. */
	if (layout->name_count == layout->name_capacity) {
		capacity = layout->name_capacity * 2 + 16;
		grown = realloc(layout->names, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		layout->names = grown;
		layout->name_capacity = capacity;
	}

	/* Adds the entry. */
	layout->names[layout->name_count].key = key;
	layout->names[layout->name_count].key_length = key_length;
	layout->names[layout->name_count].value = value;
	layout->name_count++;

	/* Succeeded: the entry is listed. */
	return 0;
}

/*
 * Sorts the name tree's entries by their names' bytes, as a name tree
 * orders them.  The entries are few, so an insertion sort does.
 */
static void
sort_names(
	struct update_layout *layout)
{
	struct update_name moving;
	size_t index;
	size_t place;
	int order;

	/* Inserts each entry among the sorted ones before it. */
	for (index = 1; index < layout->name_count; index++) {
		moving = layout->names[index];
		place = index;
		while (place > 0) {
			order = compare_names(&layout->names[place - 1], &moving);
			if (order <= 0)
				break;
			layout->names[place] = layout->names[place - 1];
			place--;
		}

		/* The entry goes into the gap. */
		layout->names[place] = moving;
	}
}

/* Orders two names by their bytes, a shorter name first when one begins the other. */
static int
compare_names(
	const struct update_name *left,
	const struct update_name *right)
{
	size_t shorter;
	int difference;

	/* Compares the bytes both have. */
	shorter = left->key_length;
	if (right->key_length < shorter)
		shorter = right->key_length;
	difference = memcmp(left->key, right->key, shorter);
	if (difference != 0)
		return difference;

	/* The shorter name comes first. */
	if (left->key_length < right->key_length)
		return -1;
	if (left->key_length > right->key_length)
		return 1;

	/* The same name. */
	return 0;
}

/* Tells whether a file specification names the attached file (by /UF or /F). */
static int
names_attachment(
	struct update_layout *layout,
	struct pdf_object *value,
	int *matches)
{
	struct pdf_object *specification;
	struct pdf_object *file_name;
	const char *attachment;
	size_t length;
	int difference;
	int error;

	/* Nothing matches yet. */
	*matches = 0;
	attachment = layout->writer->attachment.name;
	length = strlen(attachment);

	/* The specification; anything but a dictionary names no file. */
	error = pdf_reader_resolve(layout->base, value, &specification);
	if (error != 0)
		return error;
	if (specification->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* The Unicode name, or else the plain one. */
	error = pdf_reader_resolve_key(layout->base, specification, "UF", &file_name);
	if (error != 0)
		return error;
	if (file_name->type != PDF_OBJECT_STRING) {
		error = pdf_reader_resolve_key(layout->base, specification, "F", &file_name);
		if (error != 0)
			return error;
	}

	/* The specification matches when the name is the attached file's. */
	if (file_name->type == PDF_OBJECT_STRING && file_name->length == length) {
		difference = memcmp(file_name->bytes, attachment, length);
		if (difference == 0)
			*matches = 1;
	}

	/* Succeeded: matches tells. */
	return 0;
}

/*
 * Writes the information dictionary again, under its own number, with the
 * date of the save as its modification date.  A document whose trailer
 * names no information dictionary of its own is left without one.
 */
static int
write_information_update(
	struct update_layout *layout)
{
	struct pdf_object *reference;
	struct pdf_object *information;
	struct pdf_buffer *file;
	time_t modification;
	int error;

	/* The dictionary, which must be an object of its own. */
	reference = pdf_object_get(layout->trailer, "Info");
	if (reference == NULL)
		return 0;
	if (reference->type != PDF_OBJECT_REFERENCE)
		return 0;
	error = pdf_reader_resolve(layout->base, reference, &information);
	if (error != 0)
		return error;
	if (information->type != PDF_OBJECT_DICTIONARY)
		return 0;
	error = note_changed(layout, reference->number, reference->generation);
	if (error != 0)
		return error;

	/* The caller's modification date, or the time of the save. */
	modification = layout->writer->modification_time;
	if (modification == 0)
		modification = layout->now;

	/* Its entries but the modification date, then the new one. */
	file = layout->file;
	layout->offsets[reference->number] = file->length;
	pdf_buffer_printf(file, "%lu %lu obj\n<<", reference->number, reference->generation);
	write_entries_except(file, information, "ModDate", NULL, 0);
	pdf_buffer_printf(file, " /ModDate ");
	pdf_buffer_append_date(file, modification);
	pdf_buffer_printf(file, " >>\nendobj\n");

	/* Reports a dictionary that could not be written. */
	if (file->error != 0)
		return file->error;

	/* Succeeded: the dictionary carries the date of the save. */
	return 0;
}

/*
 * Writes the revision's cross-reference section, which lists only the
 * objects the revision writes, its trailer and the end marker.
 *
 * The trailer keeps the document's root and information dictionary and its
 * permanent identifier, and links the document's newest section by /Prev.
 */
static void
write_cross_reference_update(
	struct update_layout *layout)
{
	unsigned char version_id[PDF_WRITER_ID_SIZE];
	struct pdf_writer *writer;
	struct pdf_object *identifier;
	struct pdf_object *first;
	struct pdf_object *root;
	struct pdf_object *information;
	struct pdf_buffer *file;
	size_t table_offset;
	size_t previous;
	size_t newest;
	size_t number;
	size_t run;
	int error;

	/* Derives the revision's identifier from its own bytes. */
	writer = layout->writer;
	file = layout->file;
	pdf_writer_hash_version_id(file, layout->section, version_id);

	/* The section starts with the free list head. */
	table_offset = file->length;
	pdf_buffer_printf(file, "xref\n0 1\n0000000000 65535 f \n");

	/* Writes each run of consecutive numbers the revision writes as one subsection. */
	number = 1;
	while (number < layout->object_count) {
		/* Skips the objects the revision leaves as they are. */
		if (layout->offsets[number] == PDF_UPDATE_NOT_WRITTEN) {
			number++;
			continue;
		}

		/* The run of written objects from here. */
		run = 0;
		while (number + run < layout->object_count && layout->offsets[number + run] != PDF_UPDATE_NOT_WRITTEN)
			run++;

		/* The run's header and an entry of each object. */
		pdf_buffer_printf(file, "%lu %lu\n", (unsigned long)number, (unsigned long)run);
		for (; run > 0; run--) {
			pdf_buffer_printf(file,
					  "%010lu %05lu n \n",
					  (unsigned long)layout->offsets[number],
					  changed_generation(layout, (unsigned long)number));
			number++;
		}
	}

	/* The trailer: the object count, the root, and the information dictionary when there is one. */
	pdf_buffer_printf(file, "trailer\n<< /Size %lu /Root ", (unsigned long)layout->object_count);
	root = pdf_object_get(layout->trailer, "Root");
	write_object(file, root, 0);
	information = pdf_object_get(layout->trailer, "Info");
	if (information != NULL) {
		pdf_buffer_printf(file, " /Info ");
		write_object(file, information, 0);
	}

	/* The link to the document's newest section. */
	(void)pdf_document_get_revision(layout->base, &newest, &previous);
	pdf_buffer_printf(file, " /Prev %lu /ID [", (unsigned long)newest);

	/* The document's own identifier, whose first element the revision keeps. */
	error = pdf_reader_resolve_key(layout->base, layout->trailer, "ID", &identifier);
	first = NULL;
	if (error == 0 &&
	    identifier->type == PDF_OBJECT_ARRAY &&
	    identifier->count >= 1) {
		/* The first element, which must be a string. */
		error = pdf_reader_resolve(layout->base, identifier->values[0], &first);
		if (error != 0)
			first = NULL;
		else if (first->type != PDF_OBJECT_STRING)
			first = NULL;
	}

	/* A document without one takes the writer's own, chosen now when the caller gave none. */
	if (first == NULL && !writer->has_document_id) {
		arc4random_buf(writer->document_id, PDF_WRITER_ID_SIZE);
		writer->has_document_id = 1;
	}

	/* The permanent element, then the revision's. */
	if (first != NULL) {
		pdf_buffer_append_hex_string(file, first->bytes, first->length);
	} else {
		pdf_buffer_append_hex_string(file, writer->document_id, PDF_WRITER_ID_SIZE);
	}

	/* The revision's element closes the identifier and the trailer. */
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_hex_string(file, version_id, PDF_WRITER_ID_SIZE);
	pdf_buffer_printf(file, "] >>\n");

	/* The offset of the section and the end marker. */
	pdf_buffer_printf(file, "startxref\n%lu\n%%%%EOF\n", (unsigned long)table_offset);
}

/* Reports the generation an object of the revision is written with: its own for an object written again, 0 for a new one. */
static unsigned long
changed_generation(
	const struct update_layout *layout,
	unsigned long number)
{
	size_t index;

	/* An object of the document written again keeps its generation. */
	for (index = 0; index < layout->changed_count; index++) {
		if (layout->changed[index].number == number)
			return layout->changed[index].generation;
	}

	/* A new object is generation 0. */
	return 0;
}

/*
 * Writes the entries of a dictionary (" /key value" each) but those of up
 * to two keys (NULL: none).
 */
static void
write_entries_except(
	struct pdf_buffer *file,
	const struct pdf_object *dictionary,
	const char *first,
	const char *second,
	int depth)
{
	size_t index;
	int skipped;

	/* Writes each entry the caller does not replace. */
	for (index = 0; index < dictionary->count; index++) {
		/* Leaves out the keys the caller writes itself. */
		skipped = 0;
		if (first != NULL)
			skipped = key_is(dictionary->keys[index], first);
		if (!skipped && second != NULL)
			skipped = key_is(dictionary->keys[index], second);
		if (skipped)
			continue;

		/* The key and its value, as the reader read them. */
		pdf_buffer_append(file, " ", 1);
		write_name(file, dictionary->keys[index]->bytes, dictionary->keys[index]->length);
		pdf_buffer_append(file, " ", 1);
		write_object(file, dictionary->values[index], depth + 1);
	}
}

/* Writes a dictionary's entries but four keys (ws175-p005: a page's resources but the categories the update adds to; ws216-p002 the patterns). */
static void
write_entries_but(
	struct pdf_buffer *file,
	const struct pdf_object *dictionary,
	const char *first,
	const char *second,
	const char *third,
	const char *fourth)
{
	size_t index;
	int skipped;

	/* Each entry but the four. */
	for (index = 0; index < dictionary->count; index++) {
		skipped = key_is(dictionary->keys[index], first);
		if (!skipped)
			skipped = key_is(dictionary->keys[index], second);
		if (!skipped)
			skipped = key_is(dictionary->keys[index], third);
		if (!skipped)
			skipped = key_is(dictionary->keys[index], fourth);
		if (skipped)
			continue;

		/* The key and its value, as the reader read them. */
		pdf_buffer_append(file, " ", 1);
		write_name(file, dictionary->keys[index]->bytes, dictionary->keys[index]->length);
		pdf_buffer_append(file, " ", 1);
		write_object(file, dictionary->values[index], 1);
	}
}

/*
 * Writes a dictionary without one of its keys (ws175-p004: a marked
 * content's properties without /ActualText, design.md [M10][N15]).
 */
void
pdf_writer_write_dictionary_except(
	struct pdf_buffer *file,
	const struct pdf_object *dictionary,
	const char *key)
{
	/* The entries but the key, between the brackets. */
	pdf_buffer_append(file, "<<", 2);
	write_entries_except(file, dictionary, key, NULL, 0);
	pdf_buffer_append(file, " >>", 3);
}

/*
 * Writes an object as the reader read it: references as references,
 * strings in hexadecimal.  A stream, which is only ever an object of its
 * own, cannot be written in place and fails the buffer.
 */
static void
write_object(
	struct pdf_buffer *file,
	const struct pdf_object *object,
	int depth)
{
	size_t index;

	/* Refuses nesting deeper than the reader reads. */
	if (depth > PDF_READER_DEPTH_MAX) {
		pdf_buffer_fail(file, PDF_EFORMAT);
		return;
	}

	/* A missing object is null. */
	if (object == NULL) {
		pdf_buffer_printf(file, "null");
		return;
	}

	/* Writes the object by its kind. */
	switch (object->type) {
	case PDF_OBJECT_NULL:
		pdf_buffer_printf(file, "null");
		break;
	case PDF_OBJECT_BOOLEAN:
		if (object->boolean)
			pdf_buffer_printf(file, "true");
		else
			pdf_buffer_printf(file, "false");
		break;
	case PDF_OBJECT_INTEGER:
		pdf_buffer_printf(file, "%ld", object->integer);
		break;
	case PDF_OBJECT_REAL:
		write_real(file, object->real);
		break;
	case PDF_OBJECT_NAME:
		write_name(file, object->bytes, object->length);
		break;
	case PDF_OBJECT_STRING:
		pdf_buffer_append_hex_string(file, object->bytes, object->length);
		break;
	case PDF_OBJECT_ARRAY:
		/* Each item, a space between two. */
		pdf_buffer_append(file, "[", 1);
		for (index = 0; index < object->count; index++) {
			if (index != 0)
				pdf_buffer_append(file, " ", 1);
			write_object(file, object->values[index], depth + 1);
		}

		/* The array ends. */
		pdf_buffer_append(file, "]", 1);
		break;
	case PDF_OBJECT_DICTIONARY:
		pdf_buffer_append(file, "<<", 2);
		write_entries_except(file, object, NULL, NULL, depth);
		pdf_buffer_append(file, " >>", 3);
		break;
	case PDF_OBJECT_STREAM:
		pdf_buffer_fail(file, PDF_EFORMAT);
		break;
	case PDF_OBJECT_REFERENCE:
		pdf_buffer_printf(file, "%lu %lu R", object->number, object->generation);
		break;
	}
}

/*
 * Writes a name: a slash and its bytes, with #xx for every byte that is a
 * delimiter, white space, the number sign or outside printable ASCII.
 */
static void
write_name(
	struct pdf_buffer *file,
	const unsigned char *bytes,
	size_t length)
{
	static const char delimiters[] = "()<>[]{}/%#";
	const char *delimiter;
	unsigned char byte;
	size_t index;
	int escaped;

	/* The slash that starts every name. */
	pdf_buffer_append(file, "/", 1);

	/* Each byte, escaped where a reader would take it for something else. */
	for (index = 0; index < length; index++) {
		/* A byte outside printable ASCII, or a delimiter, is written as its code. */
		byte = bytes[index];
		delimiter = strchr(delimiters, byte);
		escaped = 0;
		if (byte < 0x21 || byte > 0x7e)
			escaped = 1;
		else if (delimiter != NULL)
			escaped = 1;

		/* Writes the byte or its code. */
		if (escaped) {
			pdf_buffer_printf(file, "#%02X", (unsigned)byte);
		} else {
			pdf_buffer_append(file, &byte, 1);
		}
	}
}

/*
 * Writes a real number without exponent, with ten decimals at most and
 * its trailing zeros removed.
 */
static void
write_real(
	struct pdf_buffer *file,
	double value)
{
	char text[PDF_UPDATE_REAL_MAX];
	int length;

	/* A number that is not finite is written as zero, which every reader takes. */
	if (!(value - value == 0.0))
		value = 0.0;

	/* Formats the number with a fixed number of decimals; a number too long to format is refused. */
	length = snprintf(text, sizeof(text), "%.10f", value);
	if (length < 0 || (size_t)length >= sizeof(text)) {
		pdf_buffer_fail(file, EINVAL);
		return;
	}

	/* Removes the trailing zeros, then a trailing point. */
	while (length > 1 && text[length - 1] == '0')
		length--;
	if (length > 1 && text[length - 1] == '.')
		length--;

	/* A negative number that rounded to zero is written as zero. */
	if (length == 2 &&
	    text[0] == '-' &&
	    text[1] == '0') {
		text[0] = '0';
		length = 1;
	}

	/* Appends the number. */
	pdf_buffer_append(file, text, (size_t)length);
}

/* Writes a name with its escapes, for the clean copy (clean.c, ws175-p009). */
void
pdf_writer_write_name(
	struct pdf_buffer *file,
	const unsigned char *bytes,
	size_t length)
{
	/* The update's own writing. */
	write_name(file, bytes, length);
}

/* Writes a real number as an update does, for the clean copy (clean.c, ws175-p009). */
void
pdf_writer_write_real(
	struct pdf_buffer *file,
	double value)
{
	/* The update's own writing. */
	write_real(file, value);
}

/* Tells whether a dictionary key is a name. */
static int
key_is(
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

/* Frees what a layout allocated. */
static void
free_layout(
	struct update_layout *layout)
{
	size_t index;

	/* Each node's kids, then the arrays. */
	for (index = 0; index < layout->node_count; index++)
		free(layout->nodes[index].kids);
	free(layout->nodes);
	free(layout->changed);
	free(layout->names);
	free(layout->offsets);
	free(layout->page_objects);
	free(layout->content_objects);
	free(layout->prefix_objects);
}

/*
 * Chooses the prefix of an update's names: "ZedBSD", or
 * "ZedBSD1_" to "ZedBSD9_", the first that begins no name of any page's
 * ExtGState, XObject, Font or Pattern resources (Notes' own names of an earlier
 * revision, for one).  The last is taken when all are used; the
 * per-page check (EEXIST) still refuses a name met.
 */
static void
choose_prefix(
	struct pdf_writer *writer)
{
	unsigned candidate;
	int used;

	/* "ZedBSD" first. */
	(void)snprintf(writer->prefix_buffer, sizeof(writer->prefix_buffer), "%s", PDF_UPDATE_NAME_PREFIX);
	writer->name_prefix = writer->prefix_buffer;
	used = prefix_used(writer->base, writer->prefix_buffer);
	if (!used)
		return;

	/* Then the numbered ones. */
	for (candidate = 1U; candidate <= 9U; candidate++) {
		(void)snprintf(writer->prefix_buffer, sizeof(writer->prefix_buffer), "%s%u_", PDF_UPDATE_NAME_PREFIX, candidate);
		used = prefix_used(writer->base, writer->prefix_buffer);
		if (!used)
			return;
	}
}

/* Tells whether any page's ExtGState, XObject, Font or Pattern resources has a name that begins with a prefix. */
static int
prefix_used(
	struct pdf_document *base,
	const char *prefix)
{
	static const char *const categories[] = { "ExtGState", "XObject", "Font", "Pattern" };
	struct pdf_object *page;
	struct pdf_object *resources;
	struct pdf_object *dictionary;
	size_t length;
	size_t count;
	size_t index;
	size_t category;
	size_t key;
	int differs;
	int error;

	/* Each page's resources. */
	length = strlen(prefix);
	count = pdf_document_page_count(base);
	for (index = 0; index < count; index++) {
		error = pdf_reader_page(base, index, &page, &resources);
		if (error != 0 || resources == NULL || resources->type != PDF_OBJECT_DICTIONARY)
			continue;

		/* Each category's names. */
		for (category = 0; category < sizeof(categories) / sizeof(categories[0]); category++) {
			error = pdf_reader_resolve_key(base, resources, categories[category], &dictionary);
			if (error != 0 || dictionary->type != PDF_OBJECT_DICTIONARY)
				continue;
			for (key = 0; key < dictionary->count; key++) {
				/* A name that begins with the prefix. */
				if (dictionary->keys[key]->length < length)
					continue;
				differs = memcmp(dictionary->keys[key]->bytes, prefix, length);
				if (differs == 0)
					return 1;
			}
		}
	}

	/* No page uses it. */
	return 0;
}

/*
 * Compresses bytes into a zlib stream (libz-compat's deflate, ws175-p006):
 * *packed is a new buffer the caller frees, or NULL when the stream would
 * not be shorter (the bytes are written as they are).  Returns 0 or
 * ENOMEM.
 */
int
pdf_writer_pack(
	const unsigned char *data,
	size_t size,
	unsigned char **packed,
	size_t *packed_size)
{
	unsigned char *buffer;
	uLongf length;
	int status;

	/* Nothing to compress. */
	*packed = NULL;
	*packed_size = 0;
	if (data == NULL || size == 0)
		return 0;

	/* Room for the most the stream can take, and the stream. */
	length = compressBound((uLong)size);
	buffer = malloc(length);
	if (buffer == NULL)
		return ENOMEM;
	status = compress2(buffer, &length, data, (uLong)size, Z_DEFAULT_COMPRESSION);
	if (status == Z_MEM_ERROR) {
		free(buffer);
		return ENOMEM;
	}

	/* A stream no shorter, or one that failed otherwise, is not used. */
	if (status != Z_OK || length >= size) {
		free(buffer);
		return 0;
	}

	/* Succeeded: the shorter stream. */
	*packed = buffer;
	*packed_size = length;
	return 0;
}
