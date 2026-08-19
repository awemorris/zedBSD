/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
struct signal_name { const char *name; int number; };
static const struct signal_name names[] = {{"HUP",SIGHUP},{"INT",SIGINT},{"QUIT",SIGQUIT},{"ILL",SIGILL},{"TRAP",SIGTRAP},{"ABRT",SIGABRT},{"FPE",SIGFPE},{"KILL",SIGKILL},{"BUS",SIGBUS},{"SEGV",SIGSEGV},{"PIPE",SIGPIPE},{"ALRM",SIGALRM},{"TERM",SIGTERM},{"USR1",SIGUSR1},{"USR2",SIGUSR2},{"CHLD",SIGCHLD},{"CONT",SIGCONT},{"STOP",SIGSTOP},{"TSTP",SIGTSTP},{"TTIN",SIGTTIN},{"TTOU",SIGTTOU}};
static int parse_signal(const char *text) { char *end; long n; size_t i; if (!strncmp(text,"SIG",3)) text+=3; for(i=0;i<sizeof(names)/sizeof(names[0]);i++)if(!strcmp(text,names[i].name))return names[i].number;n=strtol(text,&end,10);return *text&&!*end&&n>=0&&n<NSIG?(int)n:-1; }
int main(int argc,char **argv){int signal_number=SIGTERM,i=1,failed=0;if(i<argc&&!strcmp(argv[i],"-l")){for(size_t n=0;n<sizeof(names)/sizeof(names[0]);n++)printf("%d %s\n",names[n].number,names[n].name);return 0;}if(i<argc&&argv[i][0]=='-'){signal_number=parse_signal(argv[i]+1);i++;}if(signal_number<0||i==argc){fprintf(stderr,"usage: kill [-signal] pid...\n       kill -l\n");return 1;}for(;i<argc;i++){char *end;long pid=strtol(argv[i],&end,10);if(!*argv[i]||*end||kill((pid_t)pid,signal_number)){command_error("kill",argv[i]);failed=1;}}return failed;}
