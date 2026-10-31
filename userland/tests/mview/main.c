/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mview program: shows a converted model in a Wayland window and lets
 * the pointer and keyboard turn, move and zoom it.
 */

#include "mview.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The model shown when --model is not given. */
#define MAIN_DEFAULT_MODEL	KEILAND_DATADIR "/mview/qs40"

/* How long an idle viewer waits for input before looking again, in milliseconds. */
#define MAIN_IDLE_WAIT		16

/* The turn of the spin demonstration, in degrees per second (one revolution in ten seconds). */
#define MAIN_SPIN_RATE		36.0

/* The most copies of the model (--copies) and submissions of a frame (--submits). */
#define MAIN_COPIES_MAX		65536U
#define MAIN_SUBMITS_MAX	4096U

/* The most swapchain images (--images). */
#define MAIN_IMAGES_MAX		8U

/* How many consecutive out-of-date swapchains are tolerated before giving up. */
#define MAIN_RECREATE_LIMIT	16U

/* A gap between presented frames the spin reports as a hitch (two refreshes at 60 Hz and more), and the hitches named one by one. */
#define MAIN_HITCH_MS		33.0
#define MAIN_HITCH_LINES	20U

/* One invocation's display, model, log token and run bounds. */
struct main_options {
	const char *display;
	const char *model;
	const char *token;
	uint32_t frames;
	uint32_t timeout;

	/* Seconds of automatic turning about the vertical axis, with the frame rate measured; zero for none. */
	uint32_t spin;

	/* Nonzero for per-pixel lighting (--shading=pixel); the default lights per vertex. */
	int pixel_shading;

	/* Nonzero for a window (--windowed) instead of fullscreen, and the size it asks for (--size). */
	int windowed;
	uint32_t width;
	uint32_t height;

	/* The content the window says it shows (--content, a wp_content_type_v1 type; none by default). */
	uint32_t content;

	/* The presentation mode asked for (--present, FIFO by default). */
	VkPresentModeKHR present;

	/* How many copies of the model each frame draws (--copies, one by default). */
	uint32_t copies;

	/* Nonzero when each copy is its own draws (--draws=each) instead of one instanced draw a group. */
	int each_draw;

	/* How many submissions draw a frame (--submits, one by default). */
	uint32_t submits;

	/* How many frames may be in flight (--inflight, one by default). */
	uint32_t inflight;

	/* Nonzero when every triangle is culled (--cull=all), for measuring the vertex work alone. */
	int cull_all;

	/* The swapchain's image count (--images), 0 for the viewer's own choice (at least three). */
	uint32_t images;
};

/* The frame-rate measurement of the spin demonstration. */
struct main_spin {
	/* When the first spinning frame was drawn, and when the latest one was presented. */
	struct timespec start;
	struct timespec last;

	/* The yaw the spin started from and the one it applied last. */
	double base_yaw;
	double applied;

	/* The frames presented after a long gap (MAIN_HITCH_MS or more, past the first two frames). */
	uint32_t hitches;

	/* Presented frames in all and in the current whole second, and that second's number. */
	uint32_t frames;
	uint32_t second_frames;
	uint32_t second;

	/* The shortest, longest and summed time between presented frames, in milliseconds. */
	double shortest;
	double longest;
	double total;

	/* Nonzero once the first spinning frame was drawn. */
	int started;
};

static int options_parse(int argc, char **argv, struct main_options *options);
static int option_number(const char *text, uint32_t maximum, uint32_t *number);
static int option_token(const char *token);
static int option_size(const char *text, uint32_t *width, uint32_t *height);
static int option_content(const char *text, uint32_t *content);
static int option_present(const char *text, VkPresentModeKHR *present);
static const char *main_present_name(VkPresentModeKHR present);
static int main_expired(const struct timespec *start, uint32_t timeout);
static void main_frame_log(const char *token, uint32_t frame, const struct mview_camera *camera);
static double main_seconds(const struct timespec *from, const struct timespec *to);
static void main_spin_turn(struct main_spin *spin, struct mview_camera *camera);
static int main_spin_presented(struct main_spin *spin, const char *token, uint32_t duration);

