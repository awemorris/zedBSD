/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bootlog service: keeps the kernel log of each boot on the boot FAT.
 *
 * A Raspberry Pi 4 whose root UFS turned read-only and whose network did
 * not come up left no log to read: the screen scrolled past it, the root
 * could not keep it and nothing could fetch it (BUG-288).  bootlog keeps
 * the kernel log in a file of the boot partition's FAT instead
 * (/boot/boot0/log/boot-NNNNNN.txt), which the root's state does not
 * touch and which any computer reads from the SD card.
 *
 * Writes are few, to spare the card and to leave the FAT little to lose to
 * a power cut: one when the service starts, one at each of a few times
 * after the boot, one when the kernel log says a word of the failures it
 * is after (with ten seconds at least between two), one when the log is
 * about to let go of text not yet kept, and one when the system stops,
 * forty-eight at most before the stop.  Each write appends only the text
 * not kept before and is made durable at once.  A file stops taking text
 * at 1 MiB, and no text is written when the FAT has less than 16 MiB
 * free, so the logs never crowd out the firmware and the kernels.  Between writes the log is
 * read from memory every two seconds; nothing is written then.
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "userland/base/bootlog/log-file.h"

/* Where the logs are kept when the command line names no directory. */
#define BOOTLOG_DIRECTORY "/boot/boot0/log"

/* How many boots' logs are kept; the oldest beyond this are removed. */
#define BOOTLOG_KEEP 32U

/* How often the kernel log is read from memory, in seconds. */
#define BOOTLOG_POLL_SECONDS 2U

/* How long the start waits for the FAT volume to appear, in seconds. */
#define BOOTLOG_WAIT_SECONDS 30U

/* How many writes a boot makes at most before the one at the stop. */
#define BOOTLOG_WRITES_MAX 48U

/* The least time between two writes a trigger asks for, in milliseconds. */
#define BOOTLOG_INTERVAL_MS 10000ULL

/* How many times a copy of the log is tried when the ring moved under it. */
#define BOOTLOG_SNAPSHOT_TRIES 4

/*
 * The most a file grows to, in bytes.  A boot whose log runs on keeps the
 * headers past this, but not the text, so the logs cannot fill the FAT the
 * firmware and the kernels live on.
 */
#define BOOTLOG_FILE_MAX (1024ULL * 1024ULL)

/* The free space, in bytes, under which no more text is put on the volume. */
#define BOOTLOG_FREE_MIN (16ULL * 1024ULL * 1024ULL)

/* Room for the header of one write. */
#define BOOTLOG_HEADER_SIZE 512U

/* Room for the reason of a pending write. */
#define BOOTLOG_REASON_SIZE 96U

/*
 * What bootlog knows of the kernel log and of its own file.
 *
 * There is one, for the life of the service.  The totals count bytes from
 * the start of the boot, as the kernel log does: written_total is how far
 * the file holds the log, scanned_total how far the trigger words were
 * looked for.  file_bytes is the size of the file.
 */
struct bootlog_state {
	const char *directory;
	int descriptor;
	uint64_t file_bytes;
	char *text;
	size_t capacity;
	size_t length;
	uint64_t dropped;
	uint64_t written_total;
	uint64_t scanned_total;
	unsigned writes;
	uint64_t last_write_ms;
	size_t milestone;
	int pending;
	int limit_reported;
	char reason[BOOTLOG_REASON_SIZE];
};

/*
 * The times after the boot at which the log is kept, in seconds.
 *
 * They cover a boot that stops in its drivers, one that stops in its
 * services, and one that broke some minutes into running.
 */
static const unsigned bootlog_milestones[] = {
	20U,
	60U,
	120U,
	300U,
};

/*
 * Nonzero once the service was told to stop.
 *
 * The signal handler sets it; the main loop reads it after every sleep,
 * which the signal cuts short, and then makes the last write.
 */
static volatile sig_atomic_t bootlog_stop;

