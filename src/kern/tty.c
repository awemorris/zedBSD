/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/tty.h"

#include "kern/clock.h"
#include "kern/file.h"
#include "kern/cdev.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/process.h"
#include "kern/sched.h"
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

struct tty_record {
	uint8_t data[TTY_LINE_MAX];
	size_t length;
	size_t offset;
	unsigned eof;
};

struct tty {
	struct spinlock lock;
	struct wait_queue read_waitq;
	struct termios termios;
	struct winsize winsize;
	pid_t session;
	pid_t foreground_pgrp;
	uint8_t edit[TTY_LINE_MAX];
	size_t edit_used;
	struct tty_record records[TTY_RECORDS];
	unsigned record_head, record_tail, record_used;
	uint8_t input[TTY_INPUT_MAX];
	unsigned input_head, input_tail, input_used;
	unsigned hungup;
};

static struct tty console_tty;
static struct spinlock console_output_lock;
static unsigned console_escape_state;
static unsigned console_escape_parameter;
static unsigned console_escape_has_parameter;

static void
tty_flush_input_locked(struct tty *tty)
{
	tty->edit_used = 0;
	tty->record_head = tty->record_tail = tty->record_used = 0;
	tty->input_head = tty->input_tail = tty->input_used = 0;
}

static void
tty_default_termios(struct termios *termios)
{
	memset(termios, 0, sizeof(*termios));
	termios->c_iflag = ICRNL | IXON;
	termios->c_oflag = OPOST | ONLCR;
	termios->c_cflag = CREAD | CS8 | CLOCAL;
	termios->c_lflag = ECHO | ECHOE | ECHOK | ICANON | IEXTEN | ISIG;
	termios->c_cc[VINTR] = 3;
	termios->c_cc[VQUIT] = 28;
	termios->c_cc[VERASE] = 8;
	termios->c_cc[VKILL] = 21;
	termios->c_cc[VEOF] = 4;
	termios->c_cc[VSTART] = 17;
	termios->c_cc[VSTOP] = 19;
	termios->c_cc[VSUSP] = 26;
	termios->c_cc[VMIN] = 1;
	termios->c_cc[VTIME] = 0;
	termios->c_ispeed = B9600;
	termios->c_ospeed = B9600;
}

int
tty_console_init(void)
{
	memset(&console_tty, 0, sizeof(console_tty));
	spin_init(&console_tty.lock, LOCK_RANK_TTY, "console tty");
	spin_init(&console_output_lock, LOCK_RANK_TTY, "console output");
	waitq_init(&console_tty.read_waitq, "console tty input");
	tty_default_termios(&console_tty.termios);
	console_tty.winsize.ws_row = HAL_CONS_ROWS;
	console_tty.winsize.ws_col = HAL_CONS_COLUMNS;
	return 0;
}

static void
tty_console_csi(unsigned command)
{
	/* Minimal ANSI cursor/erase baseline shared by every HAL console. */
	struct hal_cons_state state;
	unsigned amount = console_escape_has_parameter ?
		console_escape_parameter : 1U;

	hal_cons_save_state(&state);
	switch (command) {
	case 'A':
		state.row = amount < state.row ? state.row - amount : 0U;
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
		state.column = amount < state.column ? state.column - amount : 0U;
		(void)hal_cons_set_cursor(state.row, state.column);
		break;
	case 'G':
		state.column = amount == 0U ? 0U : amount - 1U;
		if (state.column >= HAL_CONS_COLUMNS)
			state.column = HAL_CONS_COLUMNS - 1U;
		(void)hal_cons_set_cursor(state.row, state.column);
		break;
	case 'K':
		if (amount == 2U) {
			hal_cons_clear_row(state.row);
			(void)hal_cons_set_cursor(state.row, state.column);
		} else if (!console_escape_has_parameter || amount == 0U) {
			hal_cons_clear_to_eol();
		}
		break;
	default:
		break;
	}
}

static void
tty_echo(const char *bytes, size_t length)
{
	size_t index = 0;
	unsigned long irq;

	if (length == 0)
		return;
	irq = spin_lock_irqsave(&console_output_lock);
	while (index < length) {
		if (console_escape_state == 0U) {
			size_t start = index;
			while (index < length && (unsigned char)bytes[index] != 0x1bU)
				index++;
			if (index != start)
				hal_cons_write_n(bytes + start, (unsigned)(index - start));
			if (index < length) {
				console_escape_state = 1U;
				index++;
			}
			continue;
		}
		if (console_escape_state == 1U) {
			if (bytes[index++] == '[') {
				console_escape_state = 2U;
				console_escape_parameter = 0U;
				console_escape_has_parameter = 0U;
			} else {
				static const char escape = '\033';
				hal_cons_write_n(&escape, 1U);
				hal_cons_write_n(bytes + index - 1U, 1U);
				console_escape_state = 0U;
			}
			continue;
		}
		if (bytes[index] >= '0' && bytes[index] <= '9') {
			console_escape_has_parameter = 1U;
			if (console_escape_parameter < 1000U)
				console_escape_parameter = console_escape_parameter * 10U +
				    (unsigned)(bytes[index] - '0');
			index++;
			continue;
		}
		tty_console_csi((unsigned char)bytes[index++]);
		console_escape_state = 0U;
	}
	spin_unlock_irqrestore(&console_output_lock, irq);
}