/*
 * Loads the model, opens the window and renderer, and runs until quit.
 *
 * Every outcome is one machine-readable line: MVIEW DONE on a clean end,
 * MVIEW FAILED naming the operation that failed otherwise.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct mview_model model;
	struct mview_camera camera;
	struct mview_input input;
	struct mview_window window;
	struct main_spin spin;
	struct mview_renderer renderer;
	struct timespec start;
	const char *operation;
	const char *reason;
	uint32_t completed;
	uint32_t hitches;
	uint32_t stale;
	int redraw;
	int status;
	VkResult result;
	VkResult cleanup;

	/* Nothing is owned before the first step, so cleanup is always safe. */
	memset(&model, 0, sizeof(model));
	memset(&camera, 0, sizeof(camera));
	memset(&window, 0, sizeof(window));
	memset(&renderer, 0, sizeof(renderer));

	/* Argument failure starts no connection or GPU namespace. */
	status = options_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: mview [--display=NAME] [--model=DIR] [--token=NAME] [--frames=N] [--timeout-s=N] [--spin=SECONDS] [--shading=vertex|pixel] [--windowed] [--size=WxH] [--content=none|video|game] [--present=fifo|mailbox|immediate] [--copies=N] [--draws=instanced|each] [--submits=N] [--inflight=N] [--cull=all] [--images=N]\n");
		return 2;
	}

	/* The run's deadline counts from here. */
	clock_gettime(CLOCK_MONOTONIC, &start);

	/* Reads the model before any window, so a bad model fails fast. */
	completed = 0U;
	result = VK_SUCCESS;
	reason = "quit";
	operation = "mview_model_load";
	status = mview_model_load(&model, options.model);
	if (status != 0) {
		fprintf(stderr, "MVIEW MODEL ERROR run=%s %s\n", options.token, model.error);
		goto cleanup;
	}

	/* Publishes what is about to be shown; the per-pixel shading says so at the end. */
	printf(
		"MVIEW START run=%s model=%s vertices=%u triangles=%u materials=%u textures=%u%s\n",
		options.token,
		options.model,
		model.vertex_count,
		model.triangle_count,
		model.material_count,
		model.texture_count,
		options.pixel_shading != 0 ? " shading=pixel" : "");
	fflush(stdout);

	/* Input events apply to the camera from the first dispatch on. */
	mview_input_init(&input, &camera, options.token, 0U);

	/* Creates the configured fullscreen window and binds the seat if there is one. */
	operation = "mview_window_open";
	status = mview_window_open(&window, options.display, &input, options.width, options.height, !options.windowed, options.content);
	if (status != 0)
		goto cleanup;

	/* The size the window was configured to, which the compositor's bounds may have cut (for the harness). */
	printf("MVIEW WINDOW run=%s width=%u height=%u bounds=%ux%u\n", options.token, window.width, window.height, window.bounds_width, window.bounds_height);
	fflush(stdout);

	/* Creates renderer ownership only after the native surface is configured. */
	result = mview_renderer_open(&renderer, &window, options.pixel_shading, options.present, options.copies, options.each_draw, options.submits,
				     options.inflight, options.cull_all, options.images);
	operation = renderer.operation;
	if (result != VK_SUCCESS) {
		status = -1;
		goto cleanup;
	}

	/* The presentation mode the swapchain took, the copies drawn and the content said (for the harness). */
	printf(
		"MVIEW PRESENT run=%s asked=%s mode=%s format=%u images=%u copies=%u draws=%s submits=%u inflight=%u content=%u content_manager=%d\n",
		options.token,
		main_present_name(options.present),
		main_present_name(renderer.present_mode),
		(unsigned)renderer.format,
		renderer.count,
		options.copies,
		options.each_draw != 0 ? "each" : "instanced",
		options.submits,
		options.inflight,
		options.content,
		window.content_manager != NULL ? 1 : 0);
	fflush(stdout);

	/* Uploads geometry and textures. */
	result = mview_renderer_load(&renderer, &model);
	operation = renderer.operation;
	if (result != VK_SUCCESS) {
		status = -1;
		goto cleanup;
	}

	/* The first view frames the whole model from the front. */
	mview_camera_fit(&camera, model.minimum, model.maximum, renderer.extent.width, renderer.extent.height);
	input.height = renderer.extent.height;

	/*
	 * Draws on demand: the first frame, after every view change and after a
	 * resize.  A frame count makes every iteration draw so the count advances.
	 */
	redraw = 1;
	stale = 0U;
	memset(&spin, 0, sizeof(spin));
	for (;;) {
		/* An idle viewer sleeps on the connection instead of spinning. */
		operation = "mview_window_dispatch";
		if (redraw != 0 || options.frames != 0U || options.spin != 0U) {
			status = mview_window_dispatch(&window, 0);
		} else {
			status = mview_window_dispatch(&window, MAIN_IDLE_WAIT);
		}

		/* A broken compositor connection ends the run as a failure. */
		if (status != 0)
			break;

		/* The compositor closing the window ends the run normally. */
		if (window.closed != 0) {
			reason = "closed";
			break;
		}

		/* A quit key ends the run after the events it arrived with. */
		mview_input_flush(&input);
		if (input.quit != 0) {
			reason = "quit";
			break;
		}

		/* The deadline ends the run normally. */
		status = main_expired(&start, options.timeout);
		if (status != 0) {
			status = 0;
			reason = "timeout";
			break;
		}

		/* A new configured size replaces the swapchain; the camera is kept. */
		if (window.resized != 0) {
			window.resized = 0;
			result = mview_renderer_recreate(&renderer, window.width, window.height);
			operation = renderer.operation;
			if (result != VK_SUCCESS) {
				status = -1;
				break;
			}

			input.height = renderer.extent.height;
			redraw = 1;
		}

		/* A changed view needs a new frame. */
		if (input.changed != 0)
			redraw = 1;

		/* The spin demonstration turns the model by the time since it began and draws every frame. */
		if (options.spin != 0U) {
			if (spin.started == 0) {
				/* The stages are reported over the spin; the frames before it are dropped. */
				memset(renderer.stage_cycles, 0, sizeof(renderer.stage_cycles));
				renderer.stage_frames = 0U;
				mview_renderer_span(&renderer, 0);
			}

			main_spin_turn(&spin, &camera);
			redraw = 1;
		}

		/* Nothing to draw: wait for the next event. */
		if (redraw == 0 && options.frames == 0U)
			continue;

		/* Draws and presents the current view. */
		result = mview_renderer_draw(&renderer, &model, &camera);
		operation = renderer.operation;
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			/* Nothing was presented; a bounded number of replacements is tried. */
			stale++;
			if (stale > MAIN_RECREATE_LIMIT) {
				status = -1;
				break;
			}

			result = mview_renderer_recreate(&renderer, window.width, window.height);
			operation = renderer.operation;
			if (result != VK_SUCCESS) {
				status = -1;
				break;
			}

			input.height = renderer.extent.height;
			continue;
		}

		/* Any other failure ends the run. */
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			status = -1;
			break;
		}

		/* The frame was presented; the first one and each changed one are logged. */
		stale = 0U;
		completed++;
		if (completed == 1U || input.changed != 0)
			main_frame_log(options.token, completed, &camera);

		/* The presented frame shows every change applied so far. */
		input.changed = 0;
		redraw = 0;

		/* A presented frame on a mismatched chain replaces the chain before the next one. */
		if (result == VK_SUBOPTIMAL_KHR) {
			result = mview_renderer_recreate(&renderer, window.width, window.height);
			operation = renderer.operation;
			if (result != VK_SUCCESS) {
				status = -1;
				break;
			}

			input.height = renderer.extent.height;
			redraw = 1;
		}

		/* A frame count ends the run once reached. */
		if (options.frames != 0U && completed >= options.frames) {
			reason = "frames";
			break;
		}

		/* The spin demonstration counts the frame and ends once its time is up. */
		if (options.spin != 0U) {
			hitches = spin.hitches;
			status = main_spin_presented(&spin, options.token, options.spin);
			if (spin.hitches != hitches && spin.hitches <= MAIN_HITCH_LINES) {
				/* The stages of the frame before the gap, in millions of counter cycles. */
				printf("MVIEW HITCH STAGES run=%s acquire=%.2f record=%.2f submit=%.2f present=%.2f fence=%.2f\n",
				    options.token,
				    (double)renderer.stage_last[MVIEW_STAGE_ACQUIRE] / 1e6,
				    (double)renderer.stage_last[MVIEW_STAGE_RECORD] / 1e6,
				    (double)renderer.stage_last[MVIEW_STAGE_SUBMIT] / 1e6,
				    (double)renderer.stage_last[MVIEW_STAGE_PRESENT] / 1e6,
				    (double)renderer.stage_last[MVIEW_STAGE_FENCE] / 1e6);
				fflush(stdout);
			}
			if (status != 0) {
				mview_renderer_span(&renderer, 1);
				mview_renderer_report_stages(&renderer, options.token, main_seconds(&spin.start, &spin.last));
				status = 0;
				reason = "spin";
				break;
			}
		}
	}

