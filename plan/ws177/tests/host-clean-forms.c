/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p011: the host test of the clean copy's resources of forms, Type 3
 * fonts and tiling patterns, and of the attachments' name tree, on
 * make-clean-forms.py's sample:
 *
 *   host-clean-forms IN CLEAN
 *
 * The clean copy of IN without kei-notes.bin is saved to CLEAN: the
 * images used (ImB through the form Fm2 that borrows the page's resources,
 * ImP through the tiling pattern P1 that does, ImIn in Fm1's own, ImG in
 * the Type 3 font's own) are there, those used by nothing (ImC, ImQ,
 * ImOut, ImH) are not; kei-notes.bin's specification, written in the name
 * tree's leaf and in /AF, is gone; the leaf's /Limits are (a.txt) to
 * (a.txt), the other leaf's unchanged.  Prints each check and exits 0 when
 * all passed.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The checks that passed and failed. */
static int test_passed;
static int test_failed;

static void check(int ok, const char *what);
static int file_has(const char *path, const char *text);
static int limits_are(struct pdf_document *document, struct pdf_object *leaf, const char *low, const char *high);
static int string_is(const struct pdf_object *object, const char *text);

/*
 * Runs the checks; the exit status says whether they all passed.
 */
int
main(
	int argc,
	char **argv)
{
	struct pdf_clean_counts counts;
	struct pdf_document *document;
	struct pdf_document *clean;
	struct pdf_object *trailer;
	struct pdf_object *catalog;
	struct pdf_object *names;
	struct pdf_object *tree;
	struct pdf_object *kids;
	struct pdf_object *leaf;
	struct pdf_object *pairs;
	struct pdf_object *associated;
	int error;
	int ok;

	/* The sample and the copy's path. */
	if (argc != 3)
		return 2;
	error = pdf_document_open(argv[1], &document);
	check(error == 0, "open the sample");
	if (error != 0)
		return 1;

	/* The clean copy without kei-notes.bin. */
	memset(&counts, 0, sizeof(counts));
	error = pdf_document_save_clean(document, argv[2], "kei-notes.bin", &counts);
	check(error == 0, "save the clean copy");
	pdf_document_close(document);
	if (error != 0)
		return 1;
	printf("objects %lu, resources left out %lu\n", (unsigned long)counts.objects, (unsigned long)counts.dropped);

	/* The images used, through the forms, the font and the pattern. */
	check(file_has(argv[2], "IMBIMBIMBIMB"), "ImB, drawn by Fm2 with the page's resources, is kept");
	check(file_has(argv[2], "IMPIMPIMPIMP"), "ImP, drawn by the pattern P1 with the page's resources, is kept");
	check(file_has(argv[2], "ININININININ"), "ImIn, in Fm1's own resources and drawn, is kept");
	check(file_has(argv[2], "GLYGLYGLYGLY"), "ImG, in the Type 3 font's own resources and drawn by its glyph, is kept");

	/* The images nothing draws. */
	check(!file_has(argv[2], "IMCIMCIMCIMC"), "the page's ImC is left out");
	check(!file_has(argv[2], "IMQIMQIMQIMQ"), "the page's ImQ is left out");
	check(!file_has(argv[2], "OUTOUTOUTOUT"), "Fm1's ImOut is left out");
	check(!file_has(argv[2], "HHHHHHHHHHHH"), "the Type 3 font's ImH is left out");
	check(!file_has(argv[2], "/ImOut") && !file_has(argv[2], "/ImH") && !file_has(argv[2], "/ImC") && !file_has(argv[2], "/ImQ"),
	    "no name of an image left out is written");
	check(counts.dropped >= 4, "four resources are counted as left out");

	/* The copy, read again. */
	error = pdf_document_open(argv[2], &clean);
	check(error == 0, "open the clean copy");
	if (error != 0)
		return 1;
	pdf_reader_roots(clean, &trailer, &catalog);

	/* The name tree's leaves: kei-notes.bin gone, the limits written again. */
	error = pdf_reader_resolve_key(clean, catalog, "Names", &names);
	if (error == 0)
		error = pdf_reader_resolve_key(clean, names, "EmbeddedFiles", &tree);
	if (error == 0)
		error = pdf_reader_resolve_key(clean, tree, "Kids", &kids);
	ok = error == 0 && kids->type == PDF_OBJECT_ARRAY && kids->count == 2;
	check(ok, "the name tree keeps its two leaves");
	if (ok) {
		error = pdf_reader_resolve(clean, kids->values[0], &leaf);
		if (error == 0)
			error = pdf_reader_resolve_key(clean, leaf, "Names", &pairs);
		check(error == 0 && pairs->type == PDF_OBJECT_ARRAY && pairs->count == 2 && string_is(pairs->values[0], "a.txt"),
		    "the first leaf keeps only a.txt");
		check(error == 0 && limits_are(clean, leaf, "a.txt", "a.txt"), "the first leaf's /Limits are (a.txt) (a.txt)");
		error = pdf_reader_resolve(clean, kids->values[1], &leaf);
		check(error == 0 && limits_are(clean, leaf, "z.txt", "z.txt"), "the second leaf's /Limits stay (z.txt) (z.txt)");
	}

	/* The catalog's /AF: the direct specification of kei-notes.bin gone, a.txt's kept. */
	error = pdf_reader_resolve_key(clean, catalog, "AF", &associated);
	check(error == 0 && associated->type == PDF_OBJECT_ARRAY && associated->count == 1, "/AF keeps one specification");
	check(!file_has(argv[2], "kei-notes.bin"), "kei-notes.bin is named nowhere (as text)");
	pdf_document_close(clean);

	/* The verdict. */
	printf("host-clean-forms: %d passed, %d failed\n", test_passed, test_failed);
	if (test_failed != 0)
		return 1;
	return 0;
}

