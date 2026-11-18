/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Traps and signals (POSIX XCU 2.11 and the trap builtin).
 *
 * A trapped signal only marks itself pending; the action runs at the next
 * point where the shell is between commands.  The shell keeps, for every
 * signal, why it is handled the way it is: a signal ignored when the shell
 * started stays ignored and cannot be trapped, and one the interactive shell
 * ignores for its own sake is given back to the commands it runs.
 */

#include "userland/base/sh/shell.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* The number of signal numbers the shell keeps track of. */
#define TRAP_SIGNALS 65

/* How a signal is handled, and why (signal_modes). */
#define MODE_DEFAULT		0	/* the system's default */
#define MODE_INHERITED		1	/* ignored when the shell started */
#define MODE_SHELL		2	/* ignored or caught by the shell itself */
#define MODE_TRAPPED		3	/* caught for a trap's action */
#define MODE_TRAP_IGNORED	4	/* ignored by trap '' */

/* A signal's name, as the trap and kill builtins write it. */
struct signal_name {
	const char *name;
	int number;
};

/* The signal names; kill -l lists them in the order of their numbers. */
static const struct signal_name signal_names[] = {
	{ "HUP", SIGHUP },
	{ "INT", SIGINT },
	{ "QUIT", SIGQUIT },
	{ "ILL", SIGILL },
	{ "TRAP", SIGTRAP },
	{ "ABRT", SIGABRT },
	{ "BUS", SIGBUS },
	{ "FPE", SIGFPE },
	{ "KILL", SIGKILL },
	{ "USR1", SIGUSR1 },
	{ "SEGV", SIGSEGV },
	{ "USR2", SIGUSR2 },
	{ "PIPE", SIGPIPE },
	{ "ALRM", SIGALRM },
	{ "TERM", SIGTERM },
	{ "CHLD", SIGCHLD },
	{ "CONT", SIGCONT },
	{ "STOP", SIGSTOP },
	{ "TSTP", SIGTSTP },
	{ "TTIN", SIGTTIN },
	{ "TTOU", SIGTTOU },
	{ "URG", SIGURG },
	{ "XCPU", SIGXCPU },
	{ "XFSZ", SIGXFSZ },
	{ "VTALRM", SIGVTALRM },
	{ "PROF", SIGPROF },
	{ "WINCH", SIGWINCH },
#ifdef SIGIO
	{ "IO", SIGIO },
#endif
#ifdef SIGPWR
	{ "PWR", SIGPWR },
#endif
	{ "SYS", SIGSYS },
	{ NULL, 0 }
};

/*
 * The action of each trap: NULL for none, "" to ignore.  Index 0 is EXIT.
 * Set by the trap builtin, cleared in a subshell for every trap that is not
 * an ignore.
 */
static char *trap_actions[TRAP_SIGNALS];

/* How each signal is handled at the moment (MODE_*), kept with the system's. */
static int signal_modes[TRAP_SIGNALS];

/*
 * Set by the handler for each signal whose trap has not run yet; cleared
 * when its action runs, or in a subshell.
 */
static volatile sig_atomic_t trap_pending[TRAP_SIGNALS];

/* Set when any trap is pending, so the evaluator checks one flag. */
volatile int sh_trap_pending_any;

/* Set while a trap's action runs, so that another does not start inside it. */
int sh_in_trap;

/* The last signal that marked a trap pending, which wait reports. */
volatile int sh_trap_last_signal;

/* Set when an interrupt arrived that no trap catches (interactive shell). */
volatile int sh_interrupted;

/* Set while the EXIT trap runs, so that an exit in it ends the shell. */
static int exit_trap_running;

static void trap_handler(int number);
static void interrupt_handler(int number);
static void set_disposition(int number, void (*handler)(int));
static void reset_disposition(int number);
static int parse_signal(const char *text);
static void print_traps(void);
static void shell_mode(int number, void (*handler)(int));

/*
 * Records how each signal was handled when the shell started.
 */
void
sh_trap_init(
	void)
{
	struct sigaction current;
	int number;
	int queried;

	/* A signal ignored on entry stays ignored for good. */
	for (number = 1; number < TRAP_SIGNALS; number++) {
		queried = sigaction(number, NULL, &current);
		if (queried != 0)
			continue;
		if (current.sa_handler == SIG_IGN)
			signal_modes[number] = MODE_INHERITED;
	}
}

/*
 * Sets the signals an interactive shell handles for itself: it ignores
 * QUIT and TERM, catches INT to abandon the command line, and with job
 * control ignores the stops that come from the terminal.
 */