cleanup:
	/* Cleanup never overwrites the operation that explains a failed step. */
	cleanup = mview_renderer_close(&renderer);
	mview_window_close(&window);
	mview_model_free(&model);
	if (status != 0 ||
	    result != VK_SUCCESS ||
	    cleanup != VK_SUCCESS) {
		fprintf(
			stderr,
			"MVIEW FAILED run=%s api=%s result=%d cleanup=%d errno=%d frames=%u\n",
			options.token,
			operation,
			(int)result,
			(int)cleanup,
			errno,
			completed);
		return 1;
	}

	/* Reports the run only after every owner has been released. */
	printf("MVIEW DONE run=%s frames=%u reason=%s\n", options.token, completed, reason);
	fflush(stdout);

	/* Succeeded: the run ended by request and released everything. */
	return 0;
}

/* Parses the display, model, token and run bounds. */
static int
options_parse(
	int argc,
	char **argv,
	struct main_options *options)
{
	int index;
	int match;
	int status;

	/* Without options the viewer shows the installed model until quit. */
	memset(options, 0, sizeof(*options));
	options->model = MAIN_DEFAULT_MODEL;
	options->token = "manual";
	options->width = 640U;
	options->height = 480U;
	options->content = WP_CONTENT_TYPE_V1_TYPE_NONE;
	options->present = VK_PRESENT_MODE_FIFO_KHR;
	options->copies = 1U;
	options->submits = 1U;
	options->inflight = 1U;

