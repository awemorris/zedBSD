/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){unsigned long long width=75,col=0;int i=1,c,inword=0;if(i<argc&&!strcmp(argv[i],"-w")&&++i<argc){if(command_parse_ull(argv[i++],&width)||!width)return 2;}do{FILE*f=i==argc||!strcmp(argv[i],"-")?stdin:fopen(argv[i],"r");if(!f){command_error("fmt",argv[i]);return 1;}while((c=fgetc(f))!=EOF){if(c=='\n'){int d=fgetc(f);if(d=='\n'){putchar('\n');putchar('\n');col=0;inword=0;}else{if(d!=EOF)ungetc(d,f);if(inword){putchar(' ');col++;inword=0;}}continue;}if(isspace((unsigned char)c)){if(inword){putchar(' ');col++;inword=0;}continue;}if(col>=width&&inword==0){putchar('\n');col=0;}putchar(c);col++;inword=1;}if(f!=stdin)fclose(f);++i;}while(i<argc);if(col)putchar('\n');return ferror(stdout);}
