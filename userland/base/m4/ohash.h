/* $OpenBSD: ohash.h,v 1.2 2014/06/02 18:52:03 deraadt Exp $ */
/* Copyright (c) 1999, 2004 Marc Espie <espie@openbsd.org>
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES.
 */
#ifndef OHASH_H
#define OHASH_H

#include <stddef.h>
#include <stdint.h>

struct ohash_info {
	ptrdiff_t key_offset;
	void *data;
	void *(*calloc)(size_t, size_t, void *);
	void (*free)(void *, void *);
	void *(*alloc)(size_t, void *);
};

struct _ohash_record;
struct ohash {
	struct _ohash_record *t;
	struct ohash_info info;
	unsigned int size;
	unsigned int total;
	unsigned int deleted;
};

void ohash_init(struct ohash *, unsigned int, struct ohash_info *);
void ohash_delete(struct ohash *);
unsigned int ohash_lookup_interval(struct ohash *, const char *, const char *,
				   uint32_t);
unsigned int ohash_lookup_memory(struct ohash *, const char *, size_t,
				 uint32_t);
void *ohash_find(struct ohash *, unsigned int);
void *ohash_remove(struct ohash *, unsigned int);
void *ohash_insert(struct ohash *, unsigned int, void *);
void *ohash_first(struct ohash *, unsigned int *);
void *ohash_next(struct ohash *, unsigned int *);
unsigned int ohash_entries(struct ohash *);
void *ohash_create_entry(struct ohash_info *, const char *, const char **);
uint32_t ohash_interval(const char *, const char **);
unsigned int ohash_qlookupi(struct ohash *, const char *, const char **);
unsigned int ohash_qlookup(struct ohash *, const char *);

#endif
