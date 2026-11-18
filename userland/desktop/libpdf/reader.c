/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PDF reader of libpdf.
 *
 * It reads the documents libpdf's writer produces (plan/ws079/design-pdf.md
 * sections 1 to 3) and, from stage 2, those of other programs: classic
 * cross-reference tables and cross-reference streams (with hybrid files'
 * /XRefStm), older sections through /Prev, indirect objects found through
 * them, directly or inside object streams, the page tree, the attached
 * edit data and the page content streams.  A file whose cross-references
 * cannot be read is repaired by finding its objects in its bytes.
 * An encrypted document (stage 3) is read through the standard security
 * handler with the empty user password (crypt.c): the strings of each
 * object loaded from the file and the data of each stream are decrypted;
 * one that needs a password is EACCES, and pdf_document_encrypted() tells
 * encryption apart.  It also gives an update (update.c) the parts of the
 * document a new revision refers to: the trailer, the catalog, the pages'
 * references and the revisions' sections.
 *
 * The file is not trusted.  The whole file is held in memory, every offset
 * and length read from it is checked against its size, objects are loaded
 * once and a loop of objects that need each other is refused, and the page
 * and name trees are walked at most once per node and to a bounded depth.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha2.h>
#include <time.h>

#include <pdf.h>

#include "internal.h"
#include "writer.h"

/* The largest file the reader opens. */
#define PDF_READER_FILE_MAX ((size_t)512 * 1024 * 1024)

/* The size of each read while a file is loaded. */
#define PDF_READER_READ_CHUNK 65536

/* How far from the start the header, and from the end the startxref keyword, are looked for. */
#define PDF_READER_HEADER_WINDOW 1024
#define PDF_READER_TRAILER_WINDOW 1024

/* The most cross-reference sections one /Prev chain may link. */
#define PDF_READER_SECTIONS_MAX 32

/* The largest object number, the PDF reference's implementation limit. */
#define PDF_READER_OBJECT_MAX 8388607UL

/* The most pages one document may have. */
#define PDF_READER_PAGES_MAX 1048576

/* The bytes of the first element of the trailer's file identifier. */
#define PDF_READER_ID_SIZE 16

/* The widest field of a cross-reference stream's entries, in bytes. */
#define PDF_READER_FIELD_MAX 8

/* The most objects one object stream may hold. */
#define PDF_READER_STREAM_OBJECTS_MAX ((size_t)1048576)

/*
 * The load state of an indirect object.
 *
 * LOADING marks an object whose parse is under way, so an object that needs
 * itself, through its stream length for example, is refused rather than
 * loaded forever.
 */
enum pdf_entry_state {
	PDF_ENTRY_UNLOADED = 0,
	PDF_ENTRY_LOADING,
	PDF_ENTRY_LOADED,
	PDF_ENTRY_FAILED
};

/*
 * One entry of the cross-reference table.
 *
 * sequence is the order in which the entry was read; sections are read from
 * the newest, so for one object number the lowest sequence wins.  mark is
 * the number of the last tree walk that reached the object, which keeps a
 * walk from visiting a node twice.  object is the loaded object while state
 * is LOADED, and error the reason while it is FAILED.  A compressed entry's
 * object is the index-th of the object stream numbered stream (its offset
 * is not used).
 */
struct pdf_xref_entry {
	unsigned long number;
	unsigned long generation;
	size_t offset;
	size_t sequence;
	int in_use;
	int compressed;
	unsigned long stream;
	unsigned long index;
	enum pdf_entry_state state;
	unsigned long mark;
	struct pdf_object *object;
	int error;
};

/*
 * One page of a document and the attributes it inherits.
 *
 * The values are as the page tree holds them, possibly references; they
 * are resolved when the page is asked about.  reference is the reference
 * the page tree names the page by (NULL for a page written into its
 * parent's kids directly), which an update uses to replace the page.
 */
struct pdf_reader_page {
	struct pdf_object *page;
	struct pdf_object *reference;
	struct pdf_object *media_box;
	struct pdf_object *crop_box;
	struct pdf_object *rotate;
	struct pdf_object *resources;
};

/*
 * The attributes a page tree node passes down to its pages.
 */
struct pdf_page_inheritance {
	struct pdf_object *media_box;
	struct pdf_object *crop_box;
	struct pdf_object *rotate;
	struct pdf_object *resources;
};

/*
 * An object stream, decoded once and kept while the document is open.
 *
 * bytes is its decoded data (owned is what to free), first the offset of
 * its first object in it, and numbers and offsets the object numbers and
 * the offsets (from first) its header lists, count of each.
 */
struct pdf_object_stream {
	struct pdf_object_stream *next;
	unsigned long number;
	const unsigned char *bytes;
	unsigned char *owned;
	size_t size;
	size_t first;
	size_t count;
	unsigned long *numbers;
	size_t *offsets;
};

/*
 * A document being read.
 *
 * data holds the whole file, which the document owns.  The entries are
 * sorted by object number once they are all read, with one entry per
 * number.  null_object stands for every missing object.  mark numbers the
 * current tree walk and only grows.  xref_offset is where the newest
 * cross-reference section starts, and previous_offset where the one its
 * trailer links by /Prev starts (has_previous: there is one).  fonts
 * holds the fonts the pages' text has used, NULL until the first; the
 * font reader (font.c) leaves release_fonts to free them, so that the
 * reader does not depend on it.  password is the caller's password while
 * the document is being opened (NULL and 0 for none, the empty one), and
 * NULL once it is open: the security handler keeps the key, not the
 * password.
 */
struct pdf_document {
	unsigned char *data;
	size_t size;
	size_t xref_offset;
	size_t previous_offset;
	int has_previous;
	struct pdf_arena arena;
	struct pdf_xref_entry *entries;
	size_t entries_count;
	size_t entries_capacity;
	struct pdf_object *trailer;
	struct pdf_object *catalog;
	struct pdf_object null_object;
	struct pdf_reader_page *pages;
	size_t pages_count;
	size_t pages_capacity;
	unsigned long mark;
	unsigned char id[PDF_READER_ID_SIZE];
	int has_id;
	time_t creation_time;
	time_t modification_time;
	struct pdf_font_cache *fonts;
	void (*release_fonts)(struct pdf_font_cache *cache);
	struct pdf_edit_fonts *edit_fonts;
	void (*release_edit_fonts)(struct pdf_edit_fonts *fonts);
	struct pdf_object_stream *object_streams;
	size_t object_streams_size;
	struct pdf_crypt *crypt;
	unsigned long encrypt_number;
	const unsigned char *password;
	size_t password_length;
};

static int open_owned(unsigned char *data, size_t size, const char *password, struct pdf_document **document);
static int read_file(const char *path, unsigned char **data, size_t *size);
static int check_header(const struct pdf_document *document);
static int find_startxref(struct pdf_document *document, size_t *offset);
static int read_cross_references(struct pdf_document *document, size_t offset);
static int read_section(struct pdf_document *document, size_t offset, struct pdf_object **trailer);
static int read_object_dictionary(struct pdf_document *document, size_t offset, struct pdf_object **dictionary);
static int read_subsection(struct pdf_document *document, struct pdf_lexer *lexer, long start, long count);
static int add_entry(struct pdf_document *document, unsigned long number, unsigned long generation, size_t offset, int in_use);
static int add_compressed_entry(struct pdf_document *document, unsigned long number, unsigned long stream, unsigned long index);
static int read_stream_section(struct pdf_document *document, size_t offset, struct pdf_object **trailer);
static int read_stream_entries(struct pdf_document *document, struct pdf_object *stream, const unsigned char *data, size_t size);
static unsigned long read_field(const unsigned char *bytes, long width, unsigned long fallback);
static int read_hybrid(struct pdf_document *document, struct pdf_object *trailer, size_t section_start);
static int repair(struct pdf_document *document);
static void scan_objects(struct pdf_document *document);
static int is_space_byte(unsigned char byte);
static int is_digit_byte(unsigned char byte);
static int find_repaired_trailer(struct pdf_document *document);
static int add_stream_members(struct pdf_document *document);
static int make_trailer(struct pdf_document *document, unsigned long catalog, unsigned long generation);
static int parse_compressed(struct pdf_document *document, const struct pdf_xref_entry *entry, int depth, struct pdf_object **object);
static int open_object_stream(struct pdf_document *document, unsigned long number, int depth, struct pdf_object_stream **found);
static int read_object_stream_header(struct pdf_document *document, struct pdf_object_stream *stream, struct pdf_object *dictionary);
static void free_object_streams(struct pdf_document *document);
static void sort_entries(struct pdf_document *document);
static int merge_entries(struct pdf_xref_entry *entries, size_t count);
static int compare_entries(const struct pdf_xref_entry *first, const struct pdf_xref_entry *second);
static struct pdf_xref_entry *find_entry(struct pdf_document *document, unsigned long number);
static int load_object(struct pdf_document *document, unsigned long number, unsigned long generation, int depth, struct pdf_object **object);
static int parse_indirect(struct pdf_document *document, const struct pdf_xref_entry *entry, int depth, struct pdf_object **object);
static int open_crypt(struct pdf_document *document);
static int decrypt_strings(struct pdf_document *document, struct pdf_object *object, unsigned long number, unsigned long generation, int depth);
static int read_stream_data(struct pdf_document *document, struct pdf_lexer *lexer, struct pdf_object *stream, int depth);
static int resolve(struct pdf_document *document, struct pdf_object *object, int depth, struct pdf_object **resolved);
static int resolve_key(struct pdf_document *document, const struct pdf_object *dictionary, const char *key, int depth, struct pdf_object **resolved);
static int read_catalog(struct pdf_document *document);
static int walk_page_tree(struct pdf_document *document, struct pdf_object *node, const struct pdf_page_inheritance *inherited, int depth);
static int visit_reference(struct pdf_document *document, const struct pdf_object *node);
static int add_page(struct pdf_document *document, struct pdf_object *page, struct pdf_object *node, const struct pdf_page_inheritance *inherited);
static void read_information(struct pdf_document *document);
static int read_identifier(struct pdf_document *document, struct pdf_object **first);
static time_t parse_date(const struct pdf_object *date);
static int read_digits(const unsigned char *text, size_t length, size_t *position, size_t digits, long *value);
static int read_rectangle(struct pdf_document *document, struct pdf_object *value, double rectangle[4]);
static int check_unfiltered(struct pdf_document *document, const struct pdf_object *stream);
static int hash_content_stream(struct pdf_document *document, const struct pdf_object *stream, SHA2_CTX *context);
static int find_in_associated_files(struct pdf_document *document, const char *name, const char *mime_type, struct pdf_object **stream);
static int find_in_embedded_files(struct pdf_document *document, const char *name, const char *mime_type, struct pdf_object **stream);
static int find_in_name_tree(struct pdf_document *document, struct pdf_object *node, const char *name, int depth, struct pdf_object **value);
static int match_file_specification(struct pdf_document *document, struct pdf_object *value, const char *name, const char *mime_type, struct pdf_object **stream);
static int string_equals(const struct pdf_object *string, const char *text);

/*
 * Opens a PDF file for reading.
 *
 * The whole file is read into memory, so the file may change or go away
 * while the document is open.  An encrypted document is opened with the
 * empty user password (PDF_EPASSWORD when it has another one).
 */
int
pdf_document_open(
	const char *path,
	struct pdf_document **document)
{
	int error;

	/* Opens it with no password. */
	error = pdf_document_open_password(path, NULL, document);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the open document. */
	return 0;
}

/*
 * Opens a PDF file for reading with a password.
 *
 * An encrypted document takes its user password or its owner password
 * (NULL or "" is the empty one); another password is PDF_EPASSWORD.  The
 * password is used while the document opens and is not kept.  A document
 * that is not encrypted ignores it.
 */
int
pdf_document_open_password(
	const char *path,
	const char *password,
	struct pdf_document **document)
{
	unsigned char *data;
	size_t size;
	int error;

	/* Refuses a missing path. */
	if (path == NULL)
		return EINVAL;
	if (document == NULL)
		return EINVAL;

	/* Reads the whole file. */
	data = NULL;
	size = 0;
	error = read_file(path, &data, &size);
	if (error != 0)
		return error;

	/* Reads the document from the bytes, which it now owns. */
	error = open_owned(data, size, password, document);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the open document. */
	return 0;
}

/*
 * Opens a PDF held in memory for reading.
 *
 * The document keeps its own copy of the bytes, so the caller's buffer may
 * go away.  An encrypted document is opened with the empty user password.
 */
int
pdf_document_open_memory(
	const void *data,
	size_t size,
	struct pdf_document **document)
{
	int error;

	/* Opens it with no password. */
	error = pdf_document_open_memory_password(data, size, NULL, document);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the open document. */
	return 0;
}

/*
 * Opens a PDF held in memory for reading with a password (see
 * pdf_document_open_password()).
 */
int
pdf_document_open_memory_password(
	const void *data,
	size_t size,
	const char *password,
	struct pdf_document **document)
{
	unsigned char *copy;
	int error;

	/* Refuses missing bytes, and more than the reader opens. */
	if (data == NULL && size != 0)
		return EINVAL;
	if (document == NULL)
		return EINVAL;
	if (size > PDF_READER_FILE_MAX)
		return EFBIG;

	/* Copies the bytes; one extra byte keeps an empty buffer's allocation valid. */
	copy = malloc(size + 1);
	if (copy == NULL)
		return ENOMEM;
	if (size != 0)
		memcpy(copy, data, size);

	/* Reads the document from the copy, which it now owns. */
	error = open_owned(copy, size, password, document);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the open document. */
	return 0;
}

/*
 * Closes a document and frees every object read from it.
 */
void
pdf_document_close(
	struct pdf_document *document)
{
	/* Nothing was opened. */
	if (document == NULL)
		return;

