/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * displayctl: lists the displays of the desktop and their modes, and
 * changes a display's resolution and refresh, through the compositor
 * (libkeiland's kl_system_displays_*).
 *
 *   displayctl [--porcelain] [list]
 *   displayctl [--porcelain] modes DISPLAY
 *   displayctl [--porcelain] set DISPLAY WIDTHxHEIGHT[@HZ] [--keep] [--timeout=SECONDS]
 *   displayctl [--porcelain] keep DISPLAY | revert DISPLAY | reset DISPLAY [--keep] [--timeout=SECONDS]
 *   displayctl [--porcelain] scale [DISPLAY [PERCENT|auto]]
 *
 * DISPLAY is a display's number in the list, its key, or the start of its
 * label.  A new mode is shown at once as a trial: unless it is kept within
 * the compositor's time (15 s), the mode shown before comes back.  On a
 * terminal, set and reset ask whether to keep it; elsewhere, and with
 * --timeout, they wait that long (15 s by default) for a keep from
 * anyone (displayctl keep, Settings), and the old mode comes back when
 * none comes or when displayctl is ended.  --keep keeps the new mode at
 * once, for a change made from afar to a display known to show it.
 *
 * scale lists the displays' scales (the scale shown, the user's choice,
 * the recommended one and the steps offered), or one display's; with a
 * percent (one of the steps offered, such as 125) it chooses that scale,
 * and with auto it leaves the display to the default.  A scale is shown
 * at once, without a trial, and kept for the monitor.
 *
 * --porcelain prints one record a line, as "kind key=value ...": display,
 * mode, scale, result.  The exit status is 0, 1 for a wrong use, or 2 for a
 * request refused or not answered, or no compositor.
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How long the first snapshot and an answer are waited for (ms). */
#define DISPLAYCTL_SNAPSHOT_MS	5000
#define DISPLAYCTL_ANSWER_MS	15000

/* The longest rest between two looks at the desktop (ms). */
#define DISPLAYCTL_REST_MS	50

/* The trial's time the compositor gives, when the command was not given another (s). */
#define DISPLAYCTL_TRIAL_S	15

/* The exit statuses. */
#define DISPLAYCTL_OK		0
#define DISPLAYCTL_USAGE	1
#define DISPLAYCTL_FAILED	2

/*
 * The run: the connection, the system on it, the options (the porcelain
 * output, a keep at once, the time to wait for a keep in seconds, 0 when
 * not given), and the snapshot copied last.
 */
struct displayctl {
	struct wl_display *display;
	struct kl_system *system;
	unsigned porcelain;
	unsigned keep;
	int timeout_s;
	struct kl_display displays[KL_DISPLAYS_MAX];
	size_t count;
};

/*
 * The user's answer to whether to keep a new mode, read from the terminal
 * by a thread of its own: whether it came, and whether it was yes.  The
 * lock guards both.
 */
static struct {
	struct kl_mutex lock;
	unsigned answered;
	unsigned keep;
} displayctl_answer = { KL_MUTEX_INIT, 0U, 0U };

static int displayctl_round(struct displayctl *run, int timeout_ms);
static int displayctl_snapshot(struct displayctl *run);
static int displayctl_wait(struct displayctl *run, uint32_t request, int *answer);
static int displayctl_find(struct displayctl *run, const char *word);
static void displayctl_list(struct displayctl *run);
static void displayctl_modes(struct displayctl *run, unsigned index);
static int displayctl_set(struct displayctl *run, unsigned index, const char *word, unsigned reset);
static int displayctl_simple(struct displayctl *run, unsigned index, const char *verb);
static int displayctl_trial(struct displayctl *run, const char *key, const struct kl_display_mode *wanted);
static int displayctl_parse_mode(const char *word, uint32_t *width, uint32_t *height, uint32_t *refresh_mhz);
static int displayctl_current(struct displayctl *run, const char *key, struct kl_display_mode *mode);
static const char *displayctl_state(unsigned state);
static void displayctl_refresh_text(uint32_t refresh_mhz, char *text, size_t size);
static void displayctl_aspect(uint32_t width, uint32_t height, char *text, size_t size);
static void displayctl_result(struct displayctl *run, const char *verb, int error);
static void displayctl_why(struct displayctl *run, const char *key, FILE *stream);
static const char *displayctl_error_name(int error);
static unsigned long long displayctl_clock_ms(void);
static void displayctl_read_answer(void *argument);
static void displayctl_usage(void);
static void displayctl_scale_show(struct displayctl *run, unsigned index);
static int displayctl_scale_set(struct displayctl *run, unsigned index, const char *word);
static unsigned displayctl_percent(unsigned scale96);

