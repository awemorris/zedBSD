/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <crypt.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <utmpx.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int
read_line(char *buffer, size_t size, int echo)
{
	struct termios saved, mode;
	size_t used = 0;
	char byte;
	int changed = 0;

	if (!echo && tcgetattr(STDIN_FILENO, &saved) == 0) {
		mode = saved; mode.c_lflag &= ~ECHO;
		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &mode) == 0) changed = 1;
	}
	while (used + 1U < size) {
		ssize_t count = read(STDIN_FILENO, &byte, 1);
		if (count < 0 && errno == EINTR) continue;
		if (count != 1) break;
		if (byte == '\r' || byte == '\n') break;
		buffer[used++] = byte;
	}
	buffer[used] = '\0';
	if (changed) { (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved); puts(""); }
	return used != 0 ? 0 : -1;
}

static void
utmp_fill(struct utmpx *entry, int type, pid_t pid, const char *user,
	const char *line)
{
	struct timespec now;
	const char *base = strrchr(line, '/');
	memset(entry, 0, sizeof(*entry)); entry->ut_type = (int16_t)type;
	entry->ut_pid = pid; if (base != NULL) line = base + 1;
	strncpy(entry->ut_line, line, sizeof(entry->ut_line) - 1U);
	strncpy(entry->ut_id, line, sizeof(entry->ut_id));
	if (user != NULL) strncpy(entry->ut_user, user, sizeof(entry->ut_user) - 1U);
	if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
		entry->ut_tv_sec = now.tv_sec; entry->ut_tv_usec = (int32_t)(now.tv_nsec / 1000L);
	}
}

int
main(int argc, char **argv)
{
	char name[64], password[256], tty[64];
	struct passwd account, *found; struct spwd shadow, *shadow_found;
	char pwbuf[2048], spbuf[2048]; char *hash;
	pid_t child, waited; int status = 0; struct utmpx record;
	char *shell_argv[2]; char *environment[6]; char home[320], user[96], logname[96];

	if (geteuid() != 0) { fprintf(stderr, "login: must be run as root\n"); return 1; }
	if (ttyname_r(STDIN_FILENO, tty, sizeof(tty)) != 0) strcpy(tty, "/dev/console");
	if (argc > 1) { strncpy(name, argv[1], sizeof(name)-1U); name[sizeof(name)-1U]='\0'; }
	else { printf("login: "); fflush(stdout); if (read_line(name,sizeof(name),1)!=0) return 1; }
	if (getpwnam_r(name,&account,pwbuf,sizeof(pwbuf),&found)!=0 || found==NULL ||
	    getspnam_r(name,&shadow,spbuf,sizeof(spbuf),&shadow_found)!=0 || shadow_found==NULL) {
		printf("Password: "); fflush(stdout); (void)read_line(password,sizeof(password),0);
		puts("Login incorrect"); return 1;
	}
	printf("Password: "); fflush(stdout);
	if (read_line(password,sizeof(password),0)!=0) return 1;
	if (shadow.sp_pwdp[0]=='!' || shadow.sp_pwdp[0]=='*' || shadow.sp_pwdp[0]=='\0') {
		memset(password,0,sizeof(password)); puts("Login incorrect"); return 1;
	}
	hash=crypt(password,shadow.sp_pwdp); memset(password,0,sizeof(password));
	if (hash==NULL || strcmp(hash,shadow.sp_pwdp)) { puts("Login incorrect"); return 1; }
	child=fork(); if(child<0){fprintf(stderr,"login: fork: %s\n",strerror(errno));return 1;}
	if(child==0){ const char *shell=account.pw_shell[0]?account.pw_shell:"/bin/sh";
		if(initgroups(account.pw_name,account.pw_gid)||setgid(account.pw_gid)||setuid(account.pw_uid)) _exit(126);
		if(chdir(account.pw_dir)!=0)(void)chdir("/");
		snprintf(home,sizeof(home),"HOME=%s",account.pw_dir);snprintf(user,sizeof(user),"USER=%s",account.pw_name);
		snprintf(logname,sizeof(logname),"LOGNAME=%s",account.pw_name);
		environment[0]=home;environment[1]=user;environment[2]=logname;environment[3]="PATH=/bin:/usr/bin";environment[4]="SHELL=/bin/sh";environment[5]=NULL;
		shell_argv[0]="-sh";shell_argv[1]=NULL;execve(shell,shell_argv,environment);_exit(127);}
	utmp_fill(&record,USER_PROCESS,child,account.pw_name,tty);(void)pututxline(&record);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	utmp_fill(&record,DEAD_PROCESS,child,"",tty);(void)pututxline(&record);
	if (waited < 0) {
		fprintf(stderr, "login: waitpid: %s\n", strerror(errno));
		return 1;
	}
	return WIFEXITED(status)?WEXITSTATUS(status):1;
}