static void
tty_commit_locked(struct tty *tty, unsigned eof)
{
	struct tty_record *record;
	if (tty->record_used == TTY_RECORDS)
		return;
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

void
tty_console_input_event(uint32_t event)
{
	struct tty *tty = &console_tty;
	unsigned key = event & HAL_KEY_EVENT_KEY_MASK;
	char echo[3];
	size_t echo_length = 0;
	pid_t signal_session = 0, signal_pgrp = 0;
	int signal_number = 0;
	unsigned long irq;
	uint8_t byte;
	const char *sequence = NULL;
	size_t sequence_length = 0;

	/*
	 * Expose machine-independent ANSI key sequences to noncanonical readers.
	 * Canonical input keeps its historical behavior and ignores navigation.
	 */
	switch (key) {
	case HAL_KEY_UP: sequence = "\033[A"; sequence_length = 3; break;
	case HAL_KEY_DOWN: sequence = "\033[B"; sequence_length = 3; break;
	case HAL_KEY_RIGHT: sequence = "\033[C"; sequence_length = 3; break;
	case HAL_KEY_LEFT: sequence = "\033[D"; sequence_length = 3; break;
	case HAL_KEY_HOME: sequence = "\033[H"; sequence_length = 3; break;
	case HAL_KEY_END: sequence = "\033[F"; sequence_length = 3; break;
	case HAL_KEY_INSERT: sequence = "\033[2~"; sequence_length = 4; break;
	case HAL_KEY_DELETE: sequence = "\033[3~"; sequence_length = 4; break;
	case HAL_KEY_PAGE_UP: sequence = "\033[5~"; sequence_length = 4; break;
	case HAL_KEY_PAGE_DOWN: sequence = "\033[6~"; sequence_length = 4; break;
	default: break;
	}
	if (sequence != NULL) {
		int accepted = 0;
		irq = spin_lock_irqsave(&tty->lock);
		if ((tty->termios.c_lflag & ICANON) == 0 &&
		    TTY_INPUT_MAX - tty->input_used >= sequence_length) {
			size_t index;
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

	if (key == HAL_KEY_ENTER)
		byte = '\n';
	else if (key == HAL_KEY_BACKSPACE)
		byte = 8;
	else if (key == HAL_KEY_TAB)
		byte = '\t';
	else if (key > 0xffU)
		return;
	else
		byte = (uint8_t)key;
	if ((event & HAL_KEY_EVENT_CTRL) != 0 && byte >= 'a' && byte <= 'z')
		byte = (uint8_t)(byte - 'a' + 1);
	else if ((event & HAL_KEY_EVENT_CTRL) != 0 && byte >= 'A' && byte <= 'Z')
		byte = (uint8_t)(byte - 'A' + 1);

	irq = spin_lock_irqsave(&tty->lock);
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
	if ((tty->termios.c_lflag & ISIG) != 0) {
		if (byte == tty->termios.c_cc[VINTR]) signal_number = SIGINT;
		else if (byte == tty->termios.c_cc[VQUIT]) signal_number = SIGQUIT;
		else if (byte == tty->termios.c_cc[VSUSP]) signal_number = SIGTSTP;
		if (signal_number != 0) {
			signal_session = tty->session;
			signal_pgrp = tty->foreground_pgrp;
			if ((tty->termios.c_lflag & NOFLSH) == 0)
				tty_flush_input_locked(tty);
			goto changed;
		}
	}
	if ((tty->termios.c_lflag & ICANON) != 0) {
		if (byte == tty->termios.c_cc[VERASE]) {
			if (tty->edit_used != 0) {
				tty->edit_used--;
				if ((tty->termios.c_lflag & ECHOE) != 0) {
					echo[0] = '\b'; echo[1] = ' '; echo[2] = '\b';
					echo_length = 3;
				}
			}
		} else if (byte == tty->termios.c_cc[VKILL]) {
			tty->edit_used = 0;
			if ((tty->termios.c_lflag & ECHOK) != 0) {
				echo[0] = '\n'; echo_length = 1;
			}
		} else if (byte == tty->termios.c_cc[VEOF]) {
			tty_commit_locked(tty, 1);
		} else if (byte == '\n' ||
		    (tty->termios.c_cc[VEOL] != 0 &&
		     byte == tty->termios.c_cc[VEOL])) {
			if (tty->edit_used < TTY_LINE_MAX)
				tty->edit[tty->edit_used++] = byte;
			tty_commit_locked(tty, 0);
			if ((tty->termios.c_lflag & (ECHO | ECHONL)) != 0) {
				echo[0] = (char)byte; echo_length = 1;
			}
		} else if (tty->edit_used < TTY_LINE_MAX) {
			tty->edit[tty->edit_used++] = byte;
			if ((tty->termios.c_lflag & ECHO) != 0) {
				echo[0] = (char)byte; echo_length = 1;
			}
		}
	} else if (tty->input_used < TTY_INPUT_MAX) {
		tty->input[tty->input_head] = byte;
		tty->input_head = (tty->input_head + 1U) % TTY_INPUT_MAX;
		tty->input_used++;
		waitq_wake_all(&tty->read_waitq);
		if ((tty->termios.c_lflag & ECHO) != 0) {
			echo[0] = (char)byte; echo_length = 1;
		}
	}
changed:
	poll_notify();
out:
	spin_unlock_irqrestore(&tty->lock, irq);
	tty_echo(echo, echo_length);
	if (signal_number != 0 && signal_session > 0 && signal_pgrp > 0)
		(void)process_signal_pgrp(signal_session, signal_pgrp, signal_number);
}

static int
tty_background(struct tty *tty, struct process *process, int write)
{
	pid_t session, foreground;
	unsigned lflag;
	unsigned long irq;
	int signo;

	if (process == NULL || process->controlling_tty != tty)
		return 0;
	irq = spin_lock_irqsave(&tty->lock);
	session = tty->session;
	foreground = tty->foreground_pgrp;
	lflag = tty->termios.c_lflag;
	spin_unlock_irqrestore(&tty->lock, irq);
	if (process->session != session || process->pgrp == foreground ||
	    (write && (lflag & TOSTOP) == 0))
		return 0;
	signo = write ? SIGTTOU : SIGTTIN;
	(void)process_signal_pgrp(process->session, process->pgrp, signo);
	return EINTR;
}

static ssize_t
tty_read_canonical(struct tty *tty, void *buffer, size_t size,
	int nonblocking)
{
	uint8_t *output = buffer;
	unsigned long irq = spin_lock_irqsave(&tty->lock);
	struct tty_record *record;
	size_t count;

	while (tty->record_used == 0) {
		uint64_t sequence;
		int error;
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
	record = &tty->records[tty->record_tail];
	if (record->eof && record->length == 0) {
		count = 0;
	} else {
		count = record->length - record->offset;
		if (count > size) count = size;
		memcpy(output, record->data + record->offset, count);
		record->offset += count;
	}
	if (record->offset == record->length) {
		tty->record_tail = (tty->record_tail + 1U) % TTY_RECORDS;
		tty->record_used--;
	}
	spin_unlock_irqrestore(&tty->lock, irq);
	poll_notify();
	return (ssize_t)count;
}

static ssize_t
tty_read_noncanonical(struct tty *tty, void *buffer, size_t size,
	int nonblocking)
{
	uint8_t *output = buffer;
	unsigned long irq = spin_lock_irqsave(&tty->lock);
	unsigned minimum = tty->termios.c_cc[VMIN];
	unsigned deciseconds = tty->termios.c_cc[VTIME];
	uint64_t deadline = 0;
	size_t count;

	if (minimum > size) minimum = (unsigned)size;
	if (nonblocking) minimum = 0;
	if (deciseconds != 0 && minimum == 0)
		deadline = sched_ticks() + (uint64_t)deciseconds *
		    (KERN_CLOCK_HZ / 10U);
	while (tty->input_used < minimum ||
	    (minimum == 0 && tty->input_used == 0 && deciseconds != 0)) {
		uint64_t sequence = waitq_sequence(&tty->read_waitq);
		int error;
		if (tty->hungup) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return 0;
		}
		if (nonblocking) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -EAGAIN;
		}
		if (minimum != 0 && deciseconds != 0 && tty->input_used != 0 &&
		    deadline == 0)
			deadline = sched_ticks() + (uint64_t)deciseconds *
			    (KERN_CLOCK_HZ / 10U);
		error = waitq_sleep(&tty->read_waitq, &tty->lock, sequence,
		    deadline, WAITQ_INTERRUPTIBLE);
		if (error == ETIMEDOUT)
			break;
		if (error != 0) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -error;
		}
	}
	count = tty->input_used;
	if (count > size) count = size;
	for (size_t i = 0; i < count; i++) {
		output[i] = tty->input[tty->input_tail];
		tty->input_tail = (tty->input_tail + 1U) % TTY_INPUT_MAX;
		tty->input_used--;
	}
	spin_unlock_irqrestore(&tty->lock, irq);
	poll_notify();
	return (ssize_t)count;
}

ssize_t
tty_console_read(struct file *file, void *buffer, size_t size)
{
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	unsigned canonical;
	unsigned long irq;
	int error;

	if (buffer == NULL) return -EINVAL;
	if (size == 0) return 0;
	error = tty_background(&console_tty, process, 0);
	if (error != 0) return -error;
	irq = spin_lock_irqsave(&console_tty.lock);
	canonical = console_tty.termios.c_lflag & ICANON;
	spin_unlock_irqrestore(&console_tty.lock, irq);
	return canonical ? tty_read_canonical(&console_tty, buffer, size,
	    (file->f_flags & O_NONBLOCK) != 0) :
	    tty_read_noncanonical(&console_tty, buffer, size,
	    (file->f_flags & O_NONBLOCK) != 0);
}

ssize_t
tty_console_write(struct file *file, const void *buffer, size_t size)
{
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	const char *bytes = buffer;
	unsigned oflag;
	unsigned long irq;
	int error = tty_background(&console_tty, process, 1);
	(void)file;

	if (error != 0) return -error;
	if (buffer == NULL) return -EINVAL;
	irq = spin_lock_irqsave(&console_tty.lock);
	oflag = console_tty.termios.c_oflag;
	spin_unlock_irqrestore(&console_tty.lock, irq);
	if ((oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
		size_t start = 0;
		for (size_t i = 0; i < size; i++)
			if (bytes[i] == '\n') {
				if (i != start) tty_echo(bytes + start, i - start);
				tty_echo("\r\n", 2);
				start = i + 1U;
			}
		if (start < size) tty_echo(bytes + start, size - start);
	} else {
		tty_echo(bytes, size);
	}
	return (ssize_t)size;
}

static int
tty_termios_valid(const struct termios *value)
{
	return value != NULL && (value->c_cflag & CS8) != 0 &&
	    (value->c_ispeed == B0 || value->c_ispeed == B9600 ||
	     value->c_ispeed == B19200 || value->c_ispeed == B38400) &&
	    (value->c_ospeed == B0 || value->c_ospeed == B9600 ||
	     value->c_ospeed == B19200 || value->c_ospeed == B38400);
}

static int
tty_ioctl_instance(struct tty *tty, struct file *file,
	unsigned long request, uintptr_t argument)
{
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	unsigned long irq;
	int error = 0;
	(void)file;

	switch (request) {
	case TCGETS: {
		struct termios value;
		irq = spin_lock_irqsave(&tty->lock); value = tty->termios;
		spin_unlock_irqrestore(&tty->lock, irq);
		return copyout(&value, argument, sizeof(value));
	}
	case TCSETS: case TCSETSW: case TCSETSF: {
		struct termios value;
		error = copyin(argument, &value, sizeof(value));
		if (error != 0) return error;
		if (!tty_termios_valid(&value)) return EINVAL;
		irq = spin_lock_irqsave(&tty->lock);
		if (request == TCSETSF) tty_flush_input_locked(tty);
		tty->termios = value;
		waitq_wake_all(&tty->read_waitq);
		spin_unlock_irqrestore(&tty->lock, irq);
		poll_notify(); return 0;
	}
	case TIOCGWINSZ: {
		struct winsize value;
		irq = spin_lock_irqsave(&tty->lock); value = tty->winsize;
		spin_unlock_irqrestore(&tty->lock, irq);
		return copyout(&value, argument, sizeof(value));
	}
	case TIOCSWINSZ: {
		struct winsize value;
		error = copyin(argument, &value, sizeof(value));
		if (error != 0) return error;
		irq = spin_lock_irqsave(&tty->lock); tty->winsize = value;
		spin_unlock_irqrestore(&tty->lock, irq); return 0;
	}
	case TIOCGPGRP: {
		pid_t value;
		if (process == NULL || process->controlling_tty != tty) return ENOTTY;
		irq = spin_lock_irqsave(&tty->lock); value = tty->foreground_pgrp;
		spin_unlock_irqrestore(&tty->lock, irq);
		return copyout(&value, argument, sizeof(value));
	}
	case TIOCSPGRP: {
		pid_t value;
		if (process == NULL || process->controlling_tty != tty) return ENOTTY;
		error = copyin(argument, &value, sizeof(value));
		if (error != 0) return error;
		if (!process_pgrp_in_session(process->session, value)) return EPERM;
		irq = spin_lock_irqsave(&tty->lock);
		if (tty->session != process->session) error = EPERM;
		else tty->foreground_pgrp = value;
		spin_unlock_irqrestore(&tty->lock, irq);
		poll_notify(); return error;
	}
	case TIOCSCTTY:
		if (process == NULL || process->session != process->pid) return EPERM;
		irq = spin_lock_irqsave(&tty->lock);
		if (tty->session != 0 && tty->session != process->session) error = EPERM;
		else { tty->session = process->session; tty->foreground_pgrp = process->pgrp;
			process->controlling_tty = tty; }
		spin_unlock_irqrestore(&tty->lock, irq); return error;
	case TIOCNOTTY:
		if (process == NULL || process->controlling_tty != tty) return ENOTTY;
		tty_detach_process(process); return 0;
	case TIOCFLUSH: {
		int queue;
		error = copyin(argument, &queue, sizeof(queue));
		if (error != 0) return error;
		if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH)
			return EINVAL;
		if (queue != TCOFLUSH) {
			irq = spin_lock_irqsave(&tty->lock); tty_flush_input_locked(tty);
			spin_unlock_irqrestore(&tty->lock, irq); poll_notify();
		}
		return 0;
	}
	default: return EOPNOTSUPP;
	}
}

int
tty_console_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	return tty_ioctl_instance(&console_tty, file, request, argument);
}