static void bootlog_on_signal(int signal_number);
static int bootlog_signals_install(void);
static int bootlog_volume_wait(const char *directory);
static int bootlog_volume_is_fat(const char *path);
static uint64_t bootlog_uptime_ms(void);
static void bootlog_sleep(unsigned seconds);
static int bootlog_log_total(uint64_t *total);
static int bootlog_snapshot(struct bootlog_state *state);
static void bootlog_scan(struct bootlog_state *state);
static void bootlog_request(struct bootlog_state *state, const char *reason);
static void bootlog_header_format(char *header, size_t size, const char *reason, uint64_t uptime_ms, uint64_t lost, uint64_t left_out, int read_error);
static uint64_t bootlog_room(const struct bootlog_state *state);
static void bootlog_write(struct bootlog_state *state, const char *reason);
static void bootlog_poll(struct bootlog_state *state);

/*
 * Runs the bootlog service.
 */
int
main(
	int argc,
	char **argv)
{
	struct bootlog_state state;
	const char *directory;
	unsigned long number;
	uint64_t capacity;
	size_t capacity_size;
	int installed;
	int ready;
	int read_error;
	size_t milestones;
	uint64_t now_ms;

	/* Takes the directory from the command line, or the boot FAT's. */
	directory = BOOTLOG_DIRECTORY;
	if (argc == 2) {
		directory = argv[1];
	} else if (argc != 1) {
		/* Refuses more than one operand. */
		fprintf(stderr, "usage: bootlog [directory]\n");
		return 2;
	}

	/* Lets a stop request end the waits and make the last write. */
	installed = bootlog_signals_install();
	if (installed != 0) {
		fprintf(stderr, "bootlog: cannot catch the stop signals: %s\n",
			strerror(errno));
		return 1;
	}

	/* Leaves the boot alone when the directory is not on a FAT volume. */
	ready = bootlog_volume_wait(directory);

	/* Ends quietly when the system stopped during the wait. */
	if (bootlog_stop)
		return 0;

	/* Says once why this boot keeps no log. */
	if (!ready) {
		printf("bootlog: %s is not on a FAT volume; no log is kept\n",
		       directory);
		return 0;
	}

	/* Learns how much of the log the kernel keeps. */
	capacity_size = sizeof(capacity);
	read_error = sysctlbyname("kern.msgbuf_size",
				  &capacity,
				  &capacity_size,
				  NULL,
				  0);
	if (read_error != 0 || capacity == 0) {
		fprintf(stderr, "bootlog: cannot learn the size of the kernel log: %s\n",
			strerror(errno));
		return 1;
	}

	/* Prepares the state with room for a whole copy of the log. */
	memset(&state, 0, sizeof(state));
	state.directory = directory;
	state.capacity = (size_t)capacity;
	state.text = malloc(state.capacity);
	if (state.text == NULL) {
		fprintf(stderr, "bootlog: no memory for the kernel log\n");
		return 1;
	}

	/* Creates this boot's file. */
	state.descriptor = bootlog_file_create(directory, BOOTLOG_KEEP, &number);
	if (state.descriptor < 0) {
		fprintf(stderr, "bootlog: cannot create a log in %s: %s\n",
			directory,
			strerror(errno));
		free(state.text);
		return 1;
	}
	printf("bootlog: keeping the kernel log in %s/boot-%06lu.txt\n",
	       directory,
	       number);

	/* Skips the times after the boot that went by before the service started. */
	now_ms = bootlog_uptime_ms();
	milestones = sizeof(bootlog_milestones) / sizeof(bootlog_milestones[0]);
	while (state.milestone < milestones &&
	       (uint64_t)bootlog_milestones[state.milestone] * 1000ULL <= now_ms)
		state.milestone++;

	/*
	 * Keeps what the boot has logged so far.  The trigger words in it are
	 * then kept already, so the scans start after it: a UFS line of every
	 * boot's mount does not ask for a second write.
	 */
	bootlog_write(&state, "bootlog started");
	state.scanned_total = state.written_total;

	/* Reads the log every few seconds and keeps it when there is cause. */
	while (!bootlog_stop) {
		bootlog_sleep(BOOTLOG_POLL_SECONDS);

		/* A stop request cut the sleep short; the last write follows. */
		if (bootlog_stop)
			break;

		bootlog_poll(&state);
	}

	/* Keeps the end of the log as the system stops. */
	bootlog_write(&state, "the system is stopping");
	close(state.descriptor);
	free(state.text);

	/* Succeeded: the log of this boot is on the volume. */
	return 0;
}

