/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Terminals.
 *
 * One line discipline serves the virtual consoles and the pseudo
 * terminal slaves: canonical editing with echo, signal characters,
 * software flow control, and job-control checks against the
 * controlling terminal.  The consoles render through the HAL console
 * with a small ANSI escape subset and keep per-terminal history for
 * switching; a pseudo terminal moves bytes between its master and slave
 * through a ring buffer instead.
 */

#include "kern/tty.h"

#include "kern/clock.h"
#include "kern/file.h"
#include "kern/cdev.h"
#include "kern/kmem.h"
#include "kern/input-keymap.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/syscall.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <poll.h>
#include <string.h>
#include <termios.h>

#define TTY_LINE_MAX 256U
#define TTY_RECORDS 8U
#define TTY_INPUT_MAX 512U
#define TTY_VT_COUNT 4U
#define TTY_VT_HISTORY 8192U
#define TTY_ECHO_MAX (TTY_LINE_MAX * 6U + 4U)
#define TTY_VDISABLE ((cc_t)0xffU)

/*
 * UNIX98-style pseudo terminals.  The slave uses the same termios and
 * background-process checks as the physical console; only its byte
 * transport is different.
 */
#define PTY_MAX 8U
#define PTY_OUTPUT_MAX 4096U

struct tty_record {
	uint8_t data[TTY_LINE_MAX];
	size_t length;
	size_t offset;
	unsigned eof;
};

struct tty {
	struct spinlock lock;
	struct wait_queue read_waitq;
	struct wait_queue write_waitq;
	struct termios termios;
	struct winsize winsize;
	pid_t session;
	pid_t foreground_pgrp;
	uint64_t association_generation;
	uint8_t edit[TTY_LINE_MAX];
	size_t edit_used;
	struct tty_record records[TTY_RECORDS];
	unsigned record_head;
	unsigned record_tail;
	unsigned record_used;
	uint8_t input[TTY_INPUT_MAX];
	unsigned input_head;
	unsigned input_tail;
	unsigned input_used;
	unsigned hungup;
	unsigned output_stopped;
	unsigned output_stopped_by_ixon;
	unsigned literal_next;
};

enum tty_background_operation {
	TTY_BACKGROUND_READ,
	TTY_BACKGROUND_WRITE,
	TTY_BACKGROUND_CONTROL,
};

struct tty_input_result {
	uint8_t echo[TTY_ECHO_MAX];
	size_t echo_length;
	pid_t signal_session;
	pid_t signal_pgrp;
	int signal_number;
	unsigned output_flags;
	unsigned notify;
	unsigned flow_changed;
	unsigned output_stopped;
};

struct pty_pair {
	struct spinlock lock;
	struct wait_queue output_waitq;
	struct tty slave;
	uint8_t output[PTY_OUTPUT_MAX];
	unsigned output_head;
	unsigned output_tail;
	unsigned output_used;
	unsigned slave_output_stopped;
	unsigned index;
	unsigned generation;
	unsigned active;
	unsigned locked;
	unsigned master_open;
	unsigned slave_opens;
	unsigned slave_ever_opened;
};

struct pty_handle {
	struct pty_pair *pair;
	unsigned generation;
	unsigned master;
};

static struct tty console_ttys[TTY_VT_COUNT];
static struct spinlock console_output_lock;
static unsigned active_vt;
static unsigned console_escape_state[TTY_VT_COUNT];
static unsigned console_escape_parameter[TTY_VT_COUNT];
static unsigned console_escape_has_parameter[TTY_VT_COUNT];
static char vt_history[TTY_VT_COUNT][TTY_VT_HISTORY];
static size_t vt_history_used[TTY_VT_COUNT];
static struct spinlock pty_registry_lock;
static struct pty_pair pty_pairs[PTY_MAX];

static void tty_flush_input_locked(struct tty *tty);
static void tty_default_termios(struct termios *termios);
static void tty_console_csi(unsigned vt, unsigned command);
static void tty_render(unsigned vt, const char *bytes, size_t length);
static void tty_echo(struct tty *tty, const char *bytes, size_t length);
static void tty_commit_locked(struct tty *tty, unsigned eof);
static int tty_cc_matches(const struct tty *tty, unsigned index, uint8_t byte);
static void tty_echo_append(struct tty_input_result *result, uint8_t byte);
static void tty_echo_character(const struct tty *tty, struct tty_input_result *result, uint8_t byte);
static void tty_echo_erase(const struct tty *tty, struct tty_input_result *result, uint8_t byte);
static void tty_input_byte_locked(struct tty *tty, uint8_t byte, struct tty_input_result *result);
static int tty_process_controls(struct tty *tty, struct process *process);
static void tty_advance_association_locked(struct tty *tty);
static int tty_assign_controlling(struct tty *tty, struct process *process);
static int tty_background(struct tty *tty, struct process *process, enum tty_background_operation operation);
static int tty_wait_output_enabled(struct tty *tty, struct file *file);
static ssize_t tty_read_canonical(struct tty *tty, void *buffer, size_t size, int nonblocking);
static ssize_t tty_read_noncanonical(struct tty *tty, void *buffer, size_t size, int nonblocking);
static int tty_termios_valid(const struct termios *value);
static int tty_ioctl_instance(struct tty *tty, struct file *file, unsigned long request, uintptr_t argument);
static int pty_handle_valid_locked(const struct pty_handle *handle);
static ssize_t pty_output_bytes(struct pty_pair *pair, const uint8_t *bytes, size_t length, int nonblocking);
static struct pty_pair * tty_backend_pair(struct tty *tty);
static void tty_backend_set_flow(struct tty *tty, unsigned stopped);
static int tty_backend_drain(struct tty *tty, struct file *file);
static int tty_backend_flush_output(struct tty *tty, struct file *file);
static int tty_backend_send_control(struct tty *tty, struct file *file, uint8_t byte);
static void pty_input_byte(struct pty_pair *pair, uint8_t byte);
static int pty_master_open(struct file *file);
static int pty_master_close(struct file *file);
static ssize_t pty_master_read(struct file *file, void *buffer, size_t size);
static ssize_t pty_master_write(struct file *file, const void *buffer, size_t size);
static int pty_master_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int pty_master_poll(struct file *file, short events, short *revents);
static int pty_slave_open(struct file *file);
static int pty_slave_close(struct file *file);
static ssize_t pty_slave_read(struct file *file, void *buffer, size_t size);
static ssize_t pty_slave_write(struct file *file, const void *buffer, size_t size);
static int pty_slave_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int pty_slave_poll(struct file *file, short events, short *revents);

static const struct file_ops pty_master_file_ops = {
	.read = pty_master_read,
	.write = pty_master_write,
	.ioctl = pty_master_ioctl,
	.poll = pty_master_poll,
	.close = pty_master_close,
};

static const struct cdev_ops pty_ptmx_ops = {
	.open = pty_master_open,
};

const struct file_ops tty_pty_slave_file_ops = {
	.open = pty_slave_open,
	.read = pty_slave_read,
	.write = pty_slave_write,
	.ioctl = pty_slave_ioctl,
	.poll = pty_slave_poll,
	.close = pty_slave_close,
};

/*
 * Initializes the virtual consoles with default settings.
 */
int
tty_console_init(
	void)
{
	unsigned i;

	/* Starts every console empty with the first one active. */
	memset(console_ttys, 0, sizeof(console_ttys));
	memset(vt_history_used, 0, sizeof(vt_history_used));
	active_vt = 0;
	spin_init(&console_output_lock, LOCK_RANK_TTY, "console output");
	for (i = 0; i < TTY_VT_COUNT; i++) {
		spin_init(&console_ttys[i].lock, LOCK_RANK_TTY, "virtual tty");
		waitq_init(&console_ttys[i].read_waitq, "virtual tty input");
		waitq_init(&console_ttys[i].write_waitq, "virtual tty output");
		tty_default_termios(&console_ttys[i].termios);
		console_ttys[i].winsize.ws_row = HAL_CONS_ROWS;
		console_ttys[i].winsize.ws_col = HAL_CONS_COLUMNS;
		console_ttys[i].association_generation = 1;
	}

	/* Reports the initialized consoles. */
	return 0;
}

/*
 * Reports the number of virtual consoles.
 */
unsigned
tty_vt_count(
	void)
{
	return TTY_VT_COUNT;
}

/*
 * Reports the active virtual console.
 */
unsigned
tty_vt_active(
	void)
{
	return active_vt;
}

/*
 * Switches the display to a virtual console, replaying its history.
 */
int
tty_vt_activate(
	unsigned vt)
{
	unsigned long irq;

	/* Rejects an unknown console. */
	if (vt >= TTY_VT_COUNT)
		return EINVAL;

	/* Clears the screen and re-renders the console's history. */
	irq = spin_lock_irqsave(&console_output_lock);

	active_vt = vt;
	console_escape_state[vt] = 0;
	hal_cons_clear();
	tty_render(vt, vt_history[vt], vt_history_used[vt]);
	hal_cons_update_cursor();

	spin_unlock_irqrestore(&console_output_lock, irq);

	/* Reports the switched console. */
	return 0;
}

/*
 * Feeds a keyboard event to the active console.
 *
 * Alt with a function key switches consoles, navigation keys become
 * ANSI sequences for noncanonical readers, and everything else goes
 * through the line discipline.
 */