void
sh_signals_for_interactive(
	int job_control)
{
	/* An interrupt abandons what is being typed. */
	shell_mode(SIGINT, interrupt_handler);

	/* QUIT and TERM do not end an interactive shell. */
	shell_mode(SIGQUIT, SIG_IGN);
	shell_mode(SIGTERM, SIG_IGN);

	/* With job control, the terminal cannot stop the shell. */
	if (!job_control)
		return;
	shell_mode(SIGTSTP, SIG_IGN);
	shell_mode(SIGTTIN, SIG_IGN);
	shell_mode(SIGTTOU, SIG_IGN);
}

/*
 * Sets the signals of a child the shell forks: what the shell handles for
 * itself goes back to the default, and a command run in the background
 * without job control ignores the interrupts of the terminal.
 */
void
sh_signals_for_child(
	int background,
	int job_control)
{
	int number;

	/* The shell's own handling is not the child's. */
	for (number = 1; number < TRAP_SIGNALS; number++) {
		if (signal_modes[number] != MODE_SHELL)
			continue;
		set_disposition(number, SIG_DFL);
		signal_modes[number] = MODE_DEFAULT;
	}

	/* A background command is not interrupted from the terminal. */
	if (!background || job_control)
		return;
	if (signal_modes[SIGINT] == MODE_DEFAULT ||
	    signal_modes[SIGINT] == MODE_TRAPPED) {
		set_disposition(SIGINT, SIG_IGN);
		signal_modes[SIGINT] = MODE_TRAP_IGNORED;
	}

	/* SIGQUIT likewise. */
	if (signal_modes[SIGQUIT] == MODE_DEFAULT ||
	    signal_modes[SIGQUIT] == MODE_TRAPPED) {
		set_disposition(SIGQUIT, SIG_IGN);
		signal_modes[SIGQUIT] = MODE_TRAP_IGNORED;
	}
}

/*
 * Sets the signals of a command the shell is about to replace itself with:
 * what the shell ignored for itself goes back to the default (a caught
 * signal goes back by itself across exec).
 */
void
sh_signals_for_exec(
	void)
{
	int number;

	/* Only what the shell ignored for itself needs putting back. */
	for (number = 1; number < TRAP_SIGNALS; number++) {
		if (signal_modes[number] == MODE_SHELL)
			set_disposition(number, SIG_DFL);
	}
}

/*
 * Gives the signals the shell ignores or catches for itself, which a
 * command it starts gets at their default (posix_spawn's SETSIGDEF, as
 * sh_signals_for_child does in a forked child).
 */
void
sh_signals_shell_set(
	sigset_t *set)
{
	int number;

	/* Each signal of the shell's own handling. */
	sigemptyset(set);
	for (number = 1; number < TRAP_SIGNALS; number++) {
		if (signal_modes[number] == MODE_SHELL)
			(void)sigaddset(set, number);
	}
}

/*
 * Resets the traps in a subshell: a trap that runs an action goes back to
 * the default; one that ignores stays.
 */
void
sh_trap_reset_subshell(
	void)
{
	int number;

	/* Drops each action, and the pending marks with them. */
	for (number = 0; number < TRAP_SIGNALS; number++) {
		trap_pending[number] = 0;
		if (trap_actions[number] == NULL ||
		    trap_actions[number][0] == '\0')
			continue;
		free(trap_actions[number]);
		trap_actions[number] = NULL;
		if (number > 0 && signal_modes[number] == MODE_TRAPPED) {
			set_disposition(number, SIG_DFL);
			signal_modes[number] = MODE_DEFAULT;
		}
	}

	/* Nothing is pending, and the EXIT trap has not run in the subshell. */
	sh_trap_pending_any = 0;
	exit_trap_running = 0;
}

/*
 * Reports whether any trap is set, which keeps the shell from replacing
 * itself with its last command: the trap must still be able to run.
 */
int
sh_trap_any_set(
	void)
{
	int number;

	/* Any action at all counts. */
	for (number = 0; number < TRAP_SIGNALS; number++) {
		if (trap_actions[number] != NULL &&
		    trap_actions[number][0] != '\0')
			return 1;
	}

	/* No trap runs an action. */
	return 0;
}

/*
 * Reports whether a signal is ignored (so a child inherits the ignore).
 */
int
sh_trap_ignored(
	int number)
{
	/* A number out of range is not ignored. */
	if (number <= 0 || number >= TRAP_SIGNALS)
		return 0;

	/* Ignored on entry, or by trap ''. */
	if (signal_modes[number] == MODE_INHERITED)
		return 1;
	if (signal_modes[number] == MODE_TRAP_IGNORED)
		return 1;

	/* Not ignored. */
	return 0;
}

/*
 * Runs the actions of the traps whose signals have arrived.  $? is the same
 * after the actions as before them.
 */
