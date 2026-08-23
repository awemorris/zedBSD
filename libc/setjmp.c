/* Compiler-assisted, architecture-neutral nonlocal jumps. SPDX-License-Identifier: Zlib */
#include <setjmp.h>

void
longjmp(jmp_buf environment, int value)
{
	environment[0].result = value == 0 ? 1 : value;
	__builtin_longjmp(environment[0].context, 1);
}

void
_longjmp(jmp_buf environment, int value)
{
	longjmp(environment, value);
}
