#!/usr/bin/env python3
"""The automatic helper of tests/scenarios/apps/settings/printers.md (WS145 p002 to p004).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_printers.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

The printers are plan/ws145/tests/mock-printers.py on this host (an IPP printer at /ipp/print named "Mock Printer",
and an LPD queue), listening where the target reaches this host ($SSH_CLIENT: 10.0.2.2 for a QEMU guest, whose user
network reaches this host's 127.0.0.1; the LAN address for the 5330).  The target's printtest adds them through the
desktop and prints the runner's sample.pdf to each; the documents the mock got must be the sample.  Kei's printers
file is removed before and after.
"""
import hashlib
import re
import subprocess
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_printers")

MOCK = aatlib.ROOT / "plan/ws145/tests/mock-printers.py"
CONFIG = "/home/kei/.config/keiland/printers.conf"
SAMPLE = f"{aatlib.SAMPLES}/sample.pdf"


def forget() -> None:
	run.sh(f"rm -f {CONFIG} {CONFIG}.lock")


@run.define("apps.settings.printers")
def printers(item):
	folder = run.outdir / "printers"
	folder.mkdir(parents=True, exist_ok=True)
	_, client = run.sh("echo $SSH_CLIENT", root=False)
	address = client.split()[0] if client.split() else ""
	item.check(address, "the target does not say where its SSH connection comes from")
	bind = "127.0.0.1" if address.startswith("10.0.2.") else address
	mock = subprocess.Popen([sys.executable, str(MOCK), str(folder), "--bind", bind], stdout=subprocess.PIPE, text=True)
	try:
		match = re.match(r"PORTS (\d+) (\d+)", mock.stdout.readline())
		item.check(match, "the mock printers did not start")
		ipp, lpd = int(match.group(1)), int(match.group(2))
		item.step(f"mock printers on {bind}", f"IPP {ipp}, LPD {lpd}")
		forget()
		# The two printers, added through the desktop: the IPP one names itself.
		_, out = run.as_user(f"/bin/printtest add ipp {address} {ipp}", wait=True)
		item.step("printtest add ipp", out.strip().splitlines()[-1] if out.strip() else "")
		item.check("PRINTTEST open printers=1" in out, "the desktop does not offer printers")
		item.check("PRINTTEST result error=0" in out, "the IPP printer was not added")
		_, out = run.as_user(f"/bin/printtest add lpd {address} {lpd} raw", wait=True)
		item.check("PRINTTEST result error=0" in out, "the LPD printer was not added")
		time.sleep(3.0)
		_, listed = run.as_user("/bin/printtest list", wait=True)
		item.step("printtest list", "; ".join(line for line in listed.splitlines() if "printer id=" in line))
		# The ids and the job numbers go on from what the desktop had before (an earlier scenario's printers, T1-320).
		ipp_line = re.search(r"printer id=(\d+) protocol=1 .* default=1 name=Mock Printer", listed)
		lpd_line = re.search(r"printer id=(\d+) protocol=2 ", listed)
		item.check(ipp_line, "the IPP printer is not the default named Mock Printer")
		item.check(lpd_line, "the LPD printer is not listed")
		# A PDF to each.
		_, out = run.as_user(f"/bin/printtest print {SAMPLE} 'AAT page'", wait=True, timeout=90)
		item.step("printtest print (the default, IPP)", "; ".join(out.strip().splitlines()[-3:]))
		item.check(re.search(r"PRINTTEST done job=\d+ state=4", out), "the IPP job was not done")
		_, out = run.as_user(f"/bin/printtest print --printer={lpd_line.group(1)} {SAMPLE} 'AAT LPD'", wait=True, timeout=90)
		item.step(f"printtest print --printer={lpd_line.group(1)} (LPD)", "; ".join(out.strip().splitlines()[-3:]))
		item.check(re.search(r"PRINTTEST done job=\d+ state=4", out), "the LPD job was not done")
		# The documents the mock got are the sample.
		run.aat("get", SAMPLE, str(folder / "sample.pdf"))
		want = hashlib.sha256((folder / "sample.pdf").read_bytes()).hexdigest()
		for name in ("ipp-1.pdf", "lpd-1.data"):
			path = folder / name
			got = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "none"
			item.check(got == want, f"{name} is not the sample ({got[:12]})")
		item.step("the documents the printers got", "both are the sample")
		# PDF Viewer's File > Print (Ctrl+P) on the default printer.
		mark = run.mark()
		viewer = run.open_as_user(item, f"/bin/pdfviewer {SAMPLE}")
		# A window started outside App Home has only its place in the compositor's lines: its size is PDF Viewer's own.
		ready = run.wait(r"PDFVIEWER READY width=\d+ height=\d+", mark, 10)
		if viewer is not None and ready and not viewer.sized():
			width, height = (int(value) for value in re.search(r"width=(\d+) height=(\d+)", ready).groups())
			viewer = aatlib.Window(viewer.client, viewer.surface, viewer.x, viewer.y, width, height, viewer.docked)
		time.sleep(1.0)
		run.click(*viewer.middle())
		run.key("ctrl+p")
		asked = run.wait(r"PDFVIEWER PRINT asked error=0", mark, 10)
		printed = run.wait(r"PDFVIEWER PRINT job=\d+ state=4", mark, 60)
		item.step("PDF Viewer, Ctrl+P", f"{asked}; {printed}")
		run.shot(item, "pdfviewer")
		item.check(asked and printed, "PDF Viewer did not print")
		path = folder / "ipp-2.pdf"
		got = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "none"
		item.check(got == want, f"PDF Viewer's document is not the sample ({got[:12]})")
		run.close(item, viewer)
		# A printer's name and queue changed (ws177-p025, the printers' edit since manager version 24).
		_, out = run.as_user(f"/bin/printtest edit {lpd_line.group(1)} Basement raw2", wait=True)
		item.step(f"printtest edit {lpd_line.group(1)} Basement raw2", "; ".join(line for line in out.splitlines() if "result" in line or "printer id=" in line))
		item.check(re.search(r"PRINTTEST result error=0", out), "the edit was not taken")
		item.check(re.search(rf"printer id={lpd_line.group(1)} protocol=2 .*path=raw2 default=0 name=Basement$", out, re.M),
			"the LPD printer is not named Basement with the queue raw2")
		# The Settings page.
		window, _ = run.settings(item, "printers")
		run.shot(item, "page")
		run.close(item, window)
		item.person("the Printers page: Mock Printer (Default) and Basement (LPD, raw2), each with Edit, the form, and the three "
			"jobs Done; PDF Viewer's message Printed")
	finally:
		mock.terminate()
		forget()


sys.exit(run.go(before=common.before(run), after=common.after(run)))
