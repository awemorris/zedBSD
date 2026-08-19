/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int tail_fd(int fd,unsigned long long wanted){unsigned char *data=NULL,buffer[4096];size_t used=0,capacity=0,start;for(;;){ssize_t n=read(fd,buffer,sizeof(buffer));if(n<0){free(data);return -1;}if(!n)break;if(used+(size_t)n<used){free(data);return -1;}if(used+(size_t)n>capacity){size_t next=capacity?capacity*2:4096;while(next<used+(size_t)n)next*=2;unsigned char *larger=realloc(data,next);if(!larger){free(data);return -1;}data=larger;capacity=next;}memcpy(data+used,buffer,(size_t)n);used+=(size_t)n;}if(!wanted){free(data);return 0;}start=used;if(start&&data[start-1]=='\n')start--;while(start&&wanted){start--;if(data[start]=='\n')wanted--;}if(start<used&&data[start]=='\n')start++;if(command_write_all(1,data+start,used-start)){free(data);return -1;}free(data);return 0;}
int main(int argc,char **argv){unsigned long long lines=10;int i=1,failed=0;if(i+1<argc&&!strcmp(argv[i],"-n")){if(command_parse_ull(argv[i+1],&lines))goto usage;i+=2;}if(i==argc)return tail_fd(0,lines)!=0;for(;i<argc;i++){int fd=!strcmp(argv[i],"-")?0:open(argv[i],O_RDONLY);if(fd<0||tail_fd(fd,lines)){command_error("tail",argv[i]);failed=1;}if(fd>0)close(fd);}return failed;usage:fprintf(stderr,"usage: tail [-n lines] [file...]\n");return 1;}