/*
 * Runs one command against the desktop's displays.
 */
int
main(
	int argc,
	char **argv)
{
	struct displayctl run;
	const char *command;
	const char *words[3];
	unsigned word_count;
	int index;
	int arg;
	int status;
	int differs;
	int differs_scale;

	/* Prints each line as it is made, so that a script reading the pipe sees it at once. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&run, 0, sizeof(run));

	/* The options, anywhere; the rest are the command and its words. */
	command = "list";
	word_count = 0U;
	for (arg = 1; arg < argc; arg++) {
		/* The porcelain output. */
		differs = strcmp(argv[arg], "--porcelain");
		if (differs == 0) {
			run.porcelain = 1U;
			continue;
		}

		/* A keep at once. */
		differs = strcmp(argv[arg], "--keep");
		if (differs == 0) {
			run.keep = 1U;
			continue;
		}

		/* The time to wait for a keep. */
		differs = strncmp(argv[arg], "--timeout=", 10);
		if (differs == 0) {
			run.timeout_s = atoi(argv[arg] + 10);
			if (run.timeout_s <= 0) {
				displayctl_usage();
				return DISPLAYCTL_USAGE;
			}
			continue;
		}

		/* Help, and an option not known. */
		if (argv[arg][0] == '-') {
			displayctl_usage();
			return DISPLAYCTL_USAGE;
		}

		/* The command, then its words. */
		if (word_count >= 3U) {
			displayctl_usage();
			return DISPLAYCTL_USAGE;
		}
		words[word_count] = argv[arg];
		word_count++;
	}
	if (word_count > 0U) {
		command = words[0];
		words[0] = words[1];
		words[1] = words[2];
		word_count--;
	}

	/* The compositor's displays. */
	run.display = wl_display_connect(NULL);
	if (run.display == NULL) {
		fprintf(stderr, "displayctl: cannot connect to the desktop: %s\n", strerror(errno));
		return DISPLAYCTL_FAILED;
	}
	run.system = kl_system_open(run.display);
	if (run.system == NULL) {
		fprintf(stderr, "displayctl: the desktop offers no system: %s\n", strerror(errno));
		wl_display_disconnect(run.display);
		return DISPLAYCTL_FAILED;
	}
	status = displayctl_snapshot(&run);
	if (status != 0) {
		fprintf(stderr, "displayctl: the desktop told no displays\n");
		kl_system_close(run.system);
		wl_display_disconnect(run.display);
		return DISPLAYCTL_FAILED;
	}

	/* The list. */
	status = DISPLAYCTL_USAGE;
	differs = strcmp(command, "list");
	if (differs == 0 && word_count == 0U) {
		displayctl_list(&run);
		status = DISPLAYCTL_OK;
	}

	/* Every display's scale. */
	differs_scale = strcmp(command, "scale");
	if (differs_scale == 0 && word_count == 0U) {
		for (index = 0; index < (int)run.count; index++)
			displayctl_scale_show(&run, (unsigned)index);
		status = DISPLAYCTL_OK;
	}

	/* A display's modes, a change, a keep, a revert, a reset, or its scale shown or chosen. */
	if (differs != 0 && word_count >= 1U) {
		index = displayctl_find(&run, words[0]);
		if (index < 0) {
			fprintf(stderr, "displayctl: no display %s\n", words[0]);
			status = DISPLAYCTL_FAILED;
		} else if (strcmp(command, "modes") == 0 && word_count == 1U) {
			displayctl_modes(&run, (unsigned)index);
			status = DISPLAYCTL_OK;
		} else if (strcmp(command, "set") == 0 && word_count == 2U) {
			status = displayctl_set(&run, (unsigned)index, words[1], 0U);
		} else if (strcmp(command, "reset") == 0 && word_count == 1U) {
			status = displayctl_set(&run, (unsigned)index, NULL, 1U);
		} else if ((strcmp(command, "keep") == 0 || strcmp(command, "revert") == 0) && word_count == 1U) {
			status = displayctl_simple(&run, (unsigned)index, command);
		} else if (differs_scale == 0 && word_count == 1U) {
			displayctl_scale_show(&run, (unsigned)index);
			status = DISPLAYCTL_OK;
		} else if (differs_scale == 0 && word_count == 2U) {
			status = displayctl_scale_set(&run, (unsigned)index, words[1]);
		}
	}
	if (status == DISPLAYCTL_USAGE)
		displayctl_usage();

	/* The system and the connection go (a trial not kept goes back with them). */
	kl_system_close(run.system);
	wl_display_disconnect(run.display);

	/* Succeeded: the command's status. */
	return status;
}

