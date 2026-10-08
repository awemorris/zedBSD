/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sessiond, the graphical login's session manager
 * (plan/ws035/login-manager-design.md).
 *
 * init starts it as the service "greeter" in place of the console's getty.
 * It is small and runs as root: it gives the display and the input devices
 * to the seat's user, starts the greeter (the compositor --greeter) as the
 * unprivileged _greeter account, checks the passwords the greeter sends it,
 * starts the user's session (/etc/keiland/session) as the user, and starts
 * the greeter again when the session ends.  It draws nothing and reads no
 * image or font.
 *
 * It keeps to the console when the boot parameters do not ask for the
 * graphical login (login=graphical), when there is no greeter or no display,
 * and after the greeter has failed six times in a row, the waits between
 * the tries growing from 1 to 16 seconds (BUG-122: a GPU that a crashed
 * compositor left behind may take a while to be usable again): it exits 0, and
 * init starts the console's getty that the greeter service replaces.  A GPU
 * driver may publish the display (/dev/gpu0) a moment after init has started
 * the services; while the kernel reports a GPU device still attaching
 * (hw.gpu.attaching), sessiond waits for the display for a bounded time.
 */

#include "sessiond.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* How many greeters in a row may fail before sessiond gives the console back (BUG-122: six, about half a minute in all). */
#define MAIN_GREETER_FAILURES	6

/* The wait before the second try, in seconds, doubled for each failure after it up to the longest. */
#define MAIN_RETRY_FIRST	1U
#define MAIN_RETRY_LONGEST	16U

/* The log sessiond writes its lines to. */
#define MAIN_LOG_PATH		"/var/log/sessiond.log"

/* The boot parameter that chooses the graphical login, as the kernel reports it. */
#define MAIN_LOGIN_SYSCTL	"kern.boot.login"

/* The display device the greeter draws on. */
#define MAIN_DISPLAY_PATH	"/dev/gpu0"

/* The kernel's count of GPU devices whose node is still on its way. */
#define MAIN_GPU_ATTACHING_SYSCTL	"hw.gpu.attaching"

/* How long, in milliseconds, sessiond waits for a display that a GPU driver is still attaching. */
#define MAIN_DISPLAY_WAIT_MS	15000LL

/* How long, in nanoseconds, sessiond pauses before it looks for the display again. */
#define MAIN_DISPLAY_POLL_NS	100000000L

/* The file naming the account logged in by itself at boot (2026-09-29 user decision); without it, the greeter. */
#define MAIN_AUTOLOGIN_PATH	"/etc/keiland/autologin"

/* Set by SIGTERM and SIGINT: sessiond ends its greeter or session and stops. */
volatile sig_atomic_t sessiond_stopping;

static unsigned main_retry_delay(unsigned failures);
static int main_options(struct sessiond *daemon, int count, char **arguments, int *graphical, int *console);
static int main_boot_graphical(void);
static int main_ready(struct sessiond *daemon);
static int main_wait_display(void);
static unsigned main_gpu_attaching(void);
static void main_stop(int signal_number);
static void main_open_log(void);
static int main_autologin(struct sessiond_account *account);

/*
 * Runs the session manager until it is stopped or gives the console back.
 */
