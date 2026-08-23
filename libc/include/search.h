/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SEARCH_H
#define ZEDBSD_SEARCH_H

#include <stddef.h>

typedef struct entry {
	char *key;
	void *data;
} ENTRY;

typedef enum { FIND, ENTER } ACTION;
typedef enum { preorder, postorder, endorder, leaf } VISIT;

int hcreate(size_t);
void hdestroy(void);
ENTRY *hsearch(ENTRY, ACTION);
void insque(void *, void *);
void *lfind(const void *, const void *, size_t *, size_t,
    int (*)(const void *, const void *));
void *lsearch(const void *, void *, size_t *, size_t,
    int (*)(const void *, const void *));
void remque(void *);
void *tdelete(const void *, void **, int (*)(const void *, const void *));
void *tfind(const void *, void *const *, int (*)(const void *, const void *));
void *tsearch(const void *, void **, int (*)(const void *, const void *));
void twalk(const void *, void (*)(const void *, VISIT, int));

#endif