/*
 * Asks the desktop for what it has to tell (a round trip), takes it, and
 * rests for up to timeout_ms (at most DISPLAYCTL_REST_MS) when nothing
 * changed.  Returns 0, or -1 when the desktop went.
 */
static int
displayctl_round(
	struct displayctl *run,
	int timeout_ms)
{
	unsigned changed;
	int status;

	/* The desktop's events so far. */
	status = wl_display_roundtrip(run->display);
	if (status < 0)
		return -1;

	/* The system's, and the snapshot copied again. */
	changed = 0U;
	status = kl_system_dispatch(run->system, &changed);
	if (status != 0)
		return -1;
	if ((changed & KL_SYSTEM_CHANGED_DISPLAYS) != 0U)
		run->count = kl_system_displays_get(run->system, run->displays, KL_DISPLAYS_MAX);

	/* Nothing new: a rest before the next look. */
	if (changed == 0U) {
		if (timeout_ms > DISPLAYCTL_REST_MS)
			timeout_ms = DISPLAYCTL_REST_MS;
		if (timeout_ms > 0)
			kl_thread_sleep((uint32_t)timeout_ms);
	}

	/* Succeeded: the round is over. */
	return 0;
}

/* Waits for the first snapshot of the displays; returns 0, or -1 when none came. */
static int
displayctl_snapshot(
	struct displayctl *run)
{
	unsigned long long until;
	unsigned long long now;
	unsigned capabilities;
	int error;

	/* A desktop that tells the displays. */
	capabilities = kl_system_capabilities(run->system);
	if ((capabilities & KL_SYSTEM_HAS_DISPLAYS) == 0U)
		return -1;

	/* Until the first snapshot, or the time. */
	until = displayctl_clock_ms() + DISPLAYCTL_SNAPSHOT_MS;
	for (;;) {
		run->count = kl_system_displays_get(run->system, run->displays, KL_DISPLAYS_MAX);
		if (run->count != 0U)
			return 0;
		now = displayctl_clock_ms();
		if (now >= until)
			return -1;
		error = displayctl_round(run, (int)(until - now));
		if (error != 0)
			return -1;
	}
}

/* Waits for a request's answer; returns 0 with it in answer, or -1 when none came. */
static int
displayctl_wait(
	struct displayctl *run,
	uint32_t request,
	int *answer)
{
	unsigned long long until;
	unsigned long long now;
	uint32_t finished;
	int taken;
	int error;

	/* Until the answer, or the time. */
	until = displayctl_clock_ms() + DISPLAYCTL_ANSWER_MS;
	for (;;) {
		/* The answers so far; another request's is passed by. */
		taken = kl_system_take_result(run->system, &finished, answer);
		if (taken) {
			if (finished == request)
				return 0;
			continue;
		}

		/* The time, or the desktop gone, ends the wait. */
		now = displayctl_clock_ms();
		if (now >= until)
			return -1;
		error = displayctl_round(run, (int)(until - now));
		if (error != 0)
			return -1;
	}
}

/* Finds a display by its number in the list, its key or the start of its label; returns its index, or -1. */
static int
displayctl_find(
	struct displayctl *run,
	const char *word)
{
	char *end;
	unsigned long number;
	size_t length;
	size_t index;
	int differs;

	/* A number of the list. */
	number = strtoul(word, &end, 10);
	if (end != word && *end == '\0') {
		if (number >= 1UL && number <= run->count)
			return (int)(number - 1UL);
		return -1;
	}

	/* A key. */
	for (index = 0U; index < run->count; index++) {
		differs = strcmp(run->displays[index].key, word);
		if (differs == 0)
			return (int)index;
	}

	/* The start of a label. */
	length = strlen(word);
	for (index = 0U; index < run->count; index++) {
		differs = strncmp(run->displays[index].label, word, length);
		if (differs == 0)
			return (int)index;
	}

	/* Succeeded: no such display. */
	return -1;
}

/* Prints the displays: number, label, key, the mode shown, whether it can change, and its trial. */
static void
displayctl_list(
	struct displayctl *run)
{
	const struct kl_display *display;
	struct kl_display_mode mode;
	char refresh[16];
	uint32_t remaining;
	unsigned state;
	unsigned why;
	size_t index;
	int error;

