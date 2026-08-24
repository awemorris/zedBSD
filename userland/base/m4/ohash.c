/* $OpenBSD: ohash.c,v 1.1 2014/06/02 18:52:03 deraadt Exp $ */
/* Copyright (c) 1999, 2004 Marc Espie <espie@openbsd.org>
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES.
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ohash.h"

struct _ohash_record {
	uint32_t hv;
	const char *p;
};

#define DELETED ((const char *)h)
#define NONE (h->size)
#define MINSIZE (1UL << 4)
#define MINDELETED 4

static void ohash_resize(struct ohash *);

void *
ohash_create_entry(struct ohash_info *i, const char *start, const char **end)
{
	char *p;

	if (!*end)
		*end = start + strlen(start);
	p = (i->alloc)(i->key_offset + (size_t)(*end - start) + 1, i->data);
	if (p) {
		memcpy(p + i->key_offset, start, (size_t)(*end - start));
		p[i->key_offset + (*end - start)] = '\0';
	}
	return p;
}

void
ohash_delete(struct ohash *h)
{
	(h->info.free)(h->t, h->info.data);
}

static void
ohash_resize(struct ohash *h)
{
	struct _ohash_record *n;
	size_t ns;
	unsigned int i, incr, j;

	if (4 * h->deleted < h->total)
		ns = h->size >= (UINT_MAX >> 1U) ? UINT_MAX : h->size << 1U;
	else if (3 * h->deleted > 2 * h->total)
		ns = h->size >> 1U;
	else
		ns = h->size;
	if (ns < MINSIZE)
		ns = MINSIZE;
	n = (h->info.calloc)(ns, sizeof(*n), h->info.data);
	if (!n)
		return;
	for (j = 0; j < h->size; j++) {
		if (h->t[j].p == NULL || h->t[j].p == DELETED)
			continue;
		i = h->t[j].hv % ns;
		incr = ((h->t[j].hv % (ns - 2)) & ~1U) + 1;
		while (n[i].p != NULL) {
			i += incr;
			if (i >= ns)
				i -= ns;
		}
		n[i] = h->t[j];
	}
	(h->info.free)(h->t, h->info.data);
	h->t = n;
	h->size = (unsigned int)ns;
	h->total -= h->deleted;
	h->deleted = 0;
}

void *
ohash_remove(struct ohash *h, unsigned int i)
{
	void *result = (void *)h->t[i].p;
	if (result == NULL || result == DELETED)
		return NULL;
	h->t[i].p = DELETED;
	h->deleted++;
	if (h->deleted >= MINDELETED && 4 * h->deleted > h->total)
		ohash_resize(h);
	return result;
}

void *
ohash_find(struct ohash *h, unsigned int i)
{
	return h->t[i].p == DELETED ? NULL : (void *)h->t[i].p;
}

void *
ohash_insert(struct ohash *h, unsigned int i, void *p)
{
	if (h->t[i].p == DELETED) {
		h->deleted--;
		h->t[i].p = p;
	} else {
		h->t[i].p = p;
		if (++h->total * 4 > h->size * 3)
			ohash_resize(h);
	}
	return p;
}

unsigned int
ohash_entries(struct ohash *h)
{
	return h->total - h->deleted;
}

void *
ohash_first(struct ohash *h, unsigned int *pos)
{
	*pos = 0;
	return ohash_next(h, pos);
}

void *
ohash_next(struct ohash *h, unsigned int *pos)
{
	for (; *pos < h->size; (*pos)++)
		if (h->t[*pos].p != DELETED && h->t[*pos].p != NULL)
			return (void *)h->t[(*pos)++].p;
	return NULL;
}

void
ohash_init(struct ohash *h, unsigned int size, struct ohash_info *info)
{
	h->size = 1UL << size;
	if (h->size < MINSIZE)
		h->size = MINSIZE;
	h->info = *info;
	h->t = (h->info.calloc)(h->size, sizeof(*h->t), h->info.data);
	h->total = h->deleted = 0;
}

uint32_t
ohash_interval(const char *s, const char **e)
{
	uint32_t k;
	if (!*e)
		*e = s + strlen(s);
	if (s == *e)
		k = 0;
	else
		k = (unsigned char)*s++;
	while (s != *e)
		k = ((k << 2) | (k >> 30)) ^ (unsigned char)*s++;
	return k;
}

unsigned int
ohash_lookup_interval(struct ohash *h, const char *start, const char *end,
		      uint32_t hv)
{
	unsigned int empty = NONE, i = hv % h->size;
	unsigned int incr = ((hv % (h->size - 2)) & ~1U) + 1;

	while (h->t[i].p != NULL) {
		if (h->t[i].p == DELETED) {
			if (empty == NONE)
				empty = i;
		} else if (h->t[i].hv == hv &&
			   strncmp(h->t[i].p + h->info.key_offset, start,
				   (size_t)(end - start)) == 0 &&
			   (h->t[i].p + h->info.key_offset)[end - start] ==
			       '\0') {
			if (empty != NONE) {
				h->t[empty] = h->t[i];
				h->t[i].p = DELETED;
				return empty;
			}
			return i;
		}
		i += incr;
		if (i >= h->size)
			i -= h->size;
	}
	if (empty != NONE)
		i = empty;
	h->t[i].hv = hv;
	return i;
}

unsigned int
ohash_lookup_memory(struct ohash *h, const char *k, size_t size, uint32_t hv)
{
	unsigned int empty = NONE, i = hv % h->size;
	unsigned int incr = ((hv % (h->size - 2)) & ~1U) + 1;
	while (h->t[i].p != NULL) {
		if (h->t[i].p == DELETED) {
			if (empty == NONE)
				empty = i;
		} else if (h->t[i].hv == hv &&
			   memcmp(h->t[i].p + h->info.key_offset, k, size) ==
			       0) {
			if (empty != NONE) {
				h->t[empty] = h->t[i];
				h->t[i].p = DELETED;
				return empty;
			}
			return i;
		}
		i += incr;
		if (i >= h->size)
			i -= h->size;
	}
	if (empty != NONE)
		i = empty;
	h->t[i].hv = hv;
	return i;
}

unsigned int
ohash_qlookup(struct ohash *h, const char *s)
{
	const char *e = NULL;
	return ohash_qlookupi(h, s, &e);
}

unsigned int
ohash_qlookupi(struct ohash *h, const char *s, const char **e)
{
	uint32_t hv = ohash_interval(s, e);
	return ohash_lookup_interval(h, s, *e, hv);
}
