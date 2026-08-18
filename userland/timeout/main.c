/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char**argv){unsigned long long sec;pid_t p;int st;struct timespec slice={0,10000000};unsigned long long ticks,i;if(argc<3||command_parse_ull(argv[1],&sec)){fprintf(stderr,"usage: timeout duration command [argument ...]\n");return 2;}p=fork();if(p<0){command_error("timeout",NULL);return 1;}if(!p){setpgid(0,0);command_exec(argv[2],&argv[2]);_exit(errno==ENOENT?127:126);}setpgid(p,p);ticks=sec*100;for(i=0;i<ticks;i++){pid_t w=waitpid(p,&st,WNOHANG);if(w==p)return WIFEXITED(st)?WEXITSTATUS(st):128+WTERMSIG(st);if(w<0&&errno!=EINTR)return 1;nanosleep(&slice,NULL);}kill(-p,SIGTERM);while(waitpid(p,&st,0)<0&&errno==EINTR){}return 124;}
