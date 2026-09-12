/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Drive the textured Vulkan cuboid with real time or bounded capture samples.
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "renderer.h"

/*
 * One invocation selects ordinary animation or explicit capture
 * checkpoints.
 */
struct demo_options {
	const char *device;
	const char *token;
	uint32_t duration;
	uint32_t hold;
	uint32_t milliseconds;
	int fixed;
	int verify;
};

/*
 * Forward declaration.
 */
static int parse_arguments(int argc, char **argv, struct demo_options *options);
static int parse_number(const char *text, uint32_t maximum, uint32_t *number);
static int validate_token(const char *token);
static int elapsed_milliseconds(const struct timespec *start, uint32_t *milliseconds);
static int pause_milliseconds(uint32_t milliseconds);
static int await_capture(void);
static int draw_frame(const struct demo_options *options, const char *mode, uint32_t sample, uint32_t frame, uint32_t milliseconds);
static int run_checkpoints(const struct demo_options *options, uint32_t *frames);
static int run_animation(const struct demo_options *options, uint32_t *frames);

/*
 * Present a time-driven textured cuboid and release its GPU session on exit.
 */
int
main(
	int argc,
	char **argv)
{
	struct demo_options options;
	uint32_t frames;
	uint32_t command;
	int status;
	int close_status;
	int saved_errno;

	/* Select finite ordinary animation unless the caller requests checkpoints. */
	memset(&options, 0, sizeof(options));
	options.device = "/dev/gpu0";
	options.token = "manual";
	options.duration = 10;
	options.hold = 10;
	frames = 0;

	/* Reject malformed options before opening a GPU session. */
	status = parse_arguments(argc, argv, &options);
	if (status != 0) {
		fprintf(
			stderr,
			"usage: vkdemo [--device=PATH] [--token=NAME] [--duration=0..3600 | --time-ms=0..3600000 --hold=0..120 | --verify-session]\n");
		return 2;
	}

	/* Make setup failures distinguishable from an absent process. */
	printf("VKDEMO START run=%s width=320 height=240\n", options.token);
	fflush(stdout);

	/* Create the Vulkan shaders, texture and retained frame resources. */
	status = vkdemo_initialize(options.device);
	if (status != 0)
		goto out;

	/* Exercise fixed and live checkpoints through the ordinary draw function. */
	if (options.verify != 0) {
		status = run_checkpoints(&options, &frames);
	} else {
		status = run_animation(&options, &frames);
	}

out:
	/* Preserve the rendering error while consuming all session resources. */
	saved_errno = errno;
	close_status = vkdemo_close();
	if (status != 0) {
		errno = saved_errno;
		perror("vkdemo");
		command = vkdemo_active_command();
		fprintf(
			stderr,
			"VKDEMO FAILED run=%s command=%u frames=%u\n",
			options.token,
			command,
			frames);
		return 1;
	}

	/* Treat a failed device close as a failed lifecycle check. */
	if (close_status != 0) {
		perror("vkdemo: close");
		return 1;
	}

	/* Publish completion only after the last capture and resource release. */
	printf("VKDEMO DONE run=%s frames=%u\n", options.token, frames);
	fflush(stdout);

	/* Succeeded: every requested frame was rendered and the session is closed. */
	return 0;
}

/* Decode decimal options without accepting signs, overflow or trailing text. */
static int
parse_number(
	const char *text,
	uint32_t maximum,
	uint32_t *number)
{
	uint32_t accumulated;
	uint32_t digit;

	/* Refuse an option with no numeric payload. */
	if (*text == '\0')
		return -1;

	/* Bound the accumulator before accepting each decimal digit. */
	accumulated = 0;
	while (*text != '\0') {
		/* Keep option syntax independent of locale and signed conversion. */
		if (*text < '0')
			return -1;

		/* Reject characters above the decimal digit range. */
		if (*text > '9')
			return -1;

		/* Reject values that cannot fit within this option's contract. */
		digit = (uint32_t)(*text - '0');
		if (accumulated > maximum / 10U)
			return -1;

		/* Check the last decimal place when the preceding digits reach the limit. */
		if (accumulated == maximum / 10U) {
			/* Reject a final digit beyond the option's maximum remainder. */
			if (digit > maximum % 10U)
				return -1;
		}

		/* Advance only after this digit has passed the bound. */
		accumulated = accumulated * 10U + digit;
		text++;
	}

	/* Succeeded: expose the fully validated decimal argument. */
	*number = accumulated;
	return 0;
}

