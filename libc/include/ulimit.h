/* XSI process limits. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ULIMIT_H
#define ZEDBSD_ULIMIT_H
#define UL_GETFSIZE 1
#define UL_SETFSIZE 2
#define UL_GETMAXBRK 3
#define UL_GETOPENMAX 4
long ulimit(int, ...);
#endif