int
tty_console_poll(struct file *file, short events, short *revents)
{
	short result = events & (POLLOUT | POLLWRNORM);
	unsigned long irq;
	(void)file;
	if (revents == NULL) return EINVAL;
	irq = spin_lock_irqsave(&console_tty.lock);
	if ((console_tty.termios.c_lflag & ICANON) != 0 ?
	    console_tty.record_used != 0 : console_tty.input_used != 0)
		result |= events & (POLLIN | POLLRDNORM);
	spin_unlock_irqrestore(&console_tty.lock, irq);
	*revents = result;
	return 0;
}

void
tty_attach_console(struct process *process)
{
	unsigned long irq;
	if (process == NULL || process == &process0 || process->session != process->pid)
		return;
	irq = spin_lock_irqsave(&console_tty.lock);
	if (console_tty.session == 0 || console_tty.session == process->session) {
		console_tty.session = process->session;
		console_tty.foreground_pgrp = process->pgrp;
		process->controlling_tty = &console_tty;
	}
	spin_unlock_irqrestore(&console_tty.lock, irq);
}

void
tty_detach_process(struct process *process)
{
	struct tty *tty;
	pid_t session = 0, pgrp = 0;
	unsigned long irq;
	if (process == NULL || process->controlling_tty == NULL)
		return;
	tty = process->controlling_tty;
	irq = spin_lock_irqsave(&tty->lock);
	process->controlling_tty = NULL;
	if (tty->session == process->pid) {
		session = tty->session;
		pgrp = tty->foreground_pgrp;
		tty->session = tty->foreground_pgrp = 0;
	}
	spin_unlock_irqrestore(&tty->lock, irq);
	if (session > 0 && pgrp > 0) {
		(void)process_signal_pgrp(session, pgrp, SIGHUP);
		(void)process_signal_pgrp(session, pgrp, SIGCONT);
	}
}