	/* Each display of the snapshot. */
	for (index = 0U; index < run->count; index++) {
		display = &run->displays[index];

		/* The mode shown, and its trial. */
		error = displayctl_current(run, display->key, &mode);
		if (error != 0) {
			mode.width = display->width;
			mode.height = display->height;
			mode.refresh_mhz = display->refresh_mhz;
		}
		state = KL_DISPLAY_RESOLUTION_NONE;
		remaining = 0U;
		why = KL_DISPLAY_RESOLUTION_WHY_FIXED;
		(void)kl_system_displays_resolution(run->system, display->key, &state, &remaining, &why);

		/* One line. */
		if (run->porcelain) {
			printf("display index=%u key=%s label=%s width=%u height=%u refresh_mhz=%u anchor=%u selectable=%u why=0x%x state=%s remaining_ms=%u\n",
			    (unsigned)index + 1U, display->key, display->label, mode.width, mode.height, mode.refresh_mhz,
			    (display->flags & KL_DISPLAY_ANCHOR) != 0U ? 1U : 0U, why == 0U ? 1U : 0U, why, displayctl_state(state), remaining);
			continue;
		}
		displayctl_refresh_text(mode.refresh_mhz, refresh, sizeof(refresh));
		printf("%u  %-20s %ux%u @ %s Hz  %s", (unsigned)index + 1U, display->label, mode.width, mode.height, refresh,
		    why == 0U ? "can change" : "fixed");
		if (state != KL_DISPLAY_RESOLUTION_NONE)
			printf("  (%s, %u s left)", displayctl_state(state), (remaining + 999U) / 1000U);
		printf("\n   %s\n", display->key);
	}
}

/* Prints a display's modes: the one shown marked *, the recommended +. */
static void
displayctl_modes(
	struct displayctl *run,
	unsigned index)
{
	struct kl_display_mode modes[KL_DISPLAY_MODES_MAX];
	const struct kl_display *display;
	char refresh[16];
	char aspect[16];
	size_t count;
	size_t at;

	/* The display's modes. */
	display = &run->displays[index];
	count = kl_system_displays_modes(run->system, display->key, modes, KL_DISPLAY_MODES_MAX);

	/* Each one. */
	for (at = 0U; at < count; at++) {
		if (run->porcelain) {
			printf("mode key=%s width=%u height=%u refresh_mhz=%u native=%u current=%u\n", display->key, modes[at].width, modes[at].height,
			    modes[at].refresh_mhz, (modes[at].flags & KL_DISPLAY_MODE_NATIVE) != 0U ? 1U : 0U,
			    (modes[at].flags & KL_DISPLAY_MODE_CURRENT) != 0U ? 1U : 0U);
			continue;
		}
		displayctl_refresh_text(modes[at].refresh_mhz, refresh, sizeof(refresh));
		displayctl_aspect(modes[at].width, modes[at].height, aspect, sizeof(aspect));
		printf("%c%c %5ux%-5u %7s Hz  %s%s\n", (modes[at].flags & KL_DISPLAY_MODE_CURRENT) != 0U ? '*' : ' ',
		    (modes[at].flags & KL_DISPLAY_MODE_NATIVE) != 0U ? '+' : ' ', modes[at].width, modes[at].height, refresh, aspect,
		    (modes[at].flags & KL_DISPLAY_MODE_NATIVE) != 0U ? "  (recommended)" : "");
	}

	/* Why none of them can be chosen now, if so. */
	displayctl_why(run, display->key, stdout);
}

/*
 * Changes a display's mode (word "WxH[@HZ]") or puts it back to its
 * default (reset), then follows its trial.  Returns the exit status.
 */
static int
displayctl_set(
	struct displayctl *run,
	unsigned index,
	const char *word,
	unsigned reset)
{
	struct kl_display_mode wanted;
	const char *key;
	uint32_t request;
	int answer;
	int error;

	/* The mode asked for. */
	key = run->displays[index].key;
	memset(&wanted, 0, sizeof(wanted));
	if (!reset) {
		error = displayctl_parse_mode(word, &wanted.width, &wanted.height, &wanted.refresh_mhz);
		if (error != 0)
			return DISPLAYCTL_USAGE;
		error = kl_system_displays_set_resolution(run->system, key, wanted.width, wanted.height, wanted.refresh_mhz, &request);
	} else {
		error = kl_system_displays_reset_resolution(run->system, key, &request);
	}
	if (error != 0) {
		displayctl_result(run, reset ? "reset" : "set", error);
		displayctl_why(run, key, stderr);
		return DISPLAYCTL_FAILED;
	}

	/* Its answer: the new mode shows, or why not. */
	error = displayctl_wait(run, request, &answer);
	if (error != 0) {
		displayctl_result(run, reset ? "reset" : "set", ETIMEDOUT);
		return DISPLAYCTL_FAILED;
	}
	displayctl_result(run, reset ? "reset" : "set", answer);
	if (answer != 0) {
		displayctl_why(run, key, stderr);
		return DISPLAYCTL_FAILED;
	}

	/* The trial: kept at once, asked, or waited for. */
	error = displayctl_current(run, key, &wanted);
	if (error != 0)
		memset(&wanted, 0, sizeof(wanted));
	error = displayctl_trial(run, key, &wanted);
	return error;
}

