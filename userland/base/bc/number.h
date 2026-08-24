/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BC_NUMBER_H
#define ZEDBSD_BC_NUMBER_H

#include <stddef.h>
#include <stdint.h>

struct bc_number {
	uint32_t *digit;
	size_t length;
	size_t capacity;
	int sign;
};

void bc_number_init(struct bc_number *);
void bc_number_free(struct bc_number *);
int bc_number_from_decimal(struct bc_number *, const char *, size_t);
int bc_number_copy(struct bc_number *, const struct bc_number *);
int bc_number_add(struct bc_number *, const struct bc_number *,
		  const struct bc_number *);
int bc_number_subtract(struct bc_number *, const struct bc_number *,
		       const struct bc_number *);
int bc_number_multiply(struct bc_number *, const struct bc_number *,
		       const struct bc_number *);
int bc_number_divide(struct bc_number *, struct bc_number *,
		     const struct bc_number *, const struct bc_number *);
int bc_number_power(struct bc_number *, const struct bc_number *,
		    const struct bc_number *);
int bc_number_negate(struct bc_number *, const struct bc_number *);
int bc_number_is_zero(const struct bc_number *);
int bc_number_to_ull(const struct bc_number *, unsigned long long *);
char *bc_number_to_string(const struct bc_number *);

#endif