/* Keep the run identifier unambiguous inside one whitespace-delimited marker. */
static int
validate_token(
	const char *token)
{
	uint32_t index;
	char character;

	/* Require a nonempty identifier for capture correlation. */
	if (*token == '\0')
		return -1;

	/* Accept a bounded portable identifier without control characters. */
	for (index = 0; token[index] != '\0'; index++) {
		/* Leave enough room for host-side filenames and line parsers. */
		if (index >= 64U)
			return -1;

		/* Letters and digits carry the ordinary attempt identifier. */
		character = token[index];
		if (character >= 'a') {
			/* Accept a lowercase ASCII letter within its upper bound. */
			if (character <= 'z')
				continue;
		}

		/* Accept uppercase identifiers without locale translation. */
		if (character >= 'A') {
			/* Accept an uppercase ASCII letter within its upper bound. */
			if (character <= 'Z')
				continue;
		}

		/* Accept numeric attempt suffixes. */
		if (character >= '0') {
			/* Accept a decimal digit within its upper bound. */
			if (character <= '9')
				continue;
		}

		/* Permit only punctuation that cannot split a marker field. */
		if (character == '-')
			continue;

		/* Underscores join components without splitting marker fields. */
		if (character == '_')
			continue;

		/* A period is the last accepted identifier punctuation. */
		if (character != '.')
			return -1;
	}

	/* Succeeded: the identifier is safe to embed verbatim in capture markers. */
	return 0;
}

/* Select one ordinary animation or explicitly requested checkpoint mode. */
static int
parse_arguments(
	int argc,
	char **argv,
	struct demo_options *options)
{
	int index;
	int match;
	int status;
	int duration_set;
	int hold_set;

	/* Track incompatible options independently of their default values. */
	duration_set = 0;
	hold_set = 0;

	/* Interpret each option without acquiring hardware resources. */
	for (index = 1; index < argc; index++) {
		/* Let the caller choose an ordinary GPU device node. */
		match = strncmp(argv[index], "--device=", 9);
		if (match == 0) {
			options->device = argv[index] + 9;
			continue;
		}

		/* Correlate serial markers with the caller's capture attempt. */
		match = strncmp(argv[index], "--token=", 8);
		if (match == 0) {
			options->token = argv[index] + 8;
			continue;
		}

		/* Bound ordinary runs, with zero explicitly requesting continuous use. */
		match = strncmp(argv[index], "--duration=", 11);
		if (match == 0) {
			status = parse_number(argv[index] + 11, 3600, &options->duration);
			if (status != 0)
				return -1;

			/* Record that this duration was explicitly chosen. */
			duration_set = 1;
			continue;
		}

		/* Freeze a reproducible shader time while preserving normal drawing. */
		match = strncmp(argv[index], "--time-ms=", 10);
		if (match == 0) {
			status = parse_number(argv[index] + 10, 3600000, &options->milliseconds);
			if (status != 0)
				return -1;

			/* Distinguish fixed zero from ordinary live time. */
			options->fixed = 1;
			continue;
		}

		/* Retain a fixed frame for a bounded manual screenshot interval. */
		match = strncmp(argv[index], "--hold=", 7);
		if (match == 0) {
			status = parse_number(argv[index] + 7, 120, &options->hold);
			if (status != 0)
				return -1;

			/* Limit capture holding to the explicit fixed-time mode. */
			hold_set = 1;
			continue;
		}

		/* Run the documented six-checkpoint capture protocol. */
		match = strcmp(argv[index], "--verify-session");
		if (match == 0) {
			options->verify = 1;
			continue;
		}

		/* Refuse unknown options instead of silently changing the test. */
		return -1;
	}

	/* A fixed-time request cannot also select a live duration. */
	if (options->fixed != 0) {
		/* Explicit live duration conflicts with a frozen shader time. */
		if (duration_set != 0)
			return -1;
	}

	/* A manual hold is meaningful only for a fixed frame. */
	if (hold_set != 0) {
		/* A live run owns its duration rather than a fixed capture hold. */
		if (options->fixed == 0)
			return -1;
	}

	/* The checkpoint sequence owns its own times and acknowledgment waits. */
	if (options->verify != 0) {
		/* The checkpoint sequence supplies its own three fixed shader times. */
		if (options->fixed != 0)
			return -1;

		/* Checkpoint completion is acknowledgment-driven rather than duration-driven. */
		if (duration_set != 0)
			return -1;

		/* Checkpoints already retain each image until its bounded acknowledgment. */
		if (hold_set != 0)
			return -1;
	}

