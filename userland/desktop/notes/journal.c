/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The journal of Notes: every change since the last save, on disk as soon
 * as it is made, so that a crash or a killed process loses nothing.
 *
 * One journal belongs to one document file and lives in
 * $XDG_DATA_HOME/keiland/notes/ (or ~/.local/share/keiland/notes/), named
 * after a hash of the document's absolute path.  It does not exist while
 * the document is as it was saved.  The first change after a save writes
 * a new journal (to a temporary name, then renamed over the old one): the
 * header with the document's path, a snapshot of the whole document as
 * edit data (encode.c), and the change.  Every later change is appended
 * and flushed to the disk before the document applies it.  A save removes
 * the journal.
 *
 * A record is a type byte, a 32-bit length, the body and a 32-bit FNV-1a
 * checksum of the three.  Recovery reads the snapshot and replays the
 * records up to the first one that is incomplete or damaged (the one a
 * crash cut short).  The snapshot makes recovery independent of the saved
 * PDF: it works before Notes can read PDFs back.
 *
 * Version 2 (ws175-p007, plan/ws175/phase001/design.md [H4], [N3], [N10])
 * logs the edits of the PDF's objects: an edit put at a place on a page and
 * an edit taken off, and each image an edit uses, once a journal: its
 * description and the SHA-256 of its bytes in the record, the bytes in a
 * file of their own beside the journal (<journal>.images/<number>-<hash>),
 * written before the record.  The snapshot names its images by number only
 * and is followed by their records, so the journal holds every image the
 * document uses, also those of the saved file.  A Notes that reads
 * version 1 only does not recover a version 2 journal.
 *
 * Version 3 (ws216-p003, plan/ws216/phase003/phase.md section 5) logs the
 * strokes of the drawing tools: a stroke's record carries its tool's
 * constants' version and its baked values (encode.c).  A journal is
 * written in the lowest version that holds its document: 2 while there is
 * no such stroke, and when one is drawn into a journal of version 2, the
 * journal is written anew as version 3 from the document as it stands.
 * Recovery reads both; a journal of constants this Notes does not know is
 * not recovered (ENOTSUP), so that the caller can set it aside.
 */

#include "notes.h"

#include <dirent.h>
#include <sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The magic and the versions of a journal file: with the edits of the PDF's objects, and with the drawing tools. */
#define JOURNAL_MAGIC		"ZNJL"
#define JOURNAL_VERSION_EDITS	2U
#define JOURNAL_VERSION_TOOLS	3U

/* The record types. */
#define JOURNAL_SNAPSHOT	1U
#define JOURNAL_ADD_STROKE	2U
#define JOURNAL_REMOVE_STROKE	3U
#define JOURNAL_ADD_PAGE	4U
#define JOURNAL_REMOVE_PAGE	5U
#define JOURNAL_PUT_EDIT	6U
#define JOURNAL_TAKE_EDIT	7U
#define JOURNAL_IMAGE		8U

/* The ending of the folder of the images' files beside a journal, and of a journal set aside. */
#define JOURNAL_IMAGES_SUFFIX	".images"
#define JOURNAL_KEPT_SUFFIX	".kept"

/* The file name's ending. */
#define JOURNAL_SUFFIX		".journal"

/* The longest path a journal keeps, and the largest journal read back. */
#define JOURNAL_PATH_MAX	4096U
#define JOURNAL_SIZE_MAX	(512U * 1024U * 1024U)

/*
 * The journal of one document file.
 *
 * It lives as long as Notes shows the document.  descriptor is -1 while no
 * journal file is open: before the first change, and after a save or a
 * failure removed it.  images lists the numbers of the images the open
 * journal has logged (ws175-p007).  version is the open journal's
 * (ws216-p003), the version its records are written in.
 */
struct notes_journal {
	char document_path[JOURNAL_PATH_MAX];
	char path[JOURNAL_PATH_MAX];
	int descriptor;
	unsigned version;
	uint32_t *images;
	size_t image_count;
	size_t image_capacity;
};

/*
 * The images a recovery knows (each held once): the snapshot's and those
 * of the image records, by which the edits' records name them.
 */
struct journal_images {
	struct notes_image **images;
	size_t count;
	size_t capacity;
};

static int journal_start(struct notes_journal *journal, const struct notes_document *document, int tools);
static int journal_append(struct notes_journal *journal, const struct notes_document *document, unsigned type, const struct notes_buffer *body);
static void journal_record(struct notes_buffer *out, unsigned type, const unsigned char *body, size_t length);
static uint32_t journal_checksum(const unsigned char *data, size_t length);
static int journal_make_folder(const char *path);
static int write_all(int descriptor, const unsigned char *data, size_t length);
static int read_file(const char *path, unsigned char **data, size_t *size);
static int replay(struct notes_document *document, unsigned type, unsigned version, const unsigned char *body, size_t length, struct journal_images *table, const char *journal_path);
static int journal_ready(struct notes_journal *journal, const struct notes_document *document);
static int journal_image(struct notes_journal *journal, const struct notes_document *document, const struct notes_image *image);
static int journal_logged(const struct notes_journal *journal, uint32_t id);
static int journal_image_path(const char *journal_path, uint32_t id, const unsigned char digest[32], char *path, size_t size);
static void journal_remove_images(const char *journal_path);
static int table_add(struct journal_images *table, struct notes_image *image);
static struct notes_image *table_find(const struct journal_images *table, uint32_t id);
static void table_free(struct journal_images *table);
static int replay_image(const unsigned char *body, size_t length, struct journal_images *table, const char *journal_path);
static int read_number(const unsigned char *body, size_t length, size_t *offset, uint64_t *value);

