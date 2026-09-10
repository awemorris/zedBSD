/* Exercise architectural exception entry and real signal-return/restart paths. */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#define CHECK(x) do { if (!(x)) { printf("HAL-ENTRY FAIL line=%d errno=%d\n",__LINE__,errno); return 1; } } while (0)
static volatile sig_atomic_t signals;
static void handled(int number) { (void)number;signals++;(void)write(1,"HANDLER\n",8); }
int main(void)
{
 int status,kind,pipes[2];
 const int expected[3]={SIGILL,SIGSEGV,SIGFPE};
 pid_t child,parent;
 struct sigaction action;
 char byte;
 long result;
 puts("HAL-ENTRY STEP start");fflush(stdout);
 parent=getpid();CHECK(parent>0);
 for(kind=0;kind<3;kind++) {
  printf("HAL-ENTRY STEP fault %d\n",kind);fflush(stdout);
  child=fork();CHECK(child>=0);
  if(child==0) {
   if(kind==0)__asm__ volatile("ud2" ::: "memory");
   if(kind==1)__asm__ volatile("xor %%eax,%%eax; mov (%%eax),%%eax" ::: "eax","memory","cc");
   if(kind==2)__asm__ volatile("mov $1,%%eax; xor %%edx,%%edx; xor %%ecx,%%ecx; div %%ecx" ::: "eax","ecx","edx","cc");
   _exit(99);
  }
  CHECK(waitpid(child,&status,0)==child);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status)==expected[kind]);
 }
 puts("HAL-ENTRY STEP trap");fflush(stdout);
 memset(&action,0,sizeof(action));action.sa_handler=(uint64_t)(uintptr_t)handled;sigemptyset(&action.sa_mask);
 CHECK(sigaction(SIGTRAP,&action,NULL)==0);
 __asm__ volatile("int3" ::: "memory");
 CHECK(signals==1);
 puts("HAL-ENTRY STEP EINTR");fflush(stdout);
 CHECK(sigaction(SIGALRM,&action,NULL)==0);
 CHECK(pipe(pipes)==0);alarm(1);errno=0;
 CHECK(read(pipes[0],&byte,1)==-1 && errno==EINTR);CHECK(signals==2);
 alarm(0);CHECK(close(pipes[0])==0 && close(pipes[1])==0);
 puts("HAL-ENTRY STEP restart");fflush(stdout);
 action.sa_flags=SA_RESTART;CHECK(sigaction(SIGUSR1,&action,NULL)==0);
 CHECK(pipe(pipes)==0);child=fork();CHECK(child>=0);
 if(child==0) {
  close(pipes[0]);sleep(1);if(kill(parent,SIGUSR1)!=0)_exit(2);
  sleep(1);if(write(pipes[1],"x",1)!=1)_exit(3);_exit(0);
 }
 CHECK(close(pipes[1])==0);CHECK(read(pipes[0],&byte,1)==1 && byte=='x');
 CHECK(signals==3);CHECK(close(pipes[0])==0);
 CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
 result=0x7fffffff;
 __asm__ volatile("int $0xc2" : "+a"(result) :: "memory","cc");
 CHECK(result==-ENOSYS);
 puts("HAL-ENTRY PASS syscall UD2 page divide INT3 sigreturn EINTR restart fork wait");
 return 0;
}