	/* Refuse an empty path before invoking open. */
	if (*options->device == '\0')
		return -1;

	/* Validate the printable identity used by every emitted capture marker. */
	status = validate_token(options->token);
	if (status != 0)
		return -1;

	/* Succeeded: the invocation selects one coherent drawing mode. */
	return 0;
}

/* Measure animation time without wall-clock adjustments. */
static int
elapsed_milliseconds(
	const struct timespec *start,
	uint32_t *milliseconds)
{
	struct timespec now;
	int64_t elapsed;
	int status;

	/* Sample the same monotonic source that established the animation epoch. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return -1;

	/* Preserve millisecond ordering when the nanosecond field wraps a second. */
	elapsed = (int64_t)(now.tv_sec - start->tv_sec) * 1000000000;
	elapsed += now.tv_nsec - start->tv_nsec;
	if (elapsed < 0) {
		errno = EOVERFLOW;
		return -1;
	}

	/* Round only after combining seconds and nanoseconds across a second boundary. */
	elapsed /= 1000000;
	if (elapsed > 0xffffffffU) {
		errno = EOVERFLOW;
		return -1;
	}

	/* Succeeded: return the shader time represented in the public marker. */
	*milliseconds = (uint32_t)elapsed;
	return 0;
}

/* Retain a displayed frame without busy-waiting. */
static int
pause_milliseconds(
	uint32_t milliseconds)
{
	struct timespec delay;
	struct timespec remaining;
	int status;

	/* Retry only the remaining interval after an interrupt. */
	delay.tv_sec = milliseconds / 1000U;
	delay.tv_nsec = (milliseconds % 1000U) * 1000000U;
	for (;;) {
		/* Yield the process while the displayed image remains retained. */
		status = nanosleep(&delay, &remaining);
		if (status == 0)
			break;

		/* Propagate real timer failures without pretending the hold completed. */
		if (errno != EINTR)
			return -1;

		/* Continue exactly the unfinished portion of the requested hold. */
		delay = remaining;
	}

	/* Succeeded: the requested display interval has elapsed. */
	return 0;
}

/* Wait at most thirty seconds for the host to finish capturing this frame. */
static int
await_capture(
	void)
{
	struct timespec start;
	struct pollfd input;
	uint32_t elapsed;
	char character;
	ssize_t bytes;
	int status;

	/* Establish a total deadline that cannot be extended by partial input. */
	status = clock_gettime(CLOCK_MONOTONIC, &start);
	if (status != 0)
		return -1;

	/* Observe only the documented acknowledgment channel. */
	memset(&input, 0, sizeof(input));
	input.fd = STDIN_FILENO;
	input.events = POLLIN;

	/* Accept one newline while honoring the same absolute timeout. */
	for (;;) {
		/* Recompute the remaining deadline after signals or carriage returns. */
		status = elapsed_milliseconds(&start, &elapsed);
		if (status != 0)
			return -1;

		/* A missing capture acknowledgment is a failed diagnostic session. */
		if (elapsed >= 30000U) {
			errno = ETIMEDOUT;
			return -1;
		}

		/* Sleep until stdin is readable or the capture deadline expires. */
		status = poll(&input, 1, (int)(30000U - elapsed));
		if (status < 0) {
			/* Signals may interrupt the wait without restarting its deadline. */
			if (errno == EINTR)
				continue;

			/* Preserve an actual polling failure for the caller. */
			return -1;
		}

		/* Let the deadline branch report an expired wait consistently. */
		if (status == 0)
			continue;

		/* Require readable bytes rather than accepting hangup as approval. */
		if ((input.revents & POLLIN) == 0) {
			errno = EPIPE;
			return -1;
		}

		/* Consume only one byte so that each checkpoint needs its own newline. */
		bytes = read(STDIN_FILENO, &character, 1);
		if (bytes != 1) {
			errno = EPIPE;
			return -1;
		}

		/* End this checkpoint only on the documented acknowledgment. */
		if (character == '\n')
			break;

		/* Permit CRLF terminals without accepting arbitrary response text. */
		if (character != '\r') {
			errno = EINVAL;
			return -1;
		}
	}

	/* Succeeded: the host has released this retained capture checkpoint. */
	return 0;
}