/* UNIX98-style pseudo terminals.  The slave uses the same termios and
 * background-process checks as the physical console; only its byte transport
 * is different. */
#define PTY_MAX 8U
#define PTY_OUTPUT_MAX 4096U

struct pty_pair {
	struct spinlock lock;
	struct wait_queue output_waitq;
	struct tty slave;
	uint8_t output[PTY_OUTPUT_MAX];
	unsigned output_head, output_tail, output_used;
	unsigned index, generation;
	unsigned active, locked, master_open;
	unsigned slave_opens, slave_ever_opened;
};

struct pty_handle {
	struct pty_pair *pair;
	unsigned generation;
	unsigned master;
};

static struct spinlock pty_registry_lock;
static struct pty_pair pty_pairs[PTY_MAX];
static const struct file_ops pty_master_file_ops;

static int
pty_handle_valid_locked(const struct pty_handle *handle)
{
	return handle != NULL && handle->pair->active &&
	    handle->generation == handle->pair->generation;
}

static ssize_t
pty_output_bytes(struct pty_pair *pair, const uint8_t *bytes, size_t length,
	int nonblocking)
{
	size_t done = 0;
	unsigned long irq = spin_lock_irqsave(&pair->lock);
	while (done < length) {
		if (nonblocking && done == 0 &&
		    PTY_OUTPUT_MAX - pair->output_used < length) {
			spin_unlock_irqrestore(&pair->lock, irq);
			return -EAGAIN;
		}
		while (done < length && pair->output_used < PTY_OUTPUT_MAX) {
			pair->output[pair->output_head] = bytes[done++];
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
			return done != 0 ? (ssize_t)done : -EIO;
		}
		if (nonblocking) {
			spin_unlock_irqrestore(&pair->lock, irq);
			return done != 0 ? (ssize_t)done : -EAGAIN;
		}
		{
			uint64_t sequence = waitq_sequence(&pair->output_waitq);
			int error = waitq_sleep(&pair->output_waitq, &pair->lock,
			    sequence, 0, WAITQ_INTERRUPTIBLE);
			if (error != 0) {
				spin_unlock_irqrestore(&pair->lock, irq);
				return done != 0 ? (ssize_t)done : -error;
			}
		}
	}
	spin_unlock_irqrestore(&pair->lock, irq);
	return (ssize_t)done;
}

