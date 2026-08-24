/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CRON_SPOOL "/var/spool/cron"
#define AT_SPOOL "/var/spool/at"
#define OUTPUT_SPOOL "/var/spool/cron-output"

static volatile sig_atomic_t stopping;

static void
stop(int number)
{
	(void)number;
	stopping = 1;
}

static int
field_matches(const char *field, int value)
{
	char *end;
	long expected;
	if (strcmp(field, "*") == 0)
		return 1;
	expected = strtol(field, &end, 10);
	return *field != '\0' && *end == '\0' && expected == value;
}

static void
execute_job(uid_t uid, const char *command, const char *job_id)
{
	struct passwd account, *found;
	char account_buffer[2048], output_path[320];
	pid_t child;
	int output, status;
	char *arguments[] = {"sh", "-c", (char *)command, NULL};
	snprintf(output_path, sizeof(output_path), "%s/%s", OUTPUT_SPOOL,
		 job_id);
	output = open(output_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (output < 0 ||
	    getpwuid_r(uid, &account, account_buffer, sizeof(account_buffer),
		       &found) != 0 ||
	    found == NULL) {
		if (output >= 0)
			close(output);
		return;
	}
	child = fork();
	if (child == 0) {
		(void)setpgid(0, 0);
		if (initgroups(account.pw_name, account.pw_gid) != 0 ||
		    setgid(account.pw_gid) != 0 || setuid(account.pw_uid) != 0)
			_exit(126);
		(void)chdir(account.pw_dir[0] != '\0' ? account.pw_dir : "/");
		(void)dup2(output, STDOUT_FILENO);
		(void)dup2(output, STDERR_FILENO);
		if (output > STDERR_FILENO)
			close(output);
		execv("/bin/sh", arguments);
		_exit(127);
	}
	close(output);
	if (child > 0)
		(void)waitpid(child, &status, 0);
}

static void
run_crontab(const char *path, uid_t uid, const struct tm *now)
{
	FILE *stream = fopen(path, "r");
	char line[2048];
	unsigned line_number = 0;
	if (stream == NULL)
		return;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *fields[5], *cursor = line, *command, job_id[128];
		int index;
		line_number++;
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (*cursor == '#' || *cursor == '\n' || *cursor == '\0')
			continue;
		for (index = 0; index < 5; index++) {
			fields[index] =
			    strtok(index == 0 ? cursor : NULL, " \t\r\n");
			if (fields[index] == NULL)
				break;
		}
		command = strtok(NULL, "\r\n");
		if (index != 5 || command == NULL ||
		    !field_matches(fields[0], now->tm_min) ||
		    !field_matches(fields[1], now->tm_hour) ||
		    !field_matches(fields[2], now->tm_mday) ||
		    !field_matches(fields[3], now->tm_mon + 1) ||
		    !field_matches(fields[4], now->tm_wday))
			continue;
		snprintf(job_id, sizeof(job_id), "cron.%lu.%u",
			 (unsigned long)uid, line_number);
		execute_job(uid, command, job_id);
	}
	fclose(stream);
}

static void
run_periodic(time_t current)
{
	DIR *directory = opendir(CRON_SPOOL);
	struct dirent *entry;
	struct tm now;
	if (directory == NULL || localtime_r(&current, &now) == NULL)
		return;
	while ((entry = readdir(directory)) != NULL) {
		char *end, path[320];
		unsigned long uid = strtoul(entry->d_name, &end, 10);
		if (*entry->d_name == '\0' || *end != '\0')
			continue;
		snprintf(path, sizeof(path), "%s/%s", CRON_SPOOL,
			 entry->d_name);
		run_crontab(path, (uid_t)uid, &now);
	}
	closedir(directory);
}

static void
run_at_jobs(time_t current)
{
	DIR *directory = opendir(AT_SPOOL);
	struct dirent *entry;
	if (directory == NULL)
		return;
	while ((entry = readdir(directory)) != NULL) {
		char *end, path[320], claimed[336], command[4096];
		long long when = strtoll(entry->d_name, &end, 10);
		struct stat status;
		FILE *stream;
		size_t used = 0;
		if (end == entry->d_name || *end != '.' ||
		    when > (long long)current)
			continue;
		snprintf(path, sizeof(path), "%s/%s", AT_SPOOL, entry->d_name);
		snprintf(claimed, sizeof(claimed), "%s.run", path);
		if (rename(path, claimed) != 0 || stat(claimed, &status) != 0)
			continue;
		stream = fopen(claimed, "r");
		if (stream != NULL) {
			while (fgets(command + used, sizeof(command) - used,
				     stream) != NULL) {
				if (command[used] != '#')
					used += strlen(command + used);
				if (used + 1 >= sizeof(command))
					break;
			}
			fclose(stream);
			command[used] = '\0';
			execute_job(status.st_uid, command, entry->d_name);
		}
		unlink(claimed);
	}
	closedir(directory);
}

int
main(void)
{
	time_t last_minute = (time_t)-1;
	(void)signal(SIGTERM, stop);
	(void)signal(SIGINT, stop);
	(void)mkdir("/var/spool", 0755);
	(void)mkdir(CRON_SPOOL, 01777);
	(void)mkdir(AT_SPOOL, 01777);
	(void)mkdir(OUTPUT_SPOOL, 0700);
	while (!stopping) {
		time_t now = time(NULL);
		if (now / 60 != last_minute) {
			last_minute = now / 60;
			run_periodic(now);
		}
		run_at_jobs(now);
		sleep(1);
	}
	return 0;
}
