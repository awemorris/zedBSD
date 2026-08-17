/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static unsigned long crc_byte(unsigned long crc,unsigned char byte){int bit;crc^=(unsigned long)byte<<24;for(bit=0;bit<8;bit++)crc=(crc&0x80000000UL)?(crc<<1)^0x04c11db7UL:crc<<1;return crc&0xffffffffUL;}
static int checksum(int fd,const char *name){unsigned char buffer[4096];unsigned long crc=0;unsigned long long length=0,value;for(;;){ssize_t n=read(fd,buffer,sizeof(buffer));if(n<0){command_error("cksum",name);return 1;}if(!n)break;length+=(unsigned long long)n;for(ssize_t i=0;i<n;i++)crc=crc_byte(crc,buffer[i]);}value=length;while(value){crc=crc_byte(crc,(unsigned char)value);value>>=8;}crc=(~crc)&0xffffffffUL;printf("%lu %llu%s%s\n",crc,length,name?" ":"",name?name:"");return 0;}
int main(int argc,char **argv){int i=1,failed=0;if(i==argc)return checksum(0,NULL);for(;i<argc;i++){int fd=!strcmp(argv[i],"-")?0:open(argv[i],O_RDONLY);if(fd<0){command_error("cksum",argv[i]);failed=1;continue;}failed|=checksum(fd,argv[i]);if(fd)close(fd);}return failed;}
