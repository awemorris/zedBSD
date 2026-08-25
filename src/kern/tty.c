/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
	unsigned record_head, record_tail, record_used;
	uint8_t input[TTY_INPUT_MAX];
	unsigned input_head, input_tail, input_used;
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

static struct tty console_ttys[TTY_VT_COUNT];
static struct spinlock console_output_lock;
static unsigned active_vt;
static unsigned console_escape_state[TTY_VT_COUNT];
static unsigned console_escape_parameter[TTY_VT_COUNT];
static unsigned console_escape_has_parameter[TTY_VT_COUNT];
static char vt_history[TTY_VT_COUNT][TTY_VT_HISTORY];
static size_t vt_history_used[TTY_VT_COUNT];

static void
tty_flush_input_locked(struct tty *tty)
{
	tty->edit_used = 0;
	tty->record_head = tty->record_tail = tty->record_used = 0;
	tty->input_head = tty->input_tail = tty->input_used = 0;
	tty->literal_next = 0;
}

static void
tty_default_termios(struct termios *termios)
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

int
tty_console_init(void)
{
	memset(console_ttys, 0, sizeof(console_ttys));
	memset(vt_history_used, 0, sizeof(vt_history_used));
	active_vt = 0;
	spin_init(&console_output_lock, LOCK_RANK_TTY, "console output");
	for (unsigned i = 0; i < TTY_VT_COUNT; i++) {
		spin_init(&console_ttys[i].lock, LOCK_RANK_TTY, "virtual tty");
		waitq_init(&console_ttys[i].read_waitq, "virtual tty input");
		waitq_init(&console_ttys[i].write_waitq, "virtual tty output");
		tty_default_termios(&console_ttys[i].termios);
		console_ttys[i].winsize.ws_row = HAL_CONS_ROWS;
		console_ttys[i].winsize.ws_col = HAL_CONS_COLUMNS;
		console_ttys[i].association_generation = 1;
	}
	return 0;
}

static void
tty_console_csi(unsigned vt, unsigned command)
{
	/* Minimal ANSI cursor/erase baseline shared by every HAL console. */
	struct hal_cons_state state;
	unsigned amount = console_escape_has_parameter[vt] ?
		console_escape_parameter[vt] : 1U;

	hal_cons_save_state(&state);
	switch (command) {
	case 'H':
		(void)hal_cons_set_cursor(0U, 0U);
		break;
	case 'J':
		if (amount == 2U) {
			unsigned row;
			for (row = 0; row < HAL_CONS_ROWS; row++)
				hal_cons_clear_row(row);
			(void)hal_cons_set_cursor(0U, 0U);
		}
		break;
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
		} else if (!console_escape_has_parameter[vt] || amount == 0U) {
			hal_cons_clear_to_eol();
		}
		break;
	default:
		break;
	}
}