/*
 * Writes the folder the journals live in: $XDG_DATA_HOME/keiland/notes, or
 * ~/.local/share/keiland/notes.
 *
 * Returns 0, or ENAMETOOLONG, or ENOENT when neither variable is set.
 */
int
notes_journal_folder(
	char *path,
	size_t size)
{
	const char *base;
	const char *home;
	int written;

	/* $XDG_DATA_HOME when it is set. */
	base = getenv("XDG_DATA_HOME");
	if (base != NULL && base[0] == '/') {
		written = snprintf(path, size, "%s/keiland/notes", base);
	} else {
		/* Otherwise under the home directory. */
		home = getenv("HOME");
		if (home == NULL || home[0] != '/')
			return ENOENT;
		written = snprintf(path, size, "%s/.local/share/keiland/notes", home);
	}

	/* A path that did not fit is refused. */
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the folder's path. */
	return 0;
}

/*
 * Writes the path of the journal of a document file (an absolute path).
 *
 * Returns 0, ENOENT or ENAMETOOLONG.
 */
int
notes_journal_path(
	const char *document_path,
	char *path,
	size_t size)
{
	char folder[JOURNAL_PATH_MAX];
	uint64_t hash;
	const unsigned char *byte;
	int written;
	int error;

	/* The folder. */
	error = notes_journal_folder(folder, sizeof(folder));
	if (error != 0)
		return error;

	/* The 64-bit FNV-1a hash of the document's path names the journal. */
	hash = 0xcbf29ce484222325ULL;
	for (byte = (const unsigned char *)document_path; *byte != '\0'; byte++) {
		hash ^= *byte;
		hash *= 0x100000001b3ULL;
	}

	/* The folder, the hash in hexadecimal and the ending. */
	written = snprintf(path, size, "%s/%08lx%08lx%s", folder, (unsigned long)(hash >> 32), (unsigned long)(hash & 0xffffffffU), JOURNAL_SUFFIX);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the journal's path. */
	return 0;
}

/*
 * Makes the journal of a document file; nothing is written until the first change.
 *
 * Returns NULL with errno set when the path is too long or has no folder.
 */
struct notes_journal *
notes_journal_create(
	const char *document_path)
{
	struct notes_journal *journal;
	size_t length;
	int error;

	/* A path that does not fit is refused. */
	length = strlen(document_path);
	if (length >= JOURNAL_PATH_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}

	/* Allocates the journal. */
	journal = calloc(1U, sizeof(*journal));
	if (journal == NULL)
		return NULL;

	/* The document's path and the journal's. */
	memcpy(journal->document_path, document_path, length + 1U);
	error = notes_journal_path(document_path, journal->path, sizeof(journal->path));
	if (error != 0) {
		free(journal);
		errno = error;
		return NULL;
	}

	/* Succeeded: no file is open yet. */
	journal->descriptor = -1;
	return journal;
}

/*
 * Closes the journal's file, which stays on the disk, and frees it.
 */
void
notes_journal_destroy(
	struct notes_journal *journal)
{
	/* A missing journal frees nothing. */
	if (journal == NULL)
		return;

	/* The file, when one is open, and the list of the images it logged. */
	if (journal->descriptor >= 0)
		(void)close(journal->descriptor);
	free(journal->images);
	free(journal);
}

/*
 * Tells the path of the document the journal belongs to.
 */
const char *
notes_journal_document_path(
	const struct notes_journal *journal)
{
	/* Reports the path given at creation. */
	return journal->document_path;
}

/*
 * Logs a stroke put on a page at a place.
 *
 * Returns 0, or an errno value (the change must then not be made).
 */
int
notes_journal_add_stroke(
	struct notes_journal *journal,
	const struct notes_document *document,
	size_t page,
	size_t place,
	const struct notes_stroke *stroke)
{
	struct notes_buffer body;
	unsigned constants;
	int tools;
	int error;

	/*
	 * A stroke of the drawing tools needs a journal of version 3: one of
	 * version 2 is written anew from the document as it stands, before the
	 * change, which the record then logs.
	 */
	constants = notes_tool_constants(stroke->tool);
	tools = 0;
	if (constants != 0U)
		tools = 1;
	if (tools &&
	    journal->descriptor >= 0 &&
	    journal->version < JOURNAL_VERSION_TOOLS) {
		(void)close(journal->descriptor);
		journal->descriptor = -1;
	}

	/* The journal file, when the change is the first since a save (or the old one was of version 2). */
	if (journal->descriptor < 0) {
		error = journal_start(journal, document, tools);
		if (error != 0)
			return error;
	}

	/* The page, the place and the stroke, in the journal's version. */
	notes_buffer_init(&body);
	notes_buffer_varint(&body, page);
	notes_buffer_varint(&body, place);
	error = notes_encode_stroke(stroke, journal->version, &body);
	if (error != 0) {
		notes_buffer_free(&body);
		return error;
	}

	/* Appends the record. */
	error = journal_append(journal, document, JOURNAL_ADD_STROKE, &body);
	notes_buffer_free(&body);
	if (error != 0)
		return error;

	/* Succeeded: the record is on the disk. */
	return 0;
}

/*
 * Logs a stroke taken off a page by its number.
 */
