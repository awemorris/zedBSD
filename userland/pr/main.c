/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){const char*header=NULL;int number=0,i=1;for(;i<argc;i++){if(!strcmp(argv[i],"-h")&&++i<argc)header=argv[i];else if(!strcmp(argv[i],"-n"))number=1;else break;}do{FILE*f=i==argc||!strcmp(argv[i],"-")?stdin:fopen(argv[i],"r");char*l=NULL;size_t cap=0;long n;unsigned long line=1;if(!f){command_error("pr",argv[i]);return 1;}if(header)printf("\n\n%s\n\n",header);while((n=command_read_line(f,&l,&cap))>0){if(number)printf("%5lu ",line++);fwrite(l,1,(size_t)n,stdout);}free(l);if(f!=stdin)fclose(f);++i;}while(i<argc);return ferror(stdout);}
