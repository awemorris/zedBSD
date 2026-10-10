/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Checks asynchronous CLI responses and externally reaped helpers with their real descriptors.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_backend_media *media;
	char display[4096];
	char response[4096];
	uint32_t request;
	uint32_t returned;
	unsigned tries;
	int descriptor;
	int error;
	int taken;
	int status;
	int owned;
	pid_t child;
	ssize_t bytes;
	FILE *updates;
	FILE *snapshot;
	char label[121];
	char *line;
	size_t capacity;
	unsigned album;
	unsigned records;

	/* The helper inherits a private HOME; its default library remains recoverable on disk. */
	assert(argc == 2);
	setenv("HOME", argv[1], 1);
	snprintf(display, sizeof(display), "%s/wayland-test", argv[1]);
	setenv("WAYLAND_DISPLAY", display, 1);
	media = kl_backend_media_open();
	assert(media != NULL);
	error = kl_backend_media_request(media, KL_BACKEND_MEDIA_LIST, "", -1, &request);
	assert(error == 0);

	/* Home may collect a CLI process first: its real status must still produce a successful snapshot. */
	child = waitpid(-1, &status, 0);
	assert(child > 0);
	owned = kl_backend_media_child(media, (int64_t)child, status);
	assert(owned == 1);
	taken = kl_backend_media_take(media, &returned, &error, &descriptor);
	assert(taken && returned == request && error == 0 && descriptor >= 0);
	bytes = read(descriptor, response, sizeof(response) - 1U);
	assert(bytes > 0);
	response[bytes] = 0;
	assert(strncmp(response, "# keiland-media 1\t", 18U) == 0);
	close(descriptor);

	/* Exercise both pipes beyond their kernel capacity and drain stdout after child exit. */
	updates = tmpfile();
	assert(updates != NULL);
	memset(label, 'x', sizeof(label) - 1U);
	label[sizeof(label) - 1U] = 0;
	for (album = 0U; album < 1000U; album++)
		fprintf(updates, "A\t%032u\t%s\n", album, label);
	fflush(updates);
	error = kl_backend_media_request(media, KL_BACKEND_MEDIA_APPLY, "", fileno(updates), &request);
	fclose(updates);
	assert(error == 0);
	taken = 0;
	for (tries = 0U; tries < 10000U; tries++) {
		kl_backend_media_update(media);
		taken = kl_backend_media_take(media, &returned, &error, &descriptor);
		if (taken)
			break;
		usleep(1000U);
	}

	/* Every large response row must survive nonblocking writes, EOF and delayed reaping. */
	assert(taken && returned == request && error == 0 && descriptor >= 0);
	snapshot = fdopen(descriptor, "r");
	assert(snapshot != NULL);
	line = NULL;
	capacity = 0U;
	records = 0U;
	for (;;) {
		bytes = getline(&line, &capacity, snapshot);
		if (bytes < 0)
			break;
		if (line[0] == 'A')
			records++;
	}

	/* The child cannot publish a successful truncated snapshot. */
	assert(records == 1000U);
	free(line);
	fclose(snapshot);

	/* A malformed list returns CLI errno rather than a protocol disconnect or a false success. */
	error = kl_backend_media_request(media, KL_BACKEND_MEDIA_ADD, "not-an-absolute-path\n", -1, &request);
	assert(error == 0);
	taken = 0;
	for (tries = 0U; tries < 2000U; tries++) {
		kl_backend_media_update(media);
		taken = kl_backend_media_take(media, &returned, &error, &descriptor);
		if (taken)
			break;
		usleep(1000U);
	}

	assert(taken && error != 0 && returned == request);
	if (descriptor >= 0)
		close(descriptor);
	kl_backend_media_close(media);
	printf("PASS media backend: real CLI execution, metadata FD, general child collector ownership, large bidirectional pipes, explicit failure\n");

	/* Succeeded: all owned processes, descriptors and session endpoints were released. */
	/* Succeeded: this fixture operation completed. */
	return 0;
}