static void
pty_input_byte(struct pty_pair *pair, uint8_t byte)
{
	struct tty *tty = &pair->slave;
	uint8_t echo[3];
	size_t echo_length = 0;
	pid_t signal_session = 0, signal_pgrp = 0;
	int signal_number = 0;
	unsigned output_flags = 0;
	unsigned long irq;

	irq = spin_lock_irqsave(&tty->lock);
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
	if ((tty->termios.c_lflag & ISIG) != 0) {
		if (byte == tty->termios.c_cc[VINTR])
			signal_number = SIGINT;
		else if (byte == tty->termios.c_cc[VQUIT])
			signal_number = SIGQUIT;
		else if (byte == tty->termios.c_cc[VSUSP])
			signal_number = SIGTSTP;
		if (signal_number != 0) {
			signal_session = tty->session;
			signal_pgrp = tty->foreground_pgrp;
			if ((tty->termios.c_lflag & NOFLSH) == 0)
				tty_flush_input_locked(tty);
			goto changed;
		}
	}
	if ((tty->termios.c_lflag & ICANON) != 0) {
		if (byte == tty->termios.c_cc[VERASE]) {
			if (tty->edit_used != 0) {
				tty->edit_used--;
				if ((tty->termios.c_lflag & ECHOE) != 0) {
					echo[0] = '\b'; echo[1] = ' '; echo[2] = '\b';
					echo_length = 3;
				}
			}
		} else if (byte == tty->termios.c_cc[VKILL]) {
			tty->edit_used = 0;
			if ((tty->termios.c_lflag & ECHOK) != 0) {
				echo[0] = '\n';
				echo_length = 1;
			}
		} else if (byte == tty->termios.c_cc[VEOF]) {
			tty_commit_locked(tty, 1);
		} else if (byte == '\n' ||
		    (tty->termios.c_cc[VEOL] != 0 &&
		    byte == tty->termios.c_cc[VEOL])) {
			if (tty->edit_used < TTY_LINE_MAX)
				tty->edit[tty->edit_used++] = byte;
			tty_commit_locked(tty, 0);
			if ((tty->termios.c_lflag & (ECHO | ECHONL)) != 0) {
				echo[0] = byte;
				echo_length = 1;
			}
		} else if (tty->edit_used < TTY_LINE_MAX) {
			tty->edit[tty->edit_used++] = byte;
			if ((tty->termios.c_lflag & ECHO) != 0) {
				echo[0] = byte;
				echo_length = 1;
			}
		}
	} else if (tty->input_used < TTY_INPUT_MAX) {
		tty->input[tty->input_head] = byte;
		tty->input_head = (tty->input_head + 1U) % TTY_INPUT_MAX;
		tty->input_used++;
		waitq_wake_all(&tty->read_waitq);
		if ((tty->termios.c_lflag & ECHO) != 0) {
			echo[0] = byte;
			echo_length = 1;
		}
	}
changed:
	output_flags = tty->termios.c_oflag;
	poll_notify();
out:
	spin_unlock_irqrestore(&tty->lock, irq);
	if (echo_length == 1 && echo[0] == '\n' &&
	    (output_flags & (OPOST | ONLCR)) == (OPOST | ONLCR))
		(void)pty_output_bytes(pair, (const uint8_t *)"\r\n", 2, 0);
	else
		(void)pty_output_bytes(pair, echo, echo_length, 0);
	if (signal_number != 0 && signal_session > 0 && signal_pgrp > 0)
		(void)process_signal_pgrp(signal_session, signal_pgrp,
		    signal_number);
}

