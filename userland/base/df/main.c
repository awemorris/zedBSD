/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
static unsigned long long units(unsigned long long blocks, unsigned long long fragment, unsigned long long unit) { if (!fragment) return 0; return blocks * (fragment / unit) + blocks * (fragment % unit) / unit; }
int main(int argc,char **argv){unsigned long long unit=512;int i=1,failed=0;if(i<argc&&!strcmp(argv[i],"-k")){unit=1024;i++;}printf("Filesystem %llu-blocks Used Available Capacity Mounted on\n",unit);if(i==argc){argv[argc++]="/";i=argc-1;}for(;i<argc;i++){struct statvfs s;unsigned long long total,free,available,used,percent;if(statvfs(argv[i],&s)){command_error("df",argv[i]);failed=1;continue;}total=units(s.f_blocks,s.f_frsize,unit);free=units(s.f_bfree,s.f_frsize,unit);available=units(s.f_bavail,s.f_frsize,unit);used=total-free;percent=used+available?(used*100+(used+available)-1)/(used+available):0;printf("%-10s %10llu %10llu %10llu %3llu%% %s\n",argv[i],total,used,available,percent,argv[i]);}return failed;}
