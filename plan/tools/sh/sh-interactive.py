#!/usr/bin/env python3
# ws042: the interactive shell on the guest's serial console: syntax errors,
# PS2 continuation, Ctrl-C on a partial line, aliases, here-documents, fc,
# job control (&, jobs, kill %1, Ctrl-Z, bg, fg) and line editing (cursor
# keys, backspace, history) in emacs and vi mode.
# Usage: sh-interactive.py SOCKET
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import re, sys, time
sys.path.insert(0, "plan/tools/guest")
import serial
c = serial.Console(sys.argv[1], 30.0)
serial.login(c, "root")
def step(name, send, pattern, raw=False, timeout=10.0):
    c.forget()
    if raw: c.socket.sendall(send)
    else: c.send(send)
    try:
        c.read_until(re.compile(pattern), timeout=timeout); ok = True
    except SystemExit:
        ok = False
    print(("ok " if ok else "FAIL ") + name + ": " + repr(c.pending.replace("\r", "")[-200:]))
    time.sleep(0.3)
    return ok
r = []
r.append(step("syntax error keeps shell", "if then fi", r"syntax error"))
r.append(step("shell alive after error", "echo alive-$((2+3))", r"alive-5"))
r.append(step("multi-line if with PS2", "if true", r"> "))
r.append(step("continuation completes", "then echo multi; fi", r"multi"))
r.append(step("partial line then Ctrl-C", b"echo partial", r"partial", raw=True))
r.append(step("Ctrl-C abandons line", b"\x03", r"[#$] ", raw=True))
r.append(step("shell alive after Ctrl-C", "echo after-ctrlc", r"after-ctrlc"))
r.append(step("exit status of false", "false; echo st=$?", r"st=1"))
# An alias takes effect from the next line on (XCU 2.3.1), as in dash.
r.append(step("alias defined", "alias ll='echo LL'", r"[#$] "))
r.append(step("alias in a function", "f() { ll x; }; f", r"LL x"))
r.append(step("here-doc interactive", "cat <<EOF", r"> "))
r.append(step("here-doc body", "body-line", r"> "))
r.append(step("here-doc end", "EOF", r"body-line"))
r.append(step("fc -l", "fc -l -3", r"cat <<EOF"))
# Job control: a background job, then a foreground one stopped and resumed.
r.append(step("background job", "sleep 30 &", r"\[1\] \d+"))
r.append(step("jobs lists it", "jobs", r"\[1\].*Running.*sleep 30"))
# wait with no operand is 0 (XCU wait); wait %1 gives the job's status.
r.append(step("kill %1 and wait %1", "kill %1; wait %1; echo killed-$?", r"killed-143"))
r.append(step("foreground job", "sleep 30", r"sleep 30\r?\n"))
r.append(step("Ctrl-Z stops it", b"\x1a", r"Stopped", raw=True))
r.append(step("jobs shows it stopped", "jobs", r"\[1\].*Stopped.*sleep 30"))
r.append(step("bg resumes it", "bg", r"\[1\].*sleep 30"))
r.append(step("jobs shows it running", "jobs", r"\[1\].*Running.*sleep 30"))
r.append(step("fg brings it back", "fg", r"sleep 30"))
r.append(step("Ctrl-C ends it", b"\x03", r"[#$] ", raw=True))
r.append(step("shell alive after fg", "echo after-fg", r"after-fg"))
# Line editing: cursor keys and insertion, backspace, history.
r.append(step("left arrow and insert", b"echo abc\x1b[D\x1b[DX\r", r"\naXbc\r?\n", raw=True))
r.append(step("backspace", b"echo abd\x7fc\r", r"\nabc\r?\n", raw=True))
r.append(step("history: a line", "echo hist-one", r"\nhist-one\r?\n"))
r.append(step("history: up arrow", b"\x1b[A\r", r"\nhist-one\r?\n", raw=True))
# vi mode (set -o vi, XCU sh Command Line Editing (vi-mode)).
r.append(step("vi: set -o vi", "set -o vi; echo vi-on", r"\nvi-on\r?\n"))
r.append(step("vi: a line", "echo vi-one", r"\nvi-one\r?\n"))
r.append(step("vi: k recalls it", b"\x1bk\r", r"\nvi-one\r?\n", raw=True))
r.append(step("vi: b dw i", b"echo foo bar\x1b0wdwibaz\r", r"\nbazbar\r?\n", raw=True))
r.append(step("vi: 2w cw", b"echo w1 w2 w3\x1b02wcwNEW\x1b\r", r"\nw1 NEW w3\r?\n", raw=True))
r.append(step("vi: x u", b"echo undo\x1bxu\r", r"\nundo\r?\n", raw=True))
r.append(step("vi: 3X", b"echo 12345\x1b3X\r", r"\n15\r?\n", raw=True))
r.append(step("vi: ~ r", b"echo hello\x1b0w~~\x1b$rO\r", r"\nHEllO\r?\n", raw=True))
r.append(step("vi: yw P", b"echo p1\x1b0yw$P\r", r"\npecho 1\r?\n", raw=True))
r.append(step("vi: dd and insert", b"echo xyz\x1bddiecho new\r", r"\nnew\r?\n", raw=True))
r.append(step("vi: set +o vi", "set +o vi; set -o | grep '^vi'", r"vi\s+off"))
r.append(step("emacs again: Ctrl-A", b"cho emacs-back\x01e\r", r"\nemacs-back\r?\n", raw=True))
print("INTERACTIVE %d/%d" % (sum(r), len(r)))
sys.exit(0 if all(r) else 1)
