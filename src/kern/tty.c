/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/tty.h"

#include "kern/clock.h"
#include "kern/file.h"
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
};

static struct tty console_tty;

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
	waitq_init(&console_tty.read_waitq, "console tty input");
	tty_default_termios(&console_tty.termios);
	console_tty.winsize.ws_row = HAL_CONS_ROWS;
	console_tty.winsize.ws_col = HAL_CONS_COLUMNS;
	return 0;
}

static void
tty_echo(const char *bytes, size_t length)
{
	if (length != 0)
		hal_cons_write_n(bytes, (unsigned)length);
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

int
tty_console_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	struct tty *tty = &console_tty;
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
	pid_t session = 0, pgrp = 0;
	unsigned long irq;
	if (process == NULL || process->controlling_tty != &console_tty)
		return;
	irq = spin_lock_irqsave(&console_tty.lock);
	process->controlling_tty = NULL;
	if (console_tty.session == process->pid) {
		session = console_tty.session;
		pgrp = console_tty.foreground_pgrp;
		console_tty.session = console_tty.foreground_pgrp = 0;
	}
	spin_unlock_irqrestore(&console_tty.lock, irq);
	if (session > 0 && pgrp > 0) {
		(void)process_signal_pgrp(session, pgrp, SIGHUP);
		(void)process_signal_pgrp(session, pgrp, SIGCONT);
	}
}