void
tty_console_input_event(
	uint32_t event)
{
	struct tty *tty;
	struct tty_input_result result;
	unsigned key;
	unsigned long irq;
	uint8_t byte;
	const char *sequence;
	size_t sequence_length;
	int accepted;
	size_t index;

	key = event & INPUT_KEY_MASK;
	sequence = NULL;
	sequence_length = 0;

	/* A graph-modified function key switches the console. */
	if ((event & INPUT_KEY_GRAPH) != 0 &&
	    key >= INPUT_KEY_F1 &&
	    key <= INPUT_KEY_F4) {
		(void)tty_vt_activate(key - INPUT_KEY_F1);
		return;
	}

	tty = &console_ttys[active_vt];

	/*
	 * Expose machine-independent ANSI key sequences to noncanonical
	 * readers.  Canonical input keeps its historical behavior and ignores
	 * navigation.
	 */
	switch (key) {
	case INPUT_KEY_UP:
		sequence = "\033[A";
		sequence_length = 3;
		break;
	case INPUT_KEY_DOWN:
		sequence = "\033[B";
		sequence_length = 3;
		break;
	case INPUT_KEY_RIGHT:
		sequence = "\033[C";
		sequence_length = 3;
		break;
	case INPUT_KEY_LEFT:
		sequence = "\033[D";
		sequence_length = 3;
		break;
	case INPUT_KEY_HOME:
		sequence = "\033[H";
		sequence_length = 3;
		break;
	case INPUT_KEY_END:
		sequence = "\033[F";
		sequence_length = 3;
		break;
	case INPUT_KEY_INSERT:
		sequence = "\033[2~";
		sequence_length = 4;
		break;
	case INPUT_KEY_DELETE:
		sequence = "\033[3~";
		sequence_length = 4;
		break;
	case INPUT_KEY_PAGE_UP:
		sequence = "\033[5~";
		sequence_length = 4;
		break;
	case INPUT_KEY_PAGE_DOWN:
		sequence = "\033[6~";
		sequence_length = 4;
		break;
	default:
		break;
	}

	if (sequence != NULL) {
		accepted = 0;
		irq = spin_lock_irqsave(&tty->lock);
		if ((tty->termios.c_lflag & ICANON) == 0 &&
		    TTY_INPUT_MAX - tty->input_used >= sequence_length) {
			for (index = 0; index < sequence_length; index++) {
				tty->input[tty->input_head] = (uint8_t)sequence[index];
				tty->input_head = (tty->input_head + 1U) % TTY_INPUT_MAX;
			}

			tty->input_used += sequence_length;
			waitq_wake_all(&tty->read_waitq);
			accepted = 1;
		}

		spin_unlock_irqrestore(&tty->lock, irq);
		if (accepted)
			poll_notify();
		return;
	}

	/* Maps the key to a byte, applying the control modifier. */
	if (key == INPUT_KEY_ENTER)
		byte = '\n';
	else if (key == INPUT_KEY_BACKSPACE)
		byte = 8;
	else if (key == INPUT_KEY_TAB)
		byte = '\t';
	else if (key > 0xffU)
		return;
	else
		byte = (uint8_t)key;
	if ((event & INPUT_KEY_CTRL) != 0 && byte >= 'a' && byte <= 'z')
		byte = (uint8_t)(byte - 'a' + 1);
	else if ((event & INPUT_KEY_CTRL) != 0 && byte >= 'A' && byte <= 'Z')
		byte = (uint8_t)(byte - 'A' + 1);

	/* Runs the line discipline and delivers its echo and signal. */
	irq = spin_lock_irqsave(&tty->lock);

	tty_input_byte_locked(tty, byte, &result);

	spin_unlock_irqrestore(&tty->lock, irq);

	if (result.notify)
		poll_notify();
	if (!result.output_stopped)
		tty_echo(tty, (const char *)result.echo, result.echo_length);
	if (result.signal_number != 0 &&
	    result.signal_session > 0 &&
	    result.signal_pgrp > 0)
		(void)process_signal_pgrp(result.signal_session,
		    result.signal_pgrp, result.signal_number);
}

/*
 * Reads from a virtual console, subject to job control.
 */
ssize_t
tty_vt_read(
	unsigned vt,
	struct file *file,
	void *buffer,
	size_t size)
{
	struct tty *tty;
	struct process *process;
	unsigned canonical;
	unsigned long irq;
	int error;
	int nonblocking;
	ssize_t result;

	if (curthread != NULL)
		process = curthread->proc;
	else
		process = NULL;

	/* Rejects a missing buffer or an unknown console. */
	if (buffer == NULL)
		return -EINVAL;
	if (size == 0)
		return 0;
	if (vt >= TTY_VT_COUNT)
		return -ENODEV;

	/* A background process is stopped or refused first. */
	tty = &console_ttys[vt];
	error = tty_background(tty, process, TTY_BACKGROUND_READ);
	if (error != 0)
		return -error;

	/* Reads in the mode the terminal is in. */
	irq = spin_lock_irqsave(&tty->lock);

	canonical = tty->termios.c_lflag & ICANON;

	spin_unlock_irqrestore(&tty->lock, irq);

	nonblocking = (file_status_flags_get(file) & O_NONBLOCK) != 0;
	if (canonical)
		result = tty_read_canonical(tty, buffer, size, nonblocking);
	else
		result = tty_read_noncanonical(tty, buffer, size, nonblocking);

	/* Reports the read result. */
	return result;
}

/*
 * Writes to a virtual console, subject to job control and flow control.
 */