void
sh_trap_run_pending(
	void)
{
	char *action;
	int number;
	int saved_status;
	int saved_skip;

	/* Not inside an action already. */
	if (sh_in_trap)
		return;
	sh_trap_pending_any = 0;

	/* Runs each pending action, in signal order. */
	for (number = 1; number < TRAP_SIGNALS; number++) {
		if (!trap_pending[number])
			continue;
		trap_pending[number] = 0;
		if (trap_actions[number] == NULL ||
		    trap_actions[number][0] == '\0')
			continue;

		/* The action runs with $? kept, and a break in it stands. */
		action = sh_strdup(trap_actions[number]);
		saved_status = sh_status;
		saved_skip = sh_skip;
		sh_skip = SH_SKIP_NONE;
		sh_in_trap = 1;
		(void)sh_eval_string(action, 0);
		sh_in_trap = 0;
		free(action);
		if (sh_skip == SH_SKIP_NONE)
			sh_skip = saved_skip;
		sh_status = saved_status;
	}
}

/*
 * Runs the EXIT trap, once.
 */
void
sh_trap_run_exit(
	void)
{
	char *action;

	/* Nothing to run, or already running. */
	if (exit_trap_running)
		return;
	if (trap_actions[0] == NULL || trap_actions[0][0] == '\0')
		return;

	/* The trap is taken out before it runs, so it runs only once. */
	exit_trap_running = 1;
	action = trap_actions[0];
	trap_actions[0] = NULL;
	(void)sh_eval_string(action, 0);
	free(action);
}

/*
 * Ends the shell: runs the EXIT trap, with $? the status the shell ends
 * with, then exits.  An exit inside the EXIT trap ends the shell at once.
 */
void
sh_exit(
	int status)
{
	struct sh_handler handler;
	int kind;

	/* The EXIT trap sees the status in $?; an error in it ends it. */
	if (!exit_trap_running && trap_actions[0] != NULL &&
	    trap_actions[0][0] != '\0') {
		sh_status = status;
		sh_handler_push(&handler);
		kind = setjmp(handler.environment);
		if (kind == 0)
			sh_trap_run_exit();
		sh_handler_pop(&handler);
		sh_handler = NULL;
	}

	/* Ends the process, with the buffered output written. */
	fflush(NULL);
	exit(status & 0xff);
}

/*
 * Implements the trap builtin.
 */
int
sh_trap_builtin(
	int argc,
	char **argv)
{
	const char *action;
	const char *word;
	int index;
	int number;
	int status;
	int set;

	/* -- ends the options. */
	index = 1;
	if (index < argc) {
		word = argv[index];
		if (word[0] == '-' && word[1] == '-' && word[2] == '\0')
			index++;
	}

	/* With no operands, or -p alone, lists the traps. */
	if (index >= argc) {
		print_traps();
		return 0;
	}

	/* -p alone lists them too. */
	word = argv[index];
	if (word[0] == '-' && word[1] == 'p' && word[2] == '\0' &&
	    index + 1 == argc) {
		print_traps();
		return 0;
	}

	/* Any other option is an error. */
	if (word[0] == '-' && word[1] != '\0')
		sh_error("trap: Illegal option %s", word);

	/*
	 * A lone operand, or a first operand that is a number, makes every
	 * operand a condition to reset; otherwise the first is the action.
	 */
	action = "-";
	if (index + 1 < argc && !(word[0] >= '0' && word[0] <= '9')) {
		action = word;
		index++;
	}

	/* Sets each condition; a bad one is reported and skipped. */
	status = 0;
	for (; index < argc; index++) {
		number = parse_signal(argv[index]);
		if (number < 0) {
			fprintf(stderr, "trap: %s: bad trap\n", argv[index]);
			status = 1;
			continue;
		}

		/* The condition takes the action. */
		set = sh_trap_set(number, action);
		if (!set)
			status = 1;
	}

	/* Succeeded: 0 unless a condition was bad. */
	return status;
}

/*
 * Sets the trap of one condition: "-" for the default, "" to ignore, or an
 * action.  Returns 0 when the signal may not be trapped.
 */
int
sh_trap_set(
	int number,
	const char *action)
{
	int reset;

	/* A signal ignored when a non-interactive shell started stays so. */
	if (number > 0 && signal_modes[number] == MODE_INHERITED &&
	    !sh_option[SH_OPT_INTERACTIVE])
		return 1;

	/* Replaces the action; "-" leaves none. */
	reset = action[0] == '-' && action[1] == '\0';
	free(trap_actions[number]);
	trap_actions[number] = NULL;
	if (!reset)
		trap_actions[number] = sh_strdup(action);

	/* EXIT has no signal to set. */
	if (number == 0)
		return 1;

	/* Makes the signal do what the trap says. */
	if (reset) {
		reset_disposition(number);
	} else if (action[0] == '\0') {
		set_disposition(number, SIG_IGN);
		signal_modes[number] = MODE_TRAP_IGNORED;
	} else {
		set_disposition(number, trap_handler);
		signal_modes[number] = MODE_TRAPPED;
	}

	/* Succeeded: the trap is set. */
	return 1;
}

