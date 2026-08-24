/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_SCCS_H
#define ZEDBSD_USERLAND_SCCS_H

#include <stddef.h>

struct sccs_delta {
	char *sid;
	char *timestamp;
	char *user;
	char *comment;
	char *text;
	size_t text_size;
	unsigned serial;
	unsigned predecessor;
};

struct sccs_history {
	struct sccs_delta *deltas;
	size_t count;
	char *description;
};

int sccs_load(const char *path, struct sccs_history *history);
int sccs_save(const char *path, const struct sccs_history *history);
void sccs_free(struct sccs_history *history);
const struct sccs_delta *sccs_find(const struct sccs_history *history,
				   const char *sid);
int sccs_add(struct sccs_history *history, const char *sid, const char *user,
	     const char *comment, const void *text, size_t size,
	     unsigned predecessor);
int sccs_remove(struct sccs_history *history, const char *sid);
int sccs_sid_valid(const char *sid);
int sccs_sid_next(const char *sid, int branch, char *output, size_t size);
char *sccs_gfile_name(const char *sfile);
char *sccs_aux_name(const char *sfile, char prefix);
int sccs_read_regular(const char *path, char **data, size_t *size);

#endif
