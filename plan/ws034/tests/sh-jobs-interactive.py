#!/usr/bin/env python3
# ws034-p047: the shell's job control on the guest's serial console, where
# the shell is interactive and owns the terminal (Ctrl-Z, fg, bg, reports
# at the prompt).  Usage: sh-jobs-interactive.py SOCKET
import re
import sys
import time

sys.path.insert(0, "plan/tools/guest")
import serial  # noqa: E402

console = serial.Console(sys.argv[1], 30.0)
serial.login(console, "root")
results = []


def step(name, send, pattern, raw=False, timeout=15.0):
	console.forget()
	if raw:
		console.socket.sendall(send)
	else:
		console.send(send)
	try:
		console.read_until(re.compile(pattern), timeout=timeout)
		ok = True
	except SystemExit:
		ok = False
	text = console.pending.replace("\r", "")
	results.append(ok)
	print(f"{'ok' if ok else 'FAIL'} {name}: {text.strip()[-160:]!r}")
	time.sleep(0.3)


step("start sleep 300", "sleep 300", r"sleep 300", timeout=5.0)
time.sleep(1.0)
# ws042-p003: the shell reports jobs in the POSIX format of the jobs builtin,
# "[%d] %c %s %s" (number, current mark, state, command), and bg as
# "[%d] %s" with the command.
step("Ctrl-Z stops it", b"\x1a", r"\[1\]\s*\+\s+Stopped\s+sleep 300", raw=True)
step("jobs shows Stopped", "jobs", r"\[1\]\s*\+\s+Stopped")
step("bg continues it", "bg", r"\[1\] sleep 300 &")
step("jobs shows Running", "jobs", r"\[1\]\s*\+\s+Running")
step("second job", "sleep 1 &", r"\[2\] \d+")
time.sleep(2.0)
step("Done reported at the prompt", "", r"\[2\]\s*[+ -]?\s+Done")
step("fg %1 then Ctrl-C", "fg %1", r"sleep|\S", timeout=3.0)
time.sleep(1.0)
step("Ctrl-C ends the foreground job", b"\x03", r"[#$] ", raw=True)
step("table empty", "jobs; echo END", r"END")
ok = "[1]" not in console.pending.split("END")[0].split("jobs;")[-1]
results.append(ok)
print(f"{'ok' if ok else 'FAIL'} no job left")
print(f"SHJOBS {sum(results)}/{len(results)}")
sys.exit(0 if all(results) else 1)