/* Notes a request to stop; the main loop acts on it. */
static void
bootlog_on_signal(
	int signal_number)
{
	(void)signal_number;

	/* Tells the main loop to make the last write and end. */
	bootlog_stop = 1;
}

/*
 * Catches the signals that ask the service to stop.
 *
 * Without SA_RESTART, so that the signal cuts a sleep short.
 */
static int
bootlog_signals_install(
	void)
{
	static const int signals[] = {
		SIGTERM,
		SIGINT,
		SIGHUP,
	};
	struct sigaction action;
	size_t index;
	int installed;

	/* Describes the handler once for every signal. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = bootlog_on_signal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;

	/* Installs it for each stop signal. */
	for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++) {
		installed = sigaction(signals[index], &action, NULL);
		if (installed != 0)
			return -1;
	}

	/* Succeeded: a stop request now reaches the main loop. */
	return 0;
}

/*
 * Waits for the directory's volume to be a FAT one.
 *
 * The kernel shows the boot FAT before init starts, so it is normally there
 * at once; a volume that fstab mounts may come a little later.  The
 * directory itself may not exist yet, so its parent is what is asked.
 * Reports nonzero when the volume is FAT.
 */
static int
bootlog_volume_wait(
	const char *directory)
{
	char parent[512];
	char *slash;
	unsigned waited;
	int written;
	int fat;

	/* Copies the directory's name, which a name too long for the copy cannot pass. */
	written = snprintf(parent, sizeof(parent), "%s", directory);
	if (written < 0 || (size_t)written >= sizeof(parent))
		return 0;

	/* Cuts the copy down to the directory's parent. */
	slash = strrchr(parent, '/');
	if (slash == NULL) {
		/* A relative name lives in the current directory. */
		strcpy(parent, ".");
	} else if (slash == parent) {
		/* A name directly under the root has the root as parent. */
		parent[1] = '\0';
	} else {
		/* Any other name ends at its last separator. */
		*slash = '\0';
	}

	/* Asks every few seconds until the volume is FAT, or the wait is over. */
	for (waited = 0; waited <= BOOTLOG_WAIT_SECONDS; waited += BOOTLOG_POLL_SECONDS) {
		fat = bootlog_volume_is_fat(parent);
		if (fat)
			return 1;

		/* Gives up the wait when the system stops. */
		if (bootlog_stop)
			return 0;

		bootlog_sleep(BOOTLOG_POLL_SECONDS);
	}

	/* The volume never became FAT. */
	return 0;
}

/* Reports nonzero when a path lies on a FAT volume. */
static int
bootlog_volume_is_fat(
	const char *path)
{
	struct statvfs volume;
	int asked;
	int differs;

	/* Asks which kind of volume holds the path. */
	asked = statvfs(path, &volume);
	if (asked != 0)
		return 0;

	/* The kernel names its FAT driver "fat". */
	volume.f_basetype[sizeof(volume.f_basetype) - 1U] = '\0';
	differs = strcmp(volume.f_basetype, "fat");
	if (differs != 0)
		return 0;

	/* The path is on a FAT volume. */
	return 1;
}

/* Reports the time since the boot, in milliseconds. */
static uint64_t
bootlog_uptime_ms(
	void)
{
	struct timespec now;
	int read_error;
	uint64_t milliseconds;

	/* Reads the clock that starts at the boot. */
	read_error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (read_error != 0)
		return 0;

	/* Converts it to milliseconds. */
	milliseconds = (uint64_t)now.tv_sec * 1000ULL;
	milliseconds += (uint64_t)now.tv_nsec / 1000000ULL;

	/* Succeeded: the time since the boot. */
	return milliseconds;
}

/* Sleeps for some seconds, or until a signal arrives. */
static void
bootlog_sleep(
	unsigned seconds)
{
	struct timespec interval;

	/* A signal cuts the sleep short, and the caller looks at bootlog_stop. */
	interval.tv_sec = (time_t)seconds;
	interval.tv_nsec = 0;
	(void)nanosleep(&interval, NULL);
}

