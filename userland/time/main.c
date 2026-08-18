/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char**argv){struct timespec a,b;pid_t p,w;int st;if(argc<2){fprintf(stderr,"usage: time command [argument ...]\n");return 2;}clock_gettime(CLOCK_MONOTONIC,&a);p=fork();if(p<0){command_error("time",NULL);return 1;}if(!p){command_exec(argv[1],&argv[1]);_exit(errno==ENOENT?127:126);}do w=waitpid(p,&st,0);while(w<0&&errno==EINTR);clock_gettime(CLOCK_MONOTONIC,&b);fprintf(stderr,"real %lld.%03ld\n",(long long)(b.tv_sec-a.tv_sec),(b.tv_nsec-a.tv_nsec)/1000000);if(w<0)return 1;return WIFEXITED(st)?WEXITSTATUS(st):128+WTERMSIG(st);}
