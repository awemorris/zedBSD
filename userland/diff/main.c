/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct lines{char**v;size_t n,c;};static int load(const char*p,struct lines*l){FILE*f=fopen(p,"r");char*b=NULL;size_t cap=0;long n;if(!f){command_error("diff",p);return-1;}while((n=command_read_line(f,&b,&cap))>0){char*s=malloc((size_t)n+1);if(!s)return-1;memcpy(s,b,(size_t)n+1);if(l->n==l->c){size_t c=l->c?l->c*2:32;char**v=realloc(l->v,c*sizeof(*v));if(!v)return-1;l->v=v;l->c=c;}l->v[l->n++]=s;}free(b);fclose(f);return n<0?-1:0;}
int main(int argc,char**argv){struct lines a={0},b={0};size_t i,n;int different=0;if(argc==4&&!strcmp(argv[1],"-u")){argv++;argc--;}if(argc!=3){fprintf(stderr,"usage: diff [-u] file1 file2\n");return 2;}if(load(argv[1],&a)||load(argv[2],&b))return 2;n=a.n>b.n?a.n:b.n;for(i=0;i<n;i++)if(i>=a.n||i>=b.n||strcmp(a.v[i],b.v[i])){if(!different)printf("--- %s\n+++ %s\n@@ -1,%lu +1,%lu @@\n",argv[1],argv[2],(unsigned long)a.n,(unsigned long)b.n);if(i<a.n){putchar('-');fwrite(a.v[i],1,strlen(a.v[i]),stdout);if(a.v[i][strlen(a.v[i])-1]!='\n')putchar('\n');}if(i<b.n){putchar('+');fwrite(b.v[i],1,strlen(b.v[i]),stdout);if(b.v[i][strlen(b.v[i])-1]!='\n')putchar('\n');}different=1;}for(i=0;i<a.n;i++)free(a.v[i]);for(i=0;i<b.n;i++)free(b.v[i]);free(a.v);free(b.v);return different;}