/*
 * Returns a signal's number for its name (in any case, without SIG) or its
 * number; EXIT is 0.  Returns -1 for anything else.
 */
int
sh_trap_signal_number(
	const char *text)
{
	int number;

	/* The parser of trap conditions. */
	number = parse_signal(text);

	/* Succeeded: the number, or -1. */
	return number;
}

/*
 * Returns a signal's name without SIG, or NULL.
 */
const char *
sh_trap_signal_name(
	int number)
{
	int index;

	/* Looks the number up. */
	for (index = 0; signal_names[index].name != NULL; index++) {
		if (signal_names[index].number == number)
			return signal_names[index].name;
	}

	/* Not a signal the shell names. */
	return NULL;
}

/*
 * Prints the signals in the order of their numbers, one per line (kill -l):
 * a name where the signal has one, the number where it has none.
 */
void
sh_trap_list_signals(
	void)
{
	const char *name;
	int highest;
	int index;
	int number;

	/* The highest signal with a name ends the list. */
	highest = 0;
	for (index = 0; signal_names[index].name != NULL; index++) {
		if (signal_names[index].number > highest)
			highest = signal_names[index].number;
	}

	/* 0, then each number up to it. */
	printf("0\n");
	for (number = 1; number <= highest; number++) {
		name = sh_trap_signal_name(number);
		if (name != NULL)
			printf("%s\n", name);
		else
			printf("%d\n", number);
	}
}

/* Marks a trapped signal pending. */
static void
trap_handler(
	int number)
{
	/* The action runs later, between commands. */
	if (number <= 0 || number >= TRAP_SIGNALS)
		return;
	trap_pending[number] = 1;
	sh_trap_pending_any = 1;
	sh_trap_last_signal = number;
}

/* Marks an interrupt of an interactive shell. */
static void
interrupt_handler(
	int number)
{
	/* The command line is abandoned where it is safe to. */
	(void)number;
	sh_interrupted = 1;
}

/* Sets how a signal is handled, without restarting interrupted calls. */
static void
set_disposition(
	int number,
	void (*handler)(int))
{
	struct sigaction action;

	/* A trapped signal interrupts a wait, so the wait builtin can return. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	(void)sigaction(number, &action, NULL);
}

/*
 * Puts a signal back to what it was before a trap: the default, or the
 * interactive shell's own handling of an interrupt.
 */
static void
reset_disposition(
	int number)
{
	/* What the shell handles itself, or ignored on entry, stays. */
	if (signal_modes[number] == MODE_SHELL ||
	    signal_modes[number] == MODE_INHERITED)
		return;

	/* The default, except the interrupt of an interactive shell. */
	set_disposition(number, SIG_DFL);
	signal_modes[number] = MODE_DEFAULT;
	if (number == SIGINT && sh_option[SH_OPT_INTERACTIVE]) {
		set_disposition(SIGINT, interrupt_handler);
		signal_modes[number] = MODE_SHELL;
	}
}

/* Makes the shell handle a signal for itself, unless something else does. */
static void
shell_mode(
	int number,
	void (*handler)(int))
{
	/* Only a signal at its default is taken. */
	if (signal_modes[number] != MODE_DEFAULT)
		return;
	set_disposition(number, handler);
	signal_modes[number] = MODE_SHELL;
}

/*
 * Parses a trap condition or a signal: EXIT, a name in any case (without
 * SIG, as dash has it), or a number.
 */
static int
parse_signal(
	const char *text)
{
	char *end;
	long value;
	int index;
	int compare;

	/* A number names the signal of that number; 0 is EXIT. */
	if (text[0] >= '0' && text[0] <= '9') {
		value = strtol(text, &end, 10);
		if (*end != '\0' || value < 0 || value >= TRAP_SIGNALS)
			return -1;
		return (int)value;
	}

	/* EXIT, then the names, in any case. */
	compare = strcasecmp(text, "EXIT");
	if (compare == 0)
		return 0;
	for (index = 0; signal_names[index].name != NULL; index++) {
		compare = strcasecmp(signal_names[index].name, text);
		if (compare == 0)
			return signal_names[index].number;
	}

	/* Not a condition. */
	return -1;
}

/* Prints the traps as trap commands that would set them again. */
static void
print_traps(
	void)
{
	const char *name;
	int number;

	/* EXIT first, then the signals in number order. */
	for (number = 0; number < TRAP_SIGNALS; number++) {
		if (trap_actions[number] == NULL)
			continue;
		if (number == 0)
			name = "EXIT";
		else
			name = sh_trap_signal_name(number);
		if (name == NULL)
			continue;
		fputs("trap -- ", stdout);
		sh_print_quoted_always(trap_actions[number]);
		printf(" %s\n", name);
	}
}