/*
 * Reports how many bytes the kernel has logged since the boot.
 *
 * Only the length of the log is asked, not its text, so a poll that finds
 * nothing new costs no copy.
 */
static int
bootlog_log_total(
	uint64_t *total)
{
	uint64_t dropped;
	size_t dropped_size;
	size_t length;
	int read_error;

	/* Reads how much the ring let go of. */
	dropped_size = sizeof(dropped);
	read_error = sysctlbyname("kern.msgbuf_dropped",
				  &dropped,
				  &dropped_size,
				  NULL,
				  0);
	if (read_error != 0)
		return -1;

	/* Reads how much the ring holds. */
	length = 0;
	read_error = sysctlbyname("kern.msgbuf", NULL, &length, NULL, 0);
	if (read_error != 0)
		return -1;

	/* Succeeded: the two make up everything logged. */
	*total = dropped + (uint64_t)length;
	return 0;
}

/*
 * Copies the kernel log and learns where in the boot's text the copy begins.
 *
 * The count of dropped bytes is read before and after the copy; when the
 * two differ the ring moved during the copy, and the copy is made again.
 */
static int
bootlog_snapshot(
	struct bootlog_state *state)
{
	uint64_t before;
	uint64_t after;
	size_t value_size;
	size_t length;
	int read_error;
	int tries;

	/* Copies until the ring stood still during a copy. */
	for (tries = 0; tries < BOOTLOG_SNAPSHOT_TRIES; tries++) {
		/* Reads the dropped count before the copy. */
		value_size = sizeof(before);
		read_error = sysctlbyname("kern.msgbuf_dropped",
					  &before,
					  &value_size,
					  NULL,
					  0);
		if (read_error != 0)
			return -1;

		/* Copies the ring, which always fits a buffer of its capacity. */
		length = state->capacity;
		read_error = sysctlbyname("kern.msgbuf",
					  state->text,
					  &length,
					  NULL,
					  0);
		if (read_error != 0)
			return -1;

		/* Reads the dropped count after the copy. */
		value_size = sizeof(after);
		read_error = sysctlbyname("kern.msgbuf_dropped",
					  &after,
					  &value_size,
					  NULL,
					  0);
		if (read_error != 0)
			return -1;

		/* The copy begins at the dropped count when nothing moved. */
		if (before == after) {
			state->length = length;
			state->dropped = before;
			return 0;
		}
	}

	/* The log kept moving under every copy. */
	errno = EAGAIN;
	return -1;
}

/*
 * Looks for the trigger words in the text the last scan did not see.
 *
 * The scan starts a little before that text, so a word that the previous
 * copy cut in two is found whole.
 */
static void
bootlog_scan(
	struct bootlog_state *state)
{
	char reason[BOOTLOG_REASON_SIZE];
	const char *word;
	uint64_t lost;
	size_t offset;

	/* Finds the text the last scan did not see. */
	offset = bootlog_unseen_offset(state->scanned_total,
				       state->dropped,
				       state->length,
				       &lost);
	state->scanned_total = state->dropped + (uint64_t)state->length;
	if (offset == state->length)
		return;

	/* Steps back over a word the previous copy may have cut. */
	if (lost == 0 && offset >= BOOTLOG_TRIGGER_OVERLAP)
		offset -= BOOTLOG_TRIGGER_OVERLAP;
	else if (lost == 0)
		offset = 0;

	/* Asks for a write when the new text says a trigger word. */
	word = bootlog_trigger_find(state->text + offset, state->length - offset);
	if (word == NULL)
		return;
	snprintf(reason, sizeof(reason), "the kernel log says \"%s\"", word);
	bootlog_request(state, reason);
}

/*
 * Asks for a write, for the reason given.
 *
 * A write already asked for keeps its first reason: one write keeps the
 * text of both.
 */
static void
bootlog_request(
	struct bootlog_state *state,
	const char *reason)
{
	/* Keeps the reason of the write already asked for. */
	if (state->pending)
		return;

	/* Records the write and its reason for the next poll that may write. */
	snprintf(state->reason, sizeof(state->reason), "%s", reason);
	state->pending = 1;
}

