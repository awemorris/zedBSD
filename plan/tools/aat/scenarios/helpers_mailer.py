#!/usr/bin/env python3
"""The automatic helpers of tests/scenarios/apps/mailer/ beyond open-from-home (WS169 p004, p005; ws177-p014).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_mailer.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

Mail needs a server: plan/tools/mail/fake-mail-server.py runs on this host for the scenario, with a CA and a
certificate made here.  The target reaches this host at the address its SSH connection comes from ($SSH_CLIENT:
10.0.2.2 for a QEMU guest, whose user network reaches this host's 127.0.0.1; the LAN address for the 5330), so the
server listens there and the account names its servers by that address (zedBSD's resolver does not read /etc/hosts,
so a name would need a DNS server); the certificate is for the name mail.test and that address, and the target gets
the CA in /tmp/aat-work, which Mail trusts through OpenSSL's SSL_CERT_FILE.  The servers are given with ports that are not the
TLS ones, so Mail uses STARTTLS (the fake server's IMAP and submission ports).  Kei's Mail accounts are removed on the
target before and after.

The places in Mail's window are view.c's layout at the size MAIL READY gives (the window is started outside App Home,
whose lines give no size): the sidebar 220 wide, the cards 10 apart on glass (MAIL GLASS see_through=1), the form of
a new account 560 wide in the middle of what is right of the sidebar, its fields 42 apart from 112, Sign In at 347.
"""
import re
import subprocess
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_mailer")

SERVER = aatlib.ROOT / "plan/tools/mail/fake-mail-server.py"
NAME = "mail.test"
CA = f"{aatlib.WORK}/mail-ca.pem"
PASSWORD = "secret 1"

# view.c's layout (see the docstring).
SIDEBAR = 220
GAP = 10
FORM_WIDTH = 560
FIELD_TOP = 112
FIELD_STEP = 42
FIELD_LABEL = 120
SIGN_IN_Y = 347
SWITCH_Y = 408
LIST_HEADER = 104
ROW = 78


class Server:
	"""The fake server on this host for one scenario, and the target set to reach it."""

	def __init__(self, item, arrivals: int):
		self.process = None
		folder = run.outdir / "mail-server"
		folder.mkdir(parents=True, exist_ok=True)
		self.folder = folder
		# The address the target reaches this host at.
		_, client = run.sh("echo $SSH_CLIENT", root=False)
		address = client.split()[0] if client.split() else ""
		item.check(address, "the target does not say where its SSH connection comes from")
		bind = "127.0.0.1" if address.startswith("10.0.2.") else address
		self.host = address
		# The CA and the certificate for mail.test and the address, valid from a day ago: the target's clock can be
		# behind this host's (T1-320: the guest at 02:05, the certificate from 02:06, and TLS refused it).
		since = time.strftime("%Y%m%d%H%M%SZ", time.gmtime(time.time() - 86400))
		for words in (
			["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-not_before", since,
			 "-subj", "/CN=aat-mail-ca",
			 "-keyout", str(folder / "ca.key"), "-out", str(folder / "ca.pem"),
			 "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign"],
			["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={NAME}",
			 "-keyout", str(folder / "server.key"), "-out", str(folder / "server.csr")],
		):
			made = subprocess.run(words, capture_output=True, text=True, timeout=60)
			item.check(made.returncode == 0, f"openssl: {made.stderr.strip()[-200:]}")
		(folder / "server.ext").write_text(f"subjectAltName=DNS:{NAME},IP:{address}\nbasicConstraints=CA:FALSE\n")
		made = subprocess.run(
			["openssl", "x509", "-req", "-in", str(folder / "server.csr"), "-CA", str(folder / "ca.pem"),
			 "-CAkey", str(folder / "ca.key"), "-CAcreateserial", "-days", "2", "-not_before", since,
			 "-extfile", str(folder / "server.ext"),
			 "-out", str(folder / "server.pem")], capture_output=True, text=True, timeout=60)
		item.check(made.returncode == 0, f"openssl x509: {made.stderr.strip()[-200:]}")
		# The server, its ports.
		self.process = subprocess.Popen(
			[sys.executable, str(SERVER), str(folder / "server.pem"), str(folder / "server.key"), str(folder),
			 "--bind", bind, "--arrivals", str(arrivals)], stdout=subprocess.PIPE, text=True)
		line = self.process.stdout.readline()
		match = re.match(r"PORTS (\d+) (\d+) (\d+) (\d+)", line)
		item.check(match, f"the fake server did not start: {line!r}")
		self.imap = int(match.group(2))
		self.submission = int(match.group(4))
		item.step(f"fake mail server on {bind}", f"IMAP {self.imap}, submission {self.submission}")
		# The target: the CA, no account of an earlier run.
		run.aat("put", str(folder / "ca.pem"), CA)
		run.sh(f"chmod 644 {CA}")
		forget_accounts()

	def received(self) -> list:
		"""The messages the server got by SMTP (their files)."""
		return sorted(self.folder.glob("smtp-*.eml"))

	def stop(self) -> None:
		if self.process is not None:
			self.process.terminate()
			self.process.wait(timeout=10)
		forget_accounts()


