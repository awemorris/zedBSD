/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){unsigned long long w=80,col=0;int bytes=0,i=1,c;for(;i<argc;++i){if(!strcmp(argv[i],"-b"))bytes=1;else if(!strcmp(argv[i],"-w")&&++i<argc){if(command_parse_ull(argv[i],&w)||!w){fprintf(stderr,"fold: invalid width\n");return 2;}}else break;}do{FILE*f=i==argc||!strcmp(argv[i],"-")?stdin:fopen(argv[i],"r");if(!f){command_error("fold",argv[i]);return 1;}while((c=fgetc(f))!=EOF){unsigned long long next=col;if(c=='\n'||c=='\r')next=0;else if(c=='\b'&&!bytes)next=col?col-1:0;else if(c=='\t'&&!bytes)next=(col+8)&~7ULL;else next=col+1;if(c!='\n'&&next>w){putchar('\n');col=0;next=(c=='\t'&&!bytes)?8:1;}putchar(c);col=next;}if(f!=stdin)fclose(f);++i;}while(i<argc);return ferror(stdout);}