/* Writes the header that says why and when a stretch of the log is kept. */
static void
bootlog_header_format(
	char *header,
	size_t size,
	const char *reason,
	uint64_t uptime_ms,
	uint64_t lost,
	uint64_t left_out,
	int read_error)
{
	struct statvfs root;
	struct tm calendar;
	struct tm *broken_down;
	char clock_text[32];
	size_t clock_length;
	const char *root_state;
	time_t now;
	size_t used;
	int asked;
	int written;

	/* Names the state of the root mount, which BUG-288 found read-only. */
	root_state = "unknown";
	asked = statvfs("/", &root);
	if (asked == 0 && (root.f_flag & ST_RDONLY) != 0)
		root_state = "read-only";
	else if (asked == 0)
		root_state = "read-write";

	/* Reads the wall clock, which a Raspberry Pi without a network has not set. */
	snprintf(clock_text, sizeof(clock_text), "unknown");
	now = time(NULL);
	broken_down = gmtime_r(&now, &calendar);
	if (broken_down != NULL) {
		clock_length = strftime(clock_text,
					sizeof(clock_text),
					"%Y-%m-%d %H:%M:%S UTC",
					&calendar);
		if (clock_length == 0)
			snprintf(clock_text, sizeof(clock_text), "unknown");
	}

	/* Writes the reason, the times and the root's state. */
	written = snprintf(header,
			   size,
			   "\n=== bootlog: %s; %llu.%llu s after boot; clock %s; / mounted %s ===\n",
			   reason,
			   (unsigned long long)(uptime_ms / 1000ULL),
			   (unsigned long long)((uptime_ms % 1000ULL) / 100ULL),
			   clock_text,
			   root_state);
	if (written < 0 || (size_t)written >= size)
		return;
	used = (size_t)written;

	/* Says how much of the log went before it could be kept. */
	if (lost != 0) {
		written = snprintf(header + used,
				   size - used,
				   "=== bootlog: %llu bytes of the kernel log were dropped before they were kept ===\n",
				   (unsigned long long)lost);
		if (written < 0 || (size_t)written >= size - used)
			return;
		used += (size_t)written;
	}

	/* Says how much of the log was left out for want of room. */
	if (left_out != 0) {
		written = snprintf(header + used,
				   size - used,
				   "=== bootlog: %llu bytes of the kernel log were left out: the file or the volume is full ===\n",
				   (unsigned long long)left_out);
		if (written < 0 || (size_t)written >= size - used)
			return;
		used += (size_t)written;
	}

	/* Says that the log could not be read, when it could not. */
	if (read_error != 0) {
		(void)snprintf(header + used,
			       size - used,
			       "=== bootlog: the kernel log could not be read (error %d) ===\n",
			       read_error);
	}
}

/*
 * Appends the text of the log not kept yet, under a header, to the file.
 *
 * A failed write is reported and leaves the text to the next write.
 */
static void
bootlog_write(
	struct bootlog_state *state,
	const char *reason)
{
	char header[BOOTLOG_HEADER_SIZE];
	uint64_t now_ms;
	uint64_t lost;
	uint64_t room;
	uint64_t left_out;
	size_t offset;
	size_t length;
	int read_error;
	int copied;
	int appended;

	/* Copies the log as it is now. */
	read_error = 0;
	copied = bootlog_snapshot(state);
	if (copied != 0)
		read_error = errno;

	/* Finds the text the file does not hold yet. */
	offset = 0;
	length = 0;
	lost = 0;
	if (copied == 0) {
		offset = bootlog_unseen_offset(state->written_total,
					       state->dropped,
					       state->length,
					       &lost);
		length = state->length - offset;
	}

	/* Keeps only the newest text that the file and the volume have room for. */
	left_out = 0;
	room = bootlog_room(state);
	if ((uint64_t)length > room) {
		left_out = (uint64_t)length - room;
		offset += (size_t)left_out;
		length = (size_t)room;
	}

	/* Appends the header and the text, and makes them durable. */
	now_ms = bootlog_uptime_ms();
	bootlog_header_format(header,
			      sizeof(header),
			      reason,
			      now_ms,
			      lost,
			      left_out,
			      read_error);
	appended = bootlog_file_append(state->descriptor,
				       header,
				       state->text + offset,
				       length);
	state->writes++;
	state->last_write_ms = now_ms;
	if (appended != 0) {
		fprintf(stderr, "bootlog: cannot keep the kernel log: %s\n",
			strerror(errno));
		return;
	}

	/* Records the size of the file. */
	state->file_bytes += (uint64_t)strlen(header) + (uint64_t)length;

	/* Records how far the file now holds the log, the text left out included. */
	if (copied == 0)
		state->written_total = state->dropped + (uint64_t)state->length;
}