/* Sends a keep or a revert of a display's trial and waits for its answer.  Returns the exit status. */
static int
displayctl_simple(
	struct displayctl *run,
	unsigned index,
	const char *verb)
{
	uint32_t request;
	int answer;
	int differs;
	int error;

	/* The request. */
	differs = strcmp(verb, "keep");
	if (differs == 0) {
		error = kl_system_displays_keep_resolution(run->system, run->displays[index].key, &request);
	} else {
		error = kl_system_displays_revert_resolution(run->system, run->displays[index].key, &request);
	}
	if (error != 0) {
		displayctl_result(run, verb, error);
		return DISPLAYCTL_FAILED;
	}

	/* Its answer. */
	error = displayctl_wait(run, request, &answer);
	if (error != 0)
		answer = ETIMEDOUT;
	displayctl_result(run, verb, answer);
	if (answer != 0)
		return DISPLAYCTL_FAILED;

	/* Succeeded: kept, or going back. */
	return DISPLAYCTL_OK;
}

/*
 * Follows a display's trial of a new mode (wanted): kept at once (--keep),
 * kept when the user says so on a terminal, or waited for until it ends
 * (kept by anyone, or gone back).  Prints "kept" or "reverted".  Returns
 * the exit status.
 */
static int
displayctl_trial(
	struct displayctl *run,
	const char *key,
	const struct kl_display_mode *wanted)
{
	struct kl_thread_options options;
	struct kl_display_mode shown;
	struct kl_thread *reader;
	unsigned long long until;
	unsigned long long now;
	uint32_t remaining;
	uint32_t request;
	unsigned state;
	unsigned why;
	unsigned asked;
	unsigned answered;
	unsigned keep;
	int interactive;
	int answer;
	int error;

	/* A keep at once. */
	if (run->keep) {
		error = kl_system_displays_keep_resolution(run->system, key, &request);
		if (error == 0)
			error = displayctl_wait(run, request, &answer);
		if (error != 0)
			answer = error;
		displayctl_result(run, "keep", answer);
		if (answer != 0)
			return DISPLAYCTL_FAILED;
		return DISPLAYCTL_OK;
	}

	/* On a terminal without a time given, the user is asked. */
	interactive = isatty(STDIN_FILENO);
	if (run->timeout_s != 0)
		interactive = 0;
	asked = 0U;
	if (interactive) {
		printf("Keep this resolution? [y/N] (%d s) ", DISPLAYCTL_TRIAL_S);
		fflush(stdout);

		/* The answer is read by a thread of its own, which the end of the program leaves. */
		memset(&options, 0, sizeof(options));
		options.name = "answer";
		options.run = displayctl_read_answer;
		options.priority = KL_THREAD_PRIORITY_NORMAL;
		error = kl_thread_start(&options, &reader);
		if (error == 0) {
			(void)kl_thread_detach(reader);
			asked = 1U;
		}
	}

	/* Until the trial ends, the answer, or the time. */
	until = displayctl_clock_ms() + (unsigned long long)(run->timeout_s != 0 ? run->timeout_s : DISPLAYCTL_TRIAL_S) * 1000ULL + 2000ULL;
	for (;;) {
		/* The trial over: kept or gone back. */
		state = KL_DISPLAY_RESOLUTION_NONE;
		error = kl_system_displays_resolution(run->system, key, &state, &remaining, &why);
		if (error == 0 && state == KL_DISPLAY_RESOLUTION_NONE)
			break;

		/* The time. */
		now = displayctl_clock_ms();
		if (now >= until)
			break;

		/* The user's answer, once it came. */
		if (asked) {
			kl_mutex_lock(&displayctl_answer.lock);
			answered = displayctl_answer.answered;
			keep = displayctl_answer.keep;
			kl_mutex_unlock(&displayctl_answer.lock);
			if (answered) {
				asked = 0U;
				if (keep) {
					error = kl_system_displays_keep_resolution(run->system, key, &request);
				} else {
					error = kl_system_displays_revert_resolution(run->system, key, &request);
				}
				if (error == 0)
					(void)displayctl_wait(run, request, &answer);
			}
		}

		error = displayctl_round(run, 100);
		if (error != 0)
			return DISPLAYCTL_FAILED;
	}

	/* What shows now says how it ended. */
	error = displayctl_current(run, key, &shown);
	if (error == 0 && shown.width == wanted->width && shown.height == wanted->height && shown.refresh_mhz == wanted->refresh_mhz && state == 0U) {
		if (run->porcelain) {
			printf("trial key=%s outcome=kept width=%u height=%u refresh_mhz=%u\n", key, shown.width, shown.height, shown.refresh_mhz);
		} else {
			printf("kept %ux%u\n", shown.width, shown.height);
		}
		return DISPLAYCTL_OK;
	}

	/* Succeeded as asked: the old mode is back (or coming back as displayctl ends). */
	if (run->porcelain) {
		printf("trial key=%s outcome=reverted width=%u height=%u refresh_mhz=%u\n", key, shown.width, shown.height, shown.refresh_mhz);
	} else {
		printf("reverted to %ux%u\n", shown.width, shown.height);
	}
	return DISPLAYCTL_OK;
}