int
main(
	int count,
	char **arguments)
{
	struct sessiond daemon;
	struct sessiond_account account;
	enum sessiond_greeter_end end;
	struct sigaction action;
	unsigned failures;
	unsigned delay;
	int graphical;
	int console;
	uid_t uid;
	int error;
	int ready;

	/* Only root can give devices away and start sessions as other users. */
	uid = geteuid();
	if (uid != 0) {
		fprintf(stderr, "sessiond: must be run as root\n");
		return 1;
	}

	/* Reads the options. */
	memset(&daemon, 0, sizeof(daemon));
	daemon.greeter = SESSIOND_GREETER;
	daemon.session = SESSIOND_SESSION;
	daemon.greeter_socket = -1;
	graphical = 0;
	console = 0;
	error = main_options(&daemon, count, arguments, &graphical, &console);
	if (error != 0) {
		fprintf(stderr, "usage: sessiond [--graphical | --console] [--greeter=/path] [--session=/path]\n");
		return 2;
	}

	/* The log, and the security log for logins. */
	main_open_log();
	openlog("sessiond", LOG_PID, LOG_AUTH);

	/* The boot parameters choose the console login unless an option says which. */
	if (!graphical && !console)
		graphical = main_boot_graphical();

	/* The console login: init starts the console's getty when this exits. */
	if (!graphical || console) {
		sessiond_log("SESSIOND CONSOLE reason=boot-parameters");
		return 0;
	}

	/* Without a greeter or a display there is no graphical login either. */
	ready = main_ready(&daemon);
	if (!ready)
		return 0;

	/* SIGTERM and SIGINT end the greeter or the session and then sessiond. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = main_stop;
	sigemptyset(&action.sa_mask);
	(void)sigaction(SIGTERM, &action, NULL);
	(void)sigaction(SIGINT, &action, NULL);

	/* A client of the session writing to a closed pipe must not stop sessiond. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* A device plugged in later is the seat's user's at once (BUG-264). */
	sessiond_seat_events_open();

	/* The greeter, then a session for each login, until stopped. */
	sessiond_log("SESSIOND START greeter=%s session=%s pid=%ld", daemon.greeter, daemon.session, (long)getpid());
	failures = 0;

	/* The account /etc/keiland/autologin names is logged in once at boot; its Log Out brings the greeter. */
	memset(&account, 0, sizeof(account));
	error = main_autologin(&account);
	if (error == 0) {
		sessiond_log("SESSIOND AUTOLOGIN user=%s", account.passwd.pw_name);
		syslog(LOG_NOTICE, "automatic login of %s", account.passwd.pw_name);
		(void)sessiond_session_run(&daemon, &account);
		memset(&account, 0, sizeof(account));
	}
	while (!sessiond_stopping) {
		end = sessiond_greeter_run(&daemon, &account);

		/* sessiond is being stopped. */
		if (end == SESSIOND_GREETER_STOP)
			break;

		/* A greeter that failed: after MAIN_GREETER_FAILURES in a row the console's login takes over. */
		if (end == SESSIOND_GREETER_FAILED) {
			failures++;
			if (failures >= MAIN_GREETER_FAILURES) {
				sessiond_log("SESSIOND CONSOLE reason=greeter-failed failures=%u", failures);
				syslog(LOG_ERR, "the greeter failed %u times; the console login takes over", failures);
				sessiond_seat_restore();
				return 0;
			}

			/* Another try after a wait that grows with the failures (the GPU may still be recovering). */
			delay = main_retry_delay(failures);
			sessiond_log("SESSIOND GREETER retry failures=%u delay=%u", failures, delay);
			sleep(delay);
			continue;
		}

		/* A greeter that ended by itself is started again. */
		failures = 0;
		if (end == SESSIOND_GREETER_ENDED)
			continue;

		/* A login: the user's session (the greeter left on the screen goes once it is ready), and the greeter again when it ends. */
		(void)sessiond_session_run(&daemon, &account);
		memset(&account, 0, sizeof(account));
	}

	/* Stopped: a greeter a Log Out left goes, and the devices go back to root. */
	sessiond_greeter_finish(&daemon);
	sessiond_seat_restore();
	sessiond_log("SESSIOND STOP");

	/* Succeeded: sessiond ended its greeter or session. */
	return 0;
}

/*
 * Writes one line to sessiond's log with the time in front.
 */
void
sessiond_log(
	const char *format,
	...)
{
	char text[SESSIOND_LINE_MAX];
	va_list arguments;

	/* Formats the line. */
	va_start(arguments, format);
	(void)vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);

	/* Writes it whole. */
	printf("%lld %s\n", (long long)time(NULL), text);
	fflush(stdout);
}

/*
 * Reads one line from a descriptor, a byte at a time (nothing after it is
 * taken), waiting at most timeout_ms in all.  Returns 1 with the line (its
 * line end removed; a line too long is cut), 0 when the time ran out, or
 * -1 at the end of the stream or on an error.
 */
