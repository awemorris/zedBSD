/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ED_EDITOR_H
#define ZEDBSD_ED_EDITOR_H

#include <stddef.h>

struct ed_buffer {
	char **line;
	size_t count;
	size_t capacity;
	size_t current;
};

void ed_buffer_init(struct ed_buffer *);
void ed_buffer_free(struct ed_buffer *);
int ed_buffer_copy(struct ed_buffer *, const struct ed_buffer *);
void ed_buffer_move(struct ed_buffer *, struct ed_buffer *);
int ed_buffer_insert(struct ed_buffer *, size_t, const char *);
int ed_buffer_delete(struct ed_buffer *, size_t, size_t);
int ed_buffer_replace(struct ed_buffer *, size_t, const char *);

#endif