static int
pty_master_open(struct file *file)
{
	struct pty_handle *handle;
	struct pty_pair *pair = NULL;
	unsigned i;
	unsigned long irq;

	handle = kern_malloc(sizeof(*handle));
	if (handle == NULL)
		return ENFILE;
	irq = spin_lock_irqsave(&pty_registry_lock);
	for (i = 0; i < PTY_MAX; i++)
		if (!pty_pairs[i].active) {
			pair = &pty_pairs[i];
			pair->active = 1;
			pair->locked = 1;
			pair->master_open = 1;
			pair->slave_opens = 0;
			pair->slave_ever_opened = 0;
			pair->output_head = pair->output_tail = pair->output_used = 0;
			if (++pair->generation == 0)
				pair->generation = 1;
			break;
		}
	spin_unlock_irqrestore(&pty_registry_lock, irq);
	if (pair == NULL) {
		kern_free(handle);
		return ENOSPC;
	}
	irq = spin_lock_irqsave(&pair->slave.lock);
	tty_flush_input_locked(&pair->slave);
	tty_default_termios(&pair->slave.termios);
	pair->slave.session = pair->slave.foreground_pgrp = 0;
	pair->slave.hungup = 0;
	spin_unlock_irqrestore(&pair->slave.lock, irq);
	handle->pair = pair;
	handle->generation = pair->generation;
	handle->master = 1;
	file->f_data = handle;
	file->f_ops = &pty_master_file_ops;
	return 0;
}

static int
pty_master_close(struct file *file)
{
	struct pty_handle *handle = file->f_data;
	struct pty_pair *pair;
	unsigned deactivate = 0;
	unsigned long irq;
	pid_t session = 0, pgrp = 0;

	if (handle == NULL)
		return 0;
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);
	if (pty_handle_valid_locked(handle)) {
		pair->master_open = 0;
		deactivate = pair->slave_opens == 0;
		waitq_wake_all(&pair->output_waitq);
	}
	spin_unlock_irqrestore(&pair->lock, irq);
	irq = spin_lock_irqsave(&pair->slave.lock);
	pair->slave.hungup = 1;
	session = pair->slave.session;
	pgrp = pair->slave.foreground_pgrp;
	waitq_wake_all(&pair->slave.read_waitq);
	spin_unlock_irqrestore(&pair->slave.lock, irq);
	if (session > 0 && pgrp > 0) {
		(void)process_signal_pgrp(session, pgrp, SIGHUP);
		(void)process_signal_pgrp(session, pgrp, SIGCONT);
	}
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