	/* Frees the fonts, the object streams, the objects, the tables, the bytes and the document. */
	if (document->release_fonts != NULL)
		document->release_fonts(document->fonts);
	if (document->release_edit_fonts != NULL)
		document->release_edit_fonts(document->edit_fonts);
	free_object_streams(document);
	pdf_crypt_close(document->crypt);
	pdf_arena_free(&document->arena);
	free(document->entries);
	free(document->pages);
	free(document->data);
	free(document);
}

/*
 * Reports the number of pages of a document.
 */
size_t
pdf_document_page_count(
	const struct pdf_document *document)
{
	/* Reports the number of pages the page tree listed. */
	return document->pages_count;
}

/*
 * Reports the media box, crop box and rotation of a page.
 *
 * index counts the pages from 0 in the order the page tree lists them.
 */
int
pdf_document_page_box(
	struct pdf_document *document,
	size_t index,
	struct pdf_page_box *box)
{
	struct pdf_reader_page *page;
	struct pdf_object *rotate;
	double media[4];
	double crop[4];
	long rotation;
	int error;

	/* Refuses a page the document does not have. */
	if (index >= document->pages_count)
		return EINVAL;
	page = &document->pages[index];

	/* Reads the media box, which every page must have. */
	error = read_rectangle(document, page->media_box, media);
	if (error != 0)
		return error;

	/* Reads the crop box, which defaults to the media box. */
	memcpy(crop, media, sizeof(crop));
	if (page->crop_box != NULL) {
		error = read_rectangle(document, page->crop_box, crop);
		if (error != 0)
			return error;
	}

	/* Clips the crop box to the media box; one that leaves nothing falls back to the media box. */
	if (crop[0] < media[0])
		crop[0] = media[0];
	if (crop[1] < media[1])
		crop[1] = media[1];
	if (crop[2] > media[2])
		crop[2] = media[2];
	if (crop[3] > media[3])
		crop[3] = media[3];
	if (crop[0] >= crop[2] || crop[1] >= crop[3])
		memcpy(crop, media, sizeof(crop));

	/* Reads the rotation, a multiple of 90 degrees; anything else leaves the page upright. */
	error = resolve(document, page->rotate, 0, &rotate);
	if (error != 0)
		return error;
	rotation = 0;
	if (rotate->type == PDF_OBJECT_INTEGER) {
		rotation = rotate->integer % 360;
		if (rotation < 0)
			rotation += 360;
		if (rotation % 90 != 0)
			rotation = 0;
	}

	/* Fills in the boxes. */
	box->media_left = media[0];
	box->media_bottom = media[1];
	box->media_right = media[2];
	box->media_top = media[3];
	box->crop_left = crop[0];
	box->crop_bottom = crop[1];
	box->crop_right = crop[2];
	box->crop_top = crop[3];
	box->rotation = (int)rotation;

	/* Gives the size as shown; a quarter turn swaps the sides. */
	box->width = crop[2] - crop[0];
	box->height = crop[3] - crop[1];
	if (rotation == 90 || rotation == 270) {
		box->width = crop[3] - crop[1];
		box->height = crop[2] - crop[0];
	}

	/* Succeeded: box describes the page. */
	return 0;
}

/*
 * Computes the SHA-256 of a page's content.
 *
 * The content is the page's content streams, decoded and joined in the
 * order /Contents lists them; a page without content hashes no bytes.
 * Notes compares it with the hash its edit data recorded to learn whether
 * another program changed the page.  A stream without a filter is hashed
 * as it is; one compressed with Flate alone (ws216-p002, as the writer
 * compresses Notes' pages) is decoded first, up to 64 MiB; any other
 * filter is ENOTSUP.
 */
