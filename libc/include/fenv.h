/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_FENV_H
#define ZEDBSD_FENV_H

typedef unsigned int fexcept_t;
typedef struct {
	unsigned int exceptions;
	int rounding;
} fenv_t;

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x02
#define FE_OVERFLOW   0x04
#define FE_UNDERFLOW  0x08
#define FE_INEXACT    0x10
#define FE_ALL_EXCEPT 0x1f

/* zedBSD's software floating-point ABI currently supports round-to-nearest. */
#define FE_TONEAREST 0
#define FE_DFL_ENV ((const fenv_t *)-1)

int feclearexcept(int);
int fegetenv(fenv_t *);
int fegetexceptflag(fexcept_t *, int);
int fegetround(void);
int feholdexcept(fenv_t *);
int feraiseexcept(int);
int fesetenv(const fenv_t *);
int fesetexceptflag(const fexcept_t *, int);
int fesetround(int);
int fetestexcept(int);
int feupdateenv(const fenv_t *);

#endif