int
notes_journal_remove_stroke(
	struct notes_journal *journal,
	const struct notes_document *document,
	size_t page,
	uint32_t id)
{
	struct notes_buffer body;
	int error;

	/* The page and the number. */
	notes_buffer_init(&body);
	notes_buffer_varint(&body, page);
	notes_buffer_varint(&body, id);

	/* Appends the record. */
	error = journal_append(journal, document, JOURNAL_REMOVE_STROKE, &body);
	notes_buffer_free(&body);
	if (error != 0)
		return error;

	/* Succeeded: the record is on the disk. */
	return 0;
}

/*
 * Logs a blank page put in at an index.
 */
int
notes_journal_add_page(
	struct notes_journal *journal,
	const struct notes_document *document,
	size_t index)
{
	struct notes_buffer body;
	int error;

	/* The index. */
	notes_buffer_init(&body);
	notes_buffer_varint(&body, index);

	/* Appends the record. */
	error = journal_append(journal, document, JOURNAL_ADD_PAGE, &body);
	notes_buffer_free(&body);
	if (error != 0)
		return error;

	/* Succeeded: the record is on the disk. */
	return 0;
}

/*
 * Logs the page at an index taken out.
 */
int
notes_journal_remove_page(
	struct notes_journal *journal,
	const struct notes_document *document,
	size_t index)
{
	struct notes_buffer body;
	int error;

	/* The index. */
	notes_buffer_init(&body);
	notes_buffer_varint(&body, index);

	/* Appends the record. */
	error = journal_append(journal, document, JOURNAL_REMOVE_PAGE, &body);
	notes_buffer_free(&body);
	if (error != 0)
		return error;

	/* Succeeded: the record is on the disk. */
	return 0;
}

/*
 * Removes the journal's file: the document is saved and needs no journal.
 *
 * The next change starts a new one.  Returns 0, or the errno value of a
 * removal that failed for another reason than the file being absent.
 */
int
notes_journal_discard(
	struct notes_journal *journal)
{
	int status;

	/* The open file is closed first. */
	if (journal->descriptor >= 0) {
		(void)close(journal->descriptor);
		journal->descriptor = -1;
	}

	/* The images' files beside it go first (the next journal writes them again). */
	journal_remove_images(journal->path);
	journal->image_count = 0;

	/* Removes the file; one that is not there is already discarded. */
	status = unlink(journal->path);
	if (status != 0 && errno != ENOENT)
		return errno;

	/* Succeeded: no journal is left. */
	return 0;
}

/*
 * Logs an edit put on a page at a place (ws175-p007), and before it the
 * edit's image when this journal has not logged it yet.  Returns 0, or an
 * errno value (the change must then not be made).
 */
int
notes_journal_put_edit(
	struct notes_journal *journal,
	const struct notes_document *document,
	size_t page,
	size_t place,
	const struct notes_edit *edit)
{
	struct notes_buffer body;
	int logged;
	int error;

	/* The journal, then the image it does not have. */
	error = journal_ready(journal, document);
	if (error != 0)
		return error;
	if (edit->image != NULL) {
		logged = journal_logged(journal, edit->image->id);
		if (!logged) {
			error = journal_image(journal, document, edit->image);
			if (error != 0)
				return error;
		}
	}

	/* The page, the place and the edit. */
	notes_buffer_init(&body);
	notes_buffer_varint(&body, page);
	notes_buffer_varint(&body, place);
	notes_encode_edit(&body, edit);

	/* Appends the record. */
	error = journal_append(journal, document, JOURNAL_PUT_EDIT, &body);
	notes_buffer_free(&body);
	if (error != 0)
		return error;

	/* Succeeded: the record is on the disk. */
	return 0;
}

/*
 * Logs the edit of an object taken off a page (ws175-p007): the page and
 * the object (its number or its key).
 */
int
notes_journal_take_edit(
	struct notes_journal *journal,
	const struct notes_document *document,
	size_t page,
	const struct notes_edit *which)
{
	struct notes_buffer body;
	struct notes_edit named;
	int error;

	/* The object alone, without its state. */
	memset(&named, 0, sizeof(named));
	named.flags = which->flags & NOTES_EDIT_INSERTED;
	named.id = which->id;
	named.key = which->key;
	named.transform[0] = 1.0f;
	named.transform[3] = 1.0f;

	/* The page and the object. */
	notes_buffer_init(&body);
	notes_buffer_varint(&body, page);
	notes_encode_edit(&body, &named);

	/* Appends the record. */
	error = journal_append(journal, document, JOURNAL_TAKE_EDIT, &body);
	notes_buffer_free(&body);
	if (error != 0)
		return error;

	/* Succeeded: the record is on the disk. */
	return 0;
}

/*
 * Sets a journal aside (ws175-p007, design.md [N8]): a journal whose edits
 * no longer match the document's file is not recovered and must not be
 * written over by the next one; it and its images' files take the names
 * <journal>.kept and <journal>.kept.images, which no later run looks for.
 * Returns 0 or the errno value of renaming it.
 */
int
notes_journal_set_aside(
	const char *journal_path)
{
	char kept[JOURNAL_PATH_MAX + 16U];
	char images[JOURNAL_PATH_MAX + 16U];
	char kept_images[JOURNAL_PATH_MAX + 32U];
	int status;

	/* The names. */
	(void)snprintf(kept, sizeof(kept), "%s%s", journal_path, JOURNAL_KEPT_SUFFIX);
	(void)snprintf(images, sizeof(images), "%s%s", journal_path, JOURNAL_IMAGES_SUFFIX);
	(void)snprintf(kept_images, sizeof(kept_images), "%s%s", kept, JOURNAL_IMAGES_SUFFIX);

	/* The journal, then its images' folder (which may not be there). */
	status = rename(journal_path, kept);
	if (status != 0)
		return errno;
	(void)rename(images, kept_images);

	/* Succeeded: the journal is kept apart. */
	return 0;
}

