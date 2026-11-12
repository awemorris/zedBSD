/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The guest side of the thermal QEMU test: makes up sensors and strategy
 * receivers through /dev/thermal-inject of a kernel built for tests, and
 * chooses how the next thermal emergency stops the machine.
 *
 *   thermal-inject sensor <name> zone|device <cpu 0|1> <mC> <passive mC> <critical mC> [invalid]
 *   thermal-inject receiver <name> <errno> <delay ms>
 *   thermal-inject path init|direct
 *
 * Exits 0 when the kernel took the request, 1 when it refused, 2 for a
 * command it does not know.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/thermal.h>

static int inject_request(unsigned long request, void *argument);

/*
 * Runs one injection.
 */
int
main(
	int argc,
	char **argv)
{
	struct thermal_inject_sensor sensor;
	struct thermal_inject_receiver receiver;
	struct thermal_inject_path path;
	int status;

	/* A made-up sensor and its reading. */
	if (argc >= 8 && strcmp(argv[1], "sensor") == 0) {
		memset(&sensor, 0, sizeof(sensor));
		(void)snprintf(sensor.name, sizeof(sensor.name), "%s", argv[2]);
		sensor.kind = KERN_THERMAL_KIND_DEVICE;
		if (strcmp(argv[3], "zone") == 0)
			sensor.kind = KERN_THERMAL_KIND_ZONE;
		if (atoi(argv[4]) != 0)
			sensor.flags = KERN_THERMAL_SENSOR_CPU;
		sensor.millicelsius = atoi(argv[5]);
		sensor.passive = atoi(argv[6]);
		sensor.critical = atoi(argv[7]);
		sensor.valid = 1;
		if (argc == 9 && strcmp(argv[8], "invalid") == 0)
			sensor.valid = 0;
		status = inject_request(KERN_THERMAL_INJECT_SENSOR, &sensor);
		return status;
	}

	/* A made-up receiver and its answer. */
	if (argc == 5 && strcmp(argv[1], "receiver") == 0) {
		memset(&receiver, 0, sizeof(receiver));
		(void)snprintf(receiver.name, sizeof(receiver.name), "%s", argv[2]);
		receiver.error = atoi(argv[3]);
		receiver.delay_ms = (uint32_t)atoi(argv[4]);
		status = inject_request(KERN_THERMAL_INJECT_RECEIVER, &receiver);
		return status;
	}

	/* How the next emergency stops the machine. */
	if (argc == 3 && strcmp(argv[1], "path") == 0) {
		memset(&path, 0, sizeof(path));
		path.path = KERN_THERMAL_INJECT_PATH_INIT;
		if (strcmp(argv[2], "direct") == 0)
			path.path = KERN_THERMAL_INJECT_PATH_DIRECT;
		status = inject_request(KERN_THERMAL_INJECT_PATH, &path);
		return status;
	}

	/* A command this tool does not know. */
	fprintf(stderr, "usage: thermal-inject sensor|receiver|path ...\n");
	return 2;
}

/* Sends one request to /dev/thermal-inject and says the outcome. */
static int
inject_request(
	unsigned long request,
	void *argument)
{
	int descriptor;
	int answer;

	/* The node is root's and exists only in a test kernel. */
	descriptor = open("/dev/thermal-inject", O_RDWR | O_CLOEXEC);
	if (descriptor < 0) {
		fprintf(stderr, "thermal-inject: /dev/thermal-inject: %s\n", strerror(errno));
		return 1;
	}

	/* Hands the request over. */
	answer = ioctl(descriptor, request, argument);
	if (answer < 0) {
		fprintf(stderr, "thermal-inject: %s\n", strerror(errno));
		(void)close(descriptor);
		return 1;
	}
	(void)close(descriptor);

	/* Succeeded: the kernel took it. */
	printf("thermal-inject: ok\n");
	return 0;
}