int
sessiond_read_line(
	int descriptor,
	char *line,
	size_t size,
	int timeout_ms)
{
	struct pollfd entry;
	long long deadline;
	long long left;
	size_t used;
	ssize_t count;
	char byte;
	int ready;

	/* Byte after byte until the line ends. */
	deadline = sessiond_milliseconds() + timeout_ms;
	used = 0;
	for (;;) {
		/* The time left. */
		left = deadline - sessiond_milliseconds();
		if (left <= 0)
			return 0;

		/* A byte, or the end of the wait. */
		entry.fd = descriptor;
		entry.events = POLLIN;
		entry.revents = 0;
		ready = poll(&entry, 1, (int)left);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return -1;
		if (ready == 0)
			return 0;
		count = read(descriptor, &byte, 1U);
		if (count < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (count <= 0)
			return -1;

		/* The line ends, or grows while there is room. */
		if (byte == '\n')
			break;
		if (used + 1U < size)
			line[used++] = byte;
	}

	/* Succeeded: a whole line. */
	line[used] = '\0';
	return 1;
}

/* Returns a monotonic time in milliseconds (0 when the clock cannot be read). */
long long
sessiond_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* The monotonic clock. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded. */
	return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000L);
}

/* Reads the options: which greeter and session, and whether to go graphical regardless of the boot parameters. */
static int
main_options(
	struct sessiond *daemon,
	int count,
	char **arguments,
	int *graphical,
	int *console)
{
	const char *argument;
	int index;
	int match;

	/* Each option on its own. */
	for (index = 1; index < count; index++) {
		argument = arguments[index];

		/* The graphical login, whatever the boot parameters say (tests). */
		match = strcmp(argument, "--graphical");
		if (match == 0) {
			*graphical = 1;
			continue;
		}

		/* The console login, whatever the boot parameters say. */
		match = strcmp(argument, "--console");
		if (match == 0) {
			*console = 1;
			continue;
		}

		/* Another greeter program (an absolute path). */
		match = strncmp(argument, "--greeter=", 10);
		if (match == 0 && argument[10] == '/') {
			daemon->greeter = argument + 10;
			continue;
		}

		/* Another session script (an absolute path). */
		match = strncmp(argument, "--session=", 10);
		if (match == 0 && argument[10] == '/') {
			daemon->session = argument + 10;
			continue;
		}

		/* Anything else is a mistake. */
		return EINVAL;
	}

	/* Succeeded: every option is understood. */
	return 0;
}

/* Reports whether the boot parameters ask for the graphical login (login=graphical). */
static int
main_boot_graphical(
	void)
{
	char value[32];
	size_t size;
	int error;
	int equal;

	/* Asks the kernel for the login= boot parameter. */
	memset(value, 0, sizeof(value));
	size = sizeof(value) - 1U;
	error = sysctlbyname(MAIN_LOGIN_SYSCTL, value, &size, NULL, 0);
	if (error != 0)
		return 0;

	/* Only login=graphical asks for it; login=console and no parameter do not. */
	equal = strcmp(value, "graphical");
	if (equal != 0)
		return 0;

	/* Succeeded: the boot parameters ask for the graphical login. */
	return 1;
}

/* Reports whether a graphical login can start: the greeter is there, and so is the display. */
static int
main_ready(
	struct sessiond *daemon)
{
	int error;

	/* The greeter program. */
	error = access(daemon->greeter, X_OK);
	if (error != 0) {
		sessiond_log("SESSIOND CONSOLE reason=no-greeter path=%s errno=%d", daemon->greeter, errno);
		return 0;
	}

	/* The display, which a GPU driver still attaching its device publishes shortly. */
	error = main_wait_display();
	if (error != 0) {
		sessiond_log("SESSIOND CONSOLE reason=no-display errno=%d", error);
		return 0;
	}

	/* Succeeded: the greeter can start. */
	return 1;
}

/*
 * Waits for the display while a GPU driver is still attaching a device.
 *
 * A machine where no GPU device is attaching gets its answer at once, so
 * its console login is not delayed.  Returns 0 when the display is there,
 * or the errno of the last look for it.
 */
