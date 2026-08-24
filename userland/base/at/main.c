/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AT_SPOOL "/var/spool/at"

static const char *
program_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash != NULL ? slash + 1 : path;
}

static int
job_owned(const char *name)
{
	char path[320];
	struct stat status;
	if (strchr(name, '/') != NULL ||
	    snprintf(path, sizeof(path), "%s/%s", AT_SPOOL, name) >=
		(int)sizeof(path) ||
	    stat(path, &status) != 0)
		return 0;
	return geteuid() == 0 || status.st_uid == getuid();
}

static int
list_jobs(void)
{
	DIR *directory = opendir(AT_SPOOL);
	struct dirent *entry;
	if (directory == NULL)
		return errno == ENOENT ? 0 : 1;
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.' && job_owned(entry->d_name))
			puts(entry->d_name);
	closedir(directory);
	return 0;
}

static int
remove_jobs(int argc, char **argv, int first)
{
	int failed = 0, index;
	for (index = first; index < argc; index++) {
		char path[320];
		if (!job_owned(argv[index]) ||
		    snprintf(path, sizeof(path), "%s/%s", AT_SPOOL,
			     argv[index]) >= (int)sizeof(path) ||
		    unlink(path) != 0) {
			fprintf(stderr, "at: cannot remove %s\n", argv[index]);
			failed = 1;
		}
	}
	return failed;
}

static int
parse_time(int argc, char **argv, int first, time_t *result)
{
	time_t now = time(NULL);
	if (first == argc) {
		*result = now;
		return 0;
	}
	if (strcmp(argv[first], "now") == 0) {
		if (first + 1 == argc) {
			*result = now;
			return 0;
		}
		if (first + 4 == argc && strcmp(argv[first + 1], "+") == 0) {
			char *end;
			long count = strtol(argv[first + 2], &end, 10);
			long scale =
			    strcmp(argv[first + 3], "hours") == 0 ||
				    strcmp(argv[first + 3], "hour") == 0
				? 3600
				: 60;
			if (*end == '\0' && count >= 0 &&
			    (strstr(argv[first + 3], "minute") != NULL ||
			     strstr(argv[first + 3], "hour") != NULL)) {
				*result = now + count * scale;
				return 0;
			}
		}
	}
	{
		unsigned long hour, minute;
		char text[32], *colon, *end;
		struct tm broken;
		if (first + 1 != argc || strlen(argv[first]) >= sizeof(text))
			return -1;
		strcpy(text, argv[first]);
		colon = strchr(text, ':');
		if (colon == NULL)
			return -1;
		*colon++ = '\0';
		hour = strtoul(text, &end, 10);
		if (*text == '\0' || *end != '\0')
			return -1;
		minute = strtoul(colon, &end, 10);
		if (*colon == '\0' || *end != '\0' || hour > 23 ||
		    minute > 59 || localtime_r(&now, &broken) == NULL)
			return -1;
		broken.tm_hour = (int)hour;
		broken.tm_min = (int)minute;
		broken.tm_sec = 0;
		*result = mktime(&broken);
		if (*result < now)
			*result += 24 * 60 * 60;
		return *result == (time_t)-1 ? -1 : 0;
	}
}

static int
submit_job(FILE *input, time_t when, char queue)
{
	char path[320], buffer[1024];
	int descriptor;
	FILE *output;
	(void)mkdir("/var/spool", 0755);
	(void)mkdir(AT_SPOOL, 01777);
	if (snprintf(path, sizeof(path), "%s/%lld.%lu.%ld.%c", AT_SPOOL,
		     (long long)when, (unsigned long)getuid(), (long)getpid(),
		     queue) >= (int)sizeof(path))
		return 1;
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0 || (output = fdopen(descriptor, "w")) == NULL)
		return 1;
	fprintf(output, "# zedBSD at job\n");
	while (fgets(buffer, sizeof(buffer), input) != NULL)
		if (fputs(buffer, output) == EOF)
			break;
	if (ferror(input) || ferror(output) || fflush(output) != 0 ||
	    fsync(descriptor) != 0 || fclose(output) != 0) {
		unlink(path);
		return 1;
	}
	printf("job %s at %lld\n", strrchr(path, '/') + 1, (long long)when);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *name = program_name(argv[0]), *file = NULL;
	char queue = strcmp(name, "batch") == 0 ? 'b' : 'a';
	FILE *input = stdin;
	time_t when;
	int first = 1;
	if (first < argc && strcmp(argv[first], "-l") == 0)
		return first + 1 == argc ? list_jobs() : 2;
	if (first < argc && strcmp(argv[first], "-r") == 0)
		return first + 1 < argc ? remove_jobs(argc, argv, first + 1)
					: 2;
	if (first + 1 < argc && strcmp(argv[first], "-f") == 0) {
		file = argv[first + 1];
		first += 2;
	}
	if (parse_time(argc, argv, first, &when) != 0) {
		fprintf(stderr,
			"usage: %s [-f file] time | at -l | at -r job ...\n",
			name);
		return 2;
	}
	if (file != NULL && (input = fopen(file, "r")) == NULL) {
		fprintf(stderr, "at: %s: %s\n", file, strerror(errno));
		return 1;
	}
	return submit_job(input, when, queue);
}