/* Reads "WIDTHxHEIGHT[@HZ]" (HZ with up to three decimals); returns 0, or EINVAL. */
static int
displayctl_parse_mode(
	const char *word,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_mhz)
{
	const char *at;
	char *end;
	unsigned long number;
	unsigned long fraction;
	unsigned long scale;

	/* The width. */
	number = strtoul(word, &end, 10);
	if (end == word || (*end != 'x' && *end != 'X') || number == 0UL || number > 65535UL)
		return EINVAL;
	*width = (uint32_t)number;

	/* The height. */
	at = end + 1;
	number = strtoul(at, &end, 10);
	if (end == at || number == 0UL || number > 65535UL)
		return EINVAL;
	*height = (uint32_t)number;

	/* No refresh: the compositor chooses. */
	*refresh_mhz = 0U;
	if (*end == '\0')
		return 0;
	if (*end != '@')
		return EINVAL;

	/* The refresh's whole hertz. */
	at = end + 1;
	number = strtoul(at, &end, 10);
	if (end == at || number == 0UL || number > 1000UL)
		return EINVAL;
	*refresh_mhz = (uint32_t)number * 1000U;
	if (*end == '\0')
		return 0;
	if (*end != '.')
		return EINVAL;

	/* Its decimals, up to three. */
	fraction = 0UL;
	scale = 100UL;
	for (at = end + 1; *at >= '0' && *at <= '9'; at++) {
		if (scale == 0UL)
			return EINVAL;
		fraction += (unsigned long)(*at - '0') * scale;
		scale /= 10UL;
	}
	if (*at != '\0' || at == end + 1)
		return EINVAL;
	*refresh_mhz += (uint32_t)fraction;

	/* Succeeded: the mode asked for. */
	return 0;
}

/* Finds the mode a display shows now among its modes; returns 0, or ENOENT. */
static int
displayctl_current(
	struct displayctl *run,
	const char *key,
	struct kl_display_mode *mode)
{
	struct kl_display_mode modes[KL_DISPLAY_MODES_MAX];
	size_t count;
	size_t index;

	/* The display's modes; the one marked shown. */
	count = kl_system_displays_modes(run->system, key, modes, KL_DISPLAY_MODES_MAX);
	for (index = 0U; index < count; index++) {
		if ((modes[index].flags & KL_DISPLAY_MODE_CURRENT) != 0U) {
			*mode = modes[index];
			return 0;
		}
	}

	/* Succeeded: none marked. */
	return ENOENT;
}

/* Names a trial's state. */
static const char *
displayctl_state(
	unsigned state)
{
	/* Each state. */
	switch (state) {
	case KL_DISPLAY_RESOLUTION_SWITCHING:
		return "switching";
	case KL_DISPLAY_RESOLUTION_TRIAL:
		return "trial";
	case KL_DISPLAY_RESOLUTION_REVERTING:
		return "reverting";
	default:
		break;
	}

	/* Succeeded: no trial. */
	return "none";
}

/* Writes a refresh in hertz: whole, or with two decimals ("60", "59.94"). */
static void
displayctl_refresh_text(
	uint32_t refresh_mhz,
	char *text,
	size_t size)
{
	unsigned hundredths;

	/* Rounded to hundredths. */
	hundredths = (refresh_mhz + 5U) / 10U;
	if (hundredths % 100U == 0U) {
		(void)snprintf(text, size, "%u", hundredths / 100U);
	} else {
		(void)snprintf(text, size, "%u.%02u", hundredths / 100U, hundredths % 100U);
	}
}

/* Writes a size's aspect ratio when it is a common one ("16:9"), else nothing. */
static void
displayctl_aspect(
	uint32_t width,
	uint32_t height,
	char *text,
	size_t size)
{
	unsigned long long ratio;

	/* The ratio in thousandths, matched within rounding (1366x768 is 16:9). */
	text[0] = '\0';
	if (height == 0U)
		return;
	ratio = (unsigned long long)width * 1000ULL / height;
	if (ratio >= 1770ULL && ratio <= 1785ULL)
		(void)snprintf(text, size, "16:9");
	if (ratio == 1600ULL)
		(void)snprintf(text, size, "16:10");
	if (ratio == 1333ULL)
		(void)snprintf(text, size, "4:3");
	if (ratio == 1250ULL)
		(void)snprintf(text, size, "5:4");
}