ssize_t
tty_vt_write(
	unsigned vt,
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct tty *tty;
	struct process *process;
	const char *bytes;
	unsigned oflag;
	unsigned long irq;
	int error;
	size_t start;
	size_t i;

	if (curthread != NULL)
		process = curthread->proc;
	else
		process = NULL;
	bytes = buffer;

	(void)file;

	/* Rejects an unknown console. */
	if (vt >= TTY_VT_COUNT)
		return -ENODEV;

	/* A background process is stopped or refused first. */
	tty = &console_ttys[vt];
	error = tty_background(tty, process, TTY_BACKGROUND_WRITE);
	if (error != 0)
		return -error;
	if (buffer == NULL)
		return -EINVAL;
	error = tty_wait_output_enabled(tty, file);
	if (error != 0)
		return -error;

	/* Expands newlines to carriage return and newline under ONLCR. */
	irq = spin_lock_irqsave(&tty->lock);

	oflag = tty->termios.c_oflag;

	spin_unlock_irqrestore(&tty->lock, irq);

	if ((oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
		start = 0;
		for (i = 0; i < size; i++) {
			if (bytes[i] == '\n') {
				if (i != start)
					tty_echo(tty, bytes + start, i - start);
				tty_echo(tty, "\r\n", 2);
				start = i + 1U;
			}
		}

		if (start < size)
			tty_echo(tty, bytes + start, size - start);
	} else {
		tty_echo(tty, bytes, size);
	}

	/* Reports the bytes written. */
	return (ssize_t)size;
}

/*
 * Reads from the first virtual console.
 */
ssize_t
tty_console_read(
	struct file *f,
	void *b,
	size_t n)
{
	ssize_t result;

	result = tty_vt_read(0, f, b, n);
	return result;
}

/*
 * Writes to the first virtual console.
 */
ssize_t
tty_console_write(
	struct file *f,
	const void *b,
	size_t n)
{
	ssize_t result;

	result = tty_vt_write(0, f, b, n);
	return result;
}

/*
 * Handles a control request on the first virtual console.
 */
int
tty_console_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	/* Reports the failure. */
	error = tty_vt_ioctl(0, file, request, argument);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Handles a control request on a virtual console.
 */
int
tty_vt_ioctl(
	unsigned vt,
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	if (vt >= TTY_VT_COUNT)
		return ENODEV;

	/* Reports the failure. */
	error = tty_ioctl_instance(&console_ttys[vt], file, request, argument);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the readiness of the first virtual console.
 */
int
tty_console_poll(
	struct file *file,
	short events,
	short *revents)
{
	int error;

	/* Reports the failure. */
	error = tty_vt_poll(0, file, events, revents);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the readiness of a virtual console.
 */
int
tty_vt_poll(
	unsigned vt,
	struct file *file,
	short events,
	short *revents)
{
	short result;
	struct tty *tty;
	unsigned long irq;
	int readable;

	result = 0;

	(void)file;

	/* Rejects an unknown console or a missing result. */
	if (vt >= TTY_VT_COUNT)
		return ENODEV;
	tty = &console_ttys[vt];
	if (revents == NULL)
		return EINVAL;

	/* Readable with a record or input byte; writable unless flow-stopped. */
	irq = spin_lock_irqsave(&tty->lock);

	if ((tty->termios.c_lflag & ICANON) != 0)
		readable = tty->record_used != 0;
	else
		readable = tty->input_used != 0;
	if (readable)
		result |= events & (POLLIN | POLLRDNORM);
	if (!tty->output_stopped)
		result |= events & (POLLOUT | POLLWRNORM);

	spin_unlock_irqrestore(&tty->lock, irq);

	*revents = result;

	/* Reports the readiness. */
	return 0;
}

/*
 * Makes the first console the controlling terminal of a session leader.
 */
void
tty_attach_console(
	struct process *process)
{
	if (process == NULL || process == &process0)
		return;
	(void)tty_assign_controlling(&console_ttys[0], process);
}

/*
 * Detaches a process from its controlling terminal.
 *
 * A session leader's exit releases the terminal from the whole session
 * and hangs up the foreground group.
 */
void
tty_detach_process(
	struct process *process)
{
	struct tty *tty;
	uint64_t generation;
	pid_t session;
	pid_t pgrp;
	unsigned long irq;

	session = 0;
	pgrp = 0;

	/* Ignores a process without a controlling terminal. */
	if (process == NULL)
		return;
	if (process_controlling_tty_snapshot(process, &tty, &generation) != 0)
		return;
	if (tty == NULL)
		return;

	/* A stale association only needs the process side cleared. */
	irq = spin_lock_irqsave(&tty->lock);

	if (tty->association_generation != generation) {
		spin_unlock_irqrestore(&tty->lock, irq);
		process_controlling_tty_detach_one(process, tty, generation);
		return;
	}

	/* The session leader releases the terminal from the session. */
	if (tty->session == process->pid && process->session == process->pid) {
		session = tty->session;
		pgrp = tty->foreground_pgrp;
		tty->session = 0;
		tty->foreground_pgrp = 0;
		tty_advance_association_locked(tty);
	}

	spin_unlock_irqrestore(&tty->lock, irq);

	if (session != 0)
		process_controlling_tty_detach_session(session, tty, generation);
	else
		process_controlling_tty_detach_one(process, tty, generation);

	/* Hangs up the foreground group of a released terminal. */
	if (session > 0 && pgrp > 0) {
		(void)process_signal_pgrp(session, pgrp, SIGHUP);
		(void)process_signal_pgrp(session, pgrp, SIGCONT);
	}
}

/*
 * Tests whether a pseudo terminal pair is in use.
 */
int
tty_pty_exists(
	unsigned index)
{
	unsigned result;
	unsigned long irq;

	/* Rejects an unknown pair. */
	if (index >= PTY_MAX)
		return 0;

	/* Samples the active flag under the pair lock. */
	irq = spin_lock_irqsave(&pty_pairs[index].lock);

	result = pty_pairs[index].active;

	spin_unlock_irqrestore(&pty_pairs[index].lock, irq);

	/* Reports the sampled flag. */
	return (int)result;
}

/*
 * Lists the pseudo terminal pairs in use, reporting their count.
 */
unsigned
tty_pty_snapshot(
	unsigned *indices,
	unsigned capacity)
{
	unsigned count;
	unsigned i;

	count = 0;

	/* Records each active index that fits. */
	for (i = 0; i < PTY_MAX; i++) {
		if (tty_pty_exists(i)) {
			if (indices != NULL && count < capacity)
				indices[count] = i;
			count++;
		}
	}

	/* Reports the total. */
	return count;
}

/*
 * Initializes the pseudo terminal pairs and registers the ptmx device.
 */
int
tty_pty_register(
	void)
{
	unsigned i;
	struct pty_pair *pair;
	int error;

	/* Initializes every pair inactive with default settings. */
	spin_init(&pty_registry_lock, LOCK_RANK_TTY, "pty registry");
	for (i = 0; i < PTY_MAX; i++) {
		pair = &pty_pairs[i];
		memset(pair, 0, sizeof(*pair));
		pair->index = i;
		spin_init(&pair->lock, LOCK_RANK_TTY, "pty pair");
		waitq_init(&pair->output_waitq, "pty master output");
		spin_init(&pair->slave.lock, LOCK_RANK_TTY, "pty slave");
		waitq_init(&pair->slave.read_waitq, "pty slave input");
		waitq_init(&pair->slave.write_waitq, "pty slave output flow");
		tty_default_termios(&pair->slave.termios);
		pair->slave.winsize.ws_row = HAL_CONS_ROWS;
		pair->slave.winsize.ws_col = HAL_CONS_COLUMNS;
		pair->slave.association_generation = 1;
	}

	/* Reports why the registration failed. */
	error = cdev_register("ptmx", 0x00010001U, &pty_ptmx_ops, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

#ifdef ZEDBSD_TTY_TEST
/*
 * Tests that a VLNEXT-quoted VSTOP reaches the input as a plain byte.
 */
int
tty_test_vlnext_ixon(
	void)
{
	struct tty tty;
	struct tty_input_result result;
	uint8_t stop;

	/* VLNEXT arms the quote without adding to the line. */
	memset(&tty, 0, sizeof(tty));
	tty_default_termios(&tty.termios);
	stop = tty.termios.c_cc[VSTOP];
	tty_input_byte_locked(&tty, tty.termios.c_cc[VLNEXT], &result);
	if (!tty.literal_next || tty.edit_used != 0)
		return 0;

	/* The quoted VSTOP is input, not flow control. */
	tty_input_byte_locked(&tty, stop, &result);
	if (tty.literal_next)
		return 0;
	if (tty.output_stopped)
		return 0;
	if (tty.edit_used != 1)
		return 0;
	if (tty.edit[0] != stop)
		return 0;
	return 1;
}
#endif

/* Discards every queued and partially edited input byte. */
static void
tty_flush_input_locked(
	struct tty *tty)
{
	tty->edit_used = 0;
	tty->record_head = 0;
	tty->record_tail = 0;
	tty->record_used = 0;
	tty->input_head = 0;
	tty->input_tail = 0;
	tty->input_used = 0;
	tty->literal_next = 0;
}

/* Fills in the default terminal settings. */
static void
tty_default_termios(
	struct termios *termios)
{
	memset(termios, 0, sizeof(*termios));
	memset(termios->c_cc, TTY_VDISABLE, sizeof(termios->c_cc));
	termios->c_iflag = ICRNL | IXON;
	termios->c_oflag = OPOST | ONLCR;
	termios->c_cflag = CREAD | CS8 | CLOCAL;
	termios->c_lflag = ECHO | ECHOE | ECHOK | ECHOCTL | ICANON | IEXTEN |
	    ISIG;
	termios->c_cc[VINTR] = 3;
	termios->c_cc[VQUIT] = 28;
	termios->c_cc[VERASE] = 8;
	termios->c_cc[VKILL] = 21;
	termios->c_cc[VEOF] = 4;
	termios->c_cc[VSTART] = 17;
	termios->c_cc[VSTOP] = 19;
	termios->c_cc[VSUSP] = 26;
	termios->c_cc[VWERASE] = 23;
	termios->c_cc[VLNEXT] = 22;
	termios->c_cc[VREPRINT] = 18;
	termios->c_cc[VMIN] = 1;
	termios->c_cc[VTIME] = 0;
	termios->c_ispeed = B9600;
	termios->c_ospeed = B9600;
}

/* Executes a parsed CSI command on the HAL console. */
static void
tty_console_csi(
	unsigned vt,
	unsigned command)
{
	struct hal_cons_state state;
	unsigned amount;
	unsigned row;

	/* Minimal ANSI cursor/erase baseline shared by every HAL console. */
	if (console_escape_has_parameter[vt])
		amount = console_escape_parameter[vt];
	else
		amount = 1U;
	hal_cons_save_state(&state);
	switch (command) {
	case 'H':
		(void)hal_cons_set_cursor(0U, 0U);
		break;
	case 'J':
		if (amount == 2U) {
			for (row = 0; row < HAL_CONS_ROWS; row++)
				hal_cons_clear_row(row);
			(void)hal_cons_set_cursor(0U, 0U);
		}

		break;
	case 'A':
		if (amount < state.row)
			state.row = state.row - amount;
		else
			state.row = 0U;
		(void)hal_cons_set_cursor(state.row, state.column);
		break;
	case 'B':
		state.row += amount;
		if (state.row >= HAL_CONS_ROWS)
			state.row = HAL_CONS_ROWS - 1U;
		(void)hal_cons_set_cursor(state.row, state.column);
		break;
	case 'C':
		state.column += amount;
		if (state.column >= HAL_CONS_COLUMNS)
			state.column = HAL_CONS_COLUMNS - 1U;
		(void)hal_cons_set_cursor(state.row, state.column);
		break;
	case 'D':
		if (amount < state.column)
			state.column = state.column - amount;
		else
			state.column = 0U;
		(void)hal_cons_set_cursor(state.row, state.column);
		break;
	case 'G':
		if (amount == 0U)
			state.column = 0U;
		else
			state.column = amount - 1U;
		if (state.column >= HAL_CONS_COLUMNS)
			state.column = HAL_CONS_COLUMNS - 1U;
		(void)hal_cons_set_cursor(state.row, state.column);
		break;
	case 'K':
		if (amount == 2U) {
			hal_cons_clear_row(state.row);
			(void)hal_cons_set_cursor(state.row, state.column);
		} else if (!console_escape_has_parameter[vt] || amount == 0U) {
			hal_cons_clear_to_eol();
		}

		break;
	default:
		break;
	}
}

/* Renders bytes on the HAL console, parsing CSI escapes across calls. */
static void
tty_render(
	unsigned vt,
	const char *bytes,
	size_t length)
{
	static const char escape = '\033';
	size_t index;
	size_t start;
	char byte;

	index = 0;

	/* Ignores an empty write. */
	if (length == 0)
		return;

	/* Consumes plain runs, then escape introducers, then parameters. */
	while (index < length) {
		if (console_escape_state[vt] == 0U) {
			start = index;
			while (index < length && (unsigned char)bytes[index] != 0x1bU)
				index++;
			if (index != start)
				hal_cons_write_n(bytes + start, (unsigned)(index - start));
			if (index < length) {
				console_escape_state[vt] = 1U;
				index++;
			}

			continue;
		}

		if (console_escape_state[vt] == 1U) {
			byte = bytes[index];
			index++;
			if (byte == '[') {
				console_escape_state[vt] = 2U;
				console_escape_parameter[vt] = 0U;
				console_escape_has_parameter[vt] = 0U;
			} else {
				hal_cons_write_n(&escape, 1U);
				hal_cons_write_n(bytes + index - 1U, 1U);
				console_escape_state[vt] = 0U;
			}

			continue;
		}

		if (bytes[index] >= '0' && bytes[index] <= '9') {
			console_escape_has_parameter[vt] = 1U;
			if (console_escape_parameter[vt] < 1000U)
				console_escape_parameter[vt] = console_escape_parameter[vt] * 10U +
				    (unsigned)(bytes[index] - '0');
			index++;
			continue;
		}

		tty_console_csi(vt, (unsigned char)bytes[index]);
		index++;
		console_escape_state[vt] = 0U;
	}
}

/* Appends output to a console's history and renders it when active. */
static void
tty_echo(
	struct tty *tty,
	const char *bytes,
	size_t length)
{
	unsigned vt;
	unsigned long irq;
	size_t drop;

	/* Ignores a pseudo terminal or an empty write. */
	vt = (unsigned)(tty - console_ttys);
	if (vt >= TTY_VT_COUNT || length == 0)
		return;

	/* Keeps the newest history, dropping the oldest to make room. */
	irq = spin_lock_irqsave(&console_output_lock);

	if (length >= TTY_VT_HISTORY) {
		bytes += length - TTY_VT_HISTORY;
		length = TTY_VT_HISTORY;
		vt_history_used[vt] = 0;
	} else if (vt_history_used[vt] + length > TTY_VT_HISTORY) {
		drop = vt_history_used[vt] + length - TTY_VT_HISTORY;
		memmove(vt_history[vt], vt_history[vt] + drop,
		    vt_history_used[vt] - drop);
		vt_history_used[vt] -= drop;
	}

	memcpy(vt_history[vt] + vt_history_used[vt], bytes, length);
	vt_history_used[vt] += length;
	if (vt == active_vt)
		tty_render(vt, bytes, length);

	spin_unlock_irqrestore(&console_output_lock, irq);
}

/* Completes the edited line as a record for readers. */
static void
tty_commit_locked(
	struct tty *tty,
	unsigned eof)
{
	struct tty_record *record;

	/* A full record queue drops the line. */
	if (tty->record_used == TTY_RECORDS)
		return;

	/* Copies the line into the next record and wakes the readers. */
	record = &tty->records[tty->record_head];
	memcpy(record->data, tty->edit, tty->edit_used);
	record->length = tty->edit_used;
	record->offset = 0;
	record->eof = eof;
	tty->edit_used = 0;
	tty->record_head = (tty->record_head + 1U) % TTY_RECORDS;
	tty->record_used++;
	waitq_wake_all(&tty->read_waitq);
}

/* Tests whether a byte is an enabled control character. */
static int
tty_cc_matches(
	const struct tty *tty,
	unsigned index,
	uint8_t byte)
{
	if (tty->termios.c_cc[index] == TTY_VDISABLE)
		return 0;
	if (byte != tty->termios.c_cc[index])
		return 0;
	return 1;
}

/* Appends a byte to the pending echo. */
static void
tty_echo_append(
	struct tty_input_result *result,
	uint8_t byte)
{
	if (result->echo_length < sizeof(result->echo)) {
		result->echo[result->echo_length] = byte;
		result->echo_length++;
	}
}

/* Echoes a byte, showing a control character as caret notation. */
static void
tty_echo_character(
	const struct tty *tty,
	struct tty_input_result *result,
	uint8_t byte)
{
	if ((tty->termios.c_lflag & ECHOCTL) != 0 &&
	    ((byte < 0x20U && byte != '\n' && byte != '\t') || byte == 0x7fU)) {
		tty_echo_append(result, '^');
		if (byte == 0x7fU)
			tty_echo_append(result, '?');
		else
			tty_echo_append(result, (uint8_t)(byte + '@'));
	} else {
		tty_echo_append(result, byte);
	}
}

/* Echoes the erasure of a byte, wiping both columns of caret notation. */
static void
tty_echo_erase(
	const struct tty *tty,
	struct tty_input_result *result,
	uint8_t byte)
{
	unsigned width;

	/* A control byte was echoed as two characters. */
	if ((tty->termios.c_lflag & ECHOCTL) != 0 &&
	    ((byte < 0x20U && byte != '\n' && byte != '\t') || byte == 0x7fU))
		width = 2U;
	else
		width = 1U;

	/* Rubs out each echoed column. */
	while (width != 0) {
		tty_echo_append(result, '\b');
		tty_echo_append(result, ' ');
		tty_echo_append(result, '\b');
		width--;
	}
}

/* Runs the line discipline on one input byte; the caller delivers the echo unlocked. */
static void
tty_input_byte_locked(
	struct tty *tty,
	uint8_t byte,
	struct tty_input_result *result)
{
	unsigned lflag;
	int quoted;
	uint8_t erased;
	size_t i;

	quoted = 0;

	/* Applies the input translations. */
	memset(result, 0, sizeof(*result));
	if (byte == '\r') {
		if ((tty->termios.c_iflag & IGNCR) != 0)
			goto out;
		if ((tty->termios.c_iflag & ICRNL) != 0)
			byte = '\n';
	} else if (byte == '\n' && (tty->termios.c_iflag & INLCR) != 0) {
		byte = '\r';
	}

	if ((tty->termios.c_iflag & ISTRIP) != 0)
		byte &= 0x7fU;
	lflag = tty->termios.c_lflag;

	/*
	 * VLNEXT quotes every special character interpreted by the line
	 * discipline, including VSTOP/VSTART.  Resolve it before IXON so a
	 * quoted flow-control byte reaches the readable input stream.
	 */
	if ((lflag & IEXTEN) != 0 && tty->literal_next) {
		tty->literal_next = 0;
		quoted = 1;
	} else if ((lflag & IEXTEN) != 0 && tty_cc_matches(tty, VLNEXT, byte)) {
		tty->literal_next = 1;
		if ((lflag & ECHO) != 0) {
			tty_echo_append(result, '^');
			tty_echo_append(result, '\b');
		}

		result->notify = 1;
		goto out;
	}

	/*
	 * Software output flow control consumes unquoted VSTOP/VSTART as
	 * line-control characters.  VLNEXT-quoted bytes remain ordinary
	 * input.
	 */
	if (!quoted && (tty->termios.c_iflag & IXON) != 0 &&
	    tty_cc_matches(tty, VSTOP, byte)) {
		tty->output_stopped = 1;
		tty->output_stopped_by_ixon = 1;
		result->notify = 1;
		result->flow_changed = 1;
		result->output_stopped = 1;
		goto out;
	}

	if (!quoted && (tty->termios.c_iflag & IXON) != 0 &&
	    tty_cc_matches(tty, VSTART, byte)) {
		tty->output_stopped = 0;
		tty->output_stopped_by_ixon = 0;
		waitq_wake_all(&tty->write_waitq);
		result->notify = 1;
		result->flow_changed = 1;
		result->output_stopped = 0;
		goto out;
	}

	/* A signal character is echoed, flushes the input, and is reported. */
	if (!quoted && (lflag & ISIG) != 0) {
		if (tty_cc_matches(tty, VINTR, byte))
			result->signal_number = SIGINT;
		else if (tty_cc_matches(tty, VQUIT, byte))
			result->signal_number = SIGQUIT;
		else if (tty_cc_matches(tty, VSUSP, byte))
			result->signal_number = SIGTSTP;
		if (result->signal_number != 0) {
			result->signal_session = tty->session;
			result->signal_pgrp = tty->foreground_pgrp;
			if ((lflag & ECHO) != 0)
				tty_echo_character(tty, result, byte);
			if ((lflag & NOFLSH) == 0)
				tty_flush_input_locked(tty);
			result->notify = 1;
			goto out;
		}
	}

	/* Canonical mode edits the line; otherwise the byte is queued as is. */
	if ((lflag & ICANON) != 0) {
		if (!quoted && tty_cc_matches(tty, VERASE, byte)) {
			if (tty->edit_used != 0) {
				tty->edit_used--;
				erased = tty->edit[tty->edit_used];
				if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
					tty_echo_erase(tty, result, erased);
			}
		} else if (!quoted && tty_cc_matches(tty, VKILL, byte)) {
			tty->edit_used = 0;
			if ((lflag & ECHOK) != 0)
				tty_echo_append(result, '\n');
		} else if (!quoted && (lflag & IEXTEN) != 0 &&
		    tty_cc_matches(tty, VWERASE, byte)) {
			/* Erases the trailing blanks, then the word before them. */
			while (tty->edit_used != 0 &&
			    (tty->edit[tty->edit_used - 1U] == ' ' ||
			     tty->edit[tty->edit_used - 1U] == '\t')) {
				tty->edit_used--;
				erased = tty->edit[tty->edit_used];
				if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
					tty_echo_erase(tty, result, erased);
			}
			while (tty->edit_used != 0 &&
			    tty->edit[tty->edit_used - 1U] != ' ' &&
			    tty->edit[tty->edit_used - 1U] != '\t') {
				tty->edit_used--;
				erased = tty->edit[tty->edit_used];
				if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
					tty_echo_erase(tty, result, erased);
			}
		} else if (!quoted && (lflag & IEXTEN) != 0 &&
		    tty_cc_matches(tty, VREPRINT, byte)) {
			if ((lflag & ECHO) != 0) {
				tty_echo_append(result, '\n');
				for (i = 0; i < tty->edit_used; i++)
					tty_echo_character(tty, result, tty->edit[i]);
			}
		} else if (!quoted && tty_cc_matches(tty, VEOF, byte)) {
			tty_commit_locked(tty, 1);
		} else if (!quoted &&
		    (byte == '\n' || tty_cc_matches(tty, VEOL, byte))) {
			if (tty->edit_used < TTY_LINE_MAX) {
				tty->edit[tty->edit_used] = byte;
				tty->edit_used++;
			}

			tty_commit_locked(tty, 0);
			if ((lflag & (ECHO | ECHONL)) != 0)
				tty_echo_append(result, byte);
		} else {
			if (tty->edit_used < TTY_LINE_MAX) {
				tty->edit[tty->edit_used] = byte;
				tty->edit_used++;
				if ((lflag & ECHO) != 0)
					tty_echo_character(tty, result, byte);
			}
		}
	} else if (tty->input_used < TTY_INPUT_MAX) {
		tty->input[tty->input_head] = byte;
		tty->input_head = (tty->input_head + 1U) % TTY_INPUT_MAX;
		tty->input_used++;
		waitq_wake_all(&tty->read_waitq);
		if ((lflag & ECHO) != 0)
			tty_echo_character(tty, result, byte);
	}

	result->notify = 1;
out:
	result->output_flags = tty->termios.c_oflag;
	result->output_stopped = tty->output_stopped;
}

/* Tests whether a terminal is the controlling terminal of a process. */
static int
tty_process_controls(
	struct tty *tty,
	struct process *process)
{
	uint64_t generation;
	pid_t session;
	unsigned long irq;

	/* Rejects a missing terminal or process. */
	if (tty == NULL || process == NULL)
		return 0;

	/* The session and the association generation must both match. */
	irq = spin_lock_irqsave(&tty->lock);

	generation = tty->association_generation;
	session = tty->session;

	spin_unlock_irqrestore(&tty->lock, irq);

	if (session != process->session)
		return 0;
	if (!process_controlling_tty_matches(process, tty, generation))
		return 0;
	return 1;
}

/* Starts a new association generation, skipping zero. */
static void
tty_advance_association_locked(
	struct tty *tty)
{
	tty->association_generation++;
	if (tty->association_generation == 0)
		tty->association_generation = 1;
}

/* Makes a terminal the controlling terminal of a session leader. */
static int
tty_assign_controlling(
	struct tty *tty,
	struct process *process)
{
	struct tty *existing;
	uint64_t existing_generation;
	uint64_t generation;
	unsigned long irq;
	int claimed;
	int error;

	claimed = 0;

	/* Only a session leader may claim a terminal. */
	if (tty == NULL || process == NULL || process->session != process->pid)
		return EPERM;
	if (process_controlling_tty_snapshot(process, &existing,
	    &existing_generation) != 0)
		return EINVAL;

	/* A leader that already has a terminal may only reassert this one. */
	if (existing != NULL) {
		irq = spin_lock_irqsave(&tty->lock);
		generation = tty->association_generation;
		claimed = tty->session == process->session;
		spin_unlock_irqrestore(&tty->lock, irq);
		if (existing != tty)
			return EPERM;
		if (existing_generation != generation)
			return EPERM;
		if (!claimed)
			return EPERM;
		return 0;
	}

	/* Takes a free terminal, or one already held by this session. */
	irq = spin_lock_irqsave(&tty->lock);

	if (tty->session != 0 && tty->session != process->session) {
		spin_unlock_irqrestore(&tty->lock, irq);
		return EPERM;
	}

	if (tty->association_generation == 0)
		tty->association_generation = 1;
	claimed = tty->session == 0;
	tty->session = process->session;
	tty->foreground_pgrp = process->pgrp;
	generation = tty->association_generation;

	spin_unlock_irqrestore(&tty->lock, irq);

	/* Publishes the association on the process, undoing a lost race. */
	error = process_controlling_tty_attach(process, tty, generation);
	irq = spin_lock_irqsave(&tty->lock);

	if (tty->association_generation != generation ||
	    tty->session != process->session)
		error = EBUSY;
	if (error != 0 &&
	    claimed &&
	    tty->association_generation == generation &&
	    tty->session == process->session) {
		tty->session = 0;
		tty->foreground_pgrp = 0;
		tty_advance_association_locked(tty);
	}

	spin_unlock_irqrestore(&tty->lock, irq);

	if (error != 0)
		process_controlling_tty_detach_one(process, tty, generation);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Applies the job-control rules to a background process's terminal access. */
static int
tty_background(
	struct tty *tty,
	struct process *process,
	enum tty_background_operation operation)
{
	pid_t session;
	pid_t foreground;
	unsigned lflag;
	unsigned long irq;
	int decision;
	int signo;

	/* Only the controlling terminal applies job control. */
	if (!tty_process_controls(tty, process))
		return 0;

	/* Stops the process until it is in the foreground or must fail. */
	for (;;) {
		irq = spin_lock_irqsave(&tty->lock);
		session = tty->session;
		foreground = tty->foreground_pgrp;
		lflag = tty->termios.c_lflag;
		spin_unlock_irqrestore(&tty->lock, irq);
		if (process->session != session ||
		    process->pgrp == foreground ||
		    (operation == TTY_BACKGROUND_WRITE &&
		     (lflag & TOSTOP) == 0))
			return 0;
		if (operation == TTY_BACKGROUND_READ)
			signo = SIGTTIN;
		else
			signo = SIGTTOU;
		decision = signal_job_control_decision(thread_current(), signo);
		if (decision == EIO) {
			if (operation == TTY_BACKGROUND_READ)
				return EIO;
			return 0;
		}

		if (process_pgrp_is_orphaned(process))
			return EIO;
		if (decision != 0) {
			(void)process_signal_pgrp(process->session, process->pgrp,
			    signo);
			return decision;
		}

		/*
		 * The default action consumes the signal by stopping this
		 * process.  Other members of the group receive an ordinary
		 * generated signal; excluding self avoids leaving a duplicate
		 * pending stop after SIGCONT.
		 */
		(void)process_signal_pgrp_except(process->session, process->pgrp,
		    signo, process);
		process_stop_current(signo);

		/* SIGCONT does not necessarily foreground the group. */
	}
}

/* Waits until output is not flow-stopped, failing on hangup. */
static int
tty_wait_output_enabled(
	struct tty *tty,
	struct file *file)
{
	unsigned long irq;
	uint64_t sequence;
	int error;

	irq = spin_lock_irqsave(&tty->lock);

	/* Sleeps while stopped unless the file must not block. */
	while (tty->output_stopped && !tty->hungup) {
		if (file != NULL &&
		    (file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return EAGAIN;
		}

		sequence = waitq_sequence(&tty->write_waitq);
		error = waitq_sleep(&tty->write_waitq, &tty->lock, sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return error;
		}
	}

	if (tty->hungup) {
		spin_unlock_irqrestore(&tty->lock, irq);
		return EIO;
	}

	spin_unlock_irqrestore(&tty->lock, irq);

	return 0;
}

/* Reads from the next complete line record. */
static ssize_t
tty_read_canonical(
	struct tty *tty,
	void *buffer,
	size_t size,
	int nonblocking)
{
	uint8_t *output;
	unsigned long irq;
	struct tty_record *record;
	size_t count;
	uint64_t sequence;
	int error;

	output = buffer;
	irq = spin_lock_irqsave(&tty->lock);

	/* Waits for a record; hangup is end of file. */
	while (tty->record_used == 0) {
		if (tty->hungup) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return 0;
		}

		if (nonblocking) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -EAGAIN;
		}

		sequence = waitq_sequence(&tty->read_waitq);
		error = waitq_sleep(&tty->read_waitq, &tty->lock, sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error != 0) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -error;
		}
	}

	/* Copies from the record, consuming it once fully read. */
	record = &tty->records[tty->record_tail];
	if (record->eof && record->length == 0) {
		count = 0;
	} else {
		count = record->length - record->offset;
		if (count > size)
			count = size;
		memcpy(output, record->data + record->offset, count);
		record->offset += count;
	}

	if (record->offset == record->length) {
		tty->record_tail = (tty->record_tail + 1U) % TTY_RECORDS;
		tty->record_used--;
	}

	spin_unlock_irqrestore(&tty->lock, irq);

	poll_notify();

	/* Reports the bytes read. */
	return (ssize_t)count;
}

/* Reads raw input honoring the VMIN and VTIME rules. */
static ssize_t
tty_read_noncanonical(
	struct tty *tty,
	void *buffer,
	size_t size,
	int nonblocking)
{
	uint8_t *output;
	unsigned long irq;
	unsigned minimum;
	unsigned deciseconds;
	uint64_t deadline;
	uint64_t interval;
	unsigned timed_input_used;
	size_t count;
	int error;
	uint64_t sequence;
	size_t i;

	/* Reads the VMIN and VTIME settings this read has to honour. */
	output = buffer;
	irq = spin_lock_irqsave(&tty->lock);

	minimum = tty->termios.c_cc[VMIN];
	deciseconds = tty->termios.c_cc[VTIME];
	deadline = 0;
	interval = (uint64_t)deciseconds * (KERN_CLOCK_HZ / 10U);
	timed_input_used = 0;

	/* A pure timer read arms its deadline; a resumed timed read keeps its own. */
	if (minimum > size)
		minimum = (unsigned)size;
	if (nonblocking)
		minimum = 0;
	if (deciseconds != 0 && minimum == 0) {
		error = syscall_restart_deadline_after(interval, &deadline);
		if (error != 0) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -error;
		}
	} else if (minimum != 0 &&
	    deciseconds != 0 &&
	    tty->input_used != 0 &&
	    curthread != NULL &&
	    curthread->syscall_stop_redispatch &&
	    curthread->syscall_wait_deadline_valid) {
		deadline = curthread->syscall_wait_deadline;
		timed_input_used = tty->input_used;
	}

	/* Waits for the minimum, or for any byte within the timer. */
	while (tty->input_used < minimum ||
	    (minimum == 0 && tty->input_used == 0 && deciseconds != 0)) {
		sequence = waitq_sequence(&tty->read_waitq);
		if (tty->hungup) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return 0;
		}

		if (nonblocking) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -EAGAIN;
		}

		/*
		 * For MIN>0/TIME>0, TIME is an inter-byte timer.  Re-arm it
		 * whenever newly arrived input is observed, rather than
		 * measuring once from the first byte.
		 */
		if (minimum != 0 &&
		    deciseconds != 0 &&
		    tty->input_used != 0 &&
		    tty->input_used != timed_input_used) {
			error = syscall_restart_deadline_rearm(interval, &deadline);
			if (error != 0) {
				spin_unlock_irqrestore(&tty->lock, irq);
				return -error;
			}

			timed_input_used = tty->input_used;
		}

		error = waitq_sleep(&tty->read_waitq, &tty->lock, sequence,
		    deadline, WAITQ_INTERRUPTIBLE);
		if (error == ETIMEDOUT)
			break;
		if (error != 0) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -error;
		}
	}

	/* Copies what fits out of the ring. */
	count = tty->input_used;
	if (count > size)
		count = size;
	for (i = 0; i < count; i++) {
		output[i] = tty->input[tty->input_tail];
		tty->input_tail = (tty->input_tail + 1U) % TTY_INPUT_MAX;
		tty->input_used--;
	}

	spin_unlock_irqrestore(&tty->lock, irq);

	poll_notify();

	/* Reports the bytes read. */
	return (ssize_t)count;
}

