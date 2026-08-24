/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRON_SPOOL "/var/spool/cron"

int
main(int argc, char **argv)
{
	char path[256], temporary[288], buffer[1024];
	FILE *input, *output;
	int descriptor;
	snprintf(path, sizeof(path), "%s/%lu", CRON_SPOOL,
		 (unsigned long)getuid());
	if (argc == 2 && strcmp(argv[1], "-l") == 0) {
		input = fopen(path, "r");
		if (input == NULL)
			return 1;
		while (fgets(buffer, sizeof(buffer), input) != NULL)
			fputs(buffer, stdout);
		return ferror(input) || fclose(input) != 0;
	}
	if (argc == 2 && strcmp(argv[1], "-r") == 0)
		return unlink(path) != 0;
	if (argc > 2) {
		fprintf(stderr, "usage: crontab [-l|-r|file]\n");
		return 2;
	}
	input = argc == 2 ? fopen(argv[1], "r") : stdin;
	if (input == NULL) {
		fprintf(stderr, "crontab: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	(void)mkdir("/var/spool", 0755);
	(void)mkdir(CRON_SPOOL, 01777);
	snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
		 (long)getpid());
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0 || (output = fdopen(descriptor, "w")) == NULL)
		return 1;
	while (fgets(buffer, sizeof(buffer), input) != NULL) {
		if (strchr(buffer, '\n') == NULL && !feof(input)) {
			fprintf(stderr, "crontab: line too long\n");
			unlink(temporary);
			return 1;
		}
		if (fputs(buffer, output) == EOF)
			break;
	}
	if (ferror(input) || ferror(output) || fflush(output) != 0 ||
	    fsync(descriptor) != 0 || fclose(output) != 0 ||
	    rename(temporary, path) != 0) {
		unlink(temporary);
		return 1;
	}
	if (input != stdin)
		fclose(input);
	return 0;
}