def forget_accounts() -> None:
	"""Removes kei's Mail accounts on the target (its mailer.conf and the passwords' file)."""
	run.sh("rm -f /home/kei/.config/keiland/mailer.conf /home/kei/.config/keiland/mailer-accounts")


def open_mail(item):
	"""Mail started as kei with the tests' CA; returns its window with the size MAIL READY gives, and the glass."""
	mark = run.mark()
	window = run.open_as_user(item, f"env SSL_CERT_FILE={CA} /bin/mailer")
	ready = run.wait(r"MAIL READY width=\d+ height=\d+", mark, 15)
	item.check(ready, "Mail did not say MAIL READY")
	glass = run.wait(r"MAIL GLASS see_through=\d", mark, 5)
	if not window.sized():
		width, height = (int(value) for value in re.search(r"width=(\d+) height=(\d+)", ready).groups())
		window = aatlib.Window(window.client, window.surface, window.x, window.y, width, height, window.docked)
	gap = GAP if glass and glass.endswith("=1") else 0
	return window, gap


def field_point(browser):
	"""The page's field in Browser (below "Sign-in code:" at the body's top left): a click there gives it the keyboard
	(a click elsewhere in the page takes the keyboard from it, and the code is then typed into nothing, T1-299)."""
	return browser.x + 80, browser.y + 60


def form_place(window, gap):
	"""The left and the width of the form of a new account."""
	left = SIDEBAR + gap
	area = window.width - left
	width = min(area - 48, FORM_WIDTH)
	return window.x + left + (area - width) // 2, width


