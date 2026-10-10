/* zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef MEDIASTORAGE_JSON_H
#define MEDIASTORAGE_JSON_H
#include <stdint.h>
#include <stdio.h>

/* A private JSON tree preserves unknown metadata fields through ordinary library updates. */
#define MJ_OBJECT 1
#define MJ_ARRAY 2
#define MJ_STRING 3
#define MJ_NUMBER 4
#define MJ_LITERAL 5
struct mj_value {
	int type;
	char *key;
	char *text;
	struct mj_value *child;
	struct mj_value *tail;
	struct mj_value *next;
};
struct mj_value *mj_new(int type);
void mj_free(struct mj_value *value);
int mj_read(FILE *input, struct mj_value **value);
int mj_write(FILE *output, const struct mj_value *value);
struct mj_value *mj_get(struct mj_value *object, const char *key);
int mj_set(struct mj_value *object, const char *key, int type, const char *text);
int mj_number(struct mj_value *object, const char *key, int64_t number);
int mj_attach(struct mj_value *object, const char *key, struct mj_value *value);
void mj_append(struct mj_value *array, struct mj_value *value);
#endif
