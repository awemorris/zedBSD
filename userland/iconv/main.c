/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int enc(const char*s){return!strcmp(s,"UTF-8")||!strcmp(s,"utf-8")||!strcmp(s,"UTF8")||!strcmp(s,"utf8");}static int valid(const unsigned char*b,size_t n){size_t i=0;while(i<n){unsigned c=b[i++],need;if(c<128)continue;if(c>=0xc2&&c<=0xdf)need=1;else if(c>=0xe0&&c<=0xef)need=2;else if(c>=0xf0&&c<=0xf4)need=3;else return 0;if(i+need>n)return 0;while(need--)if((b[i++]&0xc0)!=0x80)return 0;}return 1;}
int main(int argc,char**argv){const char*from="UTF-8",*to="UTF-8";int i=1,status=0;for(;i<argc;i++){if(!strcmp(argv[i],"-f")&&++i<argc)from=argv[i];else if(!strcmp(argv[i],"-t")&&++i<argc)to=argv[i];else break;}if(!enc(from)||!enc(to)){fprintf(stderr,"iconv: only UTF-8 is available\n");return 2;}do{FILE*f=i==argc||!strcmp(argv[i],"-")?stdin:fopen(argv[i],"r");unsigned char b[4096];size_t n;if(!f){command_error("iconv",argv[i]);status=1;++i;continue;}while((n=fread(b,1,sizeof(b),f))>0){if(!valid(b,n)){fprintf(stderr,"iconv: invalid UTF-8 sequence\n");status=1;break;}if(fwrite(b,1,n,stdout)!=n){status=1;break;}}if(f!=stdin)fclose(f);++i;}while(i<argc);return status;}