/* Prints how a request ended. */
static void
displayctl_result(
	struct displayctl *run,
	const char *verb,
	int error)
{
	/* One line, for people or for scripts (the error by its name: the numbers differ between systems). */
	if (run->porcelain) {
		printf("result verb=%s error=%s\n", verb, displayctl_error_name(error));
		return;
	}
	if (error == 0)
		return;
	fprintf(stderr, "displayctl: %s: %s\n", verb, strerror(error));
}

/*
 * Says, for people, why a display's resolution cannot be changed now: a
 * line for each reason the desktop gives.  Prints nothing for scripts
 * (list --porcelain gives the reasons) or when it can be changed.
 */
static void
displayctl_why(
	struct displayctl *run,
	const char *key,
	FILE *stream)
{
	uint32_t remaining;
	unsigned state;
	unsigned why;
	int error;

	/* Scripts read the reasons from the list. */
	if (run->porcelain)
		return;

	/* The reasons the desktop gives now. */
	why = 0U;
	error = kl_system_displays_resolution(run->system, key, &state, &remaining, &why);
	if (error != 0)
		return;

	/* A line for each. */
	if ((why & KL_DISPLAY_RESOLUTION_WHY_FIXED) != 0U)
		fprintf(stream, "This display's resolution cannot be changed: it offers one mode.\n");
	if ((why & KL_DISPLAY_RESOLUTION_WHY_ALONE) != 0U)
		fprintf(stream, "The resolution can be changed only while one display is shown.\n");
	if ((why & KL_DISPLAY_RESOLUTION_WHY_GAME) != 0U)
		fprintf(stream, "The resolution cannot be changed while a game is shown.\n");
	if ((why & KL_DISPLAY_RESOLUTION_WHY_ANCHOR) != 0U)
		fprintf(stream, "The resolution can be changed only on the display the desktop is on.\n");
}

/* Names an error a request is answered with ("0" for none, the number for one not named here). */
static const char *
displayctl_error_name(
	int error)
{
	static char number[16];

	/* Each error the compositor answers with. */
	switch (error) {
	case 0:
		return "0";
	case EPERM:
		return "EPERM";
	case ENOTSUP:
		return "ENOTSUP";
	case EBUSY:
		return "EBUSY";
	case EINVAL:
		return "EINVAL";
	case ENODEV:
		return "ENODEV";
	case EIO:
		return "EIO";
	case ENOBUFS:
		return "ENOBUFS";
	case ETIMEDOUT:
		return "ETIMEDOUT";
	default:
		break;
	}

	/* Succeeded: another, by its number. */
	(void)snprintf(number, sizeof(number), "%d", error);
	return number;
}

/* Reads the user's answer from the terminal (a thread of its own): yes when it starts with y. */
static void
displayctl_read_answer(
	void *argument)
{
	char line[16];
	ssize_t got;
	unsigned keep;

	/* One line. */
	(void)argument;
	got = read(STDIN_FILENO, line, sizeof(line) - 1U);
	keep = 0U;
	if (got > 0 && (line[0] == 'y' || line[0] == 'Y'))
		keep = 1U;

	/* Given to the main thread. */
	kl_mutex_lock(&displayctl_answer.lock);
	displayctl_answer.answered = 1U;
	displayctl_answer.keep = keep;
	kl_mutex_unlock(&displayctl_answer.lock);
}

