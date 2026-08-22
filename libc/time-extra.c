/* ISO C time additions. SPDX-License-Identifier: Zlib */
#include <time.h>
int timespec_get(struct timespec *time, int base)
{ if (base != TIME_UTC) return 0; return clock_gettime(CLOCK_REALTIME, time) == 0 ? base : 0; }
