/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/terminfo.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int
main(void)
{
	long parameters[9] = {4, 9};
	char output[64];

	if (terminfo_expand("%i%p1%2d,%p2%3d", parameters, output,
			    sizeof(output)) < 0 ||
	    strcmp(output, "05,010") != 0 ||
	    terminfo_expand("%p1%{2}%*%d", parameters, output, sizeof(output)) <
		0 ||
	    strcmp(output, "8") != 0 ||
	    terminfo_expand("%d", parameters, output, sizeof(output)) >= 0 ||
	    terminfo_expand("%p1%d", parameters, output, 1) >= 0)
		return 1;
	if (terminfo_expand("%?%p1%{4}%>%tlarge%e%?%p1%{4}%=%tequal%eless%;%;",
			    parameters, output, sizeof(output)) < 0 ||
	    strcmp(output, "equal") != 0)
		return 1;
	parameters[0] = LONG_MAX;
	errno = 0;
	if (terminfo_expand("%p1%{1}%+%d", parameters, output,
			    sizeof(output)) >= 0 ||
	    errno != EOVERFLOW)
		return 1;
	return 0;
}
