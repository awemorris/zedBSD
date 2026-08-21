/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define TOP_MAX_PROCESSES 256U
#define TOP_CLEAR_SCREEN "\033[H\033[2J"
#define fputs(text, stream) ((void)(stream), command_write_all(STDOUT_FILENO, \
	strcmp((text), "\033[H\033[J") == 0 ? TOP_CLEAR_SCREEN : (text), \
	strlen(strcmp((text), "\033[H\033[J") == 0 ? TOP_CLEAR_SCREEN : (text))))
#define puts(text) printf("%s\n", (text))

static char state_letter(unsigned state){static const char map[]="?RTXZX";return state<sizeof(map)-1U?map[state]:'?';}
static const char*user_name(uid_t uid,char b[16]){if(uid==0)return "root";snprintf(b,16,"%u",(unsigned)uid);return b;}
static const char*leaf(const char*s){const char*p=strrchr(s,'/');return p?p+1:s;}
static int snapshot(int fd,struct process_info*p,size_t*n){int32_t cursor=-1;size_t used=0;while(used<TOP_MAX_PROCESSES){memset(&p[used],0,sizeof(p[used]));p[used].pid=cursor;if(ioctl(fd,ZEDBSD_SYSTEM_GET_PROCESS,&p[used])!=0){if(errno==ENOENT)break;return-1;}p[used].command[ZEDBSD_SYSTEM_PROCESS_COMMAND_MAX-1U]='\0';cursor=p[used].pid;used++;}*n=used;return 0;}
static void human(uint64_t bytes,char out[16]){static const char units[]="BKMGT";unsigned u=0;uint64_t scale=1;while(u+1U<sizeof(units)-1U&&bytes>=scale*1024U){scale*=1024U;u++;}if(u==0)snprintf(out,16,"%lluB",(unsigned long long)bytes);else snprintf(out,16,"%llu%c",(unsigned long long)((bytes+scale/2U)/scale),units[u]);}
static void draw(int fd,int batch){struct vm_statistics vm;struct process_info p[TOP_MAX_PROCESSES];struct timespec now;size_t count=0,i;unsigned running=0,sleeping=0,stopped=0,zombie=0;char total[16],freeb[16],used[16],swap[16],swapfree[16];if(ioctl(fd,ZEDBSD_SYSTEM_GET_VMSTAT,&vm)!=0||snapshot(fd,p,&count)!=0)return;for(i=0;i<count;i++){if(p[i].state==1)running++;else if(p[i].state==2)stopped++;else if(p[i].state==4)zombie++;else sleeping++;}clock_gettime(CLOCK_MONOTONIC,&now);human(vm.physical_total,total);human(vm.physical_free,freeb);human(vm.physical_total-vm.physical_free,used);human(vm.swap_total*ZEDBSD_SYSTEM_SWAP_PAGE_SIZE,swap);human(vm.swap_free*ZEDBSD_SYSTEM_SWAP_PAGE_SIZE,swapfree);if(!batch)fputs("\033[H\033[J",stdout);printf("top - up %lld days, %02lld:%02lld,  %zu tasks\n",(long long)(now.tv_sec/86400),(long long)(now.tv_sec/3600%24),(long long)(now.tv_sec/60%60),count);printf("Tasks: %3zu total, %3u running, %3u sleeping, %3u stopped, %3u zombie\n",count,running,sleeping,stopped,zombie);printf("%%Cpu(s):  0.0 us,  0.0 sy,  0.0 ni, 100.0 id,  0.0 wa\n");printf("MiB Mem : %8s total, %8s free, %8s used\n",total,freeb,used);printf("MiB Swap: %8s total, %8s free\n\n",swap,swapfree);puts("    PID USER      PR  NI    VIRT    RES    SHR S  %CPU %MEM     TIME+ COMMAND");for(i=0;i<count;i++){char ub[16],virt[16];const char*u=user_name(p[i].uid,ub);human(p[i].virtual_bytes,virt);printf("%7d %-8.8s  20   0 %7s      0      0 %c   0.0  0.0   0:00.00 %s\n",p[i].pid,u,virt,state_letter(p[i].state),p[i].command[0]?leaf(p[i].command):"kernel");}fflush(stdout);}
int main(int argc,char**argv){int fd,batch=0,iterations=-1,delay=1000,i;struct termios saved,raw;int tty=0;for(i=1;i<argc;i++){if(!strcmp(argv[i],"-b"))batch=1;else if(!strcmp(argv[i],"-n")&&i+1<argc)iterations=atoi(argv[++i]);else if(!strcmp(argv[i],"-d")&&i+1<argc)delay=atoi(argv[++i])*1000;else{fprintf(stderr,"usage: top [-b] [-n count] [-d seconds]\n");return 2;}}fd=open("/dev/system",O_RDONLY);if(fd<0){command_error("top","/dev/system");return 1;}if(!batch&&isatty(STDIN_FILENO)&&tcgetattr(STDIN_FILENO,&saved)==0){raw=saved;raw.c_lflag&=~(ECHO|ICANON);raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;if(tcsetattr(STDIN_FILENO,TCSANOW,&raw)==0)tty=1;fputs("\033[?25l",stdout);}for(i=0;iterations<0||i<iterations;i++){struct pollfd in={STDIN_FILENO,POLLIN,0};char key;draw(fd,batch);if(iterations>=0&&i+1>=iterations)break;if(poll(&in,1,delay)>0&&read(STDIN_FILENO,&key,1)==1&&(key=='q'||key=='Q'))break;}if(tty){tcsetattr(STDIN_FILENO,TCSANOW,&saved);fputs("\033[?25h\n",stdout);}close(fd);return 0;}
