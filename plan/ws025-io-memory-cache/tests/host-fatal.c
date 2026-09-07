/* Terminates a host fixture if the production HAL invariant fails. */
#include <stdio.h>
#include <stdlib.h>

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "%s:%d: %s\n", file, line, message);
	abort();
}