	/* Applies each explicit option in command-line order. */
	for (index = 1; index < argc; index++) {
		/* An explicit display name cannot be empty. */
		match = strncmp(argv[index], "--display=", 10U);
		if (match == 0) {
			options->display = argv[index] + 10U;
			if (*options->display == '\0')
				return -1;

			/* Immutable argv storage keeps the name valid through setup. */
			continue;
		}

		/* An explicit model directory cannot be empty. */
		match = strncmp(argv[index], "--model=", 8U);
		if (match == 0) {
			options->model = argv[index] + 8U;
			if (*options->model == '\0')
				return -1;

			/* The directory is checked when the model is read. */
			continue;
		}

		/* The token labels every log line and must be a plain word. */
		match = strncmp(argv[index], "--token=", 8U);
		if (match == 0) {
			options->token = argv[index] + 8U;
			status = option_token(options->token);
			if (status != 0)
				return -1;

			/* The accepted token is used unchanged. */
			continue;
		}

		/* Zero frames means until quit; a positive count draws continuously. */
		match = strncmp(argv[index], "--frames=", 9U);
		if (match == 0) {
			status = option_number(argv[index] + 9U, 1000000U, &options->frames);
			if (status != 0)
				return -1;

			/* The bound applies from the first frame. */
			continue;
		}

		/* Zero seconds means no deadline. */
		match = strncmp(argv[index], "--spin=", 7U);
		if (match == 0) {
			status = option_number(argv[index] + 7U, 3600U, &options->spin);
			if (status != 0)
				return status;
			continue;
		}

		/* The lighting: per vertex (the default) or per pixel. */
		match = strncmp(argv[index], "--shading=", 10U);
		if (match == 0) {
			match = strcmp(argv[index] + 10U, "pixel");
			if (match == 0) {
				options->pixel_shading = 1;
				continue;
			}

			/* Anything but the two names is refused. */
			match = strcmp(argv[index] + 10U, "vertex");
			if (match != 0)
				return -1;
			options->pixel_shading = 0;
			continue;
		}

		/* A window the compositor places, not fullscreen. */
		match = strcmp(argv[index], "--windowed");
		if (match == 0) {
			options->windowed = 1;
			continue;
		}

		/* The size the window asks for when the compositor leaves it to the viewer. */
		match = strncmp(argv[index], "--size=", 7U);
		if (match == 0) {
			status = option_size(argv[index] + 7U, &options->width, &options->height);
			if (status != 0)
				return -1;
			continue;
		}

		/* The content the window says it shows. */
		match = strncmp(argv[index], "--content=", 10U);
		if (match == 0) {
			status = option_content(argv[index] + 10U, &options->content);
			if (status != 0)
				return -1;
			continue;
		}

		/* The presentation mode asked of the swapchain. */
		match = strncmp(argv[index], "--present=", 10U);
		if (match == 0) {
			status = option_present(argv[index] + 10U, &options->present);
			if (status != 0)
				return -1;
			continue;
		}

		/* The copies of the model drawn each frame, at least one. */
		match = strncmp(argv[index], "--copies=", 9U);
		if (match == 0) {
			status = option_number(argv[index] + 9U, MAIN_COPIES_MAX, &options->copies);
			if (status != 0)
				return -1;
			if (options->copies == 0U)
				return -1;
			continue;
		}

		/* One instanced draw a material group, or each copy its own draws. */
		match = strcmp(argv[index], "--draws=instanced");
		if (match == 0) {
			options->each_draw = 0;
			continue;
		}
		match = strcmp(argv[index], "--draws=each");
		if (match == 0) {
			options->each_draw = 1;
			continue;
		}

		/* Every triangle culled, for measuring the vertex work alone. */
		match = strcmp(argv[index], "--cull=all");
		if (match == 0) {
			options->cull_all = 1;
			continue;
		}

		/* The frames in flight, one or two. */
		match = strncmp(argv[index], "--inflight=", 11U);
		if (match == 0) {
			status = option_number(argv[index] + 11U, MVIEW_INFLIGHT_MAX, &options->inflight);
			if (status != 0)
				return -1;
			if (options->inflight == 0U)
				return -1;
			continue;
		}

		/* The submissions of a frame, at least one. */
		match = strncmp(argv[index], "--submits=", 10U);
		if (match == 0) {
			status = option_number(argv[index] + 10U, MAIN_SUBMITS_MAX, &options->submits);
			if (status != 0)
				return -1;
			if (options->submits == 0U)
				return -1;
			continue;
		}

		/* The swapchain's image count, two to eight. */
		match = strncmp(argv[index], "--images=", 9U);
		if (match == 0) {
			status = option_number(argv[index] + 9U, MAIN_IMAGES_MAX, &options->images);
			if (status != 0)
				return -1;
			if (options->images < 2U)
				return -1;
			continue;
		}

		/* Zero seconds means no deadline. */
		match = strncmp(argv[index], "--timeout-s=", 12U);
		if (match == 0) {
			status = option_number(argv[index] + 12U, 86400U, &options->timeout);
			if (status != 0)
				return -1;

			/* The deadline counts from program start. */
			continue;
		}

		/* Unknown options cannot silently change the intended run. */
		return -1;
	}

