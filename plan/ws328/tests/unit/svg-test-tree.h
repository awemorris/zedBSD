/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A tree for the SVG unit tests: a small markup string read into nodes,
 * and the struct svg_tree functions over them (svg/svg-tree.h), so the
 * SVG modules are tested without the engine's DOM.
 *
 * The markup is a subset of XML enough for test pictures: elements with
 * attributes in single or double quotes, self-closing elements, text
 * (kept as a nameless node), comments <!-- --> (skipped), and the
 * entities &lt; &gt; &amp; &quot; &apos; in attribute values.  Element
 * names are taken as SVG names as written, except that a name with a
 * prefix other than "svg:" (xlink:href aside, which only attributes have)
 * is an element of another namespace (name NULL).  Attribute names keep
 * their prefix ("xlink:href"); the attribute function maps "href" to
 * href, else xlink:href, as svg-tree.h asks.  An element whose name
 * starts with "html:" is foreign too.
 *
 * parse_color understands the CSS named colors black, white, red, lime,
 * green, blue, yellow, cyan, magenta, gray, transparent, and #rgb,
 * #rgba, #rrggbb, #rrggbbaa: enough for tests (the engine gives the full
 * CSS color parser).
 *
 * Errors make test_tree_parse return NULL; the tests treat that as a
 * failed check.  Everything lives in one arena freed by test_tree_free.
 */

#ifndef WS328_TEST_SVG_TREE_H
#define WS328_TEST_SVG_TREE_H

#include "svg/svg-tree.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most attributes of one test element. */
#define TEST_ATTRIBUTES	32

/* One node: an element (name set) or text (name NULL, foreign 0), with its attributes and its relatives. */
struct test_node {
	char *name;
	int foreign;
	int attribute_count;
	char *keys[TEST_ATTRIBUTES];
	char *values[TEST_ATTRIBUTES];
	struct test_node *first_child;
	struct test_node *last_child;
	struct test_node *next_sibling;
	struct test_node *parent;
};

/* A parsed test document: its root element and everything allocated for it. */
struct test_document {
	struct test_node *root;
	void **blocks;
	size_t block_count;
	size_t block_capacity;
};

/* Allocates zeroed memory owned by the document (NULL on failure). */
CHECK_UNUSED static void *
test_alloc(
	struct test_document *document,
	size_t size)
{
	void **grown;
	void *block;
	size_t capacity;

	/* Room in the table of blocks. */
	if (document->block_count == document->block_capacity) {
		capacity = document->block_capacity == 0 ? 64 : document->block_capacity * 2;
		grown = realloc(document->blocks, capacity * sizeof(*grown));
		if (grown == NULL)
			return NULL;
		document->blocks = grown;
		document->block_capacity = capacity;
	}

	/* The block itself. */
	block = calloc(1, size == 0 ? 1 : size);
	if (block == NULL)
		return NULL;
	document->blocks[document->block_count++] = block;
	return block;
}

/* Copies length bytes into a NUL-terminated string of the document, decoding the five entities. */
CHECK_UNUSED static char *
test_copy(
	struct test_document *document,
	const char *text,
	size_t length)
{
	char *copy;
	size_t in;
	size_t out;

	copy = test_alloc(document, length + 1);
	if (copy == NULL)
		return NULL;
	out = 0;
	for (in = 0; in < length; in++) {
		if (text[in] == '&' && strncmp(text + in, "&lt;", 4) == 0) {
			copy[out++] = '<';
			in += 3;
		} else if (text[in] == '&' && strncmp(text + in, "&gt;", 4) == 0) {
			copy[out++] = '>';
			in += 3;
		} else if (text[in] == '&' && strncmp(text + in, "&amp;", 5) == 0) {
			copy[out++] = '&';
			in += 4;
		} else if (text[in] == '&' && strncmp(text + in, "&quot;", 6) == 0) {
			copy[out++] = '"';
			in += 5;
		} else if (text[in] == '&' && strncmp(text + in, "&apos;", 6) == 0) {
			copy[out++] = '\'';
			in += 5;
		} else {
			copy[out++] = text[in];
		}
	}
	copy[out] = '\0';
	return copy;
}

/* Frees a test document. */
CHECK_UNUSED static void
test_tree_free(
	struct test_document *document)
{
	size_t index;