static ssize_t
pty_master_read(struct file *file, void *buffer, size_t size)
{
	struct pty_handle *handle = file->f_data;
	struct pty_pair *pair;
	uint8_t *output = buffer;
	unsigned long irq;
	size_t count, i;

	if (handle == NULL || buffer == NULL)
		return -EINVAL;
	if (size == 0)
		return 0;
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);
	while (pty_handle_valid_locked(handle) && pair->output_used == 0 &&
	    (!pair->slave_ever_opened || pair->slave_opens != 0)) {
		uint64_t sequence;
		int error;
		if ((file->f_flags & O_NONBLOCK) != 0) {
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
	count = pair->output_used < size ? pair->output_used : size;
	for (i = 0; i < count; i++) {
		output[i] = pair->output[pair->output_tail];
		pair->output_tail = (pair->output_tail + 1U) % PTY_OUTPUT_MAX;
		pair->output_used--;
	}
	waitq_wake_all(&pair->output_waitq);
	spin_unlock_irqrestore(&pair->lock, irq);
	poll_notify();
	return (ssize_t)count;
}

static ssize_t
pty_master_write(struct file *file, const void *buffer, size_t size)
{
	struct pty_handle *handle = file->f_data;
	const uint8_t *input = buffer;
	struct pty_pair *pair;
	unsigned long irq;
	size_t i;

	if (handle == NULL || buffer == NULL)
		return -EINVAL;
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);
	if (!pty_handle_valid_locked(handle) || !pair->master_open ||
	    (pair->slave_ever_opened && pair->slave_opens == 0)) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return -EIO;
	}
	spin_unlock_irqrestore(&pair->lock, irq);
	for (i = 0; i < size; i++)
		pty_input_byte(pair, input[i]);
	return (ssize_t)size;
}

static int
pty_master_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct pty_handle *handle = file->f_data;
	struct pty_pair *pair;
	unsigned long irq;
	int error = 0;

	if (handle == NULL)
		return EINVAL;
	pair = handle->pair;
	switch (request) {
	case TIOCGPTN: {
		uint32_t number = pair->index;
		return copyout(&number, argument, sizeof(number));
	}
	case TIOCSPTLCK: {
		int32_t locked;
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
	}
	default:
		return tty_ioctl_instance(&pair->slave, file, request, argument);
	}
}