/*
 * Rebuilds a document from a journal file.
 *
 * The document is made here from the journal's snapshot, then every intact
 * record is replayed onto it; *records tells how many were.  The path of
 * the journal's document is written to document_path.  Returns 0, ENOENT
 * when there is no journal, EINVAL for a file that is not a journal or has
 * no snapshot, ENOTSUP for a journal of a tool's constants this Notes does
 * not know (ws216-p003; the caller sets it aside), or ENOMEM.
 */
int
notes_journal_recover(
	const char *journal_path,
	struct notes_document *document,
	char *document_path,
	size_t size,
	size_t *records)
{
	struct journal_images table;
	unsigned char *data;
	size_t length;
	size_t offset;
	size_t path_length;
	size_t body_length;
	size_t page;
	size_t at;
	uint32_t stored;
	uint32_t computed;
	unsigned type;
	unsigned version;
	int have_snapshot;
	int matched;
	int error;

	/* Reads the whole file. */
	*records = 0;
	data = NULL;
	length = 0;
	error = read_file(journal_path, &data, &length);
	if (error != 0)
		return error;

	/* The header: the magic, then a version this file writes (2, or 3 with the drawing tools). */
	matched = -1;
	if (length >= 12U)
		matched = memcmp(data, JOURNAL_MAGIC, 4U);
	if (matched != 0 ||
	    (data[4] != JOURNAL_VERSION_EDITS && data[4] != JOURNAL_VERSION_TOOLS) ||
	    data[5] != 0U) {
		free(data);
		return EINVAL;
	}
	version = data[4];

	/* The document's path follows the header. */
	path_length = (size_t)data[8] | ((size_t)data[9] << 8) | ((size_t)data[10] << 16) | ((size_t)data[11] << 24);
	if (path_length > length - 12U || path_length >= size) {
		free(data);
		return EINVAL;
	}

	/* The path, and the records after it. */
	memcpy(document_path, data + 12U, path_length);
	document_path[path_length] = '\0';
	offset = 12U + path_length;

	/* Each record whose bytes are all there and whose checksum holds. */
	have_snapshot = 0;
	memset(&table, 0, sizeof(table));
	while (length - offset >= 9U) {
		/* The type and the body's length, which must fit with the checksum. */
		type = data[offset];
		body_length = (size_t)data[offset + 1U] | ((size_t)data[offset + 2U] << 8) | ((size_t)data[offset + 3U] << 16) | ((size_t)data[offset + 4U] << 24);
		if (body_length > length - offset - 9U)
			break;

		/* The checksum over the type, the length and the body. */
		stored = (uint32_t)data[offset + 5U + body_length];
		stored |= (uint32_t)data[offset + 6U + body_length] << 8;
		stored |= (uint32_t)data[offset + 7U + body_length] << 16;
		stored |= (uint32_t)data[offset + 8U + body_length] << 24;
		computed = journal_checksum(data + offset, 5U + body_length);
		if (stored != computed)
			break;

		/* The first record is the snapshot the others change. */
		if (!have_snapshot) {
			if (type != JOURNAL_SNAPSHOT)
				break;
			error = notes_decode_document(data + offset + 5U, body_length, document);
			if (error != 0) {
				free(data);
				return error;
			}

			/* The snapshot's images, which the records that follow give their bytes and name. */
			for (page = 0; page < document->page_count && error == 0; page++) {
				for (at = 0; at < document->pages[page]->edit_count && error == 0; at++) {
					if (document->pages[page]->edits[at]->image != NULL)
						error = table_add(&table, document->pages[page]->edits[at]->image);
				}
			}

			/* Memory gone ends the recovery. */
			if (error != 0) {
				table_free(&table);
				notes_document_free(document);
				free(data);
				return error;
			}

			/* The records that follow change it. */
			have_snapshot = 1;
		} else {
			/*
			 * A later record is a change; one that does not apply ends the
			 * replay, and one of constants this Notes does not know ends the
			 * recovery (the journal is not to be replayed in part).
			 */
			error = replay(document, type, version, data + offset + 5U, body_length, &table, journal_path);
			if (error == ENOTSUP) {
				table_free(&table);
				notes_document_free(document);
				free(data);
				return ENOTSUP;
			}
			if (error != 0)
				break;
		}

		/* The next record. */
		(*records)++;
		offset += 9U + body_length;
	}

	/* A journal without its snapshot rebuilds nothing; the recovery's hold on the images goes. */
	free(data);
	table_free(&table);
	if (!have_snapshot)
		return EINVAL;

	/* Succeeded: the document as it was when the journal was last written. */
	document->dirty = 1;
	return 0;
}

/*
 * Finds the most recently written journal in the folder.
 *
 * Returns 0 with its path, or ENOENT when there is none.
 */
