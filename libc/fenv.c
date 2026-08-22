/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <fenv.h>

/* Floating-point is implemented in software, so the environment is kept per
 * execution context by libc instead of being read from a hardware control
 * register.  The weak process-wide cell is replaced by a TLS implementation
 * in userland when threading is active. */
static fenv_t default_environment = { 0U, FE_TONEAREST };

__attribute__((weak)) fenv_t *
__libc_fenv_location(void)
{
	return &default_environment;
}

int
feclearexcept(int exceptions)
{
	if ((exceptions & ~FE_ALL_EXCEPT) != 0)
		return -1;
	__libc_fenv_location()->exceptions &= ~(unsigned int)exceptions;
	return 0;
}

int
fegetenv(fenv_t *environment)
{
	if (environment == 0)
		return -1;
	*environment = *__libc_fenv_location();
	return 0;
}

int
fegetexceptflag(fexcept_t *flag, int exceptions)
{
	if (flag == 0 || (exceptions & ~FE_ALL_EXCEPT) != 0)
		return -1;
	*flag = __libc_fenv_location()->exceptions & (unsigned int)exceptions;
	return 0;
}

int
fegetround(void)
{
	return __libc_fenv_location()->rounding;
}

int
feholdexcept(fenv_t *environment)
{
	if (fegetenv(environment) != 0)
		return -1;
	return feclearexcept(FE_ALL_EXCEPT);
}

int
feraiseexcept(int exceptions)
{
	if ((exceptions & ~FE_ALL_EXCEPT) != 0)
		return -1;
	__libc_fenv_location()->exceptions |= (unsigned int)exceptions;
	return 0;
}

int
fesetenv(const fenv_t *environment)
{
	if (environment == FE_DFL_ENV) {
		*__libc_fenv_location() = (fenv_t){ 0U, FE_TONEAREST };
		return 0;
	}
	if (environment == 0 || environment->rounding != FE_TONEAREST ||
	    (environment->exceptions & ~FE_ALL_EXCEPT) != 0)
		return -1;
	*__libc_fenv_location() = *environment;
	return 0;
}

int
fesetexceptflag(const fexcept_t *flag, int exceptions)
{
	fenv_t *environment;

	if (flag == 0 || (exceptions & ~FE_ALL_EXCEPT) != 0)
		return -1;
	environment = __libc_fenv_location();
	environment->exceptions = (environment->exceptions &
	    ~(unsigned int)exceptions) | (*flag & (unsigned int)exceptions);
	return 0;
}

int
fesetround(int rounding)
{
	if (rounding != FE_TONEAREST)
		return -1;
	__libc_fenv_location()->rounding = rounding;
	return 0;
}

int
fetestexcept(int exceptions)
{
	if ((exceptions & ~FE_ALL_EXCEPT) != 0)
		return 0;
	return (int)(__libc_fenv_location()->exceptions &
	    (unsigned int)exceptions);
}

int
feupdateenv(const fenv_t *environment)
{
	unsigned int exceptions = __libc_fenv_location()->exceptions;

	if (fesetenv(environment) != 0)
		return -1;
	return feraiseexcept((int)exceptions);
}