/* Tests that requested terminal settings are supported. */
static int
tty_termios_valid(
	const struct termios *value)
{
	/* Only eight-bit characters are supported. */
	if (value == NULL)
		return 0;
	if ((value->c_cflag & CS8) == 0)
		return 0;

	/* Only a few nominal speeds are accepted. */
	if (value->c_ispeed != B0 &&
	    value->c_ispeed != B9600 &&
	    value->c_ispeed != B19200 &&
	    value->c_ispeed != B38400)
		return 0;
	if (value->c_ospeed != B0 &&
	    value->c_ospeed != B9600 &&
	    value->c_ospeed != B19200 &&
	    value->c_ospeed != B38400)
		return 0;
	return 1;
}

/* Handles a terminal control request for a console or pseudo terminal slave. */
static int
tty_ioctl_instance(
	struct tty *tty,
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct process *process;
	struct termios termios_value;
	struct winsize winsize_value;
	pid_t pid_value;
	pid_t session;
	pid_t pgrp;
	uint32_t index;
	unsigned long irq;
	int error;
	int flow_resumed;
	int changed;
	int queue;
	int action;
	uint8_t character;
	int duration;

	/* Names the calling process, if the request came from one. */
	if (curthread != NULL)
		process = curthread->proc;
	else
		process = NULL;
	error = 0;
	index = 0;
	(void)index;

	switch (request) {
	case TCGETS:
		/* Copies the settings out. */
		irq = spin_lock_irqsave(&tty->lock);
		termios_value = tty->termios;
		spin_unlock_irqrestore(&tty->lock, irq);
		error = copyout(&termios_value, argument, sizeof(termios_value));
		return error;
	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		/* Installs new settings, draining or flushing as requested. */
		flow_resumed = 0;
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0)
			return error;
		error = copyin(argument, &termios_value, sizeof(termios_value));
		if (error != 0)
			return error;
		if (!tty_termios_valid(&termios_value))
			return EINVAL;
		if (request != TCSETS) {
			error = tty_backend_drain(tty, file);
			if (error != 0)
				return error;
		}

		irq = spin_lock_irqsave(&tty->lock);
		if (request == TCSETSF)
			tty_flush_input_locked(tty);

		/* Turning IXON off releases a stop it caused. */
		if ((termios_value.c_iflag & IXON) == 0 &&
		    tty->output_stopped_by_ixon) {
			tty->output_stopped = 0;
			tty->output_stopped_by_ixon = 0;
			flow_resumed = 1;
		}

		tty->termios = termios_value;
		waitq_wake_all(&tty->read_waitq);
		waitq_wake_all(&tty->write_waitq);
		spin_unlock_irqrestore(&tty->lock, irq);
		if (flow_resumed)
			tty_backend_set_flow(tty, 0);
		poll_notify();
		return 0;
	case TIOCGWINSZ:
		/* Copies the window size out. */
		irq = spin_lock_irqsave(&tty->lock);
		winsize_value = tty->winsize;
		spin_unlock_irqrestore(&tty->lock, irq);
		error = copyout(&winsize_value, argument, sizeof(winsize_value));
		return error;
	case TIOCSWINSZ:
		/* Installs a window size, telling the foreground group of a change. */
		error = copyin(argument, &winsize_value, sizeof(winsize_value));
		if (error != 0)
			return error;
		irq = spin_lock_irqsave(&tty->lock);
		changed = memcmp(&tty->winsize, &winsize_value,
		    sizeof(winsize_value)) != 0;
		tty->winsize = winsize_value;
		session = tty->session;
		pgrp = tty->foreground_pgrp;
		spin_unlock_irqrestore(&tty->lock, irq);
#ifdef SIGWINCH
		if (changed && session > 0 && pgrp > 0)
			(void)process_signal_pgrp(session, pgrp, SIGWINCH);
#else
		(void)changed;
		(void)session;
		(void)pgrp;
#endif
		return 0;
	case TIOCGPGRP:
		/* Reports the foreground group to a controlled process. */
		if (!tty_process_controls(tty, process))
			return ENOTTY;
		irq = spin_lock_irqsave(&tty->lock);
		pid_value = tty->foreground_pgrp;
		spin_unlock_irqrestore(&tty->lock, irq);
		error = copyout(&pid_value, argument, sizeof(pid_value));
		return error;
	case TIOCGSID:
		/* Reports the session to a controlled process. */
		if (!tty_process_controls(tty, process))
			return ENOTTY;
		irq = spin_lock_irqsave(&tty->lock);
		pid_value = tty->session;
		spin_unlock_irqrestore(&tty->lock, irq);
		error = copyout(&pid_value, argument, sizeof(pid_value));
		return error;
	case TIOCSPGRP:
		/* Sets the foreground group to one of the session's groups. */
		if (!tty_process_controls(tty, process))
			return ENOTTY;
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0)
			return error;
		error = copyin(argument, &pid_value, sizeof(pid_value));
		if (error != 0)
			return error;
		if (!process_pgrp_in_session(process->session, pid_value))
			return EPERM;
		irq = spin_lock_irqsave(&tty->lock);
		if (tty->session != process->session)
			error = EPERM;
		else
			tty->foreground_pgrp = pid_value;
		spin_unlock_irqrestore(&tty->lock, irq);
		poll_notify();
		return error;
	case TIOCSCTTY:
		error = tty_assign_controlling(tty, process);
		return error;
	case TIOCNOTTY:
		if (!tty_process_controls(tty, process))
			return ENOTTY;
		tty_detach_process(process);
		return 0;
	case TIOCFLUSH:
		/* Discards queued input, output, or both. */
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0)
			return error;
		error = copyin(argument, &queue, sizeof(queue));
		if (error != 0)
			return error;
		if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH)
			return EINVAL;
		if (queue != TCOFLUSH) {
			irq = spin_lock_irqsave(&tty->lock);
			tty_flush_input_locked(tty);
			spin_unlock_irqrestore(&tty->lock, irq);
			poll_notify();
		}

		if (queue != TCIFLUSH) {
			error = tty_backend_flush_output(tty, file);
			if (error != 0)
				return error;
		}

		return 0;
	case TCXONC:
		/* Suspends or resumes output, or sends a flow character. */
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0)
			return error;
		error = copyin(argument, &action, sizeof(action));
		if (error != 0)
			return error;
		if (action != TCOOFF &&
		    action != TCOON &&
		    action != TCIOFF &&
		    action != TCION)
			return EINVAL;
		if (action == TCOOFF || action == TCOON) {
			irq = spin_lock_irqsave(&tty->lock);
			if (tty->hungup) {
				error = EIO;
			} else {
				tty->output_stopped = action == TCOOFF;
				tty->output_stopped_by_ixon = 0;
				if (action == TCOON)
					waitq_wake_all(&tty->write_waitq);
			}

			spin_unlock_irqrestore(&tty->lock, irq);
			if (error == 0)
				tty_backend_set_flow(tty, action == TCOOFF);
			poll_notify();
			return error;
		}

		irq = spin_lock_irqsave(&tty->lock);
		if (action == TCIOFF)
			character = tty->termios.c_cc[VSTOP];
		else
			character = tty->termios.c_cc[VSTART];
		spin_unlock_irqrestore(&tty->lock, irq);
		if (character == TTY_VDISABLE)
			return 0;
		error = tty_backend_send_control(tty, file, character);
		return error;
	case TIOCDRAIN:
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0)
			return error;
		error = tty_backend_drain(tty, file);
		return error;
	case TCSBRK:
		/* Drains the output; virtual terminals have no break line to assert. */
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0)
			return error;
		error = copyin(argument, &duration, sizeof(duration));
		if (error != 0)
			return error;
		if (duration < 0)
			return EINVAL;
		error = tty_backend_drain(tty, file);
		if (error != 0)
			return error;
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

