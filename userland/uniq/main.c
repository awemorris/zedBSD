/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void emit(const char *s, long n, unsigned long count, int c, int d, int u) { if ((d && count < 2) || (u && count != 1)) return; if (c) printf("%7lu ", count); fwrite(s, 1, (size_t)n, stdout); }
int main(int argc, char **argv) { int c=0,d=0,u=0,i=1; FILE *f=stdin,*o=stdout; char *a=NULL,*b=NULL; size_t ca=0,cb=0; long na,nb; unsigned long count=1;
	for (;i<argc&&argv[i][0]=='-';++i) { if(!strcmp(argv[i],"-c"))c=1; else if(!strcmp(argv[i],"-d"))d=1; else if(!strcmp(argv[i],"-u"))u=1; else break; }
	if(i<argc&&!strcmp(argv[i],"-"))++i; else if(i<argc&&(f=fopen(argv[i++],"r"))==NULL){command_error("uniq",argv[i-1]);return 1;} if(i<argc&&(o=fopen(argv[i++],"w"))==NULL){command_error("uniq",argv[i-1]);return 1;} if(i!=argc){fprintf(stderr,"usage: uniq [-cdu] [input [output]]\n");return 2;} stdout=o;
	na=command_read_line(f,&a,&ca); if(na<=0)return na<0; while((nb=command_read_line(f,&b,&cb))>0){if(na==nb&&!memcmp(a,b,(size_t)na))++count;else{emit(a,na,count,c,d,u);{char*t=a;a=b;b=t;size_t z=ca;ca=cb;cb=z;}na=nb;count=1;}} emit(a,na,count,c,d,u); free(a);free(b); if(f!=stdin)fclose(f); if(o!=stdout)fclose(o); return nb<0||ferror(stdout); }
