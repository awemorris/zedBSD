/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int scan(int fd,const char *name,unsigned minimum){unsigned char in[4096],out[4096];size_t used=0;for(;;){ssize_t n=read(fd,in,sizeof(in));if(n<0){command_error("strings",name);return 1;}for(ssize_t i=0;i<n;i++){unsigned char c=in[i];if(c>=0x20&&c<=0x7e){if(used<sizeof(out))out[used++]=c;}else{if(used>=minimum){command_write_all(1,out,used);command_write_all(1,"\n",1);}used=0;}}if(!n)break;}if(used>=minimum){command_write_all(1,out,used);command_write_all(1,"\n",1);}return 0;}
int main(int argc,char **argv){unsigned minimum=4;int i=1,failed=0;if(i+1<argc&&!strcmp(argv[i],"-n")){unsigned long long v;if(command_parse_ull(argv[i+1],&v)||v>4096)goto usage;minimum=(unsigned)v;i+=2;}if(i==argc)return scan(0,NULL,minimum);for(;i<argc;i++){int fd=open(argv[i],O_RDONLY);if(fd<0){command_error("strings",argv[i]);failed=1;continue;}failed|=scan(fd,argv[i],minimum);close(fd);}return failed;usage:fprintf(stderr,"usage: strings [-n length] [file...]\n");return 1;}
