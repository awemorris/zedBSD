/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <time.h>

static int leap(long long y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static void civil(time_t t, long long *y, int *mo, int *d, int *h, int *mi, int *s)
{
	static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
	long long days = t / 86400, sec = t % 86400;
	int m = 0;
	if (sec < 0) { sec += 86400; --days; }
	*y = 1970;
	while (days >= 365 + leap(*y)) { days -= 365 + leap(*y); ++*y; }
	while (days < 0) { --*y; days += 365 + leap(*y); }
	while (days >= md[m] + (m == 1 && leap(*y))) {
		days -= md[m] + (m == 1 && leap(*y)); ++m;
	}
	*mo = m + 1; *d = (int)days + 1; *h = (int)(sec / 3600);
	*mi = (int)(sec / 60 % 60); *s = (int)(sec % 60);
}
int main(int argc, char **argv)
{
	struct timespec ts; long long y; int mo, d, h, mi, s, i;
	const char *fmt = "%Y-%m-%d %H:%M:%S UTC";
	if (argc > 2 || (argc == 2 && argv[1][0] != '+')) {
		fprintf(stderr, "usage: date [+format]\n"); return 2;
	}
	if (argc == 2) fmt = argv[1] + 1;
	if (clock_gettime(CLOCK_REALTIME, &ts)) { command_error("date", NULL); return 1; }
	civil(ts.tv_sec, &y, &mo, &d, &h, &mi, &s);
	for (i = 0; fmt[i]; ++i) {
		if (fmt[i] != '%' || !fmt[i + 1]) { putchar(fmt[i]); continue; }
		switch (fmt[++i]) {
		case '%': putchar('%'); break; case 'Y': printf("%04lld", y); break;
		case 'm': printf("%02d", mo); break; case 'd': printf("%02d", d); break;
		case 'H': printf("%02d", h); break; case 'M': printf("%02d", mi); break;
		case 'S': printf("%02d", s); break;
		case 's': printf("%lld", (long long)ts.tv_sec); break;
		default: putchar('%'); putchar(fmt[i]); break;
		}
	}
	putchar('\n'); return ferror(stdout);
}
