/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes a parsed command back as text, for the jobs listing: the words as
 * they were written, the operators between them, and compound commands on
 * one line.
 */

#include "userland/base/sh/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A string that grows as text is appended to it.
 *
 * The text is always terminated; its storage is handed to the caller of
 * sh_node_text, who frees it.
 */
struct text_buffer {
	char *text;
	size_t length;
	size_t capacity;
};

/* The operators of the redirection kinds, indexed by SH_REDIR_*. */
static const char *const redirection_operators[] = {
	"", "<", ">", ">|", ">>", "<>", "<&", ">&", "<<", "<<<"
};

static void show_node(struct text_buffer *buffer, struct sh_node *node);
static void show_simple(struct text_buffer *buffer, struct sh_node *node);
static void show_compound(struct text_buffer *buffer, struct sh_node *node);
static void show_case(struct text_buffer *buffer, struct sh_node *node);
static void show_list(struct text_buffer *buffer, struct sh_node *node);
static void show_redirections(struct text_buffer *buffer, struct sh_redirection *redirection);
static void show_word(struct text_buffer *buffer, const struct sh_token *word);
static void append(struct text_buffer *buffer, const char *text);

/*
 * Makes the text of a command, which the caller frees.
 */
char *
sh_node_text(
	struct sh_node *node)
{
	struct text_buffer buffer;

	/* Starts with an empty, terminated buffer. */
	memset(&buffer, 0, sizeof(buffer));
	append(&buffer, "");

	/* Writes the node into it. */
	show_node(&buffer, node);

	/* Succeeded: the text. */
	return buffer.text;
}

/* Writes one node and the redirections written after it. */
static void
show_node(
	struct text_buffer *buffer,
	struct sh_node *node)
{
	size_t index;

	/* Nothing to write. */
	if (node == NULL)
		return;

	/* Dispatches on the kind of node. */
	switch (node->kind) {
	case SH_NODE_SIMPLE:
		show_simple(buffer, node);
		break;
	case SH_NODE_PIPELINE:
		for (index = 0; index < node->u.pipeline.count; index++) {
			if (index > 0)
				append(buffer, " | ");
			show_node(buffer, node->u.pipeline.commands[index]);
		}

		break;
	case SH_NODE_AND:
		show_node(buffer, node->u.binary.left);
		append(buffer, " && ");
		show_node(buffer, node->u.binary.right);
		break;
	case SH_NODE_OR:
		show_node(buffer, node->u.binary.left);
		append(buffer, " || ");
		show_node(buffer, node->u.binary.right);
		break;
	case SH_NODE_SEQUENCE:
		show_list(buffer, node);
		break;
	case SH_NODE_BACKGROUND:
		show_node(buffer, node->u.body);
		append(buffer, " &");
		break;
	case SH_NODE_NOT:
		append(buffer, "! ");
		show_node(buffer, node->u.body);
		break;
	case SH_NODE_FUNCTION:
		append(buffer, node->u.function.name);
		append(buffer, "() ");
		show_node(buffer, node->u.function.body);
		break;
	default:
		show_compound(buffer, node);
		break;
	}

	/* The redirections written after it. */
	show_redirections(buffer, node->redirections);
}

/* Writes a simple command: its assignments, then its words. */
static void
show_simple(
	struct text_buffer *buffer,
	struct sh_node *node)
{
	size_t index;

	/* The assignments, separated by spaces. */
	for (index = 0; index < node->u.simple.assignment_count; index++) {
		if (index > 0)
			append(buffer, " ");
		show_word(buffer, node->u.simple.assignments[index]);
	}

	/* The words, after a space when anything came before. */
	for (index = 0; index < node->u.simple.word_count; index++) {
		if (index > 0 || node->u.simple.assignment_count > 0)
			append(buffer, " ");
		show_word(buffer, node->u.simple.words[index]);
	}
}

/* Writes a compound command on one line. */
static void
show_compound(
	struct text_buffer *buffer,
	struct sh_node *node)
{
	size_t index;