int
notes_journal_newest(
	char *path,
	size_t size)
{
	char folder[JOURNAL_PATH_MAX];
	char candidate[JOURNAL_PATH_MAX];
	struct dirent *entry;
	struct stat status;
	DIR *directory;
	size_t name_length;
	size_t suffix_length;
	time_t newest;
	int matched;
	int found;
	int written;
	int error;

	/* The folder, which may not exist yet. */
	error = notes_journal_folder(folder, sizeof(folder));
	if (error != 0)
		return error;
	directory = opendir(folder);
	if (directory == NULL)
		return ENOENT;

	/* Each file whose name ends like a journal's. */
	found = 0;
	newest = 0;
	suffix_length = strlen(JOURNAL_SUFFIX);
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Only journals: a name longer than the ending, that ends with it. */
		name_length = strlen(entry->d_name);
		if (name_length <= suffix_length)
			continue;
		matched = strcmp(entry->d_name + name_length - suffix_length, JOURNAL_SUFFIX);
		if (matched != 0)
			continue;

		/* Its time; the newest wins. */
		written = snprintf(candidate, sizeof(candidate), "%s/%s", folder, entry->d_name);
		if (written < 0 || (size_t)written >= sizeof(candidate))
			continue;
		error = stat(candidate, &status);
		if (error != 0)
			continue;
		if (found && status.st_mtime < newest)
			continue;
		if ((size_t)written >= size)
			continue;
		memcpy(path, candidate, (size_t)written + 1U);
		newest = status.st_mtime;
		found = 1;
	}

	/* The folder is not read again. */
	(void)closedir(directory);

	/* No journal. */
	if (!found)
		return ENOENT;

	/* Succeeded: the path of the newest journal. */
	return 0;
}

/* Writes a new journal: the header and a snapshot of the document, then keeps it open for appending. */
/*
 * The version is the lowest that holds the document, and the record that
 * follows when tools is nonzero (a stroke of the drawing tools): 3 with
 * those tools, 2 without.
 */