/* Tests whether a handle still refers to its pair's current generation. */
static int
pty_handle_valid_locked(
	const struct pty_handle *handle)
{
	if (handle == NULL)
		return 0;
	if (!handle->pair->active)
		return 0;
	if (handle->generation != handle->pair->generation)
		return 0;
	return 1;
}

/* Queues slave output for the master, waiting for room unless nonblocking. */
static ssize_t
pty_output_bytes(
	struct pty_pair *pair,
	const uint8_t *bytes,
	size_t length,
	int nonblocking)
{
	size_t done;
	unsigned long irq;
	uint64_t sequence;
	int error;

	done = 0;
	irq = spin_lock_irqsave(&pair->lock);

	/* Fills the ring, sleeping for room while the master is open. */
	while (done < length) {
		if (nonblocking && done == 0 &&
		    PTY_OUTPUT_MAX - pair->output_used < length) {
			spin_unlock_irqrestore(&pair->lock, irq);
			return -EAGAIN;
		}
		while (done < length && pair->output_used < PTY_OUTPUT_MAX) {
			pair->output[pair->output_head] = bytes[done];
			done++;
			pair->output_head = (pair->output_head + 1U) % PTY_OUTPUT_MAX;
			pair->output_used++;
		}

		if (done != 0) {
			waitq_wake_all(&pair->output_waitq);
			poll_notify();
		}

		if (done == length)
			break;
		if (!pair->active || !pair->master_open) {
			spin_unlock_irqrestore(&pair->lock, irq);
			if (done != 0)
				return (ssize_t)done;
			return -EIO;
		}

		if (nonblocking) {
			spin_unlock_irqrestore(&pair->lock, irq);
			if (done != 0)
				return (ssize_t)done;
			return -EAGAIN;
		}

		sequence = waitq_sequence(&pair->output_waitq);
		error = waitq_sleep(&pair->output_waitq, &pair->lock,
		    sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error != 0) {
			spin_unlock_irqrestore(&pair->lock, irq);
			if (done != 0)
				return (ssize_t)done;
			return -error;
		}
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	/* Reports the bytes queued. */
	return (ssize_t)done;
}

/* Finds the pseudo terminal pair a slave belongs to, or NULL for a console. */
static struct pty_pair *
tty_backend_pair(
	struct tty *tty)
{
	unsigned index;

	for (index = 0; index < PTY_MAX; index++) {
		if (&pty_pairs[index].slave == tty)
			return &pty_pairs[index];
	}

	return NULL;
}

/* Tells a pseudo terminal master whether slave output is flow-stopped. */
static void
tty_backend_set_flow(
	struct tty *tty,
	unsigned stopped)
{
	struct pty_pair *pair;
	unsigned long irq;

	/* A console has no master to tell. */
	pair = tty_backend_pair(tty);
	if (pair == NULL)
		return;
	irq = spin_lock_irqsave(&pair->lock);

	pair->slave_output_stopped = stopped != 0;
	waitq_wake_all(&pair->output_waitq);

	spin_unlock_irqrestore(&pair->lock, irq);
}

/* Waits until queued slave output has been read by the master. */
static int
tty_backend_drain(
	struct tty *tty,
	struct file *file)
{
	struct pty_pair *pair;
	unsigned long irq;
	uint64_t sequence;
	int error;

	pair = tty_backend_pair(tty);

	(void)file;

	/* Console writes reach the HAL synchronously and have no queued bytes. */
	if (pair == NULL)
		return 0;

	/* Sleeps while bytes remain and the master can still read them. */
	irq = spin_lock_irqsave(&pair->lock);

	while (pair->active && pair->master_open && pair->output_used != 0) {
		sequence = waitq_sequence(&pair->output_waitq);
		error = waitq_sleep(&pair->output_waitq, &pair->lock, sequence,
		    0, WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&pair->lock, irq);
			return error;
		}
	}

	if (!pair->active || !pair->master_open) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return EIO;
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	return 0;
}