	/* Dispatches on the kind of compound command. */
	switch (node->kind) {
	case SH_NODE_IF:
		append(buffer, "if ");
		show_list(buffer, node->u.branch.condition);
		append(buffer, "; then ");
		show_list(buffer, node->u.branch.then_part);
		if (node->u.branch.else_part != NULL) {
			append(buffer, "; else ");
			show_list(buffer, node->u.branch.else_part);
		}

		/* The end of the if. */
		append(buffer, "; fi");
		break;
	case SH_NODE_WHILE:
	case SH_NODE_UNTIL:
		if (node->kind == SH_NODE_WHILE)
			append(buffer, "while ");
		else
			append(buffer, "until ");
		show_list(buffer, node->u.loop.condition);
		append(buffer, "; do ");
		show_list(buffer, node->u.loop.body);
		append(buffer, "; done");
		break;
	case SH_NODE_FOR:
		append(buffer, "for ");
		show_word(buffer, node->u.iterate.name);
		if (node->u.iterate.has_in) {
			append(buffer, " in");
			for (index = 0; index < node->u.iterate.word_count;
			     index++) {
				append(buffer, " ");
				show_word(buffer, node->u.iterate.words[index]);
			}
		}

		/* The body. */
		append(buffer, "; do ");
		show_list(buffer, node->u.iterate.body);
		append(buffer, "; done");
		break;
	case SH_NODE_CASE:
		show_case(buffer, node);
		break;
	case SH_NODE_GROUP:
		append(buffer, "{ ");
		show_list(buffer, node->u.body);
		append(buffer, "; }");
		break;
	case SH_NODE_COND:
		append(buffer, "[[ ... ]]");
		break;
	case SH_NODE_ARITH:
		append(buffer, "((");
		append(buffer, node->u.arith);
		append(buffer, "))");
		break;
	case SH_NODE_ARITH_FOR:
		append(buffer, "for ((");
		append(buffer, node->u.arith_for.init);
		append(buffer, "; ");
		append(buffer, node->u.arith_for.test);
		append(buffer, "; ");
		append(buffer, node->u.arith_for.step);
		append(buffer, ")); do ");
		show_list(buffer, node->u.arith_for.body);
		append(buffer, "; done");
		break;
	case SH_NODE_SUBSHELL:
		append(buffer, "(");
		show_list(buffer, node->u.body);
		append(buffer, ")");
		break;
	default:
		break;
	}
}

/* Writes a case command: its word, then each arm's patterns and list. */
static void
show_case(
	struct text_buffer *buffer,
	struct sh_node *node)
{
	struct sh_case_item *item;
	size_t index;

	/* The word. */
	append(buffer, "case ");
	show_word(buffer, node->u.select.word);
	append(buffer, " in");

	/* Each arm: its patterns joined by |, then its list. */
	for (item = node->u.select.items; item != NULL; item = item->next) {
		append(buffer, " ");
		for (index = 0; index < item->pattern_count; index++) {
			if (index > 0)
				append(buffer, "|");
			show_word(buffer, item->patterns[index]);
		}

		/* The body of the item. */
		append(buffer, ") ");
		show_list(buffer, item->body);
		append(buffer, ";;");
	}

	/* The end of the case. */
	append(buffer, " esac");
}

/* Writes a list, its commands separated by ; (or a space after &). */
static void
show_list(
	struct text_buffer *buffer,
	struct sh_node *node)
{
	/* Nothing to write. */
	if (node == NULL)
		return;

	/* A command that is not a sequence is written as it is. */
	if (node->kind != SH_NODE_SEQUENCE) {
		show_node(buffer, node);
		return;
	}

	/* A sequence is its left, then its right after a separator. */
	show_list(buffer, node->u.binary.left);
	if (node->u.binary.right == NULL)
		return;
	if (node->u.binary.left != NULL &&
	    node->u.binary.left->kind == SH_NODE_BACKGROUND)
		append(buffer, " ");
	else
		append(buffer, "; ");
	show_list(buffer, node->u.binary.right);
}

/* Writes redirections as descriptor, operator and word. */
static void
show_redirections(
	struct text_buffer *buffer,
	struct sh_redirection *redirection)
{
	char number[16];

	/* Each one; a here-document's body is not written. */
	for (;
	     redirection != NULL;
	     redirection = redirection->next) {
		snprintf(number, sizeof(number), " %d", redirection->descriptor);
		append(buffer, number);
		append(buffer, redirection_operators[redirection->op]);
		if (redirection->op == SH_REDIR_HEREDOC)
			append(buffer, "...");
		else
			show_word(buffer, redirection->word);
	}
}

/* Writes a word as it was written. */
static void
show_word(
	struct text_buffer *buffer,
	const struct sh_token *word)
{
	/* The raw text is the word as written. */
	if (word == NULL || word->raw == NULL)
		return;
	append(buffer, word->raw);
}

/* Appends text to a buffer. */
static void
append(
	struct text_buffer *buffer,
	const char *text)
{
	size_t length;
	size_t capacity;

	/* Grows the buffer in powers of two, keeping room for the terminator. */
	length = strlen(text);
	if (buffer->length + length + 1U > buffer->capacity) {
		capacity = buffer->capacity;
		if (capacity == 0)
			capacity = 64U;
		while (buffer->length + length + 1U > capacity)
			capacity *= 2U;
		buffer->text = sh_realloc(buffer->text, capacity);
		buffer->capacity = capacity;
	}

	/* Copies the text with its terminator. */
	memcpy(buffer->text + buffer->length, text, length + 1U);
	buffer->length += length;
}