static void
tty_render(unsigned vt, const char *bytes, size_t length)
{
	size_t index = 0;

	if (length == 0)
		return;
	while (index < length) {
		if (console_escape_state[vt] == 0U) {
			size_t start = index;
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
			if (bytes[index++] == '[') {
				console_escape_state[vt] = 2U;
				console_escape_parameter[vt] = 0U;
				console_escape_has_parameter[vt] = 0U;
			} else {
				static const char escape = '\033';
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
		tty_console_csi(vt, (unsigned char)bytes[index++]);
		console_escape_state[vt] = 0U;
	}
}

static void
tty_echo(struct tty *tty, const char *bytes, size_t length)
{
	unsigned vt = (unsigned)(tty - console_ttys);
	unsigned long irq;
	if (vt >= TTY_VT_COUNT || length == 0) return;
	irq = spin_lock_irqsave(&console_output_lock);
	if (length >= TTY_VT_HISTORY) {
		bytes += length - TTY_VT_HISTORY;
		length = TTY_VT_HISTORY;
		vt_history_used[vt] = 0;
	} else if (vt_history_used[vt] + length > TTY_VT_HISTORY) {
		size_t drop = vt_history_used[vt] + length - TTY_VT_HISTORY;
		memmove(vt_history[vt], vt_history[vt] + drop,
		    vt_history_used[vt] - drop);
		vt_history_used[vt] -= drop;
	}
	memcpy(vt_history[vt] + vt_history_used[vt], bytes, length);
	vt_history_used[vt] += length;
	if (vt == active_vt) tty_render(vt, bytes, length);
	spin_unlock_irqrestore(&console_output_lock, irq);
}

unsigned tty_vt_count(void) { return TTY_VT_COUNT; }
unsigned tty_vt_active(void) { return active_vt; }
int tty_vt_activate(unsigned vt)
{
	unsigned long irq;
	if (vt >= TTY_VT_COUNT) return EINVAL;
	irq = spin_lock_irqsave(&console_output_lock);
	active_vt = vt;
	console_escape_state[vt] = 0;
	hal_cons_clear();
	tty_render(vt, vt_history[vt], vt_history_used[vt]);
	hal_cons_update_cursor();
	spin_unlock_irqrestore(&console_output_lock, irq);
	return 0;
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

static int
tty_cc_matches(const struct tty *tty, unsigned index, uint8_t byte)
{
	return tty->termios.c_cc[index] != TTY_VDISABLE &&
	    byte == tty->termios.c_cc[index];
}

static void
tty_echo_append(struct tty_input_result *result, uint8_t byte)
{
	if (result->echo_length < sizeof(result->echo))
		result->echo[result->echo_length++] = byte;
}

static void
tty_echo_character(const struct tty *tty, struct tty_input_result *result,
	uint8_t byte)
{
	if ((tty->termios.c_lflag & ECHOCTL) != 0 &&
	    ((byte < 0x20U && byte != '\n' && byte != '\t') || byte == 0x7fU)) {
		tty_echo_append(result, '^');
		tty_echo_append(result, byte == 0x7fU ? '?' : (uint8_t)(byte + '@'));
	} else {
		tty_echo_append(result, byte);
	}
}

static void
tty_echo_erase(const struct tty *tty, struct tty_input_result *result,
	uint8_t byte)
{
	unsigned width = (tty->termios.c_lflag & ECHOCTL) != 0 &&
	    ((byte < 0x20U && byte != '\n' && byte != '\t') || byte == 0x7fU) ?
	    2U : 1U;

	while (width-- != 0) {
		tty_echo_append(result, '\b');
		tty_echo_append(result, ' ');
		tty_echo_append(result, '\b');
	}
}

/*
 * Common console/PTY line discipline.  Transport-specific code only has to
 * deliver result->echo after dropping tty->lock.
 */
static void
tty_input_byte_locked(struct tty *tty, uint8_t byte,
	struct tty_input_result *result)
{
	unsigned lflag;
	int quoted = 0;

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
	/* VLNEXT quotes every special character interpreted by the line
	 * discipline, including VSTOP/VSTART.  Resolve it before IXON so a quoted
	 * flow-control byte reaches the readable input stream. */
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

	/* Software output flow control consumes unquoted VSTOP/VSTART as
	 * line-control characters.  VLNEXT-quoted bytes remain ordinary input. */
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

	if ((lflag & ICANON) != 0) {
		if (!quoted && tty_cc_matches(tty, VERASE, byte)) {
			if (tty->edit_used != 0) {
				uint8_t erased = tty->edit[--tty->edit_used];
				if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
					tty_echo_erase(tty, result, erased);
			}
		} else if (!quoted && tty_cc_matches(tty, VKILL, byte)) {
			tty->edit_used = 0;
			if ((lflag & ECHOK) != 0)
				tty_echo_append(result, '\n');
		} else if (!quoted && (lflag & IEXTEN) != 0 &&
		    tty_cc_matches(tty, VWERASE, byte)) {
			while (tty->edit_used != 0 &&
			    (tty->edit[tty->edit_used - 1U] == ' ' ||
			     tty->edit[tty->edit_used - 1U] == '\t')) {
				uint8_t erased = tty->edit[--tty->edit_used];
				if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
					tty_echo_erase(tty, result, erased);
			}
			while (tty->edit_used != 0 &&
			    tty->edit[tty->edit_used - 1U] != ' ' &&
			    tty->edit[tty->edit_used - 1U] != '\t') {
				uint8_t erased = tty->edit[--tty->edit_used];
				if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
					tty_echo_erase(tty, result, erased);
			}
		} else if (!quoted && (lflag & IEXTEN) != 0 &&
		    tty_cc_matches(tty, VREPRINT, byte)) {
			if ((lflag & ECHO) != 0) {
				tty_echo_append(result, '\n');
				for (size_t i = 0; i < tty->edit_used; i++)
					tty_echo_character(tty, result, tty->edit[i]);
			}
		} else if (!quoted && tty_cc_matches(tty, VEOF, byte)) {
			tty_commit_locked(tty, 1);
		} else if (!quoted &&
		    (byte == '\n' || tty_cc_matches(tty, VEOL, byte))) {
			if (tty->edit_used < TTY_LINE_MAX)
				tty->edit[tty->edit_used++] = byte;
			tty_commit_locked(tty, 0);
			if ((lflag & (ECHO | ECHONL)) != 0)
				tty_echo_append(result, byte);
		} else {
			if (tty->edit_used < TTY_LINE_MAX) {
				tty->edit[tty->edit_used++] = byte;
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

void
tty_console_input_event(uint32_t event)
{
	struct tty *tty;
	struct tty_input_result result;
	unsigned key = event & INPUT_KEY_MASK;
	unsigned long irq;
	uint8_t byte;
	const char *sequence = NULL;
	size_t sequence_length = 0;
	if ((event & INPUT_KEY_GRAPH) != 0 &&
	    key >= INPUT_KEY_F1 && key <= INPUT_KEY_F4) {
		(void)tty_vt_activate(key - INPUT_KEY_F1);
		return;
	}
	tty = &console_ttys[active_vt];

	/*
	 * Expose machine-independent ANSI key sequences to noncanonical readers.
	 * Canonical input keeps its historical behavior and ignores navigation.
	 */
	switch (key) {
	case INPUT_KEY_UP: sequence = "\033[A"; sequence_length = 3; break;
	case INPUT_KEY_DOWN: sequence = "\033[B"; sequence_length = 3; break;
	case INPUT_KEY_RIGHT: sequence = "\033[C"; sequence_length = 3; break;
	case INPUT_KEY_LEFT: sequence = "\033[D"; sequence_length = 3; break;
	case INPUT_KEY_HOME: sequence = "\033[H"; sequence_length = 3; break;
	case INPUT_KEY_END: sequence = "\033[F"; sequence_length = 3; break;
	case INPUT_KEY_INSERT: sequence = "\033[2~"; sequence_length = 4; break;
	case INPUT_KEY_DELETE: sequence = "\033[3~"; sequence_length = 4; break;
	case INPUT_KEY_PAGE_UP: sequence = "\033[5~"; sequence_length = 4; break;
	case INPUT_KEY_PAGE_DOWN: sequence = "\033[6~"; sequence_length = 4; break;
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

	irq = spin_lock_irqsave(&tty->lock);
	tty_input_byte_locked(tty, byte, &result);
	spin_unlock_irqrestore(&tty->lock, irq);
	if (result.notify)
		poll_notify();
	if (!result.output_stopped)
		tty_echo(tty, (const char *)result.echo, result.echo_length);
	if (result.signal_number != 0 && result.signal_session > 0 &&
	    result.signal_pgrp > 0)
		(void)process_signal_pgrp(result.signal_session,
		    result.signal_pgrp, result.signal_number);
}

static int
tty_process_controls(struct tty *tty, struct process *process)
{
	uint64_t generation;
	pid_t session;
	unsigned long irq;

	if (tty == NULL || process == NULL)
		return 0;
	irq = spin_lock_irqsave(&tty->lock);
	generation = tty->association_generation;
	session = tty->session;
	spin_unlock_irqrestore(&tty->lock, irq);
	return session == process->session &&
	    process_controlling_tty_matches(process, tty, generation);
}

static void
tty_advance_association_locked(struct tty *tty)
{
	if (++tty->association_generation == 0)
		tty->association_generation = 1;
}

static int
tty_assign_controlling(struct tty *tty, struct process *process)
{
	struct tty *existing;
	uint64_t existing_generation, generation;
	unsigned long irq;
	int claimed = 0, error;

	if (tty == NULL || process == NULL || process->session != process->pid)
		return EPERM;
	if (process_controlling_tty_snapshot(process, &existing,
	    &existing_generation) != 0)
		return EINVAL;
	if (existing != NULL) {
		irq = spin_lock_irqsave(&tty->lock);
		generation = tty->association_generation;
		claimed = tty->session == process->session;
		spin_unlock_irqrestore(&tty->lock, irq);
		return existing == tty && existing_generation == generation &&
		    claimed ? 0 : EPERM;
	}
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
	error = process_controlling_tty_attach(process, tty, generation);
	irq = spin_lock_irqsave(&tty->lock);
	if (tty->association_generation != generation ||
	    tty->session != process->session)
		error = EBUSY;
	if (error != 0 && claimed && tty->association_generation == generation &&
	    tty->session == process->session) {
		tty->session = tty->foreground_pgrp = 0;
		tty_advance_association_locked(tty);
	}
	spin_unlock_irqrestore(&tty->lock, irq);
	if (error != 0)
		process_controlling_tty_detach_one(process, tty, generation);
	return error;
}

static int
tty_background(struct tty *tty, struct process *process,
	enum tty_background_operation operation)
{
	pid_t session, foreground;
	unsigned lflag;
	unsigned long irq;
	int decision, signo;

	if (!tty_process_controls(tty, process))
		return 0;
	for (;;) {
		irq = spin_lock_irqsave(&tty->lock);
		session = tty->session;
		foreground = tty->foreground_pgrp;
		lflag = tty->termios.c_lflag;
		spin_unlock_irqrestore(&tty->lock, irq);
		if (process->session != session || process->pgrp == foreground ||
		    (operation == TTY_BACKGROUND_WRITE &&
		     (lflag & TOSTOP) == 0))
			return 0;
		signo = operation == TTY_BACKGROUND_READ ? SIGTTIN : SIGTTOU;
		decision = signal_job_control_decision(thread_current(), signo);
		if (decision == EIO)
			return operation == TTY_BACKGROUND_READ ? EIO : 0;
		if (process_pgrp_is_orphaned(process))
			return EIO;
		if (decision != 0) {
			(void)process_signal_pgrp(process->session, process->pgrp,
			    signo);
			return decision;
		}
		/* The default action consumes the signal by stopping this process.
		 * Other members of the group receive an ordinary generated signal;
		 * excluding self avoids leaving a duplicate pending stop after
		 * SIGCONT. */
		(void)process_signal_pgrp_except(process->session, process->pgrp,
		    signo, process);
		process_stop_current(signo);
		/* SIGCONT does not necessarily foreground the group. */
	}
}

static int
tty_wait_output_enabled(struct tty *tty, struct file *file)
{
	unsigned long irq = spin_lock_irqsave(&tty->lock);

	while (tty->output_stopped && !tty->hungup) {
		uint64_t sequence;
		int error;

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
	uint64_t interval = (uint64_t)deciseconds * (KERN_CLOCK_HZ / 10U);
	unsigned timed_input_used = 0;
	size_t count;
	int error;

	if (minimum > size) minimum = (unsigned)size;
	if (nonblocking) minimum = 0;
	if (deciseconds != 0 && minimum == 0) {
		error = syscall_restart_deadline_after(interval, &deadline);
		if (error != 0) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -error;
		}
	} else if (minimum != 0 && deciseconds != 0 &&
	    tty->input_used != 0 && curthread != NULL &&
	    curthread->syscall_stop_redispatch &&
	    curthread->syscall_wait_deadline_valid) {
		deadline = curthread->syscall_wait_deadline;
		timed_input_used = tty->input_used;
	}
	while (tty->input_used < minimum ||
	    (minimum == 0 && tty->input_used == 0 && deciseconds != 0)) {
		uint64_t sequence = waitq_sequence(&tty->read_waitq);
		if (tty->hungup) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return 0;
		}
		if (nonblocking) {
			spin_unlock_irqrestore(&tty->lock, irq);
			return -EAGAIN;
		}
		/* For MIN>0/TIME>0, TIME is an inter-byte timer.  Re-arm it
		 * whenever newly arrived input is observed, rather than measuring
		 * once from the first byte. */
		if (minimum != 0 && deciseconds != 0 && tty->input_used != 0 &&
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
tty_vt_read(unsigned vt, struct file *file, void *buffer, size_t size)
{
	struct tty *tty;
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	unsigned canonical;
	unsigned long irq;
	int error;

	if (buffer == NULL) return -EINVAL;
	if (size == 0) return 0;
	if (vt >= TTY_VT_COUNT) return -ENODEV;
	tty = &console_ttys[vt];
	error = tty_background(tty, process, TTY_BACKGROUND_READ);
	if (error != 0) return -error;
	irq = spin_lock_irqsave(&tty->lock);
	canonical = tty->termios.c_lflag & ICANON;
	spin_unlock_irqrestore(&tty->lock, irq);
	return canonical ? tty_read_canonical(tty, buffer, size,
	    (file_status_flags_get(file) & O_NONBLOCK) != 0) :
	    tty_read_noncanonical(tty, buffer, size,
	    (file_status_flags_get(file) & O_NONBLOCK) != 0);
}

ssize_t
tty_vt_write(unsigned vt, struct file *file, const void *buffer, size_t size)
{
	struct tty *tty;
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	const char *bytes = buffer;
	unsigned oflag;
	unsigned long irq;
	int error;
	(void)file;

	if (vt >= TTY_VT_COUNT) return -ENODEV;
	tty = &console_ttys[vt];
	error = tty_background(tty, process, TTY_BACKGROUND_WRITE);
	if (error != 0) return -error;
	if (buffer == NULL) return -EINVAL;
	error = tty_wait_output_enabled(tty, file);
	if (error != 0) return -error;
	irq = spin_lock_irqsave(&tty->lock);
	oflag = tty->termios.c_oflag;
	spin_unlock_irqrestore(&tty->lock, irq);
	if ((oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
		size_t start = 0;
		for (size_t i = 0; i < size; i++)
			if (bytes[i] == '\n') {
				if (i != start) tty_echo(tty, bytes + start, i - start);
				tty_echo(tty, "\r\n", 2);
				start = i + 1U;
			}
		if (start < size) tty_echo(tty, bytes + start, size - start);
	} else {
		tty_echo(tty, bytes, size);
	}
	return (ssize_t)size;
}

ssize_t tty_console_read(struct file *f, void *b, size_t n)
{ return tty_vt_read(0, f, b, n); }
ssize_t tty_console_write(struct file *f, const void *b, size_t n)
{ return tty_vt_write(0, f, b, n); }

static int
tty_termios_valid(const struct termios *value)
{
	return value != NULL && (value->c_cflag & CS8) != 0 &&
	    (value->c_ispeed == B0 || value->c_ispeed == B9600 ||
	     value->c_ispeed == B19200 || value->c_ispeed == B38400) &&
	    (value->c_ospeed == B0 || value->c_ospeed == B9600 ||
	     value->c_ospeed == B19200 || value->c_ospeed == B38400);
}

static int tty_backend_drain(struct tty *, struct file *);
static int tty_backend_flush_output(struct tty *, struct file *);
static int tty_backend_send_control(struct tty *, struct file *, uint8_t);
static void tty_backend_set_flow(struct tty *, unsigned);

static int
tty_ioctl_instance(struct tty *tty, struct file *file,
	unsigned long request, uintptr_t argument)
{
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	unsigned long irq;
	int error = 0;

	switch (request) {
	case TCGETS: {
		struct termios value;
		irq = spin_lock_irqsave(&tty->lock); value = tty->termios;
		spin_unlock_irqrestore(&tty->lock, irq);
		return copyout(&value, argument, sizeof(value));
	}
	case TCSETS: case TCSETSW: case TCSETSF: {
		struct termios value;
		int flow_resumed = 0;
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0) return error;
		error = copyin(argument, &value, sizeof(value));
		if (error != 0) return error;
		if (!tty_termios_valid(&value)) return EINVAL;
		if (request != TCSETS &&
		    (error = tty_backend_drain(tty, file)) != 0)
			return error;
		irq = spin_lock_irqsave(&tty->lock);
		if (request == TCSETSF) tty_flush_input_locked(tty);
		if ((value.c_iflag & IXON) == 0 && tty->output_stopped_by_ixon) {
			tty->output_stopped = 0;
			tty->output_stopped_by_ixon = 0;
			flow_resumed = 1;
		}
		tty->termios = value;
		waitq_wake_all(&tty->read_waitq);
		waitq_wake_all(&tty->write_waitq);
		spin_unlock_irqrestore(&tty->lock, irq);
		if (flow_resumed)
			tty_backend_set_flow(tty, 0);
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
		pid_t session, pgrp;
		int changed;
		error = copyin(argument, &value, sizeof(value));
		if (error != 0) return error;
		irq = spin_lock_irqsave(&tty->lock);
		changed = memcmp(&tty->winsize, &value, sizeof(value)) != 0;
		tty->winsize = value;
		session = tty->session;
		pgrp = tty->foreground_pgrp;
		spin_unlock_irqrestore(&tty->lock, irq);
#ifdef SIGWINCH
		if (changed && session > 0 && pgrp > 0)
			(void)process_signal_pgrp(session, pgrp, SIGWINCH);
#else
		(void)changed; (void)session; (void)pgrp;
#endif
		return 0;
	}
	case TIOCGPGRP: {
		pid_t value;
		if (!tty_process_controls(tty, process)) return ENOTTY;
		irq = spin_lock_irqsave(&tty->lock); value = tty->foreground_pgrp;
		spin_unlock_irqrestore(&tty->lock, irq);
		return copyout(&value, argument, sizeof(value));
	}
	case TIOCGSID: {
		pid_t value;
		if (!tty_process_controls(tty, process)) return ENOTTY;
		irq = spin_lock_irqsave(&tty->lock); value = tty->session;
		spin_unlock_irqrestore(&tty->lock, irq);
		return copyout(&value, argument, sizeof(value));
	}
	case TIOCSPGRP: {
		pid_t value;
		if (!tty_process_controls(tty, process)) return ENOTTY;
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0) return error;
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
		return tty_assign_controlling(tty, process);
	case TIOCNOTTY:
		if (!tty_process_controls(tty, process)) return ENOTTY;
		tty_detach_process(process); return 0;
	case TIOCFLUSH: {
		int queue;
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0) return error;
		error = copyin(argument, &queue, sizeof(queue));
		if (error != 0) return error;
		if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH)
			return EINVAL;
		if (queue != TCOFLUSH) {
			irq = spin_lock_irqsave(&tty->lock); tty_flush_input_locked(tty);
			spin_unlock_irqrestore(&tty->lock, irq); poll_notify();
		}
		if (queue != TCIFLUSH &&
		    (error = tty_backend_flush_output(tty, file)) != 0)
			return error;
		return 0;
	}
	case TCXONC: {
		int action;
		uint8_t character;
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0) return error;
		error = copyin(argument, &action, sizeof(action));
		if (error != 0) return error;
		if (action != TCOOFF && action != TCOON &&
		    action != TCIOFF && action != TCION)
			return EINVAL;
		if (action == TCOOFF || action == TCOON) {
			irq = spin_lock_irqsave(&tty->lock);
			if (tty->hungup)
				error = EIO;
			else {
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
		character = tty->termios.c_cc[action == TCIOFF ? VSTOP : VSTART];
		spin_unlock_irqrestore(&tty->lock, irq);
		return character == TTY_VDISABLE ? 0 :
		    tty_backend_send_control(tty, file, character);
	}
	case TIOCDRAIN:
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		return error != 0 ? error : tty_backend_drain(tty, file);
	case TCSBRK: {
		int duration;
		error = tty_background(tty, process, TTY_BACKGROUND_CONTROL);
		if (error != 0) return error;
		error = copyin(argument, &duration, sizeof(duration));
		if (error != 0) return error;
		if (duration < 0) return EINVAL;
		error = tty_backend_drain(tty, file);
		if (error != 0) return error;
		/* Virtual terminals have no serial break line to assert. */
		return 0;
	}
	default: return EOPNOTSUPP;
	}
}

int
tty_console_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	return tty_vt_ioctl(0, file, request, argument);
}

int tty_vt_ioctl(unsigned vt, struct file *file, unsigned long request,
	uintptr_t argument)
{
	return vt < TTY_VT_COUNT ?
	    tty_ioctl_instance(&console_ttys[vt], file, request, argument) : ENODEV;
}

int
tty_console_poll(struct file *file, short events, short *revents)
{
	return tty_vt_poll(0, file, events, revents);
}

int tty_vt_poll(unsigned vt, struct file *file, short events, short *revents)
{
	short result = 0;
	struct tty *tty;
	unsigned long irq;
	(void)file;
	if (vt >= TTY_VT_COUNT) return ENODEV;
	tty = &console_ttys[vt];
	if (revents == NULL) return EINVAL;
	irq = spin_lock_irqsave(&tty->lock);
	if ((tty->termios.c_lflag & ICANON) != 0 ?
	    tty->record_used != 0 : tty->input_used != 0)
		result |= events & (POLLIN | POLLRDNORM);
	if (!tty->output_stopped)
		result |= events & (POLLOUT | POLLWRNORM);
	spin_unlock_irqrestore(&tty->lock, irq);
	*revents = result;
	return 0;
}

void
tty_attach_console(struct process *process)
{
	if (process == NULL || process == &process0)
		return;
	(void)tty_assign_controlling(&console_ttys[0], process);
}

void
tty_detach_process(struct process *process)
{
	struct tty *tty;
	uint64_t generation;
	pid_t session = 0, pgrp = 0;
	unsigned long irq;
	if (process == NULL || process_controlling_tty_snapshot(process, &tty,
	    &generation) != 0 || tty == NULL)
		return;
	irq = spin_lock_irqsave(&tty->lock);
	if (tty->association_generation != generation) {
		spin_unlock_irqrestore(&tty->lock, irq);
		process_controlling_tty_detach_one(process, tty, generation);
		return;
	}
	if (tty->session == process->pid && process->session == process->pid) {
		session = tty->session;
		pgrp = tty->foreground_pgrp;
		tty->session = tty->foreground_pgrp = 0;
		tty_advance_association_locked(tty);
	}
	spin_unlock_irqrestore(&tty->lock, irq);
	if (session != 0)
		process_controlling_tty_detach_session(session, tty, generation);
	else
		process_controlling_tty_detach_one(process, tty, generation);
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
	unsigned slave_output_stopped;
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

static struct pty_pair *
tty_backend_pair(struct tty *tty)
{
	unsigned index;

	for (index = 0; index < PTY_MAX; index++)
		if (&pty_pairs[index].slave == tty)
			return &pty_pairs[index];
	return NULL;
}

static void
tty_backend_set_flow(struct tty *tty, unsigned stopped)
{
	struct pty_pair *pair = tty_backend_pair(tty);
	unsigned long irq;

	if (pair == NULL)
		return;
	irq = spin_lock_irqsave(&pair->lock);
	pair->slave_output_stopped = stopped != 0;
	waitq_wake_all(&pair->output_waitq);
	spin_unlock_irqrestore(&pair->lock, irq);
}

static int
tty_backend_drain(struct tty *tty, struct file *file)
{
	struct pty_pair *pair = tty_backend_pair(tty);
	unsigned long irq;
	(void)file;

	/* Console writes reach the HAL synchronously and have no queued bytes. */
	if (pair == NULL)
		return 0;
	irq = spin_lock_irqsave(&pair->lock);
	while (pair->active && pair->master_open && pair->output_used != 0) {
		uint64_t sequence = waitq_sequence(&pair->output_waitq);
		int error = waitq_sleep(&pair->output_waitq, &pair->lock, sequence,
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

static int
tty_backend_flush_output(struct tty *tty, struct file *file)
{
	struct pty_pair *pair = tty_backend_pair(tty);
	unsigned long irq;
	(void)file;

	if (pair == NULL)
		return 0;
	irq = spin_lock_irqsave(&pair->lock);
	if (!pair->active || !pair->master_open) {
		spin_unlock_irqrestore(&pair->lock, irq);
		return EIO;
	}
	pair->output_head = pair->output_tail = pair->output_used = 0;
	waitq_wake_all(&pair->output_waitq);
	spin_unlock_irqrestore(&pair->lock, irq);
	poll_notify();
	return 0;
}

static int
tty_backend_send_control(struct tty *tty, struct file *file, uint8_t byte)
{
	struct pty_pair *pair = tty_backend_pair(tty);
	ssize_t result;

	/* A virtual console has no peer serial line to receive flow characters. */
	if (pair == NULL)
		return 0;
	result = pty_output_bytes(pair, &byte, 1,
	    file != NULL && (file_status_flags_get(file) & O_NONBLOCK) != 0);
	return result == 1 ? 0 : (result < 0 ? (int)-result : EIO);
}

static void
pty_input_byte(struct pty_pair *pair, uint8_t byte)
{
	struct tty *tty = &pair->slave;
	struct tty_input_result result;
	unsigned long irq;
	size_t index;

	irq = spin_lock_irqsave(&tty->lock);
	tty_input_byte_locked(tty, byte, &result);
	spin_unlock_irqrestore(&tty->lock, irq);
	if (result.flow_changed)
		tty_backend_set_flow(tty, result.output_stopped);
	if (result.notify)
		poll_notify();
	for (index = 0; index < result.echo_length; index++) {
		if (result.echo[index] == '\n' &&
		    (result.output_flags & (OPOST | ONLCR)) == (OPOST | ONLCR))
			(void)pty_output_bytes(pair, (const uint8_t *)"\r\n", 2, 0);
		else
			(void)pty_output_bytes(pair, &result.echo[index], 1, 0);
	}
	if (result.signal_number != 0 && result.signal_session > 0 &&
	    result.signal_pgrp > 0)
		(void)process_signal_pgrp(result.signal_session,
		    result.signal_pgrp, result.signal_number);
}

static int
pty_master_open(struct file *file)
{
	struct pty_handle *handle;
	struct pty_pair *pair = NULL;
	pid_t old_session;
	uint64_t old_association_generation;
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
			pair->slave_output_stopped = 0;
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
	old_session = pair->slave.session;
	old_association_generation = pair->slave.association_generation;
	tty_advance_association_locked(&pair->slave);
	tty_flush_input_locked(&pair->slave);
	tty_default_termios(&pair->slave.termios);
	pair->slave.session = pair->slave.foreground_pgrp = 0;
	pair->slave.hungup = 0;
	pair->slave.output_stopped = 0;
	pair->slave.output_stopped_by_ixon = 0;
	pair->slave.literal_next = 0;
	spin_unlock_irqrestore(&pair->slave.lock, irq);
	if (old_session > 0)
		process_controlling_tty_detach_session(old_session, &pair->slave,
		    old_association_generation);
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
	uint64_t association_generation = 0;

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
	pair->slave.output_stopped = 0;
	pair->slave.output_stopped_by_ixon = 0;
	session = pair->slave.session;
	pgrp = pair->slave.foreground_pgrp;
	association_generation = pair->slave.association_generation;
	if (session > 0) {
		pair->slave.session = pair->slave.foreground_pgrp = 0;
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
	while (pty_handle_valid_locked(handle) &&
	    (pair->output_used == 0 || pair->slave_output_stopped) &&
	    (!pair->slave_ever_opened || pair->slave_opens != 0)) {
		uint64_t sequence;
		int error;
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
	if (process != NULL && (file_status_flags_get(file) & O_NOCTTY) == 0 &&
	    process->session == process->pid)
		(void)tty_assign_controlling(&pair->slave, process);
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
	error = tty_background(tty, process, TTY_BACKGROUND_READ);
	if (error != 0)
		return -error;
	irq = spin_lock_irqsave(&tty->lock);
	canonical = tty->termios.c_lflag & ICANON;
	spin_unlock_irqrestore(&tty->lock, irq);
	return canonical ? tty_read_canonical(tty, buffer, size,
	    (file_status_flags_get(file) & O_NONBLOCK) != 0) :
	    tty_read_noncanonical(tty, buffer, size,
	    (file_status_flags_get(file) & O_NONBLOCK) != 0);
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
	error = tty_background(tty, process, TTY_BACKGROUND_WRITE);
	if (error != 0)
		return -error;
	error = tty_wait_output_enabled(tty, file);
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
		    (file_status_flags_get(file) & O_NONBLOCK) != 0);
		if (written != (ssize_t)output_length) {
			/* A transformed byte is emitted atomically for nonblocking
			 * files; blocking writes complete it before returning. */
			if (written > 0 &&
			    (file_status_flags_get(file) & O_NONBLOCK) == 0)
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
	unsigned output_stopped;
	unsigned long irq;
	if (handle == NULL || revents == NULL)
		return EINVAL;
	pair = handle->pair;
	tty = &pair->slave;
	irq = spin_lock_irqsave(&tty->lock);
	if ((tty->termios.c_lflag & ICANON) != 0 ?
	    tty->record_used != 0 : tty->input_used != 0)
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
		waitq_init(&pair->slave.write_waitq, "pty slave output flow");
		tty_default_termios(&pair->slave.termios);
		pair->slave.winsize.ws_row = HAL_CONS_ROWS;
		pair->slave.winsize.ws_col = HAL_CONS_COLUMNS;
		pair->slave.association_generation = 1;
	}
	return cdev_register("ptmx", 0x00010001U, &pty_ptmx_ops, NULL);
}

#ifdef ZEDBSD_TTY_TEST
int
tty_test_vlnext_ixon(void)
{
	struct tty tty;
	struct tty_input_result result;
	uint8_t stop;

	memset(&tty, 0, sizeof(tty));
	tty_default_termios(&tty.termios);
	stop = tty.termios.c_cc[VSTOP];
	tty_input_byte_locked(&tty, tty.termios.c_cc[VLNEXT], &result);
	if (!tty.literal_next || tty.edit_used != 0)
		return 0;
	tty_input_byte_locked(&tty, stop, &result);
	return !tty.literal_next && !tty.output_stopped && tty.edit_used == 1 &&
	    tty.edit[0] == stop;
}
#endif