/* Discards queued slave output. */
static int
tty_backend_flush_output(
	struct tty *tty,
	struct file *file)
{
	struct pty_pair *pair;
	unsigned long irq;

	pair = tty_backend_pair(tty);

	(void)file;

	/* A console has nothing queued. */
	if (pair == NULL)
		return 0;

	/* Empties the ring while the master is open. */
	irq = spin_lock_irqsave(&pair->lock);

	if (!pair->active || !pair->master_open) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return EIO;
	}

	pair->output_head = 0;
	pair->output_tail = 0;
	pair->output_used = 0;
	waitq_wake_all(&pair->output_waitq);

	spin_unlock_irqrestore(&pair->lock, irq);

	poll_notify();
	return 0;
}

/* Sends a flow-control character to the master. */
static int
tty_backend_send_control(
	struct tty *tty,
	struct file *file,
	uint8_t byte)
{
	struct pty_pair *pair;
	ssize_t result;
	int nonblocking;

	/* A virtual console has no peer serial line to receive flow characters. */
	pair = tty_backend_pair(tty);
	if (pair == NULL)
		return 0;

	/* Queues the single byte. */
	nonblocking = 0;
	if (file != NULL && (file_status_flags_get(file) & O_NONBLOCK) != 0)
		nonblocking = 1;
	result = pty_output_bytes(pair, &byte, 1, nonblocking);
	if (result == 1)
		return 0;
	if (result < 0)
		return (int)-result;
	return EIO;
}

