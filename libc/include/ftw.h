/* SUSv4 file tree walking interface. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_FTW_H
#define ZEDBSD_FTW_H
#include <zedbsd/features.h>
#include <sys/stat.h>

#define FTW_F 0
#define FTW_D 1
#define FTW_DNR 2
#define FTW_NS 3
#define FTW_SL 4
#define FTW_DP 5
#define FTW_SLN 6

#define FTW_PHYS  0x01
#define FTW_MOUNT 0x02
#define FTW_DEPTH 0x04
#define FTW_CHDIR 0x08

struct FTW { int base; int level; };
#if __ZEDBSD_LEGACY_VISIBLE
int ftw(const char *, int (*)(const char *, const struct stat *, int), int);
#endif
int nftw(const char *, int (*)(const char *, const struct stat *, int,
    struct FTW *), int, int);
#endif