static int
main_wait_display(
	void)
{
	struct stat status;
	struct timespec pause;
	long long start;
	long long waited;
	unsigned attaching;
	unsigned looks;
	int error;

	/* Looks for the display until it is there, no device is attaching, or the time is up. */
	start = sessiond_milliseconds();
	looks = 0U;
	for (;;) {
		/* The display is there. */
		error = stat(MAIN_DISPLAY_PATH, &status);
		if (error == 0)
			break;
		error = errno;
		looks++;

		/*
		 * No GPU device is on its way, so the display will not come.  A
		 * node published between the look and the count is taken by a
		 * last look.
		 */
		attaching = main_gpu_attaching();
		if (attaching == 0U) {
			error = stat(MAIN_DISPLAY_PATH, &status);
			if (error == 0)
				break;
			return errno;
		}

		/* The wait is bounded: a driver that never publishes gives the console back. */
		waited = sessiond_milliseconds() - start;
		if (waited >= MAIN_DISPLAY_WAIT_MS) {
			sessiond_log("SESSIOND DISPLAY timeout waited_ms=%lld attaching=%u", waited, attaching);
			return error;
		}

		/* Looks again after a moment. */
		pause.tv_sec = 0;
		pause.tv_nsec = MAIN_DISPLAY_POLL_NS;
		(void)nanosleep(&pause, NULL);
	}

	/* Notes a wait in the log, so the time the GPU took can be read afterwards. */
	if (looks != 0U) {
		waited = sessiond_milliseconds() - start;
		sessiond_log("SESSIOND DISPLAY waited_ms=%lld", waited);
	}

	/* Succeeded: the display is there. */
	return 0;
}

/* Reports how many GPU devices the kernel still has on their way to a node (0 when it cannot tell). */
static unsigned
main_gpu_attaching(
	void)
{
	uint32_t attaching;
	size_t size;
	int error;

	/* Asks the kernel; a kernel without the count never makes sessiond wait. */
	attaching = 0U;
	size = sizeof(attaching);
	error = sysctlbyname(MAIN_GPU_ATTACHING_SYSCTL, &attaching, &size, NULL, 0);
	if (error != 0)
		return 0U;

	/* An answer of another size is not the count. */
	if (size != sizeof(attaching))
		return 0U;

	/* Succeeded: the number of devices still attaching. */
	return (unsigned)attaching;
}

/* Asks the main loop to end the greeter or the session and stop. */
static void
main_stop(
	int signal_number)
{
	(void)signal_number;

	/* The loops see this between their steps. */
	sessiond_stopping = 1;
}

/* Sends sessiond's output to its log (the console has no one reading it). */
static void
main_open_log(
	void)
{
	int descriptor;

	/* The log's directory may not be there yet on a new root. */
	(void)mkdir("/var/log", 0755);

	/* Appends to the log; without one, output stays where it was. */
	descriptor = open(MAIN_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
	if (descriptor < 0)
		return;

	/* Standard output and error go to it; the original descriptor is not kept. */
	(void)dup2(descriptor, STDOUT_FILENO);
	(void)dup2(descriptor, STDERR_FILENO);
	(void)close(descriptor);
	setvbuf(stdout, NULL, _IOLBF, 0);
}

/*
 * Looks up the account /etc/keiland/autologin names (its first line) for
 * the login at boot.  Returns 0, or ENOENT when there is no such file or
 * no such account (root is never logged in by itself).
 */
static int
main_autologin(
	struct sessiond_account *account)
{
	struct passwd *found;
	char name[64];
	size_t length;
	FILE *file;
	char *line;
	int error;

	/* The file's first line, without its newline. */
	file = fopen(MAIN_AUTOLOGIN_PATH, "r");
	if (file == NULL)
		return ENOENT;
	line = fgets(name, sizeof(name), file);
	(void)fclose(file);
	if (line == NULL)
		return ENOENT;
	length = strcspn(name, "\r\n \t");
	name[length] = '\0';
	if (length == 0U)
		return ENOENT;

	/* The account by its name. */
	found = NULL;
	error = getpwnam_r(name, &account->passwd, account->buffer, sizeof(account->buffer), &found);
	if (error != 0 || found == NULL) {
		sessiond_log("SESSIOND AUTOLOGIN no account=%s", name);
		return ENOENT;
	}

	/* root is not logged in without a password. */
	if (account->passwd.pw_uid == 0) {
		sessiond_log("SESSIOND AUTOLOGIN refused account=%s uid=0", name);
		return ENOENT;
	}

	/* Succeeded: the account to log in. */
	return 0;
}

/* Reports the wait before the next greeter after some failures in a row: 1, 2, 4, 8, then 16 seconds. */
static unsigned
main_retry_delay(
	unsigned failures)
{
	unsigned delay;
	unsigned count;

	/* Doubled for each failure after the first, up to the longest. */
	delay = MAIN_RETRY_FIRST;
	for (count = 1; count < failures && delay < MAIN_RETRY_LONGEST; count++)
		delay *= 2U;
	if (delay > MAIN_RETRY_LONGEST)
		delay = MAIN_RETRY_LONGEST;

	/* The wait. */
	return delay;
}