/* Counts and prints a check. */
static void
check(
	int ok,
	const char *what)
{
	/* A check that passed. */
	if (ok) {
		test_passed++;
		printf("ok %s\n", what);
		return;
	}

	/* A failure. */
	test_failed++;
	printf("FAIL %s\n", what);
}

/* Tells whether a file's bytes hold a text. */
static int
file_has(
	const char *path,
	const char *text)
{
	unsigned char *bytes;
	FILE *stream;
	size_t length;
	size_t size;
	size_t at;
	int found;

	/* The whole file. */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return 0;
	bytes = malloc(16U * 1024U * 1024U);
	size = 0;
	if (bytes != NULL)
		size = fread(bytes, 1, 16U * 1024U * 1024U, stream);
	fclose(stream);

	/* The text anywhere in it. */
	found = 0;
	length = strlen(text);
	for (at = 0; bytes != NULL && at + length <= size && !found; at++)
		found = memcmp(bytes + at, text, length) == 0;
	free(bytes);
	return found;
}

/* Tells whether a name tree node's /Limits are two strings. */
static int
limits_are(
	struct pdf_document *document,
	struct pdf_object *leaf,
	const char *low,
	const char *high)
{
	struct pdf_object *limits;
	int error;

	/* The array of two. */
	error = pdf_reader_resolve_key(document, leaf, "Limits", &limits);
	if (error != 0 || limits->type != PDF_OBJECT_ARRAY || limits->count != 2)
		return 0;

	/* Its strings. */
	if (!string_is(limits->values[0], low))
		return 0;
	return string_is(limits->values[1], high);
}

/* Tells whether an object is a string of a text's bytes. */
static int
string_is(
	const struct pdf_object *object,
	const char *text)
{
	/* The same length and bytes. */
	if (object->type != PDF_OBJECT_STRING || object->length != strlen(text))
		return 0;
	return memcmp(object->bytes, text, object->length) == 0;
}
