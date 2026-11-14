/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test tools themselves: the reference coverage on shapes whose
 * coverage is known by hand, the test tree's markup reader, and the
 * pseudo-random generator.  It calls no function of the modules, so it
 * passes on the skeletons and keeps the tools honest for the
 * implementation Phases.
 */

#include "check.h"
#include "raster-reference.h"
#include "svg-test-tree.h"

/* Fills polylines by hand (no module function) with one contour of n points. */
static void
harness_contour(
	struct vec_lines *lines,
	struct vec_vertex *vertices,
	struct vec_contour *contour,
	const float *points,
	size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		vertices[i].x = points[2 * i];
		vertices[i].y = points[2 * i + 1];
		vertices[i].flags = 0;
	}
	contour->first = 0;
	contour->count = n;
	contour->closed = 1;
	memset(lines, 0, sizeof(*lines));
	lines->vertices = vertices;
	lines->vertex_count = n;
	lines->contours = contour;
	lines->contour_count = 1;
}

/* The reference on a rectangle, a triangle and two overlapping squares. */
static void
harness_reference(void)
{
	static const float square[] = { 1.25f, 1.5f, 3.75f, 1.5f, 3.75f, 3.0f, 1.25f, 3.0f };
	static const float triangle[] = { 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 4.0f };
	struct vec_vertex vertices[8];
	struct vec_contour contour;
	struct vec_lines lines;
	unsigned char alpha[16];

	/*
	 * The rectangle [1.25, 3.75) x [1.5, 3): pixel (1, 1) is covered
	 * 0.75 across and 0.5 down (its lower 8 of 16 sample lines), so
	 * 0.375 -> 96; pixel (2, 2) wholly -> 255; pixel (3, 2) 0.75 -> 191.
	 */
	harness_contour(&lines, vertices, &contour, square, 4);
	reference_coverage(&lines, VEC_FILL_NONZERO, 0.0, 0.0, 0, 0, 4, 4, alpha);
	CHECK_LONG(alpha[1 * 4 + 1], 96);
	CHECK_LONG(alpha[2 * 4 + 2], 255);
	CHECK_LONG(alpha[2 * 4 + 3], 191);
	CHECK_LONG(alpha[0 * 4 + 0], 0);

	/* The same moved by half a pixel right: pixel (1, 1) now 0.25 across (1.75 to 2) -> 0.125 -> 32. */
	reference_coverage(&lines, VEC_FILL_NONZERO, 0.5, 0.0, 0, 0, 4, 4, alpha);
	CHECK_LONG(alpha[1 * 4 + 1], 32);

	/*
	 * The right triangle with legs 4: the pixel (0, 0) is cut by x + y = 4
	 * outside it, so it is whole; pixel (1, 2) is cut through its corner
	 * (1, 3): the half below the diagonal x + y = 4 within [1, 2) x [2, 3)
	 * is outside, so 0.5 -> 128 (the sampled value may be one step away).
	 */
	harness_contour(&lines, vertices, &contour, triangle, 3);
	reference_coverage(&lines, VEC_FILL_NONZERO, 0.0, 0.0, 0, 0, 4, 4, alpha);
	CHECK_LONG(alpha[0], 255);
	CHECK_NEAR(alpha[2 * 4 + 1], 128, 1);
	CHECK_LONG(alpha[3 * 4 + 3], 0);
}

/* The markup reader and the tree functions. */
static void
harness_tree(void)
{
	struct test_document document;
	struct test_node *root;
	struct svg_tree tree;
	const struct svg_node *child;
	const struct svg_node *found;
	uint32_t color;

	root = test_tree_parse(&document,
	    "<svg viewBox='0 0 10 10'><!-- a comment --><rect id=\"r\" width=\"5\" height='5' fill='#f00'/>"
	    "<use xlink:href='#r'/><html:div/>text</svg>");
	CHECK(root != NULL);
	if (root == NULL)
		return;
	test_tree_make(&tree, &document);
	CHECK(strcmp(tree.name(tree.context, (const struct svg_node *)(const void *)root), "svg") == 0);
	child = tree.first_child(tree.context, (const struct svg_node *)(const void *)root);
	CHECK(child != NULL && strcmp(tree.name(tree.context, child), "rect") == 0);
	CHECK(child != NULL && strcmp(tree.attribute(tree.context, child, "height"), "5") == 0);
	child = tree.next_sibling(tree.context, child);
	CHECK(child != NULL && strcmp(tree.attribute(tree.context, child, "href"), "#r") == 0);
	child = tree.next_sibling(tree.context, child);
	CHECK(child != NULL && tree.name(tree.context, child) == NULL);
	found = tree.find_id(tree.context, "r");
	CHECK(found != NULL && strcmp(tree.name(tree.context, found), "rect") == 0);
	CHECK(tree.find_id(tree.context, "none") == NULL);
	CHECK(tree.parse_color(tree.context, "#f00", 4, &color) == 0 && color == 0xffff0000U);
	CHECK(tree.parse_color(tree.context, "#00ff0080", 9, &color) == 0 && color == 0x8000ff00U);
	CHECK(tree.parse_color(tree.context, "nocolor", 7, &color) == EINVAL);
	test_tree_free(&document);

	/* Malformed markup is refused. */
	CHECK(test_tree_parse(&document, "<svg><rect></svg") == NULL);
	CHECK(test_tree_parse(&document, "<svg width=5/>") == NULL);
}

/* The generator repeats from a seed. */
static void
harness_random(void)
{
	uint32_t a;
	uint32_t b;
	int i;
	float value;

	a = 1;
	b = 1;
	for (i = 0; i < 100; i++)
		CHECK(check_random(&a) == check_random(&b));
	for (i = 0; i < 1000; i++) {
		value = check_random_float(&a, -2.0f, 3.0f);
		CHECK(value >= -2.0f && value < 3.0f);
	}
}

int
main(void)
{
	harness_reference();
	harness_tree();
	harness_random();
	return check_finish("harness");
}
