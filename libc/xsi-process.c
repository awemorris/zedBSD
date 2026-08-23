/* XSI compatibility wrappers that require no new kernel mechanism. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ulimit.h>
#include <unistd.h>

static int concurrency_hint;

int pthread_getconcurrency(void)
{ return __atomic_load_n(&concurrency_hint, __ATOMIC_RELAXED); }

int pthread_setconcurrency(int value)
{
	if (value < 0) return EINVAL;
	__atomic_store_n(&concurrency_hint, value, __ATOMIC_RELAXED);
	return 0;
}

static int
signal_set(int signal_number, sigset_t *set)
{
	if (sigemptyset(set) != 0 || sigaddset(set, signal_number) != 0) return -1;
	return 0;
}

int
killpg(pid_t group, int signal_number)
{
	if (group <= 1) { errno = EINVAL; return -1; }
	return kill(-group, signal_number);
}

int sighold(int number)
{ sigset_t set; return signal_set(number,&set) ? -1 : sigprocmask(SIG_BLOCK,&set,NULL); }
int sigrelse(int number)
{ sigset_t set; return signal_set(number,&set) ? -1 : sigprocmask(SIG_UNBLOCK,&set,NULL); }

int
sigignore(int number)
{
	struct sigaction action;
	memset(&action,0,sizeof(action));
	action.sa_handler=(uint64_t)(uintptr_t)SIG_IGN;
	return sigaction(number,&action,NULL);
}

int
siginterrupt(int number, int interrupt)
{
	struct sigaction action;
	if(sigaction(number,NULL,&action)!=0)return -1;
	if(interrupt)action.sa_flags&=~SA_RESTART;else action.sa_flags|=SA_RESTART;
	return sigaction(number,&action,NULL);
}

int
sigpause(int number)
{
	sigset_t mask;
	if(number<=0 || number>=NSIG){errno=EINVAL;return -1;}
	if(sigprocmask(SIG_SETMASK,NULL,&mask)!=0)return -1;
	if(sigdelset(&mask,number)!=0)return -1;
	return sigsuspend(&mask);
}

sighandler_t
sigset(int number, sighandler_t disposition)
{
	struct sigaction old_action, action;
	sigset_t old_mask, one;
	int was_held;
	if(signal_set(number,&one)!=0 || sigprocmask(SIG_SETMASK,NULL,&old_mask)!=0)return SIG_ERR;
	was_held=sigismember(&old_mask,number);
	if(sigaction(number,NULL,&old_action)!=0)return SIG_ERR;
	if(disposition==SIG_HOLD){
		if(sigprocmask(SIG_BLOCK,&one,NULL)!=0)return SIG_ERR;
	} else {
		memset(&action,0,sizeof(action));action.sa_handler=(uint64_t)(uintptr_t)disposition;
		if(sigaction(number,&action,NULL)!=0 || sigprocmask(SIG_UNBLOCK,&one,NULL)!=0)return SIG_ERR;
	}
	return was_held?SIG_HOLD:(sighandler_t)(uintptr_t)old_action.sa_handler;
}

int
setpgrp(void)
{ return setpgid(0,0); }

int
utimes(const char *path, const struct timeval times[2])
{
	struct timespec converted[2];
	int index;
	if(times==NULL)return utimensat(AT_FDCWD,path,NULL,0);
	for(index=0;index<2;index++){
		if(times[index].tv_usec<0 || times[index].tv_usec>=1000000){errno=EINVAL;return -1;}
		converted[index].tv_sec=times[index].tv_sec;
		converted[index].tv_nsec=times[index].tv_usec*1000L;
	}
	return utimensat(AT_FDCWD,path,converted,0);
}

key_t
ftok(const char *path, int project)
{
	struct stat status;
	if(stat(path,&status)!=0)return (key_t)-1;
	return (key_t)(((project&0xff)<<24)|((status.st_dev&0xff)<<16)|(status.st_ino&0xffff));
}

long
gethostid(void)
{
	FILE *file=fopen("/etc/hostid","rb");
	unsigned char bytes[4];
	char hostname[256];
	uint32_t value=2166136261U;
	size_t index;
	if(file!=NULL){
		if(fread(bytes,1,4,file)==4){fclose(file);return(long)((uint32_t)bytes[0]<<24|(uint32_t)bytes[1]<<16|(uint32_t)bytes[2]<<8|bytes[3]);}
		fclose(file);
	}
	if(gethostname(hostname,sizeof(hostname))!=0)return -1;
	for(index=0;hostname[index]!='\0';index++){value^=(unsigned char)hostname[index];value*=16777619U;}
	return(long)value;
}

long
ulimit(int command, ...)
{
	va_list arguments;
	(void)arguments;
	switch(command){
	case UL_GETFSIZE: return LONG_MAX;
	case UL_GETOPENMAX: return sysconf(_SC_OPEN_MAX);
	case UL_GETMAXBRK: errno=EINVAL; return -1;
	case UL_SETFSIZE:
		va_start(arguments,command);(void)va_arg(arguments,long);va_end(arguments);
		errno=EPERM;return -1;
	default: errno=EINVAL;return -1;
	}
}