	for (index = 0; index < document->block_count; index++)
		free(document->blocks[index]);
	free(document->blocks);
	memset(document, 0, sizeof(*document));
}

/* Appends a child to a parent node. */
CHECK_UNUSED static void
test_append(
	struct test_node *parent,
	struct test_node *child)
{
	child->parent = parent;
	if (parent->last_child == NULL)
		parent->first_child = child;
	else
		parent->last_child->next_sibling = child;
	parent->last_child = child;
}

/*
 * Parses test markup into a document; returns its root element, or NULL
 * for malformed markup (the document is then freed).
 */
CHECK_UNUSED static struct test_node *
test_tree_parse(
	struct test_document *document,
	const char *markup)
{
	struct test_node holder;
	struct test_node *open;
	struct test_node *node;
	const char *p;
	const char *start;
	char quote;
	size_t length;

	memset(document, 0, sizeof(*document));
	memset(&holder, 0, sizeof(holder));
	open = &holder;
	p = markup;
	while (*p != '\0') {
		if (strncmp(p, "<!--", 4) == 0) {
			start = strstr(p + 4, "-->");
			if (start == NULL)
				goto fail;
			p = start + 3;
			continue;
		}
		if (p[0] == '<' && p[1] == '/') {
			p += 2;
			while (*p != '\0' && *p != '>')
				p++;
			if (*p != '>' || open == &holder)
				goto fail;
			p++;
			open = open->parent;
			continue;
		}
		if (*p == '<') {
			node = test_alloc(document, sizeof(*node));
			if (node == NULL)
				goto fail;
			p++;
			start = p;
			while (*p != '\0' && !isspace((unsigned char)*p) && *p != '/' && *p != '>')
				p++;
			length = (size_t)(p - start);
			if (length == 0)
				goto fail;
			node->name = test_copy(document, start, length);
			if (node->name == NULL)
				goto fail;
			if (strchr(node->name, ':') != NULL && strncmp(node->name, "svg:", 4) != 0)
				node->foreign = 1;
			if (strncmp(node->name, "svg:", 4) == 0)
				memmove(node->name, node->name + 4, strlen(node->name + 4) + 1);
			for (;;) {
				while (isspace((unsigned char)*p))
					p++;
				if (*p == '/' || *p == '>' || *p == '\0')
					break;
				start = p;
				while (*p != '\0' && *p != '=' && !isspace((unsigned char)*p))
					p++;
				length = (size_t)(p - start);
				while (isspace((unsigned char)*p))
					p++;
				if (*p != '=' || length == 0 || node->attribute_count >= TEST_ATTRIBUTES)
					goto fail;
				p++;
				while (isspace((unsigned char)*p))
					p++;
				quote = *p;
				if (quote != '"' && quote != '\'')
					goto fail;
				node->keys[node->attribute_count] = test_copy(document, start, length);
				p++;
				start = p;
				while (*p != '\0' && *p != quote)
					p++;
				if (*p != quote)
					goto fail;
				node->values[node->attribute_count] = test_copy(document, start, (size_t)(p - start));
				if (node->keys[node->attribute_count] == NULL || node->values[node->attribute_count] == NULL)
					goto fail;
				node->attribute_count++;
				p++;
			}
			test_append(open, node);
			if (*p == '/') {
				p++;
				if (*p != '>')
					goto fail;
				p++;
				continue;
			}
			if (*p != '>')
				goto fail;
			p++;
			open = node;
			continue;
		}
		start = p;
		while (*p != '\0' && *p != '<')
			p++;
		node = test_alloc(document, sizeof(*node));
		if (node == NULL)
			goto fail;
		test_append(open, node);
	}
	if (open != &holder || holder.first_child == NULL)
		goto fail;

	/* The root is the first element at the top. */
	for (node = holder.first_child; node != NULL; node = node->next_sibling) {
		if (node->name != NULL) {
			node->parent = NULL;
			document->root = node;
			return node;
		}
	}

fail:
	test_tree_free(document);
	return NULL;
}

/* The struct svg_tree functions over a test document (context is the document). */
CHECK_UNUSED static const struct svg_node *
test_first_child(
	void *context,
	const struct svg_node *node)
{
	(void)context;
	return (const struct svg_node *)((const struct test_node *)(const void *)node)->first_child;
}

