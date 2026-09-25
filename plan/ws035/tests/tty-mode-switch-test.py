#!/usr/bin/env python3
"""Type-ahead across a line/character mode switch of the terminal (ws035-p043).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The functions are taken from src/kern/tty.c as they are and built against a
small stand-in for the terminal: the structure layout, the buffer sizes and
the control characters the functions read.  Input typed while a program
switches between canonical and non-canonical mode must reach it whole and in
order, whichever way the switch goes.
"""
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]


def function(text, name):
    """Returns one static function definition, from its return type to its end."""
    match = re.search(r"\nstatic [^\n]+\n" + re.escape(name) + r"\(\n", text)
    start = match.start() + 1
    end = text.index("\n}\n", start) + 3
    return text[start:end]


def main():
    text = (REPO / "src/kern/tty.c").read_text()
    constants = "\n".join(re.findall(r"^#define TTY_(?:LINE_MAX|RECORDS|INPUT_MAX|VDISABLE) .*$",
                                     text, re.M))
    record = text[text.index("struct tty_record {"):text.index("};", text.index("struct tty_record {")) + 2]
    source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char cc_t;
enum { VEOF, VEOL, VERASE, VKILL, NCCS = 20 };
#define ICANON 0x100U
struct termios { unsigned c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_cc[NCCS]; };
struct wait_queue { int unused; };
static unsigned wakes;
static void waitq_wake_all(struct wait_queue *q) { (void)q; wakes++; }
#define kern_memcpy memcpy
'''
    source += constants + "\n" + record + r'''
struct tty {
	struct wait_queue read_waitq;
	struct termios termios;
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
};
'''
    for name in ("tty_cc_matches", "tty_commit_locked", "tty_switch_mode_locked"):
        source += function(text, name) + "\n"
    source += r'''
static struct tty t;

/* Takes everything a non-canonical read would see. */
static size_t
drain(char *out)
{
	size_t n = 0;
	while (t.input_used != 0) {
		out[n++] = (char)t.input[t.input_tail];
		t.input_tail = (t.input_tail + 1U) % TTY_INPUT_MAX;
		t.input_used--;
	}
	out[n] = 0;
	return n;
}

static void
reset(unsigned lflag)
{
	memset(&t, 0, sizeof(t));
	memset(t.termios.c_cc, TTY_VDISABLE, sizeof(t.termios.c_cc));
	t.termios.c_cc[VEOF] = 4;
	t.termios.c_lflag = lflag;
}

static void
typed_canonical(const char *line_or_partial)
{
	/* What the discipline leaves: a finished line is a record, the rest is edit. */
	const char *p = line_or_partial;
	for (; *p; p++) {
		t.edit[t.edit_used++] = (uint8_t)*p;
		if (*p == '\n')
			tty_commit_locked(&t, 0);
	}
}

static void
typed_raw(const char *s)
{
	for (; *s; s++) {
		t.input[t.input_head] = (uint8_t)*s;
		t.input_head = (t.input_head + 1U) % TTY_INPUT_MAX;
		t.input_used++;
	}
}

static void
set_lflag(unsigned lflag)
{
	unsigned old = t.termios.c_lflag;
	t.termios.c_lflag = lflag;
	tty_switch_mode_locked(&t, old);
}

int
main(void)
{
	char out[1024];

	/* A shell's editor turns canonical mode off with a line and a half typed. */
	reset(ICANON);
	typed_canonical("echo one\nls /d");
	set_lflag(0);
	assert(t.record_used == 0 && t.edit_used == 0);
	typed_raw("ev\n");
	drain(out);
	assert(strcmp(out, "echo one\nls /dev\n") == 0);

	/* A record partly read already gives only its unread rest. */
	reset(ICANON);
	typed_canonical("abcdef\n");
	t.records[t.record_tail].offset = 3;
	set_lflag(0);
	drain(out);
	assert(strcmp(out, "def\n") == 0);

	/* Going back to canonical mode, pending characters become lines. */
	reset(0);
	typed_raw("first\nsecond\nthi");
	set_lflag(ICANON);
	assert(t.input_used == 0 && t.record_used == 2 && t.edit_used == 3);
	assert(t.records[0].length == 6 && memcmp(t.records[0].data, "first\n", 6) == 0);
	assert(t.records[1].length == 7 && memcmp(t.records[1].data, "second\n", 7) == 0);
	assert(memcmp(t.edit, "thi", 3) == 0);

	/* VEOL ends a line too. */
	reset(0);
	t.termios.c_cc[VEOL] = ';';
	typed_raw("a;b");
	set_lflag(ICANON);
	assert(t.record_used == 1 && t.records[0].length == 2 && t.edit_used == 1);

	/* A setting that keeps the mode moves nothing. */
	reset(ICANON);
	typed_canonical("keep");
	set_lflag(ICANON | 0x8U);
	assert(t.edit_used == 4 && t.input_used == 0);

	/* The character queue never overflows: the excess is dropped. */
	reset(ICANON);
	for (unsigned i = 0; i < TTY_RECORDS; i++)
		typed_canonical("0123456789012345678901234567890123456789012345678901234567890123\n");
	set_lflag(0);
	assert(t.input_used == TTY_INPUT_MAX && t.record_used == 0);

	printf("tty-mode-switch-test: PASS\n");
	return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "test.c"
        path.write_text(source)
        binary = Path(directory) / "test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
