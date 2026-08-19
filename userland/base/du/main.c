/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static unsigned long long walk(const char*p,int all,int*bad){struct stat st;DIR*d;struct dirent*e;unsigned long long total;if(lstat(p,&st)){command_error("du",p);*bad=1;return 0;}total=(unsigned long long)(st.st_blocks<0?0:st.st_blocks);if(!S_ISDIR(st.st_mode))return total;d=opendir(p);if(!d){command_error("du",p);*bad=1;return total;}while((e=readdir(d))){char*q;size_t a,b;if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;a=strlen(p);b=strlen(e->d_name);q=malloc(a+b+2);if(!q){*bad=1;break;}memcpy(q,p,a);if(a&&p[a-1]!='/')q[a++]='/';memcpy(q+a,e->d_name,b+1);{unsigned long long n=walk(q,all,bad);total+=n;if(all)printf("%llu\t%s\n",n,q);}free(q);}closedir(d);return total;}
int main(int argc,char**argv){int all=0,i=1,bad=0;if(i<argc&&!strcmp(argv[i],"-a")){all=1;i++;}if(i==argc){argv[--i]=(char*)".";argc=i+1;}for(;i<argc;i++)printf("%llu\t%s\n",walk(argv[i],all,&bad),argv[i]);return bad;}