	/* Succeeded: every option has a finite, understood meaning. */
	return 0;
}

/* Parses WIDTHxHEIGHT, each from 64 to 4096. */
static int
option_size(
	const char *text,
	uint32_t *width,
	uint32_t *height)
{
	char *end;
	unsigned long parsed;

	/* The width, then an x. */
	parsed = strtoul(text, &end, 10);
	if (end == text || *end != 'x' || parsed < 64UL || parsed > 4096UL)
		return -1;
	*width = (uint32_t)parsed;

	/* The height, then the end. */
	text = end + 1;
	parsed = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || parsed < 64UL || parsed > 4096UL)
		return -1;
	*height = (uint32_t)parsed;

	/* Succeeded. */
	return 0;
}

/* Parses the content type's name: none, video or game. */
static int
option_content(
	const char *text,
	uint32_t *content)
{
	int match;

	/* No content type said. */
	match = strcmp(text, "none");
	if (match == 0) {
		*content = WP_CONTENT_TYPE_V1_TYPE_NONE;
		return 0;
	}

	/* A video. */
	match = strcmp(text, "video");
	if (match == 0) {
		*content = WP_CONTENT_TYPE_V1_TYPE_VIDEO;
		return 0;
	}

	/* A game: the compositor may show the fullscreen window straight on the display. */
	match = strcmp(text, "game");
	if (match == 0) {
		*content = WP_CONTENT_TYPE_V1_TYPE_GAME;
		return 0;
	}

	/* Anything else is refused. */
	return -1;
}