/* Feeds one byte from the master through the slave's line discipline. */
static void
pty_input_byte(
	struct pty_pair *pair,
	uint8_t byte)
{
	struct tty *tty;
	struct tty_input_result result;
	unsigned long irq;
	size_t index;

	tty = &pair->slave;

	/* Runs the line discipline. */
	irq = spin_lock_irqsave(&tty->lock);

	tty_input_byte_locked(tty, byte, &result);

	spin_unlock_irqrestore(&tty->lock, irq);

	if (result.flow_changed)
		tty_backend_set_flow(tty, result.output_stopped);
	if (result.notify)
		poll_notify();

	/* Echoes back to the master, expanding newlines under ONLCR. */
	for (index = 0; index < result.echo_length; index++) {
		if (result.echo[index] == '\n' &&
		    (result.output_flags & (OPOST | ONLCR)) == (OPOST | ONLCR))
			(void)pty_output_bytes(pair, (const uint8_t *)"\r\n", 2, 0);
		else
			(void)pty_output_bytes(pair, &result.echo[index], 1, 0);
	}

	/* Delivers a signal character's signal. */
	if (result.signal_number != 0 &&
	    result.signal_session > 0 &&
	    result.signal_pgrp > 0)
		(void)process_signal_pgrp(result.signal_session,
		    result.signal_pgrp, result.signal_number);
}

/* Opens the multiplexer, allocating a fresh pseudo terminal pair. */
static int
pty_master_open(
	struct file *file)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	pid_t old_session;
	uint64_t old_association_generation;
	unsigned i;
	unsigned long irq;

	pair = NULL;

	/* Allocates the handle and claims a free pair. */
	handle = kern_malloc(sizeof(*handle));
	if (handle == NULL)
		return ENFILE;
	irq = spin_lock_irqsave(&pty_registry_lock);

	for (i = 0; i < PTY_MAX; i++) {
		if (!pty_pairs[i].active) {
			pair = &pty_pairs[i];
			pair->active = 1;
			pair->locked = 1;
			pair->master_open = 1;
			pair->slave_opens = 0;
			pair->slave_ever_opened = 0;
			pair->output_head = 0;
			pair->output_tail = 0;
			pair->output_used = 0;
			pair->slave_output_stopped = 0;
			pair->generation++;
			if (pair->generation == 0)
				pair->generation = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&pty_registry_lock, irq);

	if (pair == NULL) {
		kern_free(handle);
		return ENOSPC;
	}

	/* Resets the slave, detaching any session of its previous life. */
	irq = spin_lock_irqsave(&pair->slave.lock);

	old_session = pair->slave.session;
	old_association_generation = pair->slave.association_generation;
	tty_advance_association_locked(&pair->slave);
	tty_flush_input_locked(&pair->slave);
	tty_default_termios(&pair->slave.termios);
	pair->slave.session = 0;
	pair->slave.foreground_pgrp = 0;
	pair->slave.hungup = 0;
	pair->slave.output_stopped = 0;
	pair->slave.output_stopped_by_ixon = 0;
	pair->slave.literal_next = 0;

	spin_unlock_irqrestore(&pair->slave.lock, irq);

	if (old_session > 0)
		process_controlling_tty_detach_session(old_session, &pair->slave,
		    old_association_generation);

	/* Binds the file to the master side. */
	handle->pair = pair;
	handle->generation = pair->generation;
	handle->master = 1;
	file->f_data = handle;
	file->f_ops = &pty_master_file_ops;
	return 0;
}

/* Closes the master, hanging up the slave and its session. */
static int
pty_master_close(
	struct file *file)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	unsigned deactivate;
	unsigned long irq;
	pid_t session;
	pid_t pgrp;
	uint64_t association_generation;

	handle = file->f_data;
	deactivate = 0;
	session = 0;
	pgrp = 0;
	association_generation = 0;

	/* Ignores a file that never bound a pair. */
	if (handle == NULL)
		return 0;

	/* Marks the master closed; the pair dies with the last slave. */
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);

	if (pty_handle_valid_locked(handle)) {
		pair->master_open = 0;
		deactivate = pair->slave_opens == 0;
		waitq_wake_all(&pair->output_waitq);
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	/* Hangs up the slave and releases its session. */
	irq = spin_lock_irqsave(&pair->slave.lock);

	pair->slave.hungup = 1;
	pair->slave.output_stopped = 0;
	pair->slave.output_stopped_by_ixon = 0;
	session = pair->slave.session;
	pgrp = pair->slave.foreground_pgrp;
	association_generation = pair->slave.association_generation;
	if (session > 0) {
		pair->slave.session = 0;
		pair->slave.foreground_pgrp = 0;
		tty_advance_association_locked(&pair->slave);
	}

	waitq_wake_all(&pair->slave.read_waitq);
	waitq_wake_all(&pair->slave.write_waitq);

	spin_unlock_irqrestore(&pair->slave.lock, irq);

	if (session > 0)
		process_controlling_tty_detach_session(session, &pair->slave,
		    association_generation);
	if (session > 0 && pgrp > 0) {
		(void)process_signal_pgrp(session, pgrp, SIGHUP);
		(void)process_signal_pgrp(session, pgrp, SIGCONT);
	}

	/* Frees the pair when no slave remains. */
	if (deactivate) {
		irq = spin_lock_irqsave(&pty_registry_lock);
		pair->active = 0;
		spin_unlock_irqrestore(&pty_registry_lock, irq);
	}

	kern_free(handle);
	file->f_data = NULL;
	poll_notify();
	return 0;
}

/* Reads slave output from the master side. */
static ssize_t
pty_master_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	uint8_t *output;
	unsigned long irq;
	size_t count;
	size_t i;
	uint64_t sequence;
	int error;

	handle = file->f_data;
	output = buffer;

	/* Rejects an unbound file or a missing buffer. */
	if (handle == NULL || buffer == NULL)
		return -EINVAL;
	if (size == 0)
		return 0;

	/* Waits for output while the slave is open and not flow-stopped. */
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);

	while (pty_handle_valid_locked(handle) &&
	    (pair->output_used == 0 || pair->slave_output_stopped) &&
	    (!pair->slave_ever_opened || pair->slave_opens != 0)) {
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&pair->lock, irq);
			return -EAGAIN;
		}

		sequence = waitq_sequence(&pair->output_waitq);
		error = waitq_sleep(&pair->output_waitq, &pair->lock, sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error != 0) {
			spin_unlock_irqrestore(&pair->lock, irq);
			return -error;
		}
	}

	if (!pty_handle_valid_locked(handle)) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return -EIO;
	}

	if (pair->output_used == 0) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return 0;
	}

	/* Copies what fits out of the ring. */
	if (pair->output_used < size)
		count = pair->output_used;
	else
		count = size;
	for (i = 0; i < count; i++) {
		output[i] = pair->output[pair->output_tail];
		pair->output_tail = (pair->output_tail + 1U) % PTY_OUTPUT_MAX;
		pair->output_used--;
	}

	waitq_wake_all(&pair->output_waitq);

	spin_unlock_irqrestore(&pair->lock, irq);

	poll_notify();

	/* Reports the bytes read. */
	return (ssize_t)count;
}

