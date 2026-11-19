/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The service requests of a node: gpu GPU reset (GPU_DEVICE_RESET), which
 * ends every program that uses the device and sets a failed device up
 * again, and gpu GPU diagnose (GPU_DIAGNOSE), the driver's reports as raw
 * bytes, which this build does not make yet: it says so and fails.
 */

#include "gpu-command.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The room of the answer to the reset's question. */
#define GPU_ANSWER_MAX		16U

static int gpu_service_confirm(const struct gpu_invocation *command);

/*
 * Resets a node's device: asks first on a terminal unless --yes, opens the
 * node (root's open of a failed device takes only the reset) and asks the
 * kernel for GPU_DEVICE_RESET, which tells every other open of the loss,
 * waits for them to close and resets the hardware through the driver.
 * The reset's own result is shown, and a reset that did not set the device
 * up again fails.
 */
int
gpu_service_reset(
	const struct gpu_invocation *command)
{
	struct gpu_device_reset request;
	int confirmed;
	int status;
	int error;
	int fd;

	/* The reset ends every program on the device: only a yes goes on. */
	confirmed = gpu_service_confirm(command);
	if (confirmed != GPU_EXIT_OK)
		return confirmed;

	/* The node, as root opens a failed device too. */
	error = gpu_node_open(command->node, &fd);
	if (error != 0) {
		gpu_fail(command, "reset", error, "%s: %s", command->node, strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* The request; a wait of 0 is the kernel's own. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	if (command->wait_given)
		request.wait_ms = command->wait_ms;
	status = ioctl(fd, GPU_DEVICE_RESET, &request);
	error = errno;
	(void)close(fd);

	/* A refused request: not root, a device that cannot reset, another reset. */
	if (status != 0) {
		gpu_fail(command, "reset", error, "%s: the reset was refused: %s", command->node, strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* A reset that ran and did not set the device up again (the other opens did not close in time, or the driver's reset failed). */
	if (request.result != 0) {
		gpu_fail(command,
			 "reset",
			 request.result,
			 "%s: the reset failed: %s (%u ms, %u opens lost the device)",
			 command->node,
			 strerror(request.result),
			 request.milliseconds,
			 request.lost);
		return GPU_EXIT_FAILED;
	}

	/* The answer, for people or for a script. */
	if (command->porcelain) {
		gpu_record_begin("result");
		gpu_field_text("verb", "reset", GPU_NODE_PATH_MAX);
		gpu_field_text("node", command->node, GPU_NODE_PATH_MAX);
		gpu_field_unsigned("milliseconds", request.milliseconds);
		gpu_field_unsigned("resets", request.resets);
		gpu_field_unsigned("lost", request.lost);
		gpu_record_end();
	} else {
		printf("gpu%u (%s): reset in %u ms; %u reset%s since the device was found; %u open%s lost the device\n",
		       command->unit,
		       command->node,
		       request.milliseconds,
		       request.resets,
		       request.resets == 1U ? "" : "s",
		       request.lost,
		       request.lost == 1U ? "" : "s");
	}

	/* Succeeded: the device runs again. */
	return GPU_EXIT_OK;
}

/*
 * Asks on a terminal whether to reset; --yes answers for it, and without a
 * terminal --yes is required.  Returns GPU_EXIT_OK for a yes, or the exit
 * status that ends the command.
 */
static int
gpu_service_confirm(
	const struct gpu_invocation *command)
{
	char answer[GPU_ANSWER_MAX];
	char *line;
	int terminal;

	/* --yes is the answer given in advance. */
	if (command->yes)
		return GPU_EXIT_OK;

	/* A script cannot be asked. */
	terminal = isatty(STDIN_FILENO);
	if (terminal == 0) {
		gpu_fail(command, "reset", EINVAL, "%s: a reset without a terminal needs --yes", command->node);
		return GPU_EXIT_USAGE;
	}

	/* The question, with what the reset ends. */
	printf("Reset gpu%u (%s)? Every program that uses it, the desktop included, loses it. [y/N] ", command->unit, command->node);
	fflush(stdout);
	line = fgets(answer, sizeof(answer), stdin);
	if (line == NULL || (answer[0] != 'y' && answer[0] != 'Y')) {
		gpu_fail(command, "reset", ECANCELED, "%s: not reset", command->node);
		return GPU_EXIT_FAILED;
	}

	/* Succeeded: the person said yes. */
	return GPU_EXIT_OK;
}

/*
 * Lists a driver's report kinds, or reads one report.
 *
 * This build refuses the request.
 */
int
gpu_service_diagnose(
	const struct gpu_invocation *command)
{
	/* Says that the reports are not in this build. */
	gpu_fail(command, "diagnose", ENOTSUP, "diagnose is not available in this build");

	/* A refused request. */
	return GPU_EXIT_FAILED;
}