/* Parses the presentation mode's name: fifo, mailbox or immediate. */
static int
option_present(
	const char *text,
	VkPresentModeKHR *present)
{
	int match;

	/* One image a display refresh, the default. */
	match = strcmp(text, "fifo");
	if (match == 0) {
		*present = VK_PRESENT_MODE_FIFO_KHR;
		return 0;
	}

	/* The newest image replaces a waiting one: the frame rate is the GPU's. */
	match = strcmp(text, "mailbox");
	if (match == 0) {
		*present = VK_PRESENT_MODE_MAILBOX_KHR;
		return 0;
	}

	/* Every image shown at once, tearing allowed. */
	match = strcmp(text, "immediate");
	if (match == 0) {
		*present = VK_PRESENT_MODE_IMMEDIATE_KHR;
		return 0;
	}

	/* Anything else is refused. */
	return -1;
}

/* Names a presentation mode as --present does. */
static const char *
main_present_name(
	VkPresentModeKHR present)
{
	/* The three modes the option names. */
	if (present == VK_PRESENT_MODE_FIFO_KHR)
		return "fifo";
	if (present == VK_PRESENT_MODE_MAILBOX_KHR)
		return "mailbox";
	if (present == VK_PRESENT_MODE_IMMEDIATE_KHR)
		return "immediate";

	/* Succeeded: another mode, which the viewer never asks for. */
	return "other";
}

/* Rejects signs, overflow and trailing input before an option changes runtime bounds. */
static int
option_number(
	const char *text,
	uint32_t maximum,
	uint32_t *number)
{
	uint32_t parsed;
	uint32_t digit;

	/* Every numeric option has a bounded decimal grammar. */
	parsed = 0U;
	if (*text == '\0')
		return -1;

	/* Extends the parsed value only after the next decimal digit passes the bound. */
	while (*text != '\0') {
		/* Signs, prefixes and fractions are not part of the grammar. */
		if (*text < '0' || *text > '9')
			return -1;

		/* Refuses overflow before multiplication or addition can lose information. */
		digit = (uint32_t)(*text - '0');
		if (parsed > maximum / 10U ||
		    (parsed == maximum / 10U && digit > maximum % 10U))
			return -1;

		/* The checked digit advances both the value and its unconsumed input suffix. */
		parsed = parsed * 10U + digit;
		text++;
	}

	/* Publishes the option only after its entire string is accepted. */
	*number = parsed;

	/* Succeeded: the caller receives one decimal value within its requested bound. */
	return 0;
}

/* Accepts a token of 1 to 64 letters, digits, '-' and '_'. */
static int
option_token(
	const char *token)
{
	const char *position;

	/* An empty token would make log lines ambiguous. */
	if (*token == '\0')
		return -1;

	/* Whitespace and control characters cannot reach machine logs. */
	for (position = token; *position != '\0'; position++) {
		if ((*position >= 'a' && *position <= 'z') ||
		    (*position >= 'A' && *position <= 'Z') ||
		    (*position >= '0' && *position <= '9') ||
		    *position == '-' ||
		    *position == '_')
			continue;

		/* Any other character is refused. */
		return -1;
	}

	/* The harness does not accept arbitrarily long identity fields. */
	if (position - token > 64)
		return -1;

	/* Succeeded: the token is a plain bounded word. */
	return 0;
}

/* Reports whether the run's deadline, if any, has passed. */
static int
main_expired(
	const struct timespec *start,
	uint32_t timeout)
{
	struct timespec now;
	time_t elapsed;

	/* Zero means the run has no deadline. */
	if (timeout == 0U)
		return 0;

	/* Whole seconds since start are compared, rounding the start down. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed = now.tv_sec - start->tv_sec;
	if (now.tv_nsec < start->tv_nsec)
		elapsed--;

	/* The deadline has passed once the full timeout has elapsed. */
	if (elapsed >= (time_t)timeout)
		return 1;

	/* The run still has time. */
	return 0;
}