/* Reads a steady clock in milliseconds. */
static unsigned long long
displayctl_clock_ms(
	void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: milliseconds. */
	return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

/* Prints how to use the command. */
static void
displayctl_usage(void)
{
	fprintf(stderr,
	    "usage: displayctl [--porcelain] [list]\n"
	    "       displayctl [--porcelain] modes DISPLAY\n"
	    "       displayctl [--porcelain] set DISPLAY WIDTHxHEIGHT[@HZ] [--keep] [--timeout=SECONDS]\n"
	    "       displayctl [--porcelain] keep|revert DISPLAY\n"
	    "       displayctl [--porcelain] reset DISPLAY [--keep] [--timeout=SECONDS]\n"
	    "       displayctl [--porcelain] scale [DISPLAY [PERCENT|auto]]\n");
}

/* Prints a display's scale: the scale shown, the user's choice, the recommended one, the steps offered, and why not. */
static void
displayctl_scale_show(
	struct displayctl *run,
	unsigned index)
{
	struct kl_display_scale scale;
	const struct kl_display *display;
	char chosen[16];
	char offered[64];
	size_t used;
	unsigned at;
	int written;
	int error;

	/* What the desktop told of the display's scale. */
	display = &run->displays[index];
	error = kl_system_displays_scale(run->system, display->key, &scale);
	if (error != 0) {
		if (run->porcelain) {
			printf("scale index=%u key=%s error=%s\n", index + 1U, display->key, displayctl_error_name(error));
			return;
		}
		printf("%u  %-20s scale unknown (%s)\n", index + 1U, display->label, strerror(error));
		return;
	}

	/* The choice, and the steps offered, in percent. */
	(void)snprintf(chosen, sizeof(chosen), "auto");
	if (scale.chosen != 0U)
		(void)snprintf(chosen, sizeof(chosen), "%u", displayctl_percent(scale.chosen));
	used = 0U;
	offered[0] = '\0';
	for (at = 0U; at < scale.offered_count; at++) {
		written = snprintf(offered + used, sizeof(offered) - used, "%s%u", at == 0U ? "" : ",", displayctl_percent(scale.offered[at]));
		if (written < 0 || (size_t)written >= sizeof(offered) - used)
			break;
		used += (size_t)written;
	}

	/* One line, for scripts or for people. */
	if (run->porcelain) {
		printf("scale index=%u key=%s current=%u chosen=%s recommended=%u offered=%s width=%u height=%u why=0x%x\n", index + 1U, display->key,
		    displayctl_percent(scale.current), chosen, displayctl_percent(scale.recommended), offered, scale.width, scale.height, scale.why);
		return;
	}
	printf("%u  %-20s %u %%  (chosen %s%s, recommended %u %%)  offers %s %%  desktop %ux%u\n", index + 1U, display->label,
	    displayctl_percent(scale.current), chosen, scale.chosen != 0U ? " %" : "", displayctl_percent(scale.recommended), offered, scale.width,
	    scale.height);
	if ((scale.why & KL_DISPLAY_SCALE_WHY_FIXED) != 0U)
		printf("   This display offers 100 %% alone.\n");
	if ((scale.why & KL_DISPLAY_SCALE_WHY_ALONE) != 0U)
		printf("   The scale can be chosen only while one display is shown.\n");
	if ((scale.why & KL_DISPLAY_SCALE_WHY_ANCHOR) != 0U)
		printf("   The scale can be chosen only on the display the desktop is on.\n");
	if ((scale.why & KL_DISPLAY_SCALE_WHY_BUSY) != 0U)
		printf("   The scale cannot be chosen while the mode changes or a game is shown.\n");
}

/*
 * Chooses a display's scale (word: a percent, or auto for the default),
 * waits for its answer, and prints the display's scale after it.  Returns
 * the exit status.
 */
static int
displayctl_scale_set(
	struct displayctl *run,
	unsigned index,
	const char *word)
{
	unsigned long percent;
	unsigned scale96;
	uint32_t request;
	char *end;
	int answer;
	int differs;
	int error;

	/* The scale asked for: auto, or a percent of a quarter (the steps' 96ths are whole then). */
	scale96 = 0U;
	differs = strcmp(word, "auto");
	if (differs != 0) {
		percent = strtoul(word, &end, 10);
		if (end == word || (*end != '\0' && strcmp(end, "%") != 0))
			return DISPLAYCTL_USAGE;
		if (percent < 100UL || percent > 400UL || percent % 25UL != 0UL) {
			fprintf(stderr, "displayctl: scale: %s is not a step (100, 125, 150 ...)\n", word);
			return DISPLAYCTL_USAGE;
		}
		scale96 = (unsigned)(percent * 96UL / 100UL);
	}

	/* The request. */
	error = kl_system_displays_set_scale(run->system, run->displays[index].key, scale96, &request);
	if (error != 0) {
		displayctl_result(run, "scale", error);
		return DISPLAYCTL_FAILED;
	}

	/* Its answer. */
	error = displayctl_wait(run, request, &answer);
	if (error != 0)
		answer = ETIMEDOUT;
	displayctl_result(run, "scale", answer);
	if (answer != 0) {
		displayctl_scale_show(run, index);
		return DISPLAYCTL_FAILED;
	}

	/* The snapshot after it, and the display's scale in it. */
	(void)displayctl_round(run, DISPLAYCTL_REST_MS);
	displayctl_scale_show(run, index);

	/* Succeeded: chosen and shown. */
	return DISPLAYCTL_OK;
}

/* Reports a scale in 96ths in percent, rounded to the nearest. */
static unsigned
displayctl_percent(
	unsigned scale96)
{
	/* scale * 100 / 96, a half up. */
	return (scale96 * 100U + 48U) / 96U;
}
