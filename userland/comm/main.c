/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void out(int col,int s1,int s2,int s3,const char *p,long n){int tabs=0;if((col==1&&s1)||(col==2&&s2)||(col==3&&s3))return;if(col>1&&!s1)tabs++;if(col>2&&!s2)tabs++;while(tabs--)putchar('\t');fwrite(p,1,(size_t)n,stdout);}
int main(int argc,char**argv){int s1=0,s2=0,s3=0,i=1;FILE*a,*b;char*x=NULL,*y=NULL;size_t cx=0,cy=0;long nx,ny;for(;i<argc&&argv[i][0]=='-';++i){const char*p=argv[i]+1;while(*p){if(*p=='1')s1=1;else if(*p=='2')s2=1;else if(*p=='3')s3=1;else{fprintf(stderr,"comm: invalid option\n");return 2;}++p;}}if(argc-i!=2){fprintf(stderr,"usage: comm [-123] file1 file2\n");return 2;}a=!strcmp(argv[i],"-")?stdin:fopen(argv[i],"r");b=!strcmp(argv[i+1],"-")?stdin:fopen(argv[i+1],"r");if(!a||!b||a==b){fprintf(stderr,"comm: cannot open inputs\n");return 1;}nx=command_read_line(a,&x,&cx);ny=command_read_line(b,&y,&cy);while(nx>0||ny>0){int c;if(nx<=0)c=1;else if(ny<=0)c=-1;else{size_t n=(size_t)(nx<ny?nx:ny);c=memcmp(x,y,n);if(!c)c=(nx>ny)-(nx<ny);}if(c<=0){out(c?1:3,s1,s2,s3,x,nx);nx=command_read_line(a,&x,&cx);}if(c>=0){if(c)out(2,s1,s2,s3,y,ny);ny=command_read_line(b,&y,&cy);}}free(x);free(y);if(a!=stdin)fclose(a);if(b!=stdin)fclose(b);return nx<0||ny<0||ferror(stdout);}