CHECK_UNUSED static const struct svg_node *
test_next_sibling(
	void *context,
	const struct svg_node *node)
{
	(void)context;
	return (const struct svg_node *)((const struct test_node *)(const void *)node)->next_sibling;
}

CHECK_UNUSED static const char *
test_name(
	void *context,
	const struct svg_node *node)
{
	const struct test_node *test;

	(void)context;
	test = (const struct test_node *)(const void *)node;
	if (test->name == NULL || test->foreign)
		return NULL;
	return test->name;
}

CHECK_UNUSED static const char *
test_attribute(
	void *context,
	const struct svg_node *node,
	const char *name)
{
	const struct test_node *test;
	int index;

	(void)context;
	test = (const struct test_node *)(const void *)node;
	for (index = 0; index < test->attribute_count; index++) {
		if (strcmp(test->keys[index], name) == 0)
			return test->values[index];
	}
	if (strcmp(name, "href") == 0) {
		for (index = 0; index < test->attribute_count; index++) {
			if (strcmp(test->keys[index], "xlink:href") == 0)
				return test->values[index];
		}
	}
	return NULL;
}

/* Finds an element by id in tree order below a node. */
CHECK_UNUSED static const struct test_node *
test_find_below(
	const struct test_node *node,
	const char *id)
{
	const struct test_node *child;
	const struct test_node *found;
	int index;

	for (index = 0; index < node->attribute_count; index++) {
		if (strcmp(node->keys[index], "id") == 0 && strcmp(node->values[index], id) == 0)
			return node;
	}
	for (child = node->first_child; child != NULL; child = child->next_sibling) {
		found = test_find_below(child, id);
		if (found != NULL)
			return found;
	}
	return NULL;
}

CHECK_UNUSED static const struct svg_node *
test_find_id(
	void *context,
	const char *id)
{
	const struct test_document *document;

	document = context;
	if (document->root == NULL)
		return NULL;
	return (const struct svg_node *)(const void *)test_find_below(document->root, id);
}

/* The value of one hexadecimal digit, or -1. */
CHECK_UNUSED static int
test_hex(
	char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

CHECK_UNUSED static int
test_parse_color(
	void *context,
	const char *text,
	size_t length,
	uint32_t *color)
{
	static const struct {
		const char *name;
		uint32_t value;
	} names[] = {
		{ "black", 0xff000000U }, { "white", 0xffffffffU }, { "red", 0xffff0000U }, { "lime", 0xff00ff00U },
		{ "green", 0xff008000U }, { "blue", 0xff0000ffU }, { "yellow", 0xffffff00U }, { "cyan", 0xff00ffffU },
		{ "magenta", 0xffff00ffU }, { "gray", 0xff808080U }, { "transparent", 0x00000000U }
	};
	int digits[8];
	size_t index;
	size_t count;

	(void)context;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		if (strlen(names[index].name) == length && strncmp(names[index].name, text, length) == 0) {
			*color = names[index].value;
			return 0;
		}
	}
	if (length < 4 || text[0] != '#')
		return EINVAL;
	count = length - 1;
	if (count != 3 && count != 4 && count != 6 && count != 8)
		return EINVAL;
	for (index = 0; index < count; index++) {
		digits[index] = test_hex(text[1 + index]);
		if (digits[index] < 0)
			return EINVAL;
	}
	if (count == 3 || count == 4) {
		*color = (uint32_t)(digits[0] * 17) << 16 | (uint32_t)(digits[1] * 17) << 8 | (uint32_t)(digits[2] * 17);
		*color |= (uint32_t)(count == 4 ? digits[3] * 17 : 255) << 24;
		return 0;
	}
	*color = (uint32_t)(digits[0] * 16 + digits[1]) << 16 | (uint32_t)(digits[2] * 16 + digits[3]) << 8 |
	    (uint32_t)(digits[4] * 16 + digits[5]);
	*color |= (uint32_t)(count == 8 ? digits[6] * 16 + digits[7] : 255) << 24;
	return 0;
}

/* Fills a struct svg_tree over a parsed test document. */
CHECK_UNUSED static void
test_tree_make(
	struct svg_tree *tree,
	struct test_document *document)
{
	tree->context = document;
	tree->first_child = test_first_child;
	tree->next_sibling = test_next_sibling;
	tree->name = test_name;
	tree->attribute = test_attribute;
	tree->find_id = test_find_id;
	tree->parse_color = test_parse_color;
}

#endif
