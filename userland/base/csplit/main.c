/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int openpart(unsigned n,char*name){snprintf(name,16,"xx%02u",n);return open(name,O_WRONLY|O_CREAT|O_TRUNC,0666);}int main(int argc,char**argv){FILE*f;char*l=NULL,name[16];size_t cap=0;long n;unsigned long long target,line=1,part=0,count=0;int out;if(argc!=3||command_parse_ull(argv[2],&target)||target<1){fprintf(stderr,"usage: csplit file line-number\n");return 2;}f=!strcmp(argv[1],"-")?stdin:fopen(argv[1],"r");if(!f){command_error("csplit",argv[1]);return 1;}out=openpart(part++,name);if(out<0)return 1;while((n=command_read_line(f,&l,&cap))>0){if(line==target){printf("%llu\n",count);close(out);out=openpart(part++,name);count=0;if(out<0)return 1;}if(command_write_all(out,l,(size_t)n))return 1;count+=(unsigned long long)n;line++;}printf("%llu\n",count);close(out);free(l);if(f!=stdin)fclose(f);return n<0;}