/* Prints one MVIEW FRAME line with the view the frame shows. */
static void
main_frame_log(
	const char *token,
	uint32_t frame,
	const struct mview_camera *camera)
{
	/* The same fields and precision as the input lines, so both can be compared. */
	printf(
		"MVIEW FRAME run=%s frame=%u yaw=%.2f pitch=%.2f distance=%.4f pan=%.4f,%.4f\n",
		token,
		frame,
		(double)camera->yaw,
		(double)camera->pitch,
		(double)camera->distance,
		(double)camera->pan[0],
		(double)camera->pan[1]);
	fflush(stdout);

	/* Succeeded: the frame is visible to the harness. */
	return;
}

/* Returns the seconds from one monotonic time to a later one. */
static double
main_seconds(
	const struct timespec *from,
	const struct timespec *to)
{
	double seconds;

	/* Whole seconds and the nanosecond remainder, which may be negative. */
	seconds = (double)(to->tv_sec - from->tv_sec);
	seconds += (double)(to->tv_nsec - from->tv_nsec) / 1e9;

	/* Succeeded: the difference in seconds. */
	return seconds;
}

/*
 * Turns the model about the vertical axis to where the spin should be now.
 *
 * The angle follows the clock, not the frame count, so a slow frame rate turns
 * the model just as far and the rate can be read from the frames drawn.
 */
static void
main_spin_turn(
	struct main_spin *spin,
	struct mview_camera *camera)
{
	struct timespec now;
	double target;

	/* The first spinning frame starts the clock at the current view. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (spin->started == 0) {
		spin->started = 1;
		spin->start = now;
		spin->last = now;
		spin->base_yaw = camera->yaw;
		spin->applied = 0.0;
		spin->shortest = 1e9;
	}

	/* Applies only the turn since the last frame, so the camera keeps wrapping its yaw. */
	target = MAIN_SPIN_RATE * main_seconds(&spin->start, &now);
	mview_camera_orbit(camera, (float)(target - spin->applied), 0.0f);
	spin->applied = target;

	/* Succeeded: the camera shows the spin's current angle. */
	return;
}

/*
 * Counts one presented spinning frame and reports the frame rate.
 *
 * Logs MVIEW FPS once per whole second and MVIEW SPIN at the end.  Returns
 * nonzero once the spin has lasted its duration.
 */
static int
main_spin_presented(
	struct main_spin *spin,
	const char *token,
	uint32_t duration)
{
	struct timespec now;
	double interval;
	double elapsed;
	uint32_t second;

	/* The time since the previous presented frame feeds the frame-time figures. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	interval = 1000.0 * main_seconds(&spin->last, &now);
	spin->last = now;
	if (spin->frames != 0U) {
		spin->total += interval;
		if (interval < spin->shortest)
			spin->shortest = interval;
		if (interval > spin->longest)
			spin->longest = interval;
		if (spin->frames >= 2U && interval >= MAIN_HITCH_MS) {
			/* A long gap past the start: each of the first few named, all counted. */
			spin->hitches++;
			if (spin->hitches <= MAIN_HITCH_LINES)
				printf("MVIEW HITCH run=%s frame=%u ms=%.2f\n", token, spin->frames, interval);
		}
	}

	spin->frames++;

	/* A new whole second reports how many frames the last one presented. */
	elapsed = main_seconds(&spin->start, &now);
	second = (uint32_t)elapsed;
	if (second != spin->second) {
		printf("MVIEW FPS run=%s second=%u frames=%u\n", token, spin->second + 1U, spin->second_frames);
		fflush(stdout);
		spin->second = second;
		spin->second_frames = 0U;
	}

	spin->second_frames++;

	/* The spin goes on until its duration has passed. */
	if (elapsed < (double)duration)
		return 0;

	/* The summary: frames, time, average rate and the frame-time spread. */
	printf(
		"MVIEW SPIN run=%s frames=%u seconds=%.3f fps=%.2f frame_ms_min=%.2f frame_ms_avg=%.2f frame_ms_max=%.2f\n",
		token,
		spin->frames,
		elapsed,
		(double)(spin->frames - 1U) / elapsed,
		spin->shortest,
		spin->frames > 1U ? spin->total / (double)(spin->frames - 1U) : 0.0,
		spin->longest);
	printf("MVIEW HITCHES run=%s hitches=%u\n", token, spin->hitches);
	fflush(stdout);

	/* The spin is over. */
	return 1;
}
