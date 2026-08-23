/* XSI message formatting interface. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_FMTMSG_H
#define ZEDBSD_FMTMSG_H
#define MM_HARD  0x0001L
#define MM_SOFT  0x0002L
#define MM_FIRM  0x0004L
#define MM_APPL  0x0010L
#define MM_UTIL  0x0020L
#define MM_OPSYS 0x0040L
#define MM_RECOVER 0x0100L
#define MM_NRECOV  0x0200L
#define MM_PRINT   0x1000L
#define MM_CONSOLE 0x2000L
#define MM_NULLLBL ((char *)0)
#define MM_NULLSEV 0
#define MM_NULLMC  0L
#define MM_NULLTXT ((char *)0)
#define MM_NULLACT ((char *)0)
#define MM_NULLTAG ((char *)0)
#define MM_NOSEV 0
#define MM_HALT  1
#define MM_ERROR 2
#define MM_WARNING 3
#define MM_INFO 4
#define MM_OK 0
#define MM_NOTOK (-1)
#define MM_NOMSG 1
#define MM_NOCON 2
int fmtmsg(long, const char *, int, const char *, const char *, const char *);
#endif