/* Writes master input into the slave's line discipline. */
static ssize_t
pty_master_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct pty_handle *handle;
	const uint8_t *input;
	struct pty_pair *pair;
	unsigned long irq;
	size_t i;

	handle = file->f_data;
	input = buffer;

	/* Rejects an unbound file or a missing buffer. */
	if (handle == NULL || buffer == NULL)
		return -EINVAL;

	/* The slave must still be there to receive. */
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);

	if (!pty_handle_valid_locked(handle) ||
	    !pair->master_open ||
	    (pair->slave_ever_opened && pair->slave_opens == 0)) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return -EIO;
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	/* Feeds each byte through the line discipline. */
	for (i = 0; i < size; i++)
		pty_input_byte(pair, input[i]);
	return (ssize_t)size;
}

/* Handles the master-only controls, forwarding the rest to the slave. */
static int
pty_master_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	unsigned long irq;
	int error;
	uint32_t number;
	int32_t locked;

	handle = file->f_data;
	error = 0;

	/* Rejects an unbound file. */
	if (handle == NULL)
		return EINVAL;
	pair = handle->pair;
	switch (request) {
	case TIOCGPTN:
		/* Reports the slave's number. */
		number = pair->index;
		error = copyout(&number, argument, sizeof(number));
		return error;
	case TIOCSPTLCK:
		/* Locks or unlocks the slave against opening. */
		error = copyin(argument, &locked, sizeof(locked));
		if (error != 0)
			return error;
		irq = spin_lock_irqsave(&pair->lock);
		if (!pty_handle_valid_locked(handle))
			error = EIO;
		else
			pair->locked = locked != 0;
		spin_unlock_irqrestore(&pair->lock, irq);
		return error;
	default:
		error = tty_ioctl_instance(&pair->slave, file, request, argument);
		return error;
	}
}

/* Reports the readiness of the master side. */
static int
pty_master_poll(
	struct file *file,
	short events,
	short *revents)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	short result;
	unsigned long irq;

	handle = file->f_data;
	result = 0;

	/* Rejects an unbound file or a missing result. */
	if (handle == NULL || revents == NULL)
		return EINVAL;

	/* Readable with slave output; writable while a slave can read. */
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);

	if (!pty_handle_valid_locked(handle)) {
		result = POLLERR | POLLHUP;
	} else {
		if (pair->output_used != 0 &&
		    (!pair->slave_output_stopped || pair->slave_opens == 0))
			result |= events & (POLLIN | POLLRDNORM);
		if (pair->master_open &&
		    (!pair->slave_ever_opened || pair->slave_opens != 0))
			result |= events & (POLLOUT | POLLWRNORM);
		if (pair->slave_ever_opened && pair->slave_opens == 0)
			result |= POLLHUP;
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	*revents = result;
	return 0;
}

/* Opens a slave, making it the controlling terminal of a session leader. */
static int
pty_slave_open(
	struct file *file)
{
	uintptr_t encoded;
	struct pty_handle *handle;
	struct pty_pair *pair;
	struct process *process;
	unsigned index;
	unsigned long irq;

	/* The device node encodes the pair index plus one. */
	encoded = (uintptr_t)file->f_inode->i_data;
	if (encoded == 0)
		return ENXIO;
	index = (unsigned)(encoded - 1U);
	if (index >= PTY_MAX)
		return ENXIO;
	handle = kern_malloc(sizeof(*handle));
	if (handle == NULL)
		return ENFILE;

	/* The pair must be live with its master open and unlocked. */
	pair = &pty_pairs[index];
	irq = spin_lock_irqsave(&pair->lock);

	if (!pair->active || !pair->master_open || pair->locked) {
		spin_unlock_irqrestore(&pair->lock, irq);
		kern_free(handle);
		if (pair->locked)
			return EACCES;
		return ENXIO;
	}

	pair->slave_opens++;
	pair->slave_ever_opened = 1;
	handle->pair = pair;
	handle->generation = pair->generation;
	handle->master = 0;

	spin_unlock_irqrestore(&pair->lock, irq);

	file->f_data = handle;

	/* A session leader without O_NOCTTY takes the slave as its terminal. */
	if (curthread != NULL)
		process = curthread->proc;
	else
		process = NULL;
	if (process != NULL &&
	    (file_status_flags_get(file) & O_NOCTTY) == 0 &&
	    process->session == process->pid)
		(void)tty_assign_controlling(&pair->slave, process);
	poll_notify();
	return 0;
}

/* Closes a slave, freeing the pair when the master is gone too. */
static int
pty_slave_close(
	struct file *file)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	unsigned deactivate;
	unsigned long irq;

	handle = file->f_data;
	deactivate = 0;

	/* Ignores a file that never bound a pair. */
	if (handle == NULL)
		return 0;

	/* The last slave close wakes the master and may free the pair. */
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);

	if (pty_handle_valid_locked(handle) && pair->slave_opens != 0) {
		pair->slave_opens--;
		if (pair->slave_opens == 0) {
			waitq_wake_all(&pair->output_waitq);
			deactivate = !pair->master_open;
		}
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	if (deactivate) {
		irq = spin_lock_irqsave(&pty_registry_lock);
		pair->active = 0;
		spin_unlock_irqrestore(&pty_registry_lock, irq);
	}

	kern_free(handle);
	file->f_data = NULL;
	poll_notify();
	return 0;
}

/* Reads from a slave, subject to job control. */
static ssize_t
pty_slave_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct pty_handle *handle;
	struct tty *tty;
	struct process *process;
	unsigned canonical;
	unsigned long irq;
	int error;
	int nonblocking;
	ssize_t result;

	handle = file->f_data;
	if (curthread != NULL)
		process = curthread->proc;
	else
		process = NULL;

	/* Rejects an unbound file. */
	if (handle == NULL)
		return -EIO;

	/* A background process is stopped or refused first. */
	tty = &handle->pair->slave;
	error = tty_background(tty, process, TTY_BACKGROUND_READ);
	if (error != 0)
		return -error;

	/* Reads in the mode the terminal is in. */
	irq = spin_lock_irqsave(&tty->lock);

	canonical = tty->termios.c_lflag & ICANON;

	spin_unlock_irqrestore(&tty->lock, irq);

	nonblocking = (file_status_flags_get(file) & O_NONBLOCK) != 0;
	if (canonical)
		result = tty_read_canonical(tty, buffer, size, nonblocking);
	else
		result = tty_read_noncanonical(tty, buffer, size, nonblocking);
	return result;
}

/* Writes to a slave, subject to job control and flow control. */
static ssize_t
pty_slave_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	struct tty *tty;
	struct process *process;
	const uint8_t *bytes;
	unsigned oflag;
	unsigned long irq;
	size_t done;
	int error;
	int nonblocking;
	const uint8_t *output;
	size_t output_length;
	ssize_t written;

	/* Names the calling process, if the write came from one. */
	handle = file->f_data;
	if (curthread != NULL)
		process = curthread->proc;
	else
		process = NULL;
	bytes = buffer;
	done = 0;

	/* Rejects an unbound file or a missing buffer. */
	if (handle == NULL || buffer == NULL)
		return -EINVAL;

	/* The master must be open to receive. */
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);

	if (!pty_handle_valid_locked(handle) || !pair->master_open) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return -EIO;
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	/* A background process is stopped or refused, then flow control applies. */
	tty = &pair->slave;
	error = tty_background(tty, process, TTY_BACKGROUND_WRITE);
	if (error != 0)
		return -error;
	error = tty_wait_output_enabled(tty, file);
	if (error != 0)
		return -error;

	/* Queues each byte, expanding newlines under ONLCR. */
	irq = spin_lock_irqsave(&tty->lock);

	oflag = tty->termios.c_oflag;

	spin_unlock_irqrestore(&tty->lock, irq);

	while (done < size) {
		output = bytes + done;
		output_length = 1;
		if (bytes[done] == '\n' &&
		    (oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
			output = (const uint8_t *)"\r\n";
			output_length = 2;
		}

		nonblocking = (file_status_flags_get(file) & O_NONBLOCK) != 0;
		written = pty_output_bytes(pair, output, output_length, nonblocking);
		if (written != (ssize_t)output_length) {
			/*
			 * A transformed byte is emitted atomically for
			 * nonblocking files; blocking writes complete it before
			 * returning.
			 */
			if (written > 0) {
				/* Rechecks flags changed while output was waiting. */
				nonblocking =
				    (file_status_flags_get(file) & O_NONBLOCK) != 0;
				if (!nonblocking)
					continue;
			}

			if (done != 0)
				return (ssize_t)done;
			if (written < 0)
				return written;
			return -EAGAIN;
		}

		done++;
	}

	/* Reports the bytes written. */
	return (ssize_t)done;
}

/* Handles a terminal control request on a slave. */
static int
pty_slave_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct pty_handle *handle;
	int error;

	handle = file->f_data;
	if (handle == NULL)
		return EIO;

	/* Reports the failure. */
	error = tty_ioctl_instance(&handle->pair->slave, file, request, argument);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the readiness of a slave. */
static int
pty_slave_poll(
	struct file *file,
	short events,
	short *revents)
{
	struct pty_handle *handle;
	struct pty_pair *pair;
	struct tty *tty;
	short result;
	unsigned output_stopped;
	unsigned long irq;
	int readable;

	handle = file->f_data;
	result = 0;

	/* Rejects an unbound file or a missing result. */
	if (handle == NULL || revents == NULL)
		return EINVAL;

	/* Readable with input; writable while the master is open and not stopped. */
	pair = handle->pair;
	tty = &pair->slave;
	irq = spin_lock_irqsave(&tty->lock);

	if ((tty->termios.c_lflag & ICANON) != 0)
		readable = tty->record_used != 0;
	else
		readable = tty->input_used != 0;
	if (readable)
		result |= events & (POLLIN | POLLRDNORM);
	output_stopped = tty->output_stopped;

	spin_unlock_irqrestore(&tty->lock, irq);

	irq = spin_lock_irqsave(&pair->lock);

	if (pair->master_open) {
		if (!output_stopped)
			result |= events & (POLLOUT | POLLWRNORM);
	} else {
		result |= POLLHUP;
	}

	spin_unlock_irqrestore(&pair->lock, irq);

	*revents = result;
	return 0;
}