int
pdf_document_page_content_hash(
	struct pdf_document *document,
	size_t index,
	unsigned char digest[32])
{
	struct pdf_object *contents;
	struct pdf_object *stream;
	SHA2_CTX context;
	size_t item;
	int error;

	/* Refuses a page the document does not have. */
	if (index >= document->pages_count)
		return EINVAL;

	/* Finds the page's content: nothing, one stream or an array of them. */
	error = resolve_key(document, document->pages[index].page, "Contents", 0, &contents);
	if (error != 0)
		return error;

	/* Refuses content that is neither. */
	if (contents->type != PDF_OBJECT_NULL &&
	    contents->type != PDF_OBJECT_STREAM &&
	    contents->type != PDF_OBJECT_ARRAY)
		return PDF_EFORMAT;

	/* Hashes one stream. */
	SHA256Init(&context);
	if (contents->type == PDF_OBJECT_STREAM) {
		error = hash_content_stream(document, contents, &context);
		if (error != 0)
			return error;
	}

	/* Hashes each stream of an array in order. */
	if (contents->type == PDF_OBJECT_ARRAY) {
		for (item = 0; item < contents->count; item++) {
			/* Finds the stream; an array of anything else is malformed. */
			error = resolve(document, contents->values[item], 0, &stream);
			if (error != 0)
				return error;
			if (stream->type != PDF_OBJECT_STREAM)
				return PDF_EFORMAT;

			/* Adds its bytes, decoded when they are compressed with Flate. */
			error = hash_content_stream(document, stream, &context);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: digest is the content's hash. */
	SHA256Final(digest, &context);
	return 0;
}

/*
 * Finds an attached file by its name.
 *
 * It is pdf_document_find_attachment_type() with no media type.
 */
int
pdf_document_find_attachment(
	struct pdf_document *document,
	const char *name,
	const void **data,
	size_t *size)
{
	int error;

	/* Looks for the name with any media type. */
	error = pdf_document_find_attachment_type(document, name, NULL, data, size);
	if (error != 0)
		return error;

	/* Succeeded: data and size are the file's bytes. */
	return 0;
}

/*
 * Finds an attached file by its name and, unless mime_type is NULL, its
 * media type.
 *
 * The catalog's associated files are searched first, then the
 * EmbeddedFiles name tree.  The bytes stay in the document and live until
 * it is closed.  A file that is not there reports ENOENT.
 */
int
pdf_document_find_attachment_type(
	struct pdf_document *document,
	const char *name,
	const char *mime_type,
	const void **data,
	size_t *size)
{
	struct pdf_object *stream;
	int error;

	/* Refuses a missing name or result. */
	if (name == NULL)
		return EINVAL;
	if (data == NULL || size == NULL)
		return EINVAL;

	/* Searches the catalog's associated files, then the EmbeddedFiles name tree. */
	error = find_in_associated_files(document, name, mime_type, &stream);
	if (error == ENOENT)
		error = find_in_embedded_files(document, name, mime_type, &stream);

	/* Reports a file that is not there or could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the file's bytes are the stream's. */
	*data = document->data + stream->data_offset;
	*size = stream->data_length;
	return 0;
}

/*
 * Reports the permanent part of the document's file identifier.
 *
 * Notes gives it back to the writer, so a saved revision keeps being the
 * same document.  A document without a sixteen-byte identifier reports
 * ENOENT.
 */
int
pdf_document_get_id(
	const struct pdf_document *document,
	unsigned char id[16])
{
	/* Refuses a document without the identifier. */
	if (!document->has_id)
		return ENOENT;

	/* Copies the identifier out. */
	memcpy(id, document->id, PDF_READER_ID_SIZE);

	/* Succeeded: id holds the identifier. */
	return 0;
}

/*
 * Reports the creation and modification dates of the document.
 *
 * A date the information dictionary does not have, or that is not a valid
 * PDF date, is reported as 0.
 */
int
pdf_document_get_dates(
	const struct pdf_document *document,
	time_t *creation,
	time_t *modification)
{
	/* Copies the dates read when the document was opened. */
	*creation = document->creation_time;
	*modification = document->modification_time;

	/* Succeeded: both dates are given. */
	return 0;
}

/*
 * Reports where the newest revision's cross-reference section starts, and
 * where the one before it starts.
 *
 * A file that has been updated incrementally holds its revisions one after
 * the other, each ending with its own section, and the newest section
 * links the one before.  previous_offset is (size_t)-1 when the file has
 * one revision.  Notes compares them to learn whether the newest revision
 * is the only one added to a file it knows.
 */
int
pdf_document_get_revision(
	const struct pdf_document *document,
	size_t *xref_offset,
	size_t *previous_offset)
{
	/* The newest section, which the last startxref names. */
	*xref_offset = document->xref_offset;

	/* The section it links, if any. */
	*previous_offset = (size_t)-1;
	if (document->has_previous)
		*previous_offset = document->previous_offset;

	/* Succeeded: both offsets are given. */
	return 0;
}

/*
 * Reports whether a document is signed: whether its interactive form says
 * that it holds signatures (/SigFlags bit 1), or its catalog carries
 * permissions a signature grants (/Perms).
 *
 * Adding a revision to a signed document can invalidate its signatures, so
 * an update refuses one.
 */
int
pdf_document_signed(
	struct pdf_document *document,
	int *is_signed)
{
	struct pdf_object *permissions;
	struct pdf_object *form;
	struct pdf_object *flags;
	int error;

	/* Nothing is known to be signed yet. */
	*is_signed = 0;

	/* Permissions that a signature grants mark the document as signed. */
	error = resolve_key(document, document->catalog, "Perms", 0, &permissions);
	if (error != 0)
		return error;
	if (permissions->type != PDF_OBJECT_NULL) {
		*is_signed = 1;
		return 0;
	}

	/* The interactive form's flags. */
	error = resolve_key(document, document->catalog, "AcroForm", 0, &form);
	if (error != 0)
		return error;
	error = resolve_key(document, form, "SigFlags", 0, &flags);
	if (error != 0)
		return error;

	/* The first flag says that the form holds at least one signature. */
	if (flags->type == PDF_OBJECT_INTEGER) {
		if ((flags->integer & 1) != 0)
			*is_signed = 1;
	}

	/* Succeeded: is_signed tells. */
	return 0;
}

/*
 * Reports whether a PDF file is encrypted: the reader opens it when the
 * user password is empty (EACCES otherwise), and a program that writes
 * (Notes) refuses it.
 *
 * Only the newest trailer is read: a classic one, or the dictionary of a
 * cross-reference stream, which holds the trailer's keys.
 */
int
pdf_document_encrypted(
	const char *path,
	int *encrypted)
{
	struct pdf_document *created;
	struct pdf_object *trailer;
	struct pdf_object *encryption;
	unsigned char *data;
	size_t offset;
	size_t size;
	int error;

	/* Refuses a missing path or answer. */
	if (path == NULL)
		return EINVAL;
	if (encrypted == NULL)
		return EINVAL;

	/* Reads the whole file. */
	data = NULL;
	size = 0;
	error = read_file(path, &data, &size);
	if (error != 0)
		return error;

	/* A document that owns the bytes, to read its newest trailer with. */
	created = calloc(1, sizeof(*created));
	if (created == NULL) {
		free(data);
		return ENOMEM;
	}

	/* The document takes the bytes; a missing object is null. */
	created->data = data;
	created->size = size;
	created->null_object.type = PDF_OBJECT_NULL;

	/* Checks that the bytes start as a PDF. */
	error = check_header(created);
	if (error != 0) {
		pdf_document_close(created);
		return error;
	}

	/* Finds the newest cross-reference section. */
	error = find_startxref(created, &offset);
	if (error != 0) {
		pdf_document_close(created);
		return error;
	}

	/* Reads its trailer: a classic one, or else a cross-reference stream's dictionary. */
	error = read_section(created, offset, &trailer);
	if (error == ENOTSUP)
		error = read_object_dictionary(created, offset, &trailer);
	if (error != 0) {
		pdf_document_close(created);
		return error;
	}

	/* An /Encrypt key is what encrypts a document. */
	encryption = pdf_object_get(trailer, "Encrypt");
	*encrypted = 0;
	if (encryption != NULL)
		*encrypted = 1;
	pdf_document_close(created);

	/* Succeeded: encrypted tells. */
	return 0;
}

/*
 * Resolves an object that may be a reference, for the other parts of the
 * library.
 */
int
pdf_reader_resolve(
	struct pdf_document *document,
	struct pdf_object *object,
	struct pdf_object **resolved)
{
	int error;

	/* Follows the references from the top of the load chain. */
	error = resolve(document, object, 0, resolved);
	if (error != 0)
		return error;

	/* Succeeded: resolved is the direct object. */
	return 0;
}

/*
 * Resolves the value of a key of a dictionary or a stream, for the other
 * parts of the library; a missing key is null.
 */
int
pdf_reader_resolve_key(
	struct pdf_document *document,
	const struct pdf_object *dictionary,
	const char *key,
	struct pdf_object **resolved)
{
	int error;

	/* Finds and resolves the value from the top of the load chain. */
	error = resolve_key(document, dictionary, key, 0, resolved);
	if (error != 0)
		return error;

	/* Succeeded: resolved is the key's direct value. */
	return 0;
}

/*
 * Finds a page's dictionary and the resources it has or inherits (null
 * when it has none).
 */
int
pdf_reader_page(
	struct pdf_document *document,
	size_t index,
	struct pdf_object **page,
	struct pdf_object **resources)
{
	int error;

	/* Refuses a page the document does not have. */
	if (index >= document->pages_count)
		return EINVAL;

	/* Resolves the resources, which may be a reference. */
	error = resolve(document, document->pages[index].resources, 0, resources);
	if (error != 0)
		return error;

	/* Succeeded: the page's dictionary, which the walk resolved. */
	*page = document->pages[index].page;
	return 0;
}

/*
 * Reports an encrypted document's security handler (NULL for a document
 * that is not encrypted).
 */
struct pdf_crypt *
pdf_reader_crypt(
	const struct pdf_document *document)
{
	/* The handler read_catalog() opened. */
	return document->crypt;
}

/*
 * Reports the bytes of a document's file, into which streams point.
 */
const unsigned char *
pdf_reader_bytes(
	const struct pdf_document *document)
{
	/* The whole file, held while the document is open. */
	return document->data;
}

/*
 * Reports the fonts a document keeps, for the font reader (NULL until the
 * first font is read).
 */
struct pdf_font_cache *
pdf_reader_font_cache(
	struct pdf_document *document)
{
	/* The document's fonts. */
	return document->fonts;
}

/*
 * Gives a document the fonts it keeps until it is closed, and the call
 * that frees them then.
 */
void
pdf_reader_set_font_cache(
	struct pdf_document *document,
	struct pdf_font_cache *cache,
	void (*release)(struct pdf_font_cache *cache))
{
	/* The document owns the fonts from here on. */
	document->fonts = cache;
	document->release_fonts = release;
}

/*
 * Reports the replacement fonts the editors of a document draw with
 * (replace.c, ws175-p005; NULL until made).
 */
struct pdf_edit_fonts *
pdf_reader_edit_fonts(
	struct pdf_document *document)
{
	/* The document's. */
	return document->edit_fonts;
}

/*
 * Gives a document its replacement fonts, which release frees when the
 * document closes (after the font cache, whose faces may read their
 * bytes).
 */
void
pdf_reader_set_edit_fonts(
	struct pdf_document *document,
	struct pdf_edit_fonts *fonts,
	void (*release)(struct pdf_edit_fonts *fonts))
{
	/* The document owns them from here on. */
	document->edit_fonts = fonts;
	document->release_edit_fonts = release;
}

/*
 * Reports the size of a document's file in bytes.
 */
size_t
pdf_reader_size(
	const struct pdf_document *document)
{
	/* The file's length, which every offset in it is below. */
	return document->size;
}

/*
 * Finds the newest trailer and the catalog it names, for an update.
 */
void
pdf_reader_roots(
	struct pdf_document *document,
	struct pdf_object **trailer,
	struct pdf_object **catalog)
{
	/* Both were read when the document was opened. */
	*trailer = document->trailer;
	*catalog = document->catalog;
}

/*
 * Reports the lowest object number no object of the document uses, which
 * the first object an update adds takes.
 *
 * It is past every number the cross-reference sections list and at least
 * the newest trailer's /Size.
 */
unsigned long
pdf_reader_next_number(
	const struct pdf_document *document)
{
	const struct pdf_object *size;
	unsigned long next;

	/* One past the highest number listed; the entries are sorted by number. */
	next = 1;
	if (document->entries_count != 0)
		next = document->entries[document->entries_count - 1].number + 1;

	/* The trailer's /Size, when it is larger. */
	size = pdf_object_get(document->trailer, "Size");
	if (size != NULL && size->type == PDF_OBJECT_INTEGER) {
		if (size->integer > 0 && (unsigned long)size->integer > next)
			next = (unsigned long)size->integer;
	}

	/* Reports the first free number. */
	return next;
}

/*
 * Reports the reference the page tree names a page by, which an update
 * gives the page's new version.
 *
 * A page written out in its parent's kids has no reference of its own and
 * reports ENOTSUP.
 */
int
pdf_reader_page_reference(
	struct pdf_document *document,
	size_t index,
	struct pdf_object **reference)
{
	/* Refuses a page the document does not have. */
	if (index >= document->pages_count)
		return EINVAL;

	/* Refuses a page that is not an object of its own. */
	if (document->pages[index].reference == NULL)
		return ENOTSUP;

	/* Succeeded: the page's reference. */
	*reference = document->pages[index].reference;
	return 0;
}

/*
 * Reports a page's boxes and rotation, its own or those it inherits from
 * the page tree (ws175-p009: a clean copy writes them on the page); one it
 * has nowhere is NULL.
 */
int
pdf_reader_page_inherited(
	struct pdf_document *document,
	size_t index,
	struct pdf_object **media_box,
	struct pdf_object **crop_box,
	struct pdf_object **rotate)
{
	/* Refuses a page the document does not have. */
	if (index >= document->pages_count)
		return EINVAL;

	/* Succeeded: the values the walk of the page tree kept. */
	*media_box = document->pages[index].media_box;
	*crop_box = document->pages[index].crop_box;
	*rotate = document->pages[index].rotate;
	return 0;
}

/*
 * Reads a document from bytes the document takes over.
 *
 * On failure the bytes are freed.
 */
static int
open_owned(
	unsigned char *data,
	size_t size,
	const char *password,
	struct pdf_document **document)
{
	struct pdf_document *created;
	size_t offset;
	int repairable;
	int error;

	/* Allocates the document, which owns the bytes from here on. */
	created = calloc(1, sizeof(*created));
	if (created == NULL) {
		free(data);
		return ENOMEM;
	}

	/* The document takes the bytes; a missing object is null. */
	created->data = data;
	created->size = size;
	created->null_object.type = PDF_OBJECT_NULL;

	/* The password, for the security handler while the document opens (none is the empty one). */
	created->password = NULL;
	created->password_length = 0;
	if (password != NULL) {
		created->password = (const unsigned char *)password;
		created->password_length = strlen(password);
	}

	/* Checks that the bytes start as a PDF. */
	error = check_header(created);
	if (error != 0) {
		pdf_document_close(created);
		return error;
	}

	/* Finds the newest cross-reference section, and reads it and the older ones it links. */
	error = find_startxref(created, &offset);
	if (error == 0) {
		/* The newest section is the newest revision's, which an update links. */
		created->xref_offset = offset;
		error = read_cross_references(created, offset);
	}

	/* Reads the catalog and lists the pages. */
	if (error == 0)
		error = read_catalog(created);

	/*
	 * A document whose cross-references or catalog cannot be read is
	 * repaired from its bytes; running out of memory and a feature the
	 * reader does not have are not damage.
	 */
	repairable = 0;
	if (error != 0) {
		repairable = 1;
		if (error == ENOMEM)
			repairable = 0;
		if (error == ENOTSUP)
			repairable = 0;
		if (error == PDF_EPASSWORD)
			repairable = 0;
	}

	/* Repairs it and reads the catalog again. */
	if (repairable) {
		error = repair(created);
		if (error == 0)
			error = read_catalog(created);
	}

	/* The password is the caller's, and is not kept past the opening. */
	created->password = NULL;
	created->password_length = 0;

	/* Refuses a document that cannot be read even so. */
	if (error != 0) {
		pdf_document_close(created);
		return error;
	}

	/* Reads the identifier and the dates, which a document may lack. */
	read_information(created);

	/* Succeeded: the caller owns the document. */
	*document = created;
	return 0;
}

/* Reads a whole file into a new allocation. */
static int
read_file(
	const char *path,
	unsigned char **data,
	size_t *size)
{
	unsigned char *buffer;
	unsigned char *grown;
	size_t length;
	size_t capacity;
	size_t got;
	FILE *stream;
	int failed;
	int error;

	/* Opens the file. */
	stream = fopen(path, "rb");
	if (stream == NULL) {
		error = errno;
		return error;
	}

	/* Reads chunks until the end, growing the buffer ahead of each one. */
	buffer = NULL;
	length = 0;
	capacity = 0;
	for (;;) {
		/* Refuses a file larger than the reader opens. */
		if (length > PDF_READER_FILE_MAX) {
			free(buffer);
			fclose(stream);
			return EFBIG;
		}

		/* Makes room for the next chunk. */
		if (capacity - length < PDF_READER_READ_CHUNK) {
			capacity = capacity * 2 + PDF_READER_READ_CHUNK;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				free(buffer);
				fclose(stream);
				return ENOMEM;
			}

			/* The grown buffer is the file's. */
			buffer = grown;
		}

		/* Reads the chunk; a short one is the end or an error. */
		got = fread(buffer + length, 1, PDF_READER_READ_CHUNK, stream);
		length += got;
		if (got < PDF_READER_READ_CHUNK)
			break;
	}

	/* Reports a read error rather than a short file. */
	failed = ferror(stream);
	fclose(stream);
	if (failed) {
		free(buffer);
		return EIO;
	}

	/* Refuses a last chunk that took the file past the limit. */
	if (length > PDF_READER_FILE_MAX) {
		free(buffer);
		return EFBIG;
	}

	/* Succeeded: the caller owns the file's bytes. */
	*data = buffer;
	*size = length;
	return 0;
}

/* Checks that the file has a PDF header near its start. */
static int
check_header(
	const struct pdf_document *document)
{
	size_t position;
	size_t window;
	int difference;

	/* Looks for %PDF- in the first bytes, where a header after some junk is still found. */
	window = document->size;
	if (window > PDF_READER_HEADER_WINDOW)
		window = PDF_READER_HEADER_WINDOW;
	for (position = 0; position + 5 <= window; position++) {
		difference = memcmp(document->data + position, "%PDF-", 5);
		if (difference == 0)
			return 0;
	}

	/* The file is not a PDF. */
	return PDF_EFORMAT;
}

/* Finds the offset of the newest cross-reference section after the last startxref keyword. */
static int
find_startxref(
	struct pdf_document *document,
	size_t *offset)
{
	struct pdf_lexer lexer;
	struct pdf_token token;
	size_t position;
	size_t lowest;
	int difference;
	int found;
	int error;

	/* Refuses a file too short to hold the keyword. */
	if (document->size < 9)
		return PDF_EFORMAT;

	/* Looks for the last startxref in the end of the file. */
	lowest = 0;
	if (document->size > PDF_READER_TRAILER_WINDOW)
		lowest = document->size - PDF_READER_TRAILER_WINDOW;
	found = 0;
	for (position = document->size - 9; position + 1 > lowest; position--) {
		difference = memcmp(document->data + position, "startxref", 9);
		if (difference == 0) {
			found = 1;
			break;
		}
	}

	/* A file without it is not a PDF. */
	if (!found)
		return PDF_EFORMAT;

	/* Reads the offset that follows the keyword. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = document->data;
	lexer.size = document->size;
	lexer.position = position + 9;
	lexer.arena = &document->arena;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;

	/* Refuses an offset that is not in the file. */
	if (token.type != PDF_TOKEN_INTEGER)
		return PDF_EFORMAT;
	if (token.integer < 0 || (unsigned long)token.integer >= document->size)
		return PDF_EFORMAT;

	/* Succeeded: offset is where the newest section starts. */
	*offset = (size_t)token.integer;
	return 0;
}

/*
 * Reads the newest cross-reference section and every older one its /Prev
 * chain links, then sorts the entries.
 *
 * The newest section's trailer is the document's.
 */
static int
read_cross_references(
	struct pdf_document *document,
	size_t offset)
{
	size_t visited[PDF_READER_SECTIONS_MAX];
	struct pdf_object *trailer;
	struct pdf_object *previous;
	size_t sections;
	size_t index;
	int error;

	/* Reads the sections from the newest along the chain. */
	for (sections = 0; sections < PDF_READER_SECTIONS_MAX; sections++) {
		/* Refuses a chain that comes back to a section it read. */
		for (index = 0; index < sections; index++) {
			if (visited[index] == offset)
				return PDF_EFORMAT;
		}

		/* The section is visited. */
		visited[sections] = offset;

		/* Reads the section and its trailer; the first trailer is the document's. */
		error = read_section(document, offset, &trailer);
		if (error != 0)
			return error;
		if (document->trailer == NULL)
			document->trailer = trailer;

		/* Ends at the oldest section. */
		previous = pdf_object_get(trailer, "Prev");
		if (previous == NULL)
			break;

		/* Refuses a link that is not an offset in the file. */
		if (previous->type != PDF_OBJECT_INTEGER)
			return PDF_EFORMAT;
		if (previous->integer < 0 || (unsigned long)previous->integer >= document->size)
			return PDF_EFORMAT;
		offset = (size_t)previous->integer;

		/* The newest section's link names the revision before the newest. */
		if (sections == 0) {
			document->previous_offset = offset;
			document->has_previous = 1;
		}
	}

	/* Refuses a chain longer than the limit. */
	if (sections == PDF_READER_SECTIONS_MAX)
		return PDF_EFORMAT;

	/* Keeps the newest entry of each object and sorts them for lookup. */
	sort_entries(document);

	/* Succeeded: every section is read. */
	return 0;
}

/*
 * Reads one classic cross-reference section and its trailer.
 *
 * A cross-reference stream starts with an object header instead of the
 * xref keyword and is not read yet.
 */
static int
read_section(
	struct pdf_document *document,
	size_t offset,
	struct pdf_object **trailer)
{
	struct pdf_lexer lexer;
	struct pdf_token token;
	struct pdf_token count;
	struct pdf_object *dictionary;
	size_t section_start;
	int is_keyword;
	int error;

	/* Reads the first token of the section. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = document->data;
	lexer.size = document->size;
	lexer.position = offset;
	lexer.arena = &document->arena;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;

	/* An object's number starts a cross-reference stream; anything but xref is not a section. */
	is_keyword = pdf_token_is_keyword(&token, "xref");
	if (!is_keyword) {
		if (token.type != PDF_TOKEN_INTEGER)
			return PDF_EFORMAT;
		error = read_stream_section(document, offset, trailer);
		if (error != 0)
			return error;
		return 0;
	}

	/* The classic section's entries start after those read so far. */
	section_start = document->entries_count;

	/* Reads the subsections until the trailer. */
	for (;;) {
		error = pdf_lexer_next(&lexer, &token);
		if (error != 0)
			return error;

		/* The trailer ends the subsections. */
		is_keyword = pdf_token_is_keyword(&token, "trailer");
		if (is_keyword)
			break;

		/* Reads the subsection's first object number and its count. */
		if (token.type != PDF_TOKEN_INTEGER)
			return PDF_EFORMAT;
		error = pdf_lexer_next(&lexer, &count);
		if (error != 0)
			return error;
		if (count.type != PDF_TOKEN_INTEGER)
			return PDF_EFORMAT;

		/* Reads the subsection's entries. */
		error = read_subsection(document, &lexer, token.integer, count.integer);
		if (error != 0)
			return error;
	}

	/* Reads the trailer's dictionary. */
	error = pdf_parse_object(&lexer, 0, &dictionary);
	if (error != 0)
		return error;
	if (dictionary->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* A hybrid file's cross-reference stream completes the table. */
	error = read_hybrid(document, dictionary, section_start);
	if (error != 0)
		return error;

	/* Succeeded: trailer is the section's trailer. */
	*trailer = dictionary;
	return 0;
}

/*
 * Reads the dictionary of the object that starts at an offset, "n g obj"
 * followed by a dictionary: a cross-reference stream's, whose keys are the
 * trailer's.
 */
static int
read_object_dictionary(
	struct pdf_document *document,
	size_t offset,
	struct pdf_object **dictionary)
{
	struct pdf_lexer lexer;
	struct pdf_token token;
	struct pdf_object *parsed;
	int is_keyword;
	int error;

	/* Reads the object number. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = document->data;
	lexer.size = document->size;
	lexer.position = offset;
	lexer.arena = &document->arena;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	if (token.type != PDF_TOKEN_INTEGER)
		return PDF_EFORMAT;

	/* Reads the generation. */
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	if (token.type != PDF_TOKEN_INTEGER)
		return PDF_EFORMAT;

	/* Reads the obj keyword. */
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	is_keyword = pdf_token_is_keyword(&token, "obj");
	if (!is_keyword)
		return PDF_EFORMAT;

	/* Parses the object, which must be a dictionary. */
	error = pdf_parse_object(&lexer, 0, &parsed);
	if (error != 0)
		return error;
	if (parsed->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* Succeeded: dictionary is the object's. */
	*dictionary = parsed;
	return 0;
}

/*
 * Reads the entries of one subsection.
 *
 * Each entry is an offset, a generation and n (in use) or f (free).  The
 * writer's entries are twenty bytes each, but any white space between the
 * fields is accepted.
 */
static int
read_subsection(
	struct pdf_document *document,
	struct pdf_lexer *lexer,
	long start,
	long count)
{
	struct pdf_token offset;
	struct pdf_token generation;
	struct pdf_token kind;
	unsigned long number;
	long index;
	int in_use;
	int is_free;
	int error;

	/* Refuses numbers outside the limit, which also keeps start + count from overflowing. */
	if (start < 0 || count < 0)
		return PDF_EFORMAT;
	if ((unsigned long)start > PDF_READER_OBJECT_MAX)
		return PDF_EFORMAT;
	if ((unsigned long)count > PDF_READER_OBJECT_MAX + 1 - (unsigned long)start)
		return PDF_EFORMAT;

	/* Refuses a count the rest of the file cannot hold at the shortest entry of six bytes. */
	if ((unsigned long)count > (document->size - lexer->position) / 6)
		return PDF_EFORMAT;

	/* Reads each entry. */
	for (index = 0; index < count; index++) {
		/* Reads the three fields. */
		error = pdf_lexer_next(lexer, &offset);
		if (error != 0)
			return error;
		error = pdf_lexer_next(lexer, &generation);
		if (error != 0)
			return error;
		error = pdf_lexer_next(lexer, &kind);
		if (error != 0)
			return error;

		/* Refuses an entry whose numbers are not numbers. */
		if (offset.type != PDF_TOKEN_INTEGER || generation.type != PDF_TOKEN_INTEGER)
			return PDF_EFORMAT;
		if (offset.integer < 0 || generation.integer < 0)
			return PDF_EFORMAT;

		/* Reads the kind of the entry. */
		in_use = pdf_token_is_keyword(&kind, "n");
		is_free = pdf_token_is_keyword(&kind, "f");
		if (!in_use && !is_free)
			return PDF_EFORMAT;

		/* Adds the entry; an object in use must start inside the file. */
		number = (unsigned long)start + (unsigned long)index;
		if (in_use && (unsigned long)offset.integer >= document->size)
			return PDF_EFORMAT;
		error = add_entry(document, number, (unsigned long)generation.integer, (size_t)offset.integer, in_use);
		if (error != 0)
			return error;
	}

	/* Succeeded: every entry of the subsection is added. */
	return 0;
}

/* Appends one cross-reference entry in the order it was read. */
static int
add_entry(
	struct pdf_document *document,
	unsigned long number,
	unsigned long generation,
	size_t offset,
	int in_use)
{
	struct pdf_xref_entry *grown;
	struct pdf_xref_entry *entry;
	size_t capacity;

	/* Grows the table when it is full. */
	if (document->entries_count == document->entries_capacity) {
		capacity = document->entries_capacity * 2;
		if (capacity == 0)
			capacity = 64;
		if (capacity > PDF_READER_ARENA_MAX / sizeof(*grown))
			return ENOMEM;
		grown = realloc(document->entries, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		document->entries = grown;
		document->entries_capacity = capacity;
	}

	/* Fills in the entry; nothing is loaded yet. */
	entry = &document->entries[document->entries_count];
	memset(entry, 0, sizeof(*entry));
	entry->number = number;
	entry->generation = generation;
	entry->offset = offset;
	entry->in_use = in_use;
	entry->sequence = document->entries_count;
	entry->state = PDF_ENTRY_UNLOADED;
	document->entries_count++;

	/* Succeeded: the entry is the table's last. */
	return 0;
}

/* Appends one entry of an object kept in an object stream, in the order it was read. */
static int
add_compressed_entry(
	struct pdf_document *document,
	unsigned long number,
	unsigned long stream,
	unsigned long index)
{
	struct pdf_xref_entry *entry;
	int error;

	/* Adds the entry as one in use at no offset; a compressed object's generation is 0. */
	error = add_entry(document, number, 0, 0, 1);
	if (error != 0)
		return error;

	/* Points it into the object stream. */
	entry = &document->entries[document->entries_count - 1];
	entry->compressed = 1;
	entry->stream = stream;
	entry->index = index;

	/* Succeeded: the entry is the table's last. */
	return 0;
}

/*
 * Reads a cross-reference stream at an offset: its entries, and its
 * dictionary, which is the section's trailer.
 *
 * The stream's length must be direct, since no object can be loaded while
 * the cross-references are read; an indirect one is found from the
 * endstream keyword.
 */
static int
read_stream_section(
	struct pdf_document *document,
	size_t offset,
	struct pdf_object **trailer)
{
	struct pdf_lexer lexer;
	struct pdf_token token;
	struct pdf_object *dictionary;
	struct pdf_object *length;
	struct pdf_object *type;
	const unsigned char *data;
	unsigned char *owned;
	size_t start;
	size_t end;
	size_t size;
	int is_keyword;
	int is_xref;
	int dct;
	int differs;
	int error;

	/* Reads the object's number, generation and obj keyword. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = document->data;
	lexer.size = document->size;
	lexer.position = offset;
	lexer.arena = &document->arena;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	if (token.type != PDF_TOKEN_INTEGER)
		return PDF_EFORMAT;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	if (token.type != PDF_TOKEN_INTEGER)
		return PDF_EFORMAT;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	is_keyword = pdf_token_is_keyword(&token, "obj");
	if (!is_keyword)
		return PDF_EFORMAT;

	/* Reads the dictionary and the stream keyword. */
	error = pdf_parse_object(&lexer, 0, &dictionary);
	if (error != 0)
		return error;
	if (dictionary->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	is_keyword = pdf_token_is_keyword(&token, "stream");
	if (!is_keyword)
		return PDF_EFORMAT;

	/* The data starts after the keyword's line end. */
	start = lexer.position;
	if (start < document->size && document->data[start] == '\r')
		start++;
	if (start < document->size && document->data[start] == '\n')
		start++;

	/* A direct length, or the distance to the endstream keyword. */
	length = pdf_object_get(dictionary, "Length");
	if (length != NULL && length->type == PDF_OBJECT_INTEGER) {
		if (length->integer < 0 || (unsigned long)length->integer > document->size - start)
			return PDF_EFORMAT;
		end = start + (size_t)length->integer;
	} else {
		/* Looks for endstream after the data. */
		for (end = start; end + 9 <= document->size; end++) {
			differs = memcmp(document->data + end, "endstream", 9);
			if (differs == 0)
				break;
		}

		/* A stream without its end is not one. */
		if (end + 9 > document->size)
			return PDF_EFORMAT;
	}

	/* The dictionary becomes the stream, which must be a cross-reference stream. */
	dictionary->type = PDF_OBJECT_STREAM;
	dictionary->data_offset = start;
	dictionary->data_length = end - start;
	type = pdf_object_get(dictionary, "Type");
	is_xref = pdf_object_is_name(type, "XRef");
	if (!is_xref)
		return PDF_EFORMAT;

	/* Decodes the entries. */
	error = pdf_filter_decode(document, dictionary, 0, &data, &size, &owned, &dct);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return PDF_EFORMAT;

	/* Reads the entries from the decoded stream. */
	error = read_stream_entries(document, dictionary, data, size);
	free(owned);
	if (error != 0)
		return error;

	/* Succeeded: the stream's dictionary is the section's trailer. */
	*trailer = dictionary;
	return 0;
}

/*
 * Reads a cross-reference stream's decoded entries: /W gives the widths of
 * the three fields, /Index the subsections ([0 Size] by default).  A field
 * of width 0 takes its default: type 1, the others 0.  A stream shorter
 * than its subsections ends where its data does.
 */
static int
read_stream_entries(
	struct pdf_document *document,
	struct pdf_object *stream,
	const unsigned char *data,
	size_t size)
{
	struct pdf_object *widths;
	struct pdf_object *count_object;
	struct pdf_object *index_object;
	long width[3];
	long first;
	long count;
	long entry;
	size_t row;
	size_t position;
	size_t pair;
	size_t pairs;
	unsigned long type;
	unsigned long second;
	unsigned long third;
	unsigned long number;
	int field;
	int error;

	/* Reads the three widths. */
	widths = pdf_object_get(stream, "W");
	if (widths == NULL)
		return PDF_EFORMAT;
	if (widths->type != PDF_OBJECT_ARRAY || widths->count != 3)
		return PDF_EFORMAT;
	row = 0;
	for (field = 0; field < 3; field++) {
		if (widths->values[field]->type != PDF_OBJECT_INTEGER)
			return PDF_EFORMAT;
		width[field] = widths->values[field]->integer;
		if (width[field] < 0 || width[field] > PDF_READER_FIELD_MAX)
			return PDF_EFORMAT;
		row += (size_t)width[field];
	}

	/* A row of no bytes holds no entry. */
	if (row == 0)
		return PDF_EFORMAT;

	/* Reads the size, which the default subsection covers. */
	count_object = pdf_object_get(stream, "Size");
	if (count_object == NULL || count_object->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (count_object->integer < 0 || (unsigned long)count_object->integer > PDF_READER_OBJECT_MAX + 1)
		return PDF_EFORMAT;

	/* The subsections: /Index's pairs, or the whole size. */
	index_object = pdf_object_get(stream, "Index");
	pairs = 1;
	if (index_object != NULL) {
		if (index_object->type != PDF_OBJECT_ARRAY || index_object->count % 2 != 0)
			return PDF_EFORMAT;
		pairs = index_object->count / 2;
	}

	/* Reads each subsection's entries. */
	position = 0;
	for (pair = 0; pair < pairs; pair++) {
		/* The subsection's first object number and its count. */
		first = 0;
		count = count_object->integer;
		if (index_object != NULL) {
			if (index_object->values[pair * 2]->type != PDF_OBJECT_INTEGER)
				return PDF_EFORMAT;
			if (index_object->values[pair * 2 + 1]->type != PDF_OBJECT_INTEGER)
				return PDF_EFORMAT;
			first = index_object->values[pair * 2]->integer;
			count = index_object->values[pair * 2 + 1]->integer;
		}

		/* Refuses a subsection past the object numbers the reader keeps. */
		if (first < 0 || count < 0)
			return PDF_EFORMAT;
		if ((unsigned long)first > PDF_READER_OBJECT_MAX)
			return PDF_EFORMAT;
		if ((unsigned long)count > PDF_READER_OBJECT_MAX + 1 - (unsigned long)first)
			return PDF_EFORMAT;

		/* Each entry of the subsection that the data holds. */
		for (entry = 0; entry < count; entry++) {
			if (row > size - position || position > size)
				return 0;
			type = read_field(data + position, width[0], 1);
			second = read_field(data + position + width[0], width[1], 0);
			third = read_field(data + position + width[0] + width[1], width[2], 0);
			position += row;
			number = (unsigned long)first + (unsigned long)entry;

			/* A free entry, one in the file, one in an object stream; another type is null. */
			error = 0;
			if (type == 0) {
				error = add_entry(document, number, third, 0, 0);
			} else if (type == 1 && second < document->size) {
				error = add_entry(document, number, third, (size_t)second, 1);
			} else if (type == 1) {
				error = add_entry(document, number, third, 0, 0);
			} else if (type == 2 && second <= PDF_READER_OBJECT_MAX) {
				error = add_compressed_entry(document, number, second, third);
			}

			/* Only running out of memory stops the section. */
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every entry the stream holds is added. */
	return 0;
}

/* Reads one big-endian field of a cross-reference stream's entry (fallback for width 0). */
static unsigned long
read_field(
	const unsigned char *bytes,
	long width,
	unsigned long fallback)
{
	unsigned long value;
	long index;

	/* A field of no width takes its default. */
	if (width == 0)
		return fallback;

	/* The bytes, the high one first. */
	value = 0;
	for (index = 0; index < width; index++)
		value = (value << 8) | bytes[index];

	/* The field's value. */
	return value;
}

/*
 * Reads a hybrid file's cross-reference stream (/XRefStm of a classic
 * trailer).  Its entries name the objects in object streams, which the
 * table lists as free for older readers; so the table's free entries give
 * way to them, while the table's objects in use keep their place.
 */
static int
read_hybrid(
	struct pdf_document *document,
	struct pdf_object *trailer,
	size_t section_start)
{
	struct pdf_object *offset;
	struct pdf_object *stream_trailer;
	size_t stream_start;
	size_t index;
	int error;

	/* Only a trailer with a valid offset has a stream. */
	offset = pdf_object_get(trailer, "XRefStm");
	if (offset == NULL || offset->type != PDF_OBJECT_INTEGER)
		return 0;
	if (offset->integer < 0 || (unsigned long)offset->integer >= document->size)
		return 0;

	/* Reads the stream's entries; a stream that cannot be read leaves the table as it is. */
	stream_start = document->entries_count;
	error = read_stream_section(document, (size_t)offset->integer, &stream_trailer);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return 0;

	/* The table's free entries come after the stream's. */
	for (index = section_start; index < stream_start; index++) {
		if (!document->entries[index].in_use)
			document->entries[index].sequence = document->entries_count + (index - section_start);
	}

	/* Succeeded: the stream's entries complete the table. */
	return 0;
}

/*
 * Repairs a document whose cross-references or catalog cannot be read:
 * every object is found by its "n g obj" header in the bytes (the last
 * one of a number wins), the members of the object streams among them are
 * added after the objects found directly, and the trailer is the last one
 * with a catalog, or one made for the catalog object.
 */
static int
repair(
	struct pdf_document *document)
{
	int error;

	/* Forgets what the failed reading found. */
	document->entries_count = 0;
	document->trailer = NULL;
	document->catalog = NULL;
	document->pages_count = 0;
	document->has_previous = 0;

	/* Finds the objects in the bytes and orders them. */
	scan_objects(document);
	sort_entries(document);
	if (document->entries_count == 0)
		return PDF_EFORMAT;

	/* Adds the objects kept in object streams. */
	error = add_stream_members(document);
	if (error != 0)
		return error;

	/* Finds or makes the trailer. */
	error = find_repaired_trailer(document);
	if (error != 0)
		return error;

	/* Succeeded: the document can be read from the repaired table. */
	return 0;
}

/*
 * Finds each "n g obj" header in the bytes, from the end, so that a later
 * definition of a number is read first and wins.
 */
static void
scan_objects(
	struct pdf_document *document)
{
	const unsigned char *data;
	size_t position;
	size_t cursor;
	size_t digits;
	unsigned long number;
	unsigned long generation;
	unsigned long scale;
	int differs;
	int space;
	int digit;
	int error;

	/* Looks at each place obj could start, from the end. */
	data = document->data;
	for (position = document->size; position >= 3 + 4; position--) {
		/* The keyword obj, not followed by a letter (not objstm or the like). */
		differs = memcmp(data + position - 3, "obj", 3);
		if (differs != 0)
			continue;
		if (position < document->size) {
			if (data[position] >= 'a' && data[position] <= 'z')
				continue;
		}

		/* It stands after white space. */
		cursor = position - 3;
		space = is_space_byte(data[cursor - 1]);
		if (!space)
			continue;

		/* Skips the white space before it. */
		while (cursor > 0) {
			space = is_space_byte(data[cursor - 1]);
			if (!space)
				break;
			cursor--;
		}

		/* The generation's digits, read backwards. */
		generation = 0;
		scale = 1;
		for (digits = 0; cursor > 0 && digits < 6; digits++) {
			digit = is_digit_byte(data[cursor - 1]);
			if (!digit)
				break;
			generation += (unsigned long)(data[cursor - 1] - '0') * scale;
			scale *= 10;
			cursor--;
		}

		/* The generation must have digits and something before them. */
		if (digits == 0 || cursor == 0)
			continue;

		/* The number's digits before more white space. */
		space = is_space_byte(data[cursor - 1]);
		if (!space)
			continue;
		while (cursor > 0) {
			space = is_space_byte(data[cursor - 1]);
			if (!space)
				break;
			cursor--;
		}

		/* The number's digits, read backwards. */
		number = 0;
		scale = 1;
		for (digits = 0; cursor > 0 && digits < 8; digits++) {
			digit = is_digit_byte(data[cursor - 1]);
			if (!digit)
				break;
			number += (unsigned long)(data[cursor - 1] - '0') * scale;
			scale *= 10;
			cursor--;
		}

		/* The number must have digits the reader keeps. */
		if (digits == 0 || number > PDF_READER_OBJECT_MAX)
			continue;

		/* The number must start a token. */
		if (cursor > 0) {
			digit = is_digit_byte(data[cursor - 1]);
			if (digit)
				continue;
		}

		/* Adds the object; running out of memory ends the scan with what it found. */
		error = add_entry(document, number, generation, cursor, 1);
		if (error != 0)
			return;
	}
}

/* Tells whether a byte is white space where the repair scan looks for "n g obj". */
static int
is_space_byte(
	unsigned char byte)
{
	/* The white space an object's header has between its words. */
	switch (byte) {
	case ' ':
	case '\n':
	case '\r':
	case '\t':
		return 1;
	default:
		break;
	}

	/* Anything else. */
	return 0;
}

/* Tells whether a byte is a decimal digit. */
static int
is_digit_byte(
	unsigned char byte)
{
	/* The digits 0 to 9. */
	if (byte >= '0' && byte <= '9')
		return 1;

	/* Anything else. */
	return 0;
}

/*
 * Finds a repaired document's trailer: the last trailer dictionary with a
 * /Root, else the last cross-reference stream's dictionary with one, else
 * a trailer made for the object whose /Type is /Catalog.
 */
static int
find_repaired_trailer(
	struct pdf_document *document)
{
	struct pdf_lexer lexer;
	struct pdf_object *dictionary;
	struct pdf_object *object;
	struct pdf_object *type;
	struct pdf_object *root;
	size_t position;
	size_t index;
	size_t best_offset;
	unsigned long catalog;
	unsigned long catalog_generation;
	int found_catalog;
	int is_name;
	int differs;
	int later;
	int error;

	/* The last trailer keyword whose dictionary has a /Root. */
	for (position = document->size; position >= 7; position--) {
		/* The keyword. */
		differs = memcmp(document->data + position - 7, "trailer", 7);
		if (differs != 0)
			continue;

		/* The dictionary after it, which must name the catalog. */
		memset(&lexer, 0, sizeof(lexer));
		lexer.data = document->data;
		lexer.size = document->size;
		lexer.position = position;
		lexer.arena = &document->arena;
		error = pdf_parse_object(&lexer, 0, &dictionary);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0 || dictionary->type != PDF_OBJECT_DICTIONARY)
			continue;
		root = pdf_object_get(dictionary, "Root");
		if (root == NULL)
			continue;
		document->trailer = dictionary;
		return 0;
	}

	/* Else the latest cross-reference stream with a /Root, or the catalog. */
	found_catalog = 0;
	catalog = 0;
	catalog_generation = 0;
	best_offset = 0;
	for (index = 0; index < document->entries_count; index++) {
		error = load_object(document, document->entries[index].number, document->entries[index].generation, 0, &object);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			continue;
		if (object->type != PDF_OBJECT_DICTIONARY && object->type != PDF_OBJECT_STREAM)
			continue;
		type = pdf_object_get(object, "Type");

		/* A cross-reference stream with a /Root, the one furthest in the file. */
		is_name = pdf_object_is_name(type, "XRef");
		root = pdf_object_get(object, "Root");
		later = 0;
		if (is_name && root != NULL) {
			if (document->entries[index].offset >= best_offset)
				later = 1;
		}

		/* Keeps it as the trailer so far. */
		if (later) {
			document->trailer = object;
			best_offset = document->entries[index].offset;
			continue;
		}

		/* The catalog. */
		is_name = pdf_object_is_name(type, "Catalog");
		if (is_name) {
			found_catalog = 1;
			catalog = document->entries[index].number;
			catalog_generation = document->entries[index].generation;
		}
	}

	/* A cross-reference stream's dictionary serves as the trailer. */
	if (document->trailer != NULL)
		return 0;

	/* Refuses a document without any catalog. */
	if (!found_catalog)
		return PDF_EFORMAT;

	/* Makes a trailer that names the catalog. */
	error = make_trailer(document, catalog, catalog_generation);
	if (error != 0)
		return error;

	/* Succeeded: the trailer names the catalog. */
	return 0;
}

/*
 * Adds the members of every object stream the repaired table finds, after
 * the objects found directly, which therefore win.
 */
static int
add_stream_members(
	struct pdf_document *document)
{
	struct pdf_object_stream *stream;
	struct pdf_object *object;
	struct pdf_object *type;
	size_t index;
	size_t member;
	int is_stream;
	int error;

	/* Opens each object that is an object stream. */
	for (index = 0; index < document->entries_count; index++) {
		error = load_object(document, document->entries[index].number, document->entries[index].generation, 0, &object);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0 || object->type != PDF_OBJECT_STREAM)
			continue;
		type = pdf_object_get(object, "Type");
		is_stream = pdf_object_is_name(type, "ObjStm");
		if (!is_stream)
			continue;
		error = open_object_stream(document, document->entries[index].number, 0, &stream);
		if (error == ENOMEM)
			return ENOMEM;
	}

	/* Adds each member of each opened stream. */
	for (stream = document->object_streams; stream != NULL; stream = stream->next) {
		for (member = 0; member < stream->count; member++) {
			error = add_compressed_entry(document, stream->numbers[member], stream->number, member);
			if (error != 0)
				return error;
		}
	}

	/* Orders the table again, the direct objects before the members. */
	sort_entries(document);

	/* Succeeded: the members can be loaded. */
	return 0;
}

/* Makes a trailer dictionary whose /Root names an object. */
static int
make_trailer(
	struct pdf_document *document,
	unsigned long catalog,
	unsigned long generation)
{
	struct pdf_object *dictionary;
	struct pdf_object *key;
	struct pdf_object *value;
	unsigned char *name;

	/* The dictionary (the arena's memory is zeroed). */
	dictionary = pdf_arena_allocate(&document->arena, sizeof(*dictionary));
	if (dictionary == NULL)
		return ENOMEM;

	/* Its key. */
	key = pdf_arena_allocate(&document->arena, sizeof(*key));
	if (key == NULL)
		return ENOMEM;

	/* Its value. */
	value = pdf_arena_allocate(&document->arena, sizeof(*value));
	if (value == NULL)
		return ENOMEM;

	/* The key's name, with its NUL. */
	name = pdf_arena_allocate(&document->arena, 5);
	if (name == NULL)
		return ENOMEM;

	/* The array of its one key. */
	dictionary->keys = pdf_arena_allocate(&document->arena, sizeof(*dictionary->keys));
	if (dictionary->keys == NULL)
		return ENOMEM;

	/* The array of its one value. */
	dictionary->values = pdf_arena_allocate(&document->arena, sizeof(*dictionary->values));
	if (dictionary->values == NULL)
		return ENOMEM;

	/* /Root n g R. */
	memcpy(name, "Root", 5);
	key->type = PDF_OBJECT_NAME;
	key->bytes = name;
	key->length = 4;
	value->type = PDF_OBJECT_REFERENCE;
	value->number = catalog;
	value->generation = generation;
	dictionary->type = PDF_OBJECT_DICTIONARY;
	dictionary->keys[0] = key;
	dictionary->values[0] = value;
	dictionary->count = 1;

	/* Succeeded: the made trailer is the document's. */
	document->trailer = dictionary;
	return 0;
}

/*
 * Parses an object kept in an object stream: the index-th of the stream's
 * header, or the one of the entry's number when the index does not match.
 */
static int
parse_compressed(
	struct pdf_document *document,
	const struct pdf_xref_entry *entry,
	int depth,
	struct pdf_object **object)
{
	struct pdf_object_stream *stream;
	struct pdf_lexer lexer;
	struct pdf_object *parsed;
	size_t member;
	size_t position;
	int error;

	/* Opens the stream, once per document. */
	error = open_object_stream(document, entry->stream, depth + 1, &stream);
	if (error != 0)
		return error;

	/* Finds the member: at its index, or by its number. */
	member = entry->index;
	if (member >= stream->count || stream->numbers[member] != entry->number) {
		for (member = 0; member < stream->count; member++) {
			if (stream->numbers[member] == entry->number)
				break;
		}

		/* The stream does not hold the object. */
		if (member == stream->count)
			return PDF_EFORMAT;
	}

	/* Refuses an offset past the stream's data. */
	position = stream->first + stream->offsets[member];
	if (position < stream->first || position >= stream->size)
		return PDF_EFORMAT;

	/* Parses the object from the decoded bytes; its names and strings are copied into the arena. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = stream->bytes;
	lexer.size = stream->size;
	lexer.position = position;
	lexer.arena = &document->arena;
	error = pdf_parse_object(&lexer, 0, &parsed);
	if (error != 0)
		return error;

	/* Succeeded: object is the member. */
	*object = parsed;
	return 0;
}

/*
 * Finds an object stream, decoding it and reading its header the first
 * time.  The decoded streams together are bounded like the arena.
 */
static int
open_object_stream(
	struct pdf_document *document,
	unsigned long number,
	int depth,
	struct pdf_object_stream **found)
{
	struct pdf_object_stream *stream;
	struct pdf_object *object;
	const unsigned char *data;
	unsigned char *owned;
	size_t size;
	int dct;
	int error;

	/* Answers from a stream already opened. */
	for (stream = document->object_streams; stream != NULL; stream = stream->next) {
		if (stream->number == number) {
			*found = stream;
			return 0;
		}
	}

	/* Loads the stream object, whose generation is 0. */
	error = load_object(document, number, 0, depth, &object);
	if (error != 0)
		return error;
	if (object->type != PDF_OBJECT_STREAM)
		return PDF_EFORMAT;

	/* Decodes it, within the bound. */
	error = pdf_filter_decode(document, object, 0, &data, &size, &owned, &dct);
	if (error != 0)
		return error;
	if (size > PDF_READER_ARENA_MAX - document->object_streams_size) {
		free(owned);
		return ENOMEM;
	}

	/* Keeps the decoded object stream on the document. */
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL) {
		free(owned);
		return ENOMEM;
	}

	/* The stream's decoded bytes, which it owns. */
	stream->number = number;
	stream->bytes = data;
	stream->owned = owned;
	stream->size = size;

	/* Reads its header of numbers and offsets. */
	error = read_object_stream_header(document, stream, object);
	if (error != 0) {
		free(stream->numbers);
		free(stream->offsets);
		free(owned);
		free(stream);
		return error;
	}

	/* The document keeps it, counted against the decode bound. */
	stream->next = document->object_streams;
	document->object_streams = stream;
	document->object_streams_size += size;

	/* Succeeded: the stream is open. */
	*found = stream;
	return 0;
}

/* Reads an object stream's /N, /First and header pairs. */
static int
read_object_stream_header(
	struct pdf_document *document,
	struct pdf_object_stream *stream,
	struct pdf_object *dictionary)
{
	struct pdf_lexer lexer;
	struct pdf_token number;
	struct pdf_token offset;
	struct pdf_object *count;
	struct pdf_object *first;
	size_t member;
	int error;

	/* The count and the offset of the first object. */
	error = resolve_key(document, dictionary, "N", 0, &count);
	if (error != 0)
		return error;
	error = resolve_key(document, dictionary, "First", 0, &first);
	if (error != 0)
		return error;
	if (count->type != PDF_OBJECT_INTEGER || first->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (count->integer < 0 || (size_t)count->integer > PDF_READER_STREAM_OBJECTS_MAX)
		return PDF_EFORMAT;
	if (first->integer < 0 || (unsigned long)first->integer > stream->size)
		return PDF_EFORMAT;
	if ((size_t)count->integer > stream->size / 2)
		return PDF_EFORMAT;
	stream->first = (size_t)first->integer;

	/* The arrays of numbers and offsets. */
	stream->numbers = malloc(((size_t)count->integer + 1) * sizeof(*stream->numbers));
	if (stream->numbers == NULL)
		return ENOMEM;
	stream->offsets = malloc(((size_t)count->integer + 1) * sizeof(*stream->offsets));
	if (stream->offsets == NULL)
		return ENOMEM;

	/* Reads each pair before the first object; a short header ends the members there. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = stream->bytes;
	lexer.size = stream->first;
	lexer.arena = &document->arena;
	for (member = 0; member < (size_t)count->integer; member++) {
		error = pdf_lexer_next(&lexer, &number);
		if (error != 0)
			break;
		error = pdf_lexer_next(&lexer, &offset);
		if (error != 0)
			break;
		if (number.type != PDF_TOKEN_INTEGER || offset.type != PDF_TOKEN_INTEGER)
			break;
		if (number.integer < 0 || offset.integer < 0)
			break;
		stream->numbers[member] = (unsigned long)number.integer;
		stream->offsets[member] = (size_t)offset.integer;
	}

	/* The members read before any damage. */
	stream->count = member;

	/* Succeeded: the members are listed. */
	return 0;
}

/* Frees a document's decoded object streams. */
static void
free_object_streams(
	struct pdf_document *document)
{
	struct pdf_object_stream *stream;
	struct pdf_object_stream *next;

	/* Frees each stream's arrays, data and itself. */
	for (stream = document->object_streams; stream != NULL; stream = next) {
		next = stream->next;
		free(stream->numbers);
		free(stream->offsets);
		free(stream->owned);
		free(stream);
	}

	/* The document has none left. */
	document->object_streams = NULL;
}

/*
 * Sorts the entries by object number and keeps only the newest entry of
 * each number, the one read first.
 */
static void
sort_entries(
	struct pdf_document *document)
{
	size_t index;
	size_t kept;
	int error;

	/* Nothing to sort. */
	if (document->entries_count == 0)
		return;

	/* Orders the entries by number, and by reading order within a number; without memory for the sort, none is kept. */
	error = merge_entries(document->entries, document->entries_count);
	if (error != 0) {
		document->entries_count = 0;
		return;
	}

	/* Keeps the first entry of each number. */
	kept = 1;
	for (index = 1; index < document->entries_count; index++) {
		if (document->entries[index].number == document->entries[kept - 1].number)
			continue;
		document->entries[kept] = document->entries[index];
		kept++;
	}

	/* The table holds one entry a number. */
	document->entries_count = kept;
}

/*
 * Sorts entries stably by compare_entries, merging runs bottom up (the C
 * library's qsort is quadratic, and a document may have a million
 * objects).
 */
static int
merge_entries(
	struct pdf_xref_entry *entries,
	size_t count)
{
	struct pdf_xref_entry *buffer;
	struct pdf_xref_entry *source;
	struct pdf_xref_entry *target;
	struct pdf_xref_entry *swap;
	size_t width;
	size_t start;
	size_t middle;
	size_t end;
	size_t left;
	size_t right;
	size_t out;
	int take_left;
	int order;

	/* A table of one entry is sorted. */
	if (count < 2)
		return 0;

	/* The second array the runs are merged into. */
	buffer = malloc(count * sizeof(*buffer));
	if (buffer == NULL)
		return ENOMEM;

	/* Merges runs of width, doubling it, between the two arrays. */
	source = entries;
	target = buffer;
	for (width = 1; width < count; width *= 2) {
		for (start = 0; start < count; start += 2 * width) {
			/* The two runs [start, middle) and [middle, end). */
			middle = start + width;
			if (middle > count)
				middle = count;
			end = start + 2 * width;
			if (end > count)
				end = count;

			/* Takes the lower head each time, the left one on a tie. */
			left = start;
			right = middle;
			for (out = start; out < end; out++) {
				take_left = 0;
				if (right >= end) {
					/* The right run is used up. */
					take_left = 1;
				} else if (left < middle) {
					/* Both runs have a head; the right one goes first only when lower. */
					order = compare_entries(&source[left], &source[right]);
					if (order <= 0)
						take_left = 1;
				}

				/* Moves the chosen head. */
				if (take_left) {
					target[out] = source[left];
					left++;
				} else {
					target[out] = source[right];
					right++;
				}
			}
		}

		/* The merged runs are the next pass's source. */
		swap = source;
		source = target;
		target = swap;
	}

	/* The sorted entries end where the last pass wrote them. */
	if (source != entries)
		memcpy(entries, source, count * sizeof(*entries));
	free(buffer);

	/* Succeeded: the entries are in order. */
	return 0;
}

/* Orders two entries by object number, then by the order they were read. */
static int
compare_entries(
	const struct pdf_xref_entry *first,
	const struct pdf_xref_entry *second)
{
	/* A lower object number comes first. */
	if (first->number < second->number)
		return -1;
	if (first->number > second->number)
		return 1;

	/* Within a number, the entry read first, which is the newest, comes first. */
	if (first->sequence < second->sequence)
		return -1;
	if (first->sequence > second->sequence)
		return 1;

	/* The same entry. */
	return 0;
}

/* Finds the entry of an object number, or NULL when the table has none. */
static struct pdf_xref_entry *
find_entry(
	struct pdf_document *document,
	unsigned long number)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Halves the sorted table until the number is found or the range is empty. */
	low = 0;
	high = document->entries_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		if (document->entries[middle].number == number)
			return &document->entries[middle];
		if (document->entries[middle].number < number) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}

	/* The table has no entry of the number. */
	return NULL;
}

/*
 * Loads an indirect object, once.
 *
 * A number the table does not have, a free entry and a generation that
 * does not match all name the null object, as PDF defines.  depth bounds a
 * chain of objects that each need another to be read.
 */
static int
load_object(
	struct pdf_document *document,
	unsigned long number,
	unsigned long generation,
	int depth,
	struct pdf_object **object)
{
	struct pdf_xref_entry *entry;
	struct pdf_object *loaded;
	int error;

	/* Refuses a chain of loads deeper than the limit. */
	if (depth > PDF_READER_DEPTH_MAX)
		return PDF_EFORMAT;

	/* A missing, free or superseded object is null. */
	entry = find_entry(document, number);
	if (entry == NULL ||
	    !entry->in_use ||
	    entry->generation != generation) {
		*object = &document->null_object;
		return 0;
	}

	/* Answers from what an earlier load found. */
	switch (entry->state) {
	case PDF_ENTRY_LOADED:
		*object = entry->object;
		return 0;
	case PDF_ENTRY_FAILED:
		return entry->error;
	case PDF_ENTRY_LOADING:
		/* The object needs itself to be read. */
		return PDF_EFORMAT;
	case PDF_ENTRY_UNLOADED:
		break;
	}

	/* Parses the object, from the file or from its object stream, while marking it as under way. */
	entry->state = PDF_ENTRY_LOADING;
	loaded = NULL;
	if (entry->compressed) {
		error = parse_compressed(document, entry, depth, &loaded);
	} else {
		error = parse_indirect(document, entry, depth, &loaded);
	}

	/* A failed entry is not parsed again. */
	if (error != 0) {
		entry->state = PDF_ENTRY_FAILED;
		entry->error = error;
		return error;
	}

	/* Keeps the object for every later load. */
	entry->state = PDF_ENTRY_LOADED;
	entry->object = loaded;

	/* Succeeded: object is the loaded object. */
	*object = loaded;
	return 0;
}

/* Parses the indirect object an entry points at, with its stream data if it has one. */
static int
parse_indirect(
	struct pdf_document *document,
	const struct pdf_xref_entry *entry,
	int depth,
	struct pdf_object **object)
{
	struct pdf_lexer lexer;
	struct pdf_token token;
	struct pdf_object *parsed;
	struct pdf_object *type;
	size_t after_object;
	int is_keyword;
	int is_xref;
	int error;

	/* Reads the object number at the entry's offset, which must be the entry's. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = document->data;
	lexer.size = document->size;
	lexer.position = entry->offset;
	lexer.arena = &document->arena;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	if (token.type != PDF_TOKEN_INTEGER)
		return PDF_EFORMAT;
	if (token.integer < 0)
		return PDF_EFORMAT;
	if ((unsigned long)token.integer != entry->number)
		return PDF_EFORMAT;

	/* Reads the generation and the obj keyword. */
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	if (token.type != PDF_TOKEN_INTEGER)
		return PDF_EFORMAT;
	error = pdf_lexer_next(&lexer, &token);
	if (error != 0)
		return error;
	is_keyword = pdf_token_is_keyword(&token, "obj");
	if (!is_keyword)
		return PDF_EFORMAT;

	/* Parses the object itself. */
	error = pdf_parse_object(&lexer, 0, &parsed);
	if (error != 0)
		return error;

	/* A dictionary followed by the stream keyword is a stream's. */
	if (parsed->type == PDF_OBJECT_DICTIONARY) {
		after_object = lexer.position;
		error = pdf_lexer_next(&lexer, &token);
		if (error != 0)
			token.type = PDF_TOKEN_END;
		is_keyword = pdf_token_is_keyword(&token, "stream");
		if (is_keyword) {
			error = read_stream_data(document, &lexer, parsed, depth);
			if (error != 0)
				return error;
		} else {
			lexer.position = after_object;
		}
	}

	/* The object keeps its number and generation, which an encrypted document's keys use. */
	if (parsed->type == PDF_OBJECT_DICTIONARY || parsed->type == PDF_OBJECT_STREAM) {
		parsed->number = entry->number;
		parsed->generation = entry->generation;
	}

	/*
	 * An encrypted document's strings are decrypted, except the
	 * encryption dictionary's and a cross-reference stream's, which are
	 * never encrypted.
	 */
	if (document->crypt != NULL && entry->number != document->encrypt_number) {
		type = pdf_object_get(parsed, "Type");
		is_xref = pdf_object_is_name(type, "XRef");
		if (!is_xref) {
			error = decrypt_strings(document, parsed, entry->number, entry->generation, 0);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: object is the parsed object; its endobj is not needed. */
	*object = parsed;
	return 0;
}

/*
 * Opens an encrypted document's security handler: the /Encrypt dictionary
 * with the first /ID string and the password the document is opened with.
 * The objects loaded before it (by a repair's scan) are loaded again,
 * decrypted, and so are the object streams.
 */
static int
open_crypt(
	struct pdf_document *document)
{
	struct pdf_object *reference;
	struct pdf_object *encrypt;
	struct pdf_object *ids;
	struct pdf_object *first;
	const unsigned char *id;
	size_t id_length;
	size_t index;
	int error;

	/* The encryption dictionary, whose own strings are never encrypted. */
	reference = pdf_object_get(document->trailer, "Encrypt");
	document->encrypt_number = 0;
	if (reference->type == PDF_OBJECT_REFERENCE)
		document->encrypt_number = reference->number;
	error = resolve(document, reference, 0, &encrypt);
	if (error != 0)
		return error;

	/* The first /ID string, which the older revisions' keys mix in. */
	id = NULL;
	id_length = 0;
	error = resolve_key(document, document->trailer, "ID", 0, &ids);
	if (error != 0)
		return error;
	if (ids->type == PDF_OBJECT_ARRAY && ids->count > 0) {
		error = resolve(document, ids->values[0], 0, &first);
		if (error != 0)
			return error;
		if (first->type == PDF_OBJECT_STRING) {
			id = first->bytes;
			id_length = first->length;
		}
	}

	/* Opens the handler; a password that is neither the user's nor the owner's is EACCES. */
	error = pdf_crypt_open(document, encrypt, id, id_length, document->password, document->password_length, &document->crypt);
	if (error != 0)
		return error;

	/* Every object loaded so far but the encryption dictionary is loaded again, decrypted. */
	for (index = 0; index < document->entries_count; index++) {
		if (document->entries[index].number == document->encrypt_number)
			continue;
		if (document->entries[index].state == PDF_ENTRY_LOADING)
			continue;
		document->entries[index].state = PDF_ENTRY_UNLOADED;
		document->entries[index].object = NULL;
	}

	/* And the object streams decoded so far. */
	free_object_streams(document);
	document->object_streams_size = 0;

	/* Succeeded: the document is read through its handler. */
	return 0;
}

/*
 * Decrypts every string an object holds, in its arrays and dictionaries,
 * with the key of the object's number and generation.
 */
static int
decrypt_strings(
	struct pdf_document *document,
	struct pdf_object *object,
	unsigned long number,
	unsigned long generation,
	int depth)
{
	unsigned char *plain;
	size_t length;
	size_t index;
	int error;

	/* Refuses nesting past the limit. */
	if (depth > PDF_READER_DEPTH_MAX)
		return PDF_EFORMAT;

	/* A string is decrypted into the arena, with the NUL a string keeps after it. */
	if (object->type == PDF_OBJECT_STRING) {
		plain = pdf_arena_allocate(&document->arena, object->length + 1);
		if (plain == NULL)
			return ENOMEM;
		error = pdf_crypt_decrypt(document->crypt, 0, number, generation, object->bytes, object->length, plain, &length);
		if (error != 0)
			return error;
		plain[length] = '\0';
		object->bytes = plain;
		object->length = length;
		return 0;
	}

	/* An array's elements. */
	if (object->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index < object->count; index++) {
			error = decrypt_strings(document, object->values[index], number, generation, depth + 1);
			if (error != 0)
				return error;
		}

		/* Every element is decrypted. */
		return 0;
	}

	/* A dictionary's or a stream's values. */
	if (object->type == PDF_OBJECT_DICTIONARY || object->type == PDF_OBJECT_STREAM) {
		for (index = 0; index < object->count; index++) {
			error = decrypt_strings(document, object->values[index], number, generation, depth + 1);
			if (error != 0)
				return error;
		}

		/* Every value is decrypted. */
		return 0;
	}

	/* Anything else holds no string. */
	return 0;
}

/*
 * Finds the data of a stream after its stream keyword and turns the
 * dictionary into the stream.
 *
 * The data starts after the line end that follows the keyword and is as
 * long as /Length says; the endstream keyword must follow it.
 */
static int
read_stream_data(
	struct pdf_document *document,
	struct pdf_lexer *lexer,
	struct pdf_object *stream,
	int depth)
{
	struct pdf_object *length;
	struct pdf_token token;
	size_t start;
	int is_keyword;
	int error;

	/* Steps over the line end after the keyword: CR LF, LF, or a lone CR. */
	start = lexer->position;
	if (start < document->size && document->data[start] == '\r')
		start++;
	if (start < document->size && document->data[start] == '\n')
		start++;

	/* Reads the length, which may itself be an indirect object. */
	error = resolve_key(document, stream, "Length", depth + 1, &length);
	if (error != 0)
		return error;

	/* Refuses a length that is not a count of bytes inside the file. */
	if (length->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (length->integer < 0)
		return PDF_EFORMAT;
	if ((unsigned long)length->integer > document->size - start)
		return PDF_EFORMAT;

	/* Refuses data not followed by endstream, which a wrong length leaves behind. */
	lexer->position = start + (size_t)length->integer;
	error = pdf_lexer_next(lexer, &token);
	if (error != 0)
		return error;
	is_keyword = pdf_token_is_keyword(&token, "endstream");
	if (!is_keyword)
		return PDF_EFORMAT;

	/* Succeeded: the dictionary is now the stream's, with its data's range. */
	stream->type = PDF_OBJECT_STREAM;
	stream->data_offset = start;
	stream->data_length = (size_t)length->integer;
	return 0;
}

/*
 * Resolves an object that may be a reference to the object it names.
 *
 * A missing object (NULL) resolves to null.  A reference to a reference is
 * followed, to a bounded depth.
 */
static int
resolve(
	struct pdf_document *document,
	struct pdf_object *object,
	int depth,
	struct pdf_object **resolved)
{
	int hops;
	int error;

	/* A missing object is null. */
	if (object == NULL) {
		*resolved = &document->null_object;
		return 0;
	}

	/* Follows references until a direct object. */
	for (hops = 0; object->type == PDF_OBJECT_REFERENCE; hops++) {
		/* Refuses a chain of references longer than the limit. */
		if (hops > PDF_READER_DEPTH_MAX)
			return PDF_EFORMAT;
		error = load_object(document, object->number, object->generation, depth + hops, &object);
		if (error != 0)
			return error;
	}

	/* Succeeded: resolved is the direct object. */
	*resolved = object;
	return 0;
}

/*
 * Resolves the value of a key of a dictionary or a stream.
 *
 * A missing dictionary or key resolves to null.
 */
static int
resolve_key(
	struct pdf_document *document,
	const struct pdf_object *dictionary,
	const char *key,
	int depth,
	struct pdf_object **resolved)
{
	struct pdf_object *value;
	int error;

	/* Finds the value, which may be a reference. */
	value = pdf_object_get(dictionary, key);

	/* Resolves it. */
	error = resolve(document, value, depth, resolved);
	if (error != 0)
		return error;

	/* Succeeded: resolved is the key's direct value. */
	return 0;
}

/* Reads the catalog from the trailer and lists the pages of its page tree. */
static int
read_catalog(
	struct pdf_document *document)
{
	struct pdf_page_inheritance inheritance;
	struct pdf_object *encrypt;
	int error;

	/* An encrypted document is read through its security handler, opened once. */
	encrypt = pdf_object_get(document->trailer, "Encrypt");
	if (encrypt != NULL && document->crypt == NULL) {
		error = open_crypt(document);
		if (error != 0)
			return error;
	}

	/* Finds the catalog, which must be a dictionary. */
	error = resolve_key(document, document->trailer, "Root", 0, &document->catalog);
	if (error != 0)
		return error;
	if (document->catalog->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* Walks the page tree from its root, which passes nothing down. */
	memset(&inheritance, 0, sizeof(inheritance));
	document->mark++;
	error = walk_page_tree(document, pdf_object_get(document->catalog, "Pages"), &inheritance, 0);
	if (error != 0)
		return error;

	/* Succeeded: the pages are listed. */
	return 0;
}

/*
 * Walks one node of the page tree, listing its pages in order.
 *
 * A node passes its MediaBox, CropBox, Rotate and Resources down to the
 * nodes and pages under it, which may replace them.  A node reached a second time is
 * skipped, so a tree that loops ends.
 */
static int
walk_page_tree(
	struct pdf_document *document,
	struct pdf_object *node,
	const struct pdf_page_inheritance *inherited,
	int depth)
{
	struct pdf_page_inheritance inheritance;
	struct pdf_object *resolved;
	struct pdf_object *kids;
	struct pdf_object *type;
	struct pdf_object *value;
	size_t index;
	int is_pages;
	int visited;
	int error;

	/* Refuses a tree deeper than the limit. */
	if (depth > PDF_READER_DEPTH_MAX)
		return PDF_EFORMAT;

	/* Refuses a missing node. */
	if (node == NULL)
		return PDF_EFORMAT;

	/* Skips a node the walk has reached before. */
	visited = visit_reference(document, node);
	if (visited)
		return 0;

	/* Finds the node, which must be a dictionary. */
	error = resolve(document, node, 0, &resolved);
	if (error != 0)
		return error;
	if (resolved->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* Takes over the inherited attributes, replacing those the node sets. */
	inheritance = *inherited;
	value = pdf_object_get(resolved, "MediaBox");
	if (value != NULL)
		inheritance.media_box = value;
	value = pdf_object_get(resolved, "CropBox");
	if (value != NULL)
		inheritance.crop_box = value;
	value = pdf_object_get(resolved, "Rotate");
	if (value != NULL)
		inheritance.rotate = value;
	value = pdf_object_get(resolved, "Resources");
	if (value != NULL)
		inheritance.resources = value;

	/* Tells a node of the tree from a page: by its type, or by its kids when it has no type. */
	error = resolve_key(document, resolved, "Type", 0, &type);
	if (error != 0)
		return error;
	kids = pdf_object_get(resolved, "Kids");
	is_pages = pdf_object_is_name(type, "Pages");
	if (type->type == PDF_OBJECT_NULL && kids != NULL)
		is_pages = 1;

	/* Lists a page. */
	if (!is_pages) {
		error = add_page(document, resolved, node, &inheritance);
		if (error != 0)
			return error;
		return 0;
	}

	/* Finds the node's kids, which must be an array. */
	error = resolve(document, kids, 0, &kids);
	if (error != 0)
		return error;
	if (kids->type != PDF_OBJECT_ARRAY)
		return PDF_EFORMAT;

	/* Walks each kid in order. */
	for (index = 0; index < kids->count; index++) {
		error = walk_page_tree(document, kids->values[index], &inheritance, depth + 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the node's pages are listed. */
	return 0;
}

/*
 * Marks the object a reference names as reached by the current walk, and
 * reports whether it had been reached already.
 *
 * A direct object is part of its parent and cannot be reached twice.
 */
static int
visit_reference(
	struct pdf_document *document,
	const struct pdf_object *node)
{
	struct pdf_xref_entry *entry;

	/* Only a reference names a shared object. */
	if (node->type != PDF_OBJECT_REFERENCE)
		return 0;

	/* A reference to nothing reaches nothing that could be reached again. */
	entry = find_entry(document, node->number);
	if (entry == NULL)
		return 0;

	/* Reports an object the walk has reached. */
	if (entry->mark == document->mark)
		return 1;

	/* Marks the object for the rest of the walk. */
	entry->mark = document->mark;

	/* The object is reached for the first time. */
	return 0;
}

/* Appends a page, named by the node its parent lists, with the attributes it inherits. */
static int
add_page(
	struct pdf_document *document,
	struct pdf_object *page,
	struct pdf_object *node,
	const struct pdf_page_inheritance *inherited)
{
	struct pdf_reader_page *grown;
	size_t capacity;

	/* Refuses more pages than the limit. */
	if (document->pages_count == PDF_READER_PAGES_MAX)
		return ENOMEM;

	/* Grows the page array when it is full. */
	if (document->pages_count == document->pages_capacity) {
		capacity = document->pages_capacity * 2;
		if (capacity == 0)
			capacity = 16;
		grown = realloc(document->pages, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		document->pages = grown;
		document->pages_capacity = capacity;
	}

	/* Fills in the page; a page its parent writes out in place has no reference. */
	document->pages[document->pages_count].page = page;
	document->pages[document->pages_count].reference = NULL;
	if (node->type == PDF_OBJECT_REFERENCE)
		document->pages[document->pages_count].reference = node;
	document->pages[document->pages_count].media_box = inherited->media_box;
	document->pages[document->pages_count].crop_box = inherited->crop_box;
	document->pages[document->pages_count].rotate = inherited->rotate;
	document->pages[document->pages_count].resources = inherited->resources;
	document->pages_count++;

	/* Succeeded: the page is the document's last. */
	return 0;
}

/*
 * Reads the trailer's file identifier and the information dictionary's
 * dates.
 *
 * Neither is required, so anything missing or malformed is left unknown.
 */
static void
read_information(
	struct pdf_document *document)
{
	struct pdf_object *first;
	struct pdf_object *information;
	struct pdf_object *date;
	int error;

	/* Keeps the first element of the identifier when it is a sixteen-byte string. */
	error = read_identifier(document, &first);
	if (error == 0) {
		memcpy(document->id, first->bytes, PDF_READER_ID_SIZE);
		document->has_id = 1;
	}

	/* Finds the information dictionary. */
	error = resolve_key(document, document->trailer, "Info", 0, &information);
	if (error != 0)
		return;
	if (information->type != PDF_OBJECT_DICTIONARY)
		return;

	/* Reads the creation date. */
	error = resolve_key(document, information, "CreationDate", 0, &date);
	if (error == 0)
		document->creation_time = parse_date(date);

	/* Reads the modification date. */
	error = resolve_key(document, information, "ModDate", 0, &date);
	if (error == 0)
		document->modification_time = parse_date(date);
}

/*
 * Finds the first element of the trailer's file identifier.
 *
 * It must be a sixteen-byte string in an array of two; anything else
 * reports ENOENT.
 */
static int
read_identifier(
	struct pdf_document *document,
	struct pdf_object **first)
{
	struct pdf_object *identifier;
	struct pdf_object *element;
	int error;

	/* Finds the identifier, an array of two strings. */
	error = resolve_key(document, document->trailer, "ID", 0, &identifier);
	if (error != 0)
		return error;
	if (identifier->type != PDF_OBJECT_ARRAY)
		return ENOENT;
	if (identifier->count != 2)
		return ENOENT;

	/* Finds its first element, the permanent one. */
	error = resolve(document, identifier->values[0], 0, &element);
	if (error != 0)
		return error;
	if (element->type != PDF_OBJECT_STRING)
		return ENOENT;
	if (element->length != PDF_READER_ID_SIZE)
		return ENOENT;

	/* Succeeded: first is the identifier's permanent element. */
	*first = element;
	return 0;
}

/*
 * Converts a PDF date string, (D:YYYYMMDDHHmmSSOHH'mm'), to a time.
 *
 * Every field after the year may be left out, and O is Z, + or - before
 * the offset from universal time.  A date that is not valid, or before
 * 1970, is 0.
 */
static time_t
parse_date(
	const struct pdf_object *date)
{
	const unsigned char *text;
	size_t length;
	size_t position;
	long year;
	long month;
	long day;
	long hour;
	long minute;
	long second;
	long offset_hours;
	long offset_minutes;
	long shifted_year;
	long era;
	long year_of_era;
	long day_of_year;
	long day_of_era;
	long days;
	long *fields[5];
	size_t field;
	int sign;
	int error;
	double seconds;

	/* Only a string is a date. */
	if (date->type != PDF_OBJECT_STRING)
		return 0;
	text = date->bytes;
	length = date->length;

	/* Steps over the D: prefix, which the reference asks for but not every writer writes. */
	position = 0;
	if (length >= 2 &&
	    text[0] == 'D' &&
	    text[1] == ':')
		position = 2;

	/* Reads the year, which every date has. */
	error = read_digits(text, length, &position, 4, &year);
	if (error != 0)
		return 0;

	/* Reads each later field while they are there; the rest keep their defaults. */
	fields[0] = &month;
	fields[1] = &day;
	fields[2] = &hour;
	fields[3] = &minute;
	fields[4] = &second;
	month = 1;
	day = 1;
	hour = 0;
	minute = 0;
	second = 0;
	for (field = 0; field < 5; field++) {
		error = read_digits(text, length, &position, 2, fields[field]);
		if (error != 0)
			break;
	}

	/* Refuses fields outside their ranges. */
	if (month < 1 || month > 12)
		return 0;
	if (day < 1 || day > 31)
		return 0;
	if (hour > 23)
		return 0;
	if (minute > 59)
		return 0;
	if (second > 59)
		return 0;

	/* Reads the offset from universal time: + is ahead of it, - behind it. */
	sign = 0;
	offset_hours = 0;
	offset_minutes = 0;
	if (position < length && text[position] == '+')
		sign = 1;
	if (position < length && text[position] == '-')
		sign = -1;
	if (sign != 0) {
		position++;
		error = read_digits(text, length, &position, 2, &offset_hours);
		if (error == 0) {
			/* The minutes follow an apostrophe; a missing field leaves them zero. */
			if (position < length && text[position] == '\'')
				position++;
			error = read_digits(text, length, &position, 2, &offset_minutes);
			if (error != 0)
				offset_minutes = 0;
		}
	}

	/* Counts the days from 1970-01-01 to the date in the proleptic Gregorian calendar. */
	shifted_year = year;
	if (month <= 2)
		shifted_year = year - 1;
	era = shifted_year / 400;
	year_of_era = shifted_year - era * 400;
	if (month > 2) {
		day_of_year = (153 * (month - 3) + 2) / 5 + day - 1;
	} else {
		day_of_year = (153 * (month + 9) + 2) / 5 + day - 1;
	}

	/* The days since 1970. */
	day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	days = era * 146097 + day_of_era - 719468;

	/* Adds the time of day and removes the offset to reach universal time. */
	seconds = (double)days * 86400.0 + (double)hour * 3600.0 + (double)minute * 60.0 + (double)second;
	seconds -= (double)sign * ((double)offset_hours * 3600.0 + (double)offset_minutes * 60.0);

	/* Refuses a time before 1970 or past what a 32-bit time holds where time is 32 bits. */
	if (seconds < 0.0)
		return 0;
	if (sizeof(time_t) < 8 && seconds > 2147483647.0)
		return 0;

	/* Reports the time. */
	return (time_t)seconds;
}

/* Reads a fixed number of decimal digits at a position of a date. */
static int
read_digits(
	const unsigned char *text,
	size_t length,
	size_t *position,
	size_t digits,
	long *value)
{
	size_t index;
	long read;

	/* Refuses a field the string ends inside. */
	if (*position > length)
		return PDF_EFORMAT;
	if (length - *position < digits)
		return PDF_EFORMAT;

	/* Adds each digit, refusing anything else. */
	read = 0;
	for (index = 0; index < digits; index++) {
		if (text[*position + index] < '0' || text[*position + index] > '9')
			return PDF_EFORMAT;
		read = read * 10 + (text[*position + index] - '0');
	}

	/* Succeeded: value is the field and the position is past it. */
	*position += digits;
	*value = read;
	return 0;
}

/*
 * Reads a rectangle, an array of four numbers, with its corners put in
 * order: left, bottom, right, top.
 */
static int
read_rectangle(
	struct pdf_document *document,
	struct pdf_object *value,
	double rectangle[4])
{
	struct pdf_object *array;
	struct pdf_object *item;
	double swap;
	size_t index;
	int error;

	/* Finds the array of four items. */
	error = resolve(document, value, 0, &array);
	if (error != 0)
		return error;
	if (array->type != PDF_OBJECT_ARRAY || array->count != 4)
		return PDF_EFORMAT;

	/* Reads each coordinate, which must be a finite number. */
	for (index = 0; index < 4; index++) {
		error = resolve(document, array->values[index], 0, &item);
		if (error != 0)
			return error;
		error = pdf_object_number(item, &rectangle[index]);
		if (error != 0)
			return error;
		if (!(rectangle[index] - rectangle[index] == 0.0))
			return PDF_EFORMAT;
	}

	/* Puts the left edge before the right one. */
	if (rectangle[0] > rectangle[2]) {
		swap = rectangle[0];
		rectangle[0] = rectangle[2];
		rectangle[2] = swap;
	}

	/* Puts the bottom edge before the top one. */
	if (rectangle[1] > rectangle[3]) {
		swap = rectangle[1];
		rectangle[1] = rectangle[3];
		rectangle[3] = swap;
	}

	/* Succeeded: rectangle holds the ordered corners. */
	return 0;
}

/*
 * Checks that a stream's data needs no decoding.
 *
 * A filter, which the reader of stage 1 decodes, is reported as ENOTSUP.
 */
static int
check_unfiltered(
	struct pdf_document *document,
	const struct pdf_object *stream)
{
	struct pdf_object *filter;
	int error;

	/* Finds the filter, if any. */
	error = resolve_key(document, stream, "Filter", 0, &filter);
	if (error != 0)
		return error;

	/* No filter, or an empty list of them, leaves the data as it is. */
	if (filter->type == PDF_OBJECT_NULL)
		return 0;
	if (filter->type == PDF_OBJECT_ARRAY && filter->count == 0)
		return 0;

	/* Any filter is not decoded yet. */
	return ENOTSUP;
}

/*
 * Adds a content stream's bytes to a hash: as they are when the stream has
 * no filter, decoded when its one filter is FlateDecode without parameters
 * (and the decoded bytes are within the writer's limit), ENOTSUP for any
 * other filter.
 */
static int
hash_content_stream(
	struct pdf_document *document,
	const struct pdf_object *stream,
	SHA2_CTX *context)
{
	struct pdf_object *filter;
	struct pdf_object *name;
	struct pdf_object *parameters;
	const unsigned char *data;
	unsigned char *owned;
	size_t size;
	int is_flate;
	int dct;
	int error;

	/* A stream without a filter is hashed as it is. */
	error = check_unfiltered(document, stream);
	if (error == 0) {
		SHA256Update(context, document->data + stream->data_offset, stream->data_length);
		return 0;
	}

	/* Refuses a failure other than a filter. */
	if (error != ENOTSUP)
		return error;

	/* The filter: a name, or an array of exactly one. */
	error = resolve_key(document, stream, "Filter", 0, &filter);
	if (error != 0)
		return error;
	name = filter;
	if (filter->type == PDF_OBJECT_ARRAY) {
		if (filter->count != 1)
			return ENOTSUP;
		error = resolve(document, filter->values[0], 0, &name);
		if (error != 0)
			return error;
	}

	/* Refuses any filter but Flate. */
	is_flate = pdf_object_is_name(name, "FlateDecode");
	if (!is_flate)
		return ENOTSUP;

	/* Refuses Flate with parameters (a predictor), which the writer never gives content. */
	error = resolve_key(document, stream, "DecodeParms", 0, &parameters);
	if (error != 0)
		return error;
	if (parameters->type != PDF_OBJECT_NULL)
		return ENOTSUP;

	/* Decodes the stream. */
	error = pdf_filter_decode(document, stream, 0, &data, &size, &owned, &dct);
	if (error != 0)
		return error;

	/* Refuses decoded content past the limit the writer compresses to, which is not the writer's. */
	if (size > PDF_WRITER_FLATE_CONTENT_MAX) {
		free(owned);
		return ENOTSUP;
	}

	/* Adds the decoded bytes. */
	SHA256Update(context, data, size);
	free(owned);

	/* Succeeded: the stream is in the hash. */
	return 0;
}

/*
 * Searches the catalog's associated files (/AF) for an attached file.
 *
 * A file that is not there reports ENOENT.
 */
static int
find_in_associated_files(
	struct pdf_document *document,
	const char *name,
	const char *mime_type,
	struct pdf_object **stream)
{
	struct pdf_object *associated;
	size_t index;
	int error;

	/* Finds the list of associated files; a catalog without one has none. */
	error = resolve_key(document, document->catalog, "AF", 0, &associated);
	if (error != 0)
		return error;
	if (associated->type != PDF_OBJECT_ARRAY)
		return ENOENT;

	/* Tries each file specification in order. */
	for (index = 0; index < associated->count; index++) {
		error = match_file_specification(document, associated->values[index], name, mime_type, stream);
		if (error != ENOENT)
			return error;
	}

	/* No associated file is the one asked for. */
	return ENOENT;
}

/*
 * Searches the EmbeddedFiles name tree for an attached file.
 *
 * A file that is not there reports ENOENT.
 */
static int
find_in_embedded_files(
	struct pdf_document *document,
	const char *name,
	const char *mime_type,
	struct pdf_object **stream)
{
	struct pdf_object *names;
	struct pdf_object *tree;
	struct pdf_object *value;
	int error;

	/* Finds the tree; a catalog without one has no embedded files. */
	error = resolve_key(document, document->catalog, "Names", 0, &names);
	if (error != 0)
		return error;
	tree = pdf_object_get(names, "EmbeddedFiles");
	if (tree == NULL)
		return ENOENT;

	/* Looks the name up in a new walk of the tree. */
	document->mark++;
	error = find_in_name_tree(document, tree, name, 0, &value);
	if (error != 0)
		return error;

	/* Checks the file specification the tree gives. */
	error = match_file_specification(document, value, name, mime_type, stream);
	if (error != 0)
		return error;

	/* Succeeded: stream is the attached file. */
	return 0;
}

/*
 * Looks a name up in a name tree.
 *
 * Every node is searched rather than trusting /Limits, and a node reached
 * a second time in one search is skipped.  A name that is not there reports
 * ENOENT.
 */
static int
find_in_name_tree(
	struct pdf_document *document,
	struct pdf_object *node,
	const char *name,
	int depth,
	struct pdf_object **value)
{
	struct pdf_object *resolved;
	struct pdf_object *names;
	struct pdf_object *kids;
	struct pdf_object *key;
	size_t index;
	int visited;
	int equal;
	int error;

	/* Refuses a tree deeper than the limit. */
	if (depth > PDF_READER_DEPTH_MAX)
		return PDF_EFORMAT;

	/* Skips a node the search has reached before. */
	visited = visit_reference(document, node);
	if (visited)
		return ENOENT;

	/* Finds the node; one that is not a dictionary holds nothing. */
	error = resolve(document, node, 0, &resolved);
	if (error != 0)
		return error;
	if (resolved->type != PDF_OBJECT_DICTIONARY)
		return ENOENT;

	/* Looks through the node's own pairs of key and value. */
	error = resolve_key(document, resolved, "Names", 0, &names);
	if (error != 0)
		return error;
	if (names->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index + 1 < names->count; index += 2) {
			error = resolve(document, names->values[index], 0, &key);
			if (error != 0)
				return error;
			equal = string_equals(key, name);
			if (equal) {
				*value = names->values[index + 1];
				return 0;
			}
		}
	}

	/* Looks through the node's kids. */
	error = resolve_key(document, resolved, "Kids", 0, &kids);
	if (error != 0)
		return error;
	if (kids->type == PDF_OBJECT_ARRAY) {
		for (index = 0; index < kids->count; index++) {
			error = find_in_name_tree(document, kids->values[index], name, depth + 1, value);
			if (error != ENOENT)
				return error;
		}
	}

	/* The node and its kids do not have the name. */
	return ENOENT;
}

/*
 * Checks whether a file specification names an attached file and returns
 * its embedded file stream.
 *
 * The file's name is /UF or /F; the media type, when one is asked for, is
 * the stream's /Subtype.  A stream whose /Params /Size disagrees with its
 * length is malformed.  A specification that does not match reports ENOENT.
 */
static int
match_file_specification(
	struct pdf_document *document,
	struct pdf_object *value,
	const char *name,
	const char *mime_type,
	struct pdf_object **stream)
{
	struct pdf_object *specification;
	struct pdf_object *file_name;
	struct pdf_object *files;
	struct pdf_object *file;
	struct pdf_object *subtype;
	struct pdf_object *parameters;
	struct pdf_object *size;
	int equal;
	int error;

	/* Finds the specification, which must be a dictionary to hold an embedded file. */
	error = resolve(document, value, 0, &specification);
	if (error != 0)
		return error;
	if (specification->type != PDF_OBJECT_DICTIONARY)
		return ENOENT;

	/* Compares the name, the Unicode one first. */
	error = resolve_key(document, specification, "UF", 0, &file_name);
	if (error != 0)
		return error;
	equal = string_equals(file_name, name);
	if (!equal) {
		error = resolve_key(document, specification, "F", 0, &file_name);
		if (error != 0)
			return error;
		equal = string_equals(file_name, name);
	}

	/* Another file's specification. */
	if (!equal)
		return ENOENT;

	/* Finds the embedded file stream. */
	error = resolve_key(document, specification, "EF", 0, &files);
	if (error != 0)
		return error;
	file = pdf_object_get(files, "UF");
	if (file == NULL)
		file = pdf_object_get(files, "F");
	error = resolve(document, file, 0, &file);
	if (error != 0)
		return error;
	if (file->type != PDF_OBJECT_STREAM)
		return ENOENT;

	/* Compares the media type when one is asked for. */
	if (mime_type != NULL) {
		error = resolve_key(document, file, "Subtype", 0, &subtype);
		if (error != 0)
			return error;
		equal = pdf_object_is_name(subtype, mime_type);
		if (!equal)
			return ENOENT;
	}

	/* Refuses data that needs decoding. */
	error = check_unfiltered(document, file);
	if (error != 0)
		return error;

	/* Refuses a stream whose recorded size disagrees with its data. */
	error = resolve_key(document, file, "Params", 0, &parameters);
	if (error != 0)
		return error;
	error = resolve_key(document, parameters, "Size", 0, &size);
	if (error != 0)
		return error;
	if (size->type == PDF_OBJECT_INTEGER) {
		if (size->integer < 0)
			return PDF_EFORMAT;
		if ((unsigned long)size->integer != file->data_length)
			return PDF_EFORMAT;
	}

	/* Succeeded: stream is the attached file. */
	*stream = file;
	return 0;
}

/* Reports whether a string object holds exactly the bytes of a C string. */
static int
string_equals(
	const struct pdf_object *string,
	const char *text)
{
	size_t length;
	int difference;

	/* Only a string object can hold the bytes. */
	if (string->type != PDF_OBJECT_STRING)
		return 0;

	/* Compares the length, then the bytes. */
	length = strlen(text);
	if (string->length != length)
		return 0;
	difference = memcmp(string->bytes, text, length);
	if (difference != 0)
		return 0;

	/* The string holds the text. */
	return 1;
}
