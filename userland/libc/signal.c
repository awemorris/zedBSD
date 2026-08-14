/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/libc/syscall.h"
#include <zedbsd/syscall.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
extern void zedbsd_signal_restorer(void);
static intptr_t call(uint32_t n,uintptr_t a,uintptr_t b,uintptr_t c){intptr_t r=zedbsd_syscall6(n,a,b,c,0,0,0);if(r<0){errno=(int)-r;return-1;}return r;}
int sigaction(int s,const struct sigaction*a,struct sigaction*o){struct sigaction copy;if(a!=NULL){copy=*a;copy.sa_restorer=(uint64_t)(uintptr_t)zedbsd_signal_restorer;a=&copy;}return(int)call(ZEDBSD_SYS_sigaction,s,(uintptr_t)a,(uintptr_t)o);}
int sigprocmask(int h,const sigset_t*s,sigset_t*o){return(int)call(ZEDBSD_SYS_sigprocmask,h,(uintptr_t)s,(uintptr_t)o);}
int sigpending(sigset_t*s){return(int)call(ZEDBSD_SYS_sigpending,(uintptr_t)s,0,0);}
int sigsuspend(const sigset_t*s){return(int)call(ZEDBSD_SYS_sigsuspend,(uintptr_t)s,0,0);}
int kill(pid_t p,int s){return(int)call(ZEDBSD_SYS_kill,p,s,0);}
sighandler_t signal(int s,sighandler_t h){struct sigaction a,o;memset(&a,0,sizeof(a));a.sa_handler=(uint64_t)(uintptr_t)h;return sigaction(s,&a,&o)==0?(sighandler_t)(uintptr_t)o.sa_handler:(sighandler_t)-1;}