static int
journal_start(
	struct notes_journal *journal,
	const struct notes_document *document,
	int tools)
{
	char temporary[JOURNAL_PATH_MAX + 8U];
	struct notes_buffer snapshot;
	struct notes_buffer file;
	const struct notes_image *image;
	size_t path_length;
	size_t page;
	size_t at;
	unsigned version;
	int descriptor;
	int logged;
	int drawn;
	int status;
	int error;

	/* The version the journal's records are written in. */
	version = JOURNAL_VERSION_EDITS;
	drawn = notes_document_new_tools(document);
	if (tools || drawn)
		version = JOURNAL_VERSION_TOOLS;

	/* The folder, made when it is missing. */
	error = journal_make_folder(journal->path);
	if (error != 0)
		return error;

	/* The snapshot of the document as it stands before the change. */
	notes_buffer_init(&snapshot);
	error = notes_encode_document(document, &snapshot);
	if (error != 0) {
		notes_buffer_free(&snapshot);
		return error;
	}

	/* The header and the snapshot's record. */
	notes_buffer_init(&file);
	path_length = strlen(journal->document_path);
	notes_buffer_bytes(&file, JOURNAL_MAGIC, 4U);
	notes_buffer_u16(&file, version);
	notes_buffer_u16(&file, 0U);
	notes_buffer_u32(&file, (uint32_t)path_length);
	notes_buffer_bytes(&file, journal->document_path, path_length);
	journal_record(&file, JOURNAL_SNAPSHOT, snapshot.data, snapshot.length);
	notes_buffer_free(&snapshot);
	if (file.error != 0) {
		error = file.error;
		notes_buffer_free(&file);
		return error;
	}

	/* Writes them to a temporary file. */
	(void)snprintf(temporary, sizeof(temporary), "%s.tmp", journal->path);
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (descriptor < 0) {
		error = errno;
		notes_buffer_free(&file);
		return error;
	}

	/* The bytes. */
	error = write_all(descriptor, file.data, file.length);
	notes_buffer_free(&file);

	/* Puts the bytes on the disk before the name. */
	if (error == 0) {
		status = fsync(descriptor);
		if (status != 0)
			error = errno;
	}

	/* The file is closed whatever happened. */
	(void)close(descriptor);
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* The new journal replaces any old one in one step. */
	status = rename(temporary, journal->path);
	if (status != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* Opens it for the records that follow, in its version. */
	journal->version = version;
	journal->image_count = 0;
	journal->descriptor = open(journal->path, O_WRONLY | O_APPEND);
	if (journal->descriptor < 0)
		return errno;

	/* The snapshot's images, each once (ws175-p007). */
	for (page = 0; page < document->page_count; page++) {
		for (at = 0; at < document->pages[page]->edit_count; at++) {
			image = document->pages[page]->edits[at]->image;
			if (image == NULL)
				continue;
			logged = journal_logged(journal, image->id);
			if (logged)
				continue;
			error = journal_image(journal, document, image);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the journal holds the document as it stands. */
	return 0;
}

/* Appends one record, starting the journal first when there is none, and flushes it to the disk. */
static int
journal_append(
	struct notes_journal *journal,
	const struct notes_document *document,
	unsigned type,
	const struct notes_buffer *body)
{
	struct notes_buffer record;
	int status;
	int error;

	/* A body that failed to build is not written. */
	if (body->error != 0)
		return body->error;

	/* The journal file, when the change is the first since a save. */
	if (journal->descriptor < 0) {
		error = journal_start(journal, document, 0);
		if (error != 0)
			return error;
	}

	/* The record. */
	notes_buffer_init(&record);
	journal_record(&record, type, body->data, body->length);
	if (record.error != 0) {
		error = record.error;
		notes_buffer_free(&record);
		return error;
	}

	/* Writes it and puts it on the disk. */
	error = write_all(journal->descriptor, record.data, record.length);
	notes_buffer_free(&record);
	if (error != 0)
		return error;
	status = fsync(journal->descriptor);
	if (status != 0)
		return errno;

	/* Succeeded: the change survives a crash from now on. */
	return 0;
}

/* Appends a record: type, length, body and checksum. */
static void
journal_record(
	struct notes_buffer *out,
	unsigned type,
	const unsigned char *body,
	size_t length)
{
	size_t start;
	uint32_t checksum;

	/* The type, the length and the body. */
	start = out->length;
	notes_buffer_u8(out, type);
	notes_buffer_u32(out, (uint32_t)length);
	notes_buffer_bytes(out, body, length);
	if (out->error != 0)
		return;

	/* The checksum of the three. */
	checksum = journal_checksum(out->data + start, out->length - start);
	notes_buffer_u32(out, checksum);
}

/* Computes the 32-bit FNV-1a hash of bytes. */
static uint32_t
journal_checksum(
	const unsigned char *data,
	size_t length)
{
	uint32_t hash;
	size_t index;

	/* Each byte folds into the hash. */
	hash = 0x811c9dc5U;
	for (index = 0; index < length; index++) {
		hash ^= data[index];
		hash *= 0x01000193U;
	}

	/* Reports the hash. */
	return hash;
}

/* Makes the folders of a file's path that are missing, private to the user. */
static int
journal_make_folder(
	const char *path)
{
	char folder[JOURNAL_PATH_MAX];
	char *slash;
	size_t length;
	int status;

	/* The file's folder. */
	length = strlen(path);
	if (length >= sizeof(folder))
		return ENAMETOOLONG;
	memcpy(folder, path, length + 1U);
	slash = strrchr(folder, '/');
	if (slash == NULL || slash == folder)
		return 0;
	*slash = '\0';

	/* Each folder from the root down; one that exists is passed. */
	for (slash = folder + 1; *slash != '\0'; slash++) {
		if (*slash != '/')
			continue;
		*slash = '\0';
		status = mkdir(folder, 0700);
		*slash = '/';
		if (status != 0 && errno != EEXIST)
			return errno;
	}

	/* The last folder. */
	status = mkdir(folder, 0700);
	if (status != 0 && errno != EEXIST)
		return errno;

	/* Succeeded: the folder is there. */
	return 0;
}

/* Writes every byte, going on after interruptions and short writes. */
static int
write_all(
	int descriptor,
	const unsigned char *data,
	size_t length)
{
	ssize_t written;

	/* Until nothing is left. */
	while (length > 0U) {
		written = write(descriptor, data, length);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return EIO;
		data += written;
		length -= (size_t)written;
	}

	/* Succeeded: every byte is written. */
	return 0;
}

/* Reads a whole file into memory the caller frees. */
static int
read_file(
	const char *path,
	unsigned char **data,
	size_t *size)
{
	struct stat status;
	unsigned char *bytes;
	ssize_t got;
	size_t done;
	int descriptor;
	int error;

	/* Opens the journal file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Its size, which must be sane. */
	error = fstat(descriptor, &status);
	if (error != 0 ||
	    status.st_size < 0 ||
	    (unsigned long)status.st_size > JOURNAL_SIZE_MAX) {
		(void)close(descriptor);
		return EINVAL;
	}

	/* Allocates the bytes (at least one, for an empty file). */
	bytes = malloc((size_t)status.st_size + 1U);
	if (bytes == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* Reads until the end. */
	done = 0;
	while (done < (size_t)status.st_size) {
		got = read(descriptor, bytes + done, (size_t)status.st_size - done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		done += (size_t)got;
	}

	/* The file is closed whatever happened. */
	(void)close(descriptor);

	/* Succeeded: what could be read. */
	*data = bytes;
	*size = done;
	return 0;
}

/* Applies one recorded change to the document (an image record adds an image to the recovery's). */
static int
replay(
	struct notes_document *document,
	unsigned type,
	unsigned version,
	const unsigned char *body,
	size_t length,
	struct journal_images *table,
	const char *journal_path)
{
	struct notes_stroke *stroke;
	struct notes_page *page;
	struct notes_edit read;
	struct notes_edit *edit;
	uint64_t first;
	uint64_t second;
	uint32_t image;
	size_t offset;
	size_t used;
	size_t place;
	int error;

	/* An image: its description, the digest of its bytes, the bytes from their file. */
	if (type == JOURNAL_IMAGE)
		return replay_image(body, length, table, journal_path);

	/* Every other record starts with one or two numbers. */
	offset = 0;
	error = read_number(body, length, &offset, &first);
	if (error != 0)
		return error;

	/* Each type applies its change with the document's primitives. */
	switch (type) {
	case JOURNAL_ADD_STROKE:
		/* The page, the place and the stroke. */
		error = read_number(body, length, &offset, &second);
		if (error != 0)
			return error;
		error = notes_decode_stroke(body + offset, length - offset, version, &used, &stroke);
		if (error != 0)
			return error;
		error = notes_document_insert_stroke(document, (size_t)first, (size_t)second, stroke);
		if (error != 0) {
			notes_stroke_free(stroke);
			return error;
		}

		break;
	case JOURNAL_REMOVE_STROKE:
		/* The page and the number. */
		error = read_number(body, length, &offset, &second);
		if (error != 0)
			return error;
		stroke = notes_document_remove_stroke(document, (size_t)first, (uint32_t)second, &place);
		if (stroke == NULL)
			return EINVAL;
		notes_stroke_free(stroke);
		break;
	case JOURNAL_ADD_PAGE:
		/* A blank page the size of the first. */
		page = notes_page_create(document->pages[0]->width, document->pages[0]->height, NOTES_BACKGROUND_PLAIN);
		if (page == NULL)
			return ENOMEM;
		error = notes_document_insert_page(document, (size_t)first, page);
		if (error != 0) {
			notes_page_free(page);
			return error;
		}

		break;
	case JOURNAL_REMOVE_PAGE:
		/* The page at the index. */
		page = notes_document_remove_page(document, (size_t)first);
		if (page == NULL)
			return EINVAL;
		notes_page_free(page);
		break;
	case JOURNAL_PUT_EDIT:
		/* The page, the place, the edit and its image, one the recovery knows. */
		error = read_number(body, length, &offset, &second);
		if (error == 0)
			error = notes_decode_edit(body + offset, length - offset, &used, &read, &image);
		if (error != 0)
			return error;
		if ((read.flags & NOTES_EDIT_IMAGE) != 0U || (read.flags & (NOTES_EDIT_INSERTED | NOTES_EDIT_TEXT)) == NOTES_EDIT_INSERTED) {
			read.image = table_find(table, image);
			if (read.image == NULL) {
				free(read.text);
				return EINVAL;
			}
		}

		/* The edit on the page (its words copied). */
		edit = notes_edit_copy(&read);
		free(read.text);
		if (edit == NULL)
			return ENOMEM;
		error = notes_document_put_edit(document, (size_t)first, (size_t)second, edit);
		if (error != 0) {
			notes_edit_free(edit);
			return error;
		}

		break;
	case JOURNAL_TAKE_EDIT:
		/* The page and the object. */
		error = notes_decode_edit(body + offset, length - offset, &used, &read, &image);
		if (error != 0)
			return error;
		free(read.text);
		read.text = NULL;
		edit = notes_document_take_edit(document, (size_t)first, &read, &place);
		if (edit == NULL)
			return EINVAL;
		notes_edit_free(edit);
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the change is applied. */
	return 0;
}

/* Reads one LEB128 number from a record's body. */
static int
read_number(
	const unsigned char *body,
	size_t length,
	size_t *offset,
	uint64_t *value)
{
	unsigned shift;
	unsigned byte;

	/* Seven bits a byte, low first, while the continuation bit is set. */
	*value = 0U;
	for (shift = 0; shift < 64U; shift += 7U) {
		if (*offset >= length)
			return EINVAL;
		byte = body[*offset];
		(*offset)++;
		*value |= (uint64_t)(byte & 0x7fU) << shift;
		if ((byte & 0x80U) == 0U)
			return 0;
	}

	/* A number longer than 64 bits is malformed. */
	return EINVAL;
}

/* Starts the journal when the change is the first since a save. */
static int
journal_ready(
	struct notes_journal *journal,
	const struct notes_document *document)
{
	/* A journal that is open. */
	if (journal->descriptor >= 0)
		return 0;

	/* A new one. */
	return journal_start(journal, document, 0);
}

/*
 * Logs an image: its bytes into their file beside the journal (once: a
 * file of the same number and digest is there already), then its record
 * (its description and the digest).  Returns 0, EINVAL for an image whose
 * bytes are not known, or an errno value.
 */
static int
journal_image(
	struct notes_journal *journal,
	const struct notes_document *document,
	const struct notes_image *image)
{
	char path[JOURNAL_PATH_MAX + 64U];
	char temporary[JOURNAL_PATH_MAX + 72U];
	unsigned char digest[SHA256_DIGEST_LENGTH];
	struct notes_buffer body;
	struct stat status;
	SHA2_CTX context;
	uint32_t *grown;
	size_t capacity;
	int descriptor;
	int result;
	int error;

	/* An image with its bytes, and their digest. */
	if (image->data == NULL)
		return EINVAL;
	SHA256Init(&context);
	SHA256Update(&context, image->data, image->size);
	SHA256Final(digest, &context);

	/* The file's path, in the images' folder (made when it is missing). */
	error = journal_image_path(journal->path, image->id, digest, path, sizeof(path));
	if (error == 0)
		error = journal_make_folder(path);
	if (error != 0)
		return error;

	/* The bytes, unless a file of the same number and digest is there: a temporary file on the disk, then its name. */
	result = stat(path, &status);
	if (result != 0) {
		(void)snprintf(temporary, sizeof(temporary), "%s.tmp", path);
		descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (descriptor < 0)
			return errno;
		error = write_all(descriptor, image->data, image->size);
		if (error == 0) {
			result = fsync(descriptor);
			if (result != 0)
				error = errno;
		}

		/* The file closed, then named. */
		(void)close(descriptor);
		if (error == 0) {
			result = rename(temporary, path);
			if (result != 0)
				error = errno;
		}

		/* A file that could not be written goes. */
		if (error != 0) {
			(void)unlink(temporary);
			return error;
		}
	}

	/* The record: the description and the digest. */
	notes_buffer_init(&body);
	notes_encode_image(&body, image);
	notes_buffer_bytes(&body, digest, sizeof(digest));
	error = journal_append(journal, document, JOURNAL_IMAGE, &body);
	notes_buffer_free(&body);
	if (error != 0)
		return error;

	/* The number among the logged ones. */
	if (journal->image_count == journal->image_capacity) {
		capacity = journal->image_capacity * 2U + 8U;
		grown = realloc(journal->images, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		journal->images = grown;
		journal->image_capacity = capacity;
	}

	/* Succeeded: the image is in the journal. */
	journal->images[journal->image_count] = image->id;
	journal->image_count++;
	return 0;
}

/* Tells whether the open journal has logged an image. */
static int
journal_logged(
	const struct notes_journal *journal,
	uint32_t id)
{
	size_t at;

	/* Each number logged. */
	for (at = 0; at < journal->image_count; at++) {
		if (journal->images[at] == id)
			return 1;
	}

	/* Not yet. */
	return 0;
}

/* Writes the path of an image's file: <journal>.images/<number>-<the digest's first 8 bytes in hexadecimal>. */
static int
journal_image_path(
	const char *journal_path,
	uint32_t id,
	const unsigned char digest[32],
	char *path,
	size_t size)
{
	int written;

	/* The folder, the number and the digest's start. */
	written = snprintf(path, size, "%s%s/%lu-%02x%02x%02x%02x%02x%02x%02x%02x", journal_path, JOURNAL_IMAGES_SUFFIX, (unsigned long)id,
			   digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7]);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path. */
	return 0;
}

/* Removes the images' files beside a journal, and their folder. */
static void
journal_remove_images(
	const char *journal_path)
{
	char folder[JOURNAL_PATH_MAX + 16U];
	char file[JOURNAL_PATH_MAX + 288U];
	struct dirent *entry;
	DIR *directory;

	/* The folder, which may not be there. */
	(void)snprintf(folder, sizeof(folder), "%s%s", journal_path, JOURNAL_IMAGES_SUFFIX);
	directory = opendir(folder);
	if (directory == NULL)
		return;

	/* Each file in it. */
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		if (entry->d_name[0] == '.')
			continue;
		(void)snprintf(file, sizeof(file), "%s/%s", folder, entry->d_name);
		(void)unlink(file);
	}

	/* The folder, emptied. */
	(void)closedir(directory);
	(void)rmdir(folder);
}

/* Adds an image to a recovery's, held once more.  Returns 0 or ENOMEM. */
static int
table_add(
	struct journal_images *table,
	struct notes_image *image)
{
	struct notes_image **grown;
	struct notes_image *found;
	size_t capacity;

	/* One there already. */
	found = table_find(table, image->id);
	if (found != NULL)
		return 0;

	/* Room for one more. */
	if (table->count == table->capacity) {
		capacity = table->capacity * 2U + 8U;
		grown = realloc(table->images, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		table->images = grown;
		table->capacity = capacity;
	}

	/* Succeeded: the image, held. */
	image->refs++;
	table->images[table->count] = image;
	table->count++;
	return 0;
}

/* Finds an image of a recovery's by its number; NULL when there is none. */
static struct notes_image *
table_find(
	const struct journal_images *table,
	uint32_t id)
{
	size_t at;

	/* Each image. */
	for (at = 0; at < table->count; at++) {
		if (table->images[at]->id == id)
			return table->images[at];
	}

	/* None. */
	return NULL;
}

/* Lets go of a recovery's images and frees its list. */
static void
table_free(
	struct journal_images *table)
{
	size_t at;

	/* Each hold. */
	for (at = 0; at < table->count; at++)
		notes_image_release(table->images[at]);

	/* The list. */
	free(table->images);
	memset(table, 0, sizeof(*table));
}

/*
 * Replays an image's record: its bytes read from their file, which must
 * have the digest, given to the image the recovery knows by that number
 * (the snapshot's) or to a new one.  Returns 0, EINVAL, or ENOMEM.
 */
static int
replay_image(
	const unsigned char *body,
	size_t length,
	struct journal_images *table,
	const char *journal_path)
{
	char path[JOURNAL_PATH_MAX + 64U];
	unsigned char digest[SHA256_DIGEST_LENGTH];
	struct notes_image *read;
	struct notes_image *image;
	unsigned char *bytes;
	SHA2_CTX context;
	unsigned kind;
	size_t used;
	size_t size;
	int differs;
	int error;

	/* The description and the digest. */
	error = notes_decode_image(body, length, &used, &read);
	if (error != 0)
		return error;
	if (length - used != SHA256_DIGEST_LENGTH) {
		notes_image_release(read);
		return EINVAL;
	}

	/* The bytes from their file, with the digest. */
	error = journal_image_path(journal_path, read->id, body + used, path, sizeof(path));
	if (error == 0)
		error = read_file(path, &bytes, &size);
	if (error != 0) {
		notes_image_release(read);
		return EINVAL;
	}

	/* The digest of the bytes read. */
	SHA256Init(&context);
	SHA256Update(&context, bytes, size);
	SHA256Final(digest, &context);
	differs = memcmp(digest, body + used, sizeof(digest));
	if (differs != 0 || size == 0) {
		free(bytes);
		notes_image_release(read);
		return EINVAL;
	}

	/* The image the recovery knows by the number, or the new one. */
	kind = read->kind;
	image = table_find(table, read->id);
	if (image == NULL) {
		error = table_add(table, read);
		image = read;
	}

	/* The description's own hold goes (the recovery's stays). */
	notes_image_release(read);
	if (error != 0) {
		free(bytes);
		return error;
	}

	/* Its bytes, as they were logged. */
	free(image->data);
	image->data = bytes;
	image->size = size;
	image->kind = kind;
	return 0;
}
