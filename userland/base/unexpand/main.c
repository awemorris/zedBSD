/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
int main(int argc,char**argv){unsigned long long tab=8,col=0,spaces=0;int all=0,i=1,c;if(i<argc&&!strcmp(argv[i],"-a")){all=1;i++;}if(i<argc&&!strcmp(argv[i],"-t")&&++i<argc){if(command_parse_ull(argv[i++],&tab)||!tab)return 2;}do{FILE*f=i==argc||!strcmp(argv[i],"-")?stdin:fopen(argv[i],"r");if(!f){command_error("unexpand",argv[i]);return 1;}while((c=fgetc(f))!=EOF){if(c==' '&&(all||col==spaces)){spaces++;if((col+spaces)%tab==0){putchar('\t');col+=spaces;spaces=0;}continue;}while(spaces--){putchar(' ');col++;}spaces=0;putchar(c);if(c=='\n'||c=='\r')col=0;else if(c=='\t')col+=tab-col%tab;else col++;}while(spaces--)putchar(' ');spaces=0;if(f!=stdin)fclose(f);++i;}while(i<argc);return ferror(stdout);}
