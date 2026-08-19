/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/libc/syscall.h"
#include <zedbsd/syscall.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
extern void zedbsd_signal_restorer(void);
static intptr_t call(uint32_t n,uintptr_t a,uintptr_t b,uintptr_t c){intptr_t r=zedbsd_syscall6(n,a,b,c,0,0,0);if(r<0){errno=(int)-r;return-1;}return r;}
int sigaction(int s,const struct sigaction*a,struct sigaction*o){struct sigaction copy;if(a!=NULL){copy=*a;copy.sa_restorer=(uint64_t)(uintptr_t)zedbsd_signal_restorer;a=&copy;}return(int)call(ZEDBSD_SYS_sigaction,s,(uintptr_t)a,(uintptr_t)o);}
int sigprocmask(int h,const sigset_t*s,sigset_t*o){return(int)call(ZEDBSD_SYS_sigprocmask,h,(uintptr_t)s,(uintptr_t)o);}
int sigpending(sigset_t*s){return(int)call(ZEDBSD_SYS_sigpending,(uintptr_t)s,0,0);}
int sigsuspend(const sigset_t*s){return(int)call(ZEDBSD_SYS_sigsuspend,(uintptr_t)s,0,0);}
int kill(pid_t p,int s){return(int)call(ZEDBSD_SYS_kill,p,s,0);}
sighandler_t signal(int s,sighandler_t h){struct sigaction a,o;memset(&a,0,sizeof(a));a.sa_handler=(uint64_t)(uintptr_t)h;return sigaction(s,&a,&o)==0?(sighandler_t)(uintptr_t)o.sa_handler:(sighandler_t)-1;}

static int sigset_signo(int signo){return signo>0&&signo<NSIG;}
int sigemptyset(sigset_t*s){if(s==NULL){errno=EINVAL;return-1;}*s=0;return 0;}
int sigfillset(sigset_t*s){if(s==NULL){errno=EINVAL;return-1;}*s=UINT32_MAX;return 0;}
int sigaddset(sigset_t*s,int n){if(s==NULL||!sigset_signo(n)){errno=EINVAL;return-1;}*s|=(uint32_t)1U<<((unsigned)n-1U);return 0;}
int sigdelset(sigset_t*s,int n){if(s==NULL||!sigset_signo(n)){errno=EINVAL;return-1;}*s&=~((uint32_t)1U<<((unsigned)n-1U));return 0;}
int sigismember(const sigset_t*s,int n){if(s==NULL||!sigset_signo(n)){errno=EINVAL;return-1;}return(*s&((uint32_t)1U<<((unsigned)n-1U)))!=0;}
int sigaltstack(const stack_t*n,stack_t*o){struct zedbsd_sigaltstack in,out;intptr_t r;memset(&in,0,sizeof(in));memset(&out,0,sizeof(out));if(n!=NULL){in.ss_sp=(uapi_ptr_t)(uintptr_t)n->ss_sp;in.ss_size=n->ss_size;in.ss_flags=n->ss_flags;}r=call(ZEDBSD_SYS_sigaltstack,n!=NULL?(uintptr_t)&in:0,o!=NULL?(uintptr_t)&out:0,0);if(r==0&&o!=NULL){o->ss_sp=(void*)(uintptr_t)out.ss_sp;o->ss_size=(size_t)out.ss_size;o->ss_flags=out.ss_flags;}return(int)r;}
int sigtimedwait(const sigset_t*s,siginfo_t*i,const struct timespec*t){if(s==NULL){errno=EINVAL;return-1;}return(int)call(ZEDBSD_SYS_sigtimedwait,(uintptr_t)s,(uintptr_t)i,(uintptr_t)t);}
int sigwaitinfo(const sigset_t*s,siginfo_t*i){return sigtimedwait(s,i,NULL);}
int sigwait(const sigset_t*s,int*n){int r;if(n==NULL){errno=EINVAL;return EINVAL;}r=sigtimedwait(s,NULL,NULL);if(r<0)return errno;*n=r;return 0;}
int sigqueue(pid_t p,int n,const union sigval v){uint64_t raw=0;memcpy(&raw,&v,sizeof(raw));return(int)call(ZEDBSD_SYS_sigqueue,p,n,(uintptr_t)raw);}
int raise(int n){return kill(getpid(),n);}
void abort(void){(void)raise(SIGABRT);_exit(128+SIGABRT);}