/* Emit the identity of the actual GPU pixels after scanout succeeds. */
static int
draw_frame(
	const struct demo_options *options,
	const char *mode,
	uint32_t sample,
	uint32_t frame,
	uint32_t milliseconds)
{
	char digest[65];
	int status;

	/* Use the same Vulkan draw and readback for every mode. */
	status = vkdemo_render(milliseconds, frame, digest);
	if (status != 0)
		return -1;

	/* Publish the RGB hash of the bytes that were just presented. */
	printf(
		"VKDEMO PRESENT run=%s mode=%s sample=%u frame=%u time_ms=%u rgb_sha256=%s width=320 height=240\n",
		options->token,
		mode,
		sample,
		frame,
		milliseconds,
		digest);
	fflush(stdout);

	/* Succeeded: the capture marker names a completed visible GPU frame. */
	return 0;
}

/* Reuse one session across fixed-angle and live-time capture checkpoints. */
static int
run_checkpoints(
	const struct demo_options *options,
	uint32_t *frames)
{
	static const uint32_t fixed_times[3] = {0, 1000, 2500};
	struct timespec live_start;
	uint32_t sample;
	uint32_t milliseconds;
	int status;

	/* Present the three reproducible angles required by the texture oracle. */
	for (sample = 0; sample < 3U; sample++) {
		/* Keep all fixed checkpoints on the same ordinary draw path. */
		status = draw_frame(options, "fixed", sample + 1U, *frames + 1U, fixed_times[sample]);
		if (status != 0)
			return -1;

		/* Count only frames whose GPU readback and scanout both completed. */
		(*frames)++;

		/* Retain this exact image until the host records and checks it. */
		status = await_capture();
		if (status != 0)
			return -1;
	}

	/* Exclude shader setup and fixed capture pauses from live animation time. */
	status = clock_gettime(CLOCK_MONOTONIC, &live_start);
	if (status != 0)
		return -1;

	/* Let real elapsed time, including capture pauses, drive three live frames. */
	for (sample = 0; sample < 3U; sample++) {
		/* Pass the observed clock time unchanged to the shader and marker. */
		status = elapsed_milliseconds(&live_start, &milliseconds);
		if (status != 0)
			return -1;

		/* Reuse the retained texture, descriptors and buffers for this frame. */
		status = draw_frame(options, "live", sample + 1U, *frames + 1U, milliseconds);
		if (status != 0)
			return -1;

		/* Advance capture identity only after successful presentation. */
		(*frames)++;

		/* Keep the actual live frame stable while the host captures it. */
		status = await_capture();
		if (status != 0)
			return -1;
	}

	/* Succeeded: one session survived all six real Vulkan presentations. */
	return 0;
}

/* Animate continuously for the requested duration or hold one explicit time. */
static int
run_animation(
	const struct demo_options *options,
	uint32_t *frames)
{
	struct timespec start;
	uint32_t milliseconds;
	int status;

	/* A fixed-time request draws once and retains those actual GPU pixels. */
	if (options->fixed != 0) {
		status = draw_frame(options, "fixed", 1, 1, options->milliseconds);
		if (status != 0)
			return -1;

		/* Record the one completed presentation before waiting for manual capture. */
		*frames = 1;
		status = pause_milliseconds(options->hold * 1000U);
		if (status != 0)
			return -1;
	} else {
		/* Start ordinary animation after all expensive resource creation. */
		status = clock_gettime(CLOCK_MONOTONIC, &start);
		if (status != 0)
			return -1;

		/* Render fresh clock-driven frames until the requested interval ends. */
		for (;;) {
			/* Observe elapsed time instead of assuming a fixed rendering rate. */
			status = elapsed_milliseconds(&start, &milliseconds);
			if (status != 0)
				return -1;

			/* A finite run ends only after at least one frame was presented. */
			if (options->duration != 0) {
				/* Preserve at least one frame even when setup or scheduling runs long. */
				if (milliseconds >= options->duration * 1000U) {
					/* A completed presentation satisfies the finite animation request. */
					if (*frames != 0)
						break;
				}
			}

			/* Keep frame identities nonzero for scanout and evidence correlation. */
			if (*frames == 0xffffffffU) {
				errno = EOVERFLOW;
				return -1;
			}

			/* Render the current orientation with the retained Vulkan resources. */
			status = draw_frame(options, "live", *frames + 1U, *frames + 1U, milliseconds);
			if (status != 0)
				return -1;

			/* Count only completed visible frames. */
			(*frames)++;

			/* Yield briefly without promising a fixed rendering frame rate. */
			status = pause_milliseconds(20);
			if (status != 0)
				return -1;
		}
	}

	/* Succeeded: the requested ordinary animation or fixed capture completed. */
	return 0;
}