/*
 * Reports how many bytes of text the next write may put in the file.
 *
 * None once the file reached BOOTLOG_FILE_MAX or the volume has less than
 * BOOTLOG_FREE_MIN free; a volume whose free space cannot be read is not
 * held back.
 */
static uint64_t
bootlog_room(
	const struct bootlog_state *state)
{
	struct statvfs volume;
	uint64_t free_bytes;
	int asked;

	/* A full file takes no more text. */
	if (state->file_bytes >= BOOTLOG_FILE_MAX)
		return 0;

	/* A volume nearly full takes no more text. */
	asked = statvfs(state->directory, &volume);
	if (asked == 0) {
		free_bytes = (uint64_t)volume.f_bavail * volume.f_frsize;
		if (free_bytes < BOOTLOG_FREE_MIN)
			return 0;
	}

	/* The rest of the file's allowance. */
	return BOOTLOG_FILE_MAX - state->file_bytes;
}

/*
 * Reads the log from memory and writes it when there is cause.
 *
 * The causes are a time after the boot reached, a trigger word in the new
 * text, and a ring about to let go of text the file does not hold.  A
 * write waits for BOOTLOG_INTERVAL_MS after the previous one, and none is
 * made past BOOTLOG_WRITES_MAX until the stop.
 */
static void
bootlog_poll(
	struct bootlog_state *state)
{
	char reason[BOOTLOG_REASON_SIZE];
	uint64_t now_ms;
	uint64_t total;
	size_t milestones;
	unsigned seconds;
	int counted;
	int copied;

	now_ms = bootlog_uptime_ms();

	/* Looks for the trigger words when the kernel logged anything new. */
	counted = bootlog_log_total(&total);
	if (counted == 0 && total > state->scanned_total) {
		copied = bootlog_snapshot(state);
		if (copied == 0)
			bootlog_scan(state);
	}

	/* Asks for a write at each time after the boot that has come. */
	milestones = sizeof(bootlog_milestones) / sizeof(bootlog_milestones[0]);
	if (state->milestone < milestones &&
	    (uint64_t)bootlog_milestones[state->milestone] * 1000ULL <= now_ms) {
		/* Passes every time that has come, naming the latest in the reason. */
		seconds = 0;
		while (state->milestone < milestones &&
		       (uint64_t)bootlog_milestones[state->milestone] * 1000ULL <= now_ms) {
			seconds = bootlog_milestones[state->milestone];
			state->milestone++;
		}

		snprintf(reason, sizeof(reason), "%u s after boot", seconds);
		bootlog_request(state, reason);
	}

	/* Asks for a write before the ring lets go of text the file does not hold. */
	if (counted == 0 && total - state->written_total > (uint64_t)(state->capacity / 2U))
		bootlog_request(state, "the kernel log is filling");

	/* Nothing to do without a write asked for. */
	if (!state->pending)
		return;

	/* Drops the request once the boot has made all its writes. */
	if (state->writes >= BOOTLOG_WRITES_MAX) {
		/* Says so the first time only. */
		if (!state->limit_reported) {
			fprintf(stderr, "bootlog: %u writes made; the log is kept again only at the stop\n",
				BOOTLOG_WRITES_MAX);
			state->limit_reported = 1;
		}

		state->pending = 0;
		return;
	}

	/* Leaves the request for a later poll while the last write is recent. */
	if (now_ms - state->last_write_ms < BOOTLOG_INTERVAL_MS)
		return;

	/* Makes the write asked for. */
	state->pending = 0;
	bootlog_write(state, state->reason);
}
