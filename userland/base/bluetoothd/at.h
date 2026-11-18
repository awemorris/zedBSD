/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AT lines of HFP's service level connection (ws197-p006, plan/ws197/
 * phase006/phase.md): the bytes the phone's audio gateway sends cut into
 * lines at CR and LF (empty lines skipped, a line longer than
 * BTD_AT_LINE_MAX bytes dropped whole), and the parts of a result read:
 * its name, its comma-separated fields (a quoted field unquoted), its
 * numbers, and the indicators' names of +CIND=?.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_AT_H
#define BLUETOOTHD_AT_H

#include <stddef.h>
#include <stdint.h>

/* The longest line kept, without its end. */
#define BTD_AT_LINE_MAX		255U

/* The longest indicator's name kept, with its NUL, and the most indicators. */
#define BTD_AT_NAME_MAX		16U
#define BTD_AT_INDICATORS_MAX	20U

/*
 * The lines being cut from the bytes: the line so far, its length, and
 * whether it passed BTD_AT_LINE_MAX (then it is dropped at its end).
 */
struct btd_at_input {
	char line[BTD_AT_LINE_MAX + 1U];
	size_t used;
	int overlong;
	unsigned dropped;
};

/* What a whole line is given to. */
typedef void (*btd_at_line_fn)(void *context, const char *line);

void btd_at_input_init(struct btd_at_input *input);
void btd_at_input(struct btd_at_input *input, const uint8_t *data, size_t length, btd_at_line_fn line, void *context);
int btd_at_result(const char *line, const char *name, const char **rest);
int btd_at_field(const char *text, unsigned index, char *out, size_t size);
int btd_at_number(const char *text, unsigned index, long *value);
int btd_at_indicators(const char *text, char names[][BTD_AT_NAME_MAX], size_t capacity, size_t *count);

#endif