def sign_in(item, window, gap, server, codes: bool):
	"""Fills the form (the name, the address, the password, the two servers), the codes' switch when asked, Sign In."""
	x, width = form_place(window, gap)
	values = ["Kei Example", "kei@example.net", PASSWORD, f"{server.host}:{server.imap}", f"{server.host}:{server.submission}"]
	for index, value in enumerate(values):
		run.click(x + FIELD_LABEL + (width - FIELD_LABEL) // 2, window.y + FIELD_TOP + 16 + index * FIELD_STEP)
		time.sleep(0.2)
		run.type(value)
	if codes:
		mark = run.mark()
		run.click(x + width - 30, window.y + SWITCH_Y)
		allowed = run.wait(r"MAIL CODES allowed=\d", mark, 5)
		if allowed and allowed.endswith("=0"):
			run.click(x + width - 30, window.y + SWITCH_Y)
			allowed = run.wait(r"MAIL CODES allowed=1", mark, 5)
		item.step("Sign-in codes switched on", allowed or "")
		item.check(allowed and allowed.endswith("=1"), "the sign-in codes' switch did not turn on")
	run.shot(item, "form")
	mark = run.mark()
	run.click(x + width - 55, window.y + SIGN_IN_Y)
	signed = run.wait(r"MAIL SIGNED-IN account=0", mark, 30)
	refreshed = run.wait(r"MAIL REFRESHED account=0", mark, 30)
	failed = run.lines(r"MAIL FAILED", mark)
	item.step("Sign In", f"{signed}; {refreshed}; {failed[-1] if failed else ''}")
	item.check(signed and refreshed, f"the account did not sign in and get its mail ({failed[-1] if failed else 'no line'})")
	return mark


@run.define("apps.mailer.read-compose")
def read_compose(item):
	server = Server(item, 0)
	try:
		window, gap = open_mail(item)
		mark = sign_in(item, window, gap, server, codes=False)
		inbox = run.lines(r"MAIL MESSAGE account=0 folder=Inbox", mark)
		item.check(len(inbox) >= 3, f"the inbox has {len(inbox)} messages, not 3")
		time.sleep(0.5)
		run.shot(item, "inbox")
		# The first row (the newest) opened.
		mark = run.mark()
		run.click(window.x + SIDEBAR + gap + 170, window.y + LIST_HEADER + ROW // 2)
		opened = run.wait(r"MAIL OPEN message=\d+", mark, 10)
		item.step("clicked the first message", opened or "")
		run.shot(item, "message")
		item.check(opened, "no MAIL OPEN")
		# A new message to ben, sent.
		mark = run.mark()
		run.key("ctrl+n")
		compose = run.wait(r"MAIL COMPOSE kind=new", mark, 10)
		item.check(compose, "no MAIL COMPOSE kind=new")
		reader = SIDEBAR + gap + 340 + gap
		run.click(window.x + reader + 300, window.y + 82)
		run.type("ben@example.com")
		run.click(window.x + reader + 300, window.y + 162)
		run.type("Hello from AAT")
		run.click(window.x + reader + 300, window.y + 320)
		run.type("A test message.")
		run.shot(item, "compose")
		mark = run.mark()
		run.click(window.x + window.width - 56, window.y + 28)
		sent = run.wait(r"MAIL SENT account=0", mark, 30)
		time.sleep(0.5)
		received = server.received()
		item.step("Send", f"{sent}; the server got {len(received)}")
		run.shot(item, "sent")
		item.check(sent and received, "the message was not sent")
		if received:
			text = received[-1].read_text(errors="replace")
			item.check("Subject: Hello from AAT" in text and "A test message." in text, "the server got another message")
		item.person("the inbox's three messages, the bank's message with its code, and the message written in the screenshots")
	finally:
		server.stop()


@run.define("apps.mailer.sign-in-code")
def sign_in_code(item):
	server = Server(item, 1)
	try:
		# The page with a field that has the keyboard, in Browser.
		page = run.outdir / "mail-server" / "code.html"
		page.write_text("<!doctype html><title>Code</title><p>Sign-in code:</p>"
			"<input id=c autofocus style=\"font-size:24px;width:300px;height:40px\" "
			"oninput=\"console.log('code-length=' + this.value.length)\">\n")
		run.aat("put", str(page), f"{aatlib.WORK}/code.html")
		run.sh(f"chmod 644 {aatlib.WORK}/code.html")
		mark = run.mark()
		browser = run.open_as_user(item, f"/bin/browser {aatlib.WORK}/code.html")
		listening = run.wait(r"ZBROWSER MAIL listen error=0", mark, 15)
		item.step("Browser opened the page", listening or "")
		item.check(listening, "Browser does not listen to the mail")
		run.click(*field_point(browser))
		# Mail with the codes allowed: its idling inbox gets a message with a code.
		window, gap = open_mail(item)
		mark = sign_in(item, window, gap, server, codes=True)
		arrived = run.wait(r"MAIL MESSAGE account=0 folder=Inbox uid=\d+ arrived=1 code=1", mark, 30)
		told = run.wait(r"KWL MAIL arrived client=\d+ account=\d+ from=\d+ subject=\d+ code=4 told=1", mark, 15)
		offered = run.wait(r"ZBROWSER MAIL code length=4 titlebar=1", mark, 15)
		item.step("a message with a code came", f"{arrived}; {told}; {offered}")
		item.check(arrived and told and offered, "the code did not reach Browser")
		code_lines = [line for line in run.lines(r"(MAIL|KWL MAIL|ZBROWSER MAIL)", mark) if "7351" in line]
		item.check(not code_lines, "the code is in the log")
		# The notification's board (ws156-p003: the bottom middle, 76 high, 48 above the edge) clicked while it stays
		# (ws177-p014): the code goes into the page's field, which still has the page's focus behind Mail.
		shown = run.wait(r'KWL NOTIFY show id=\d+ client=\d+ urgent=\d title="Sign-in code from', mark, 10)
		item.step("the notification of the code showed", shown or "")
		item.check(shown, "the notification of the code did not show")
		if shown:
			number = re.search(r"id=(\d+)", shown).group(1)
			time.sleep(0.5)
			run.shot(item, "notification")
			width, height = run.screen()
			mark = run.mark()
			run.click(width // 2, height - 48 - 38)
			activated = run.wait(rf"KWL NOTIFY activate id={number}$", mark, 5)
			filled = run.wait(r"ZBROWSER MAIL fill length=4 error=0", mark, 5)
			typed = run.wait(r"ZBROWSER CONSOLE level=\d+ code-length=4", mark, 5)
			removed = run.wait(r"ZBROWSER TITLEBAR code=0", mark, 5)
			item.step("clicked the notification", f"{activated}; {filled}; {typed}; {removed}")
			item.check(activated and filled and typed and removed, "the notification's click did not fill the field in")
		# Browser in front: the field shows the code, and the titlebar has no code's control any more.
		run.close(item, window)
		run.click(*field_point(browser))
		time.sleep(0.8)
		run.shot(item, "filled")
		item.person("the field shows 7351 and Browser's titlebar has no 'Code' control")
	finally:
		server.stop()


sys.exit(run.go(before=common.before(run), after=common.after(run)))