static int
pty_master_poll(struct file *file, short events, short *revents)
{
	struct pty_handle *handle = file->f_data;
	struct pty_pair *pair;
	short result = 0;
	unsigned long irq;
	if (handle == NULL || revents == NULL)
		return EINVAL;
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);
	if (!pty_handle_valid_locked(handle))
		result = POLLERR | POLLHUP;
	else {
		if (pair->output_used != 0)
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

static int
pty_slave_open(struct file *file)
{
	uintptr_t encoded = (uintptr_t)file->f_inode->i_data;
	struct pty_handle *handle;
	struct pty_pair *pair;
	struct process *process;
	unsigned index;
	unsigned long irq;

	if (encoded == 0)
		return ENXIO;
	index = (unsigned)(encoded - 1U);
	if (index >= PTY_MAX)
		return ENXIO;
	handle = kern_malloc(sizeof(*handle));
	if (handle == NULL)
		return ENFILE;
	pair = &pty_pairs[index];
	irq = spin_lock_irqsave(&pair->lock);
	if (!pair->active || !pair->master_open || pair->locked) {
		spin_unlock_irqrestore(&pair->lock, irq);
		kern_free(handle);
		return pair->locked ? EACCES : ENXIO;
	}
	pair->slave_opens++;
	pair->slave_ever_opened = 1;
	handle->pair = pair;
	handle->generation = pair->generation;
	handle->master = 0;
	spin_unlock_irqrestore(&pair->lock, irq);
	file->f_data = handle;
	process = curthread != NULL ? curthread->proc : NULL;
	if (process != NULL && (file->f_flags & O_NOCTTY) == 0 &&
	    process->session == process->pid && process->controlling_tty == NULL) {
		irq = spin_lock_irqsave(&pair->slave.lock);
		if (pair->slave.session == 0) {
			pair->slave.session = process->session;
			pair->slave.foreground_pgrp = process->pgrp;
			process->controlling_tty = &pair->slave;
		}
		spin_unlock_irqrestore(&pair->slave.lock, irq);
	}
	poll_notify();
	return 0;
}

static int
pty_slave_close(struct file *file)
{
	struct pty_handle *handle = file->f_data;
	struct pty_pair *pair;
	unsigned deactivate = 0;
	unsigned long irq;
	if (handle == NULL)
		return 0;
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

static ssize_t
pty_slave_read(struct file *file, void *buffer, size_t size)
{
	struct pty_handle *handle = file->f_data;
	struct tty *tty;
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	unsigned canonical;
	unsigned long irq;
	int error;
	if (handle == NULL)
		return -EIO;
	tty = &handle->pair->slave;
	error = tty_background(tty, process, 0);
	if (error != 0)
		return -error;
	irq = spin_lock_irqsave(&tty->lock);
	canonical = tty->termios.c_lflag & ICANON;
	spin_unlock_irqrestore(&tty->lock, irq);
	return canonical ? tty_read_canonical(tty, buffer, size,
	    (file->f_flags & O_NONBLOCK) != 0) :
	    tty_read_noncanonical(tty, buffer, size,
	    (file->f_flags & O_NONBLOCK) != 0);
}

static ssize_t
pty_slave_write(struct file *file, const void *buffer, size_t size)
{
	struct pty_handle *handle = file->f_data;
	struct pty_pair *pair;
	struct tty *tty;
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	const uint8_t *bytes = buffer;
	unsigned oflag;
	unsigned long irq;
	size_t done = 0;
	int error;

	if (handle == NULL || buffer == NULL)
		return -EINVAL;
	pair = handle->pair;
	irq = spin_lock_irqsave(&pair->lock);
	if (!pty_handle_valid_locked(handle) || !pair->master_open) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return -EIO;
	}
	spin_unlock_irqrestore(&pair->lock, irq);
	tty = &pair->slave;
	error = tty_background(tty, process, 1);
	if (error != 0)
		return -error;
	irq = spin_lock_irqsave(&tty->lock);
	oflag = tty->termios.c_oflag;
	spin_unlock_irqrestore(&tty->lock, irq);
	while (done < size) {
		const uint8_t *output = bytes + done;
		size_t output_length = 1;
		ssize_t written;
		if (bytes[done] == '\n' &&
		    (oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
			output = (const uint8_t *)"\r\n";
			output_length = 2;
		}
		written = pty_output_bytes(pair, output, output_length,
		    (file->f_flags & O_NONBLOCK) != 0);
		if (written != (ssize_t)output_length) {
			/* A transformed byte is emitted atomically for nonblocking
			 * files; blocking writes complete it before returning. */
			if (written > 0 && (file->f_flags & O_NONBLOCK) == 0)
				continue;
			return done != 0 ? (ssize_t)done :
			    (written < 0 ? written : -EAGAIN);
		}
		done++;
	}
	return (ssize_t)done;
}

static int
pty_slave_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct pty_handle *handle = file->f_data;
	return handle != NULL ?
	    tty_ioctl_instance(&handle->pair->slave, file, request, argument) :
	    EIO;
}

static int
pty_slave_poll(struct file *file, short events, short *revents)
{
	struct pty_handle *handle = file->f_data;
	struct pty_pair *pair;
	struct tty *tty;
	short result = 0;
	unsigned long irq;
	if (handle == NULL || revents == NULL)
		return EINVAL;
	pair = handle->pair;
	tty = &pair->slave;
	irq = spin_lock_irqsave(&tty->lock);
	if ((tty->termios.c_lflag & ICANON) != 0 ?
	    tty->record_used != 0 : tty->input_used != 0)
		result |= events & (POLLIN | POLLRDNORM);
	spin_unlock_irqrestore(&tty->lock, irq);
	irq = spin_lock_irqsave(&pair->lock);
	if (pair->master_open)
		result |= events & (POLLOUT | POLLWRNORM);
	else
		result |= POLLHUP;
	spin_unlock_irqrestore(&pair->lock, irq);
	*revents = result;
	return 0;
}

const struct file_ops tty_pty_slave_file_ops = {
	.open = pty_slave_open,
	.read = pty_slave_read,
	.write = pty_slave_write,
	.ioctl = pty_slave_ioctl,
	.poll = pty_slave_poll,
	.close = pty_slave_close,
};

int
tty_pty_exists(unsigned index)
{
	unsigned result;
	unsigned long irq;
	if (index >= PTY_MAX)
		return 0;
	irq = spin_lock_irqsave(&pty_pairs[index].lock);
	result = pty_pairs[index].active;
	spin_unlock_irqrestore(&pty_pairs[index].lock, irq);
	return (int)result;
}

unsigned
tty_pty_snapshot(unsigned *indices, unsigned capacity)
{
	unsigned count = 0, i;
	for (i = 0; i < PTY_MAX; i++)
		if (tty_pty_exists(i)) {
			if (indices != NULL && count < capacity)
				indices[count] = i;
			count++;
		}
	return count;
}

int
tty_pty_register(void)
{
	unsigned i;
	spin_init(&pty_registry_lock, LOCK_RANK_TTY, "pty registry");
	for (i = 0; i < PTY_MAX; i++) {
		struct pty_pair *pair = &pty_pairs[i];
		memset(pair, 0, sizeof(*pair));
		pair->index = i;
		spin_init(&pair->lock, LOCK_RANK_TTY, "pty pair");
		waitq_init(&pair->output_waitq, "pty master output");
		spin_init(&pair->slave.lock, LOCK_RANK_TTY, "pty slave");
		waitq_init(&pair->slave.read_waitq, "pty slave input");
		tty_default_termios(&pair->slave.termios);
		pair->slave.winsize.ws_row = HAL_CONS_ROWS;
		pair->slave.winsize.ws_col = HAL_CONS_COLUMNS;
	}
	return cdev_register("ptmx", 0x00010001U, &pty_ptmx_ops, NULL);
}
