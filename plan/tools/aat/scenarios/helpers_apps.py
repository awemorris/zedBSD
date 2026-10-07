#!/usr/bin/env python3
"""The automatic helpers of tests/scenarios/apps/ (WS173 p004): each does its scenario's steps by the same id.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_apps.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

apps.files.mount-usb needs a person's hands and has no helper.  The samples
(sample.png, .jpg, .pdf, .mp4) are put in /tmp/aat-samples by samples.py
first (run-aat.sh does).
"""
import re
import statistics
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_apps")

# The scenario's directory under apps/ for each application App Home lists.
DIRECTORIES = {
	"files": "Files", "notes": "Notes", "settings": "Settings", "terminal": "Terminal", "pdfviewer": "PDF Viewer",
	"imageview": "Image Viewer", "videoplayer": "Video Player", "music": "Music", "photos": "Photos", "phone": "Phone",
	"calendar": "Calendar", "mailer": "Mail", "textedit": "Text Editor", "monitor": "System Monitor", "browser": "Browser",
}


def open_from_home(name: str):
	"""The helper of apps.<app>.open-from-home: App Home, the icon, the window, its close button."""
	def helper(item):
		window = run.launch(item, name)
		run.shot(item, "open")
		run.close(item, window)
		item.passed(f"window {window.client}:{window.surface} {window.width}x{window.height}")
	return helper


for directory, application in DIRECTORIES.items():
	run.define(f"apps.{directory}.open-from-home")(open_from_home(application))


def terminal(item):
	"""Terminal from App Home with the pointer in it."""
	window = run.launch(item, "Terminal")
	run.click(*window.middle())
	time.sleep(0.5)
	return window


@run.define("apps.terminal.type-command")
def terminal_type(item):
	terminal(item)
	run.type(f"echo aat-terminal > {aatlib.WORK}/terminal.txt")
	run.key("enter")
	time.sleep(1.0)
	_, text = run.sh(f"cat {aatlib.WORK}/terminal.txt")
	item.step("echo aat-terminal > /tmp/aat-work/terminal.txt", repr(text))
	run.shot(item, "terminal")
	item.check(text == "aat-terminal\n", f"the file is {text!r}")
	item.passed()


@run.define("apps.terminal.fullscreen-f11")
def terminal_f11(item):
	window = terminal(item)
	mark = run.mark()
	run.key("f11")
	# F11 is the Fullscreen menu item's shortcut: the compositor's menu chooses it (ZTERM MENU state ... fullscreen=1),
	# or Terminal takes the key itself on a desktop without those menus (BUG-194).
	on = run.wait(r"ZTERM FULLSCREEN key on=1|ZTERM MENU state .*fullscreen=1", mark, 10)
	hidden = run.wait(r"KWL GLASS bar hidden fullscreen=1", mark, 10)
	time.sleep(1.0)
	item.step("F11", f"{on}; {hidden}")
	run.shot(item, "fullscreen")
	item.check(on and hidden, "Terminal did not go full screen")
	mark = run.mark()
	run.key("f11")
	left = run.wait(rf"KWL GLASS fullscreen-leave surface={window.surface} via=f11 error=0|ZTERM FULLSCREEN key on=0", mark, 10)
	shown = run.wait(r"KWL GLASS bar shown", mark, 10)
	time.sleep(1.0)
	item.step("F11 again", f"{left}; {shown}")
	run.shot(item, "window")
	item.check(left and shown, "Terminal did not leave full screen (BUG-194)")
	mark = run.mark()
	run.key("f11")
	again = run.wait(r"KWL GLASS bar hidden fullscreen=1", mark, 10)
	time.sleep(0.8)
	run.key("super+down")
	down = run.wait(rf"KWL GLASS fullscreen-leave surface={window.surface} via=super-down error=0", mark, 10)
	shown = run.wait(r"KWL GLASS bar shown", mark, 10)
	time.sleep(1.0)
	item.step("F11, then Super+Down", f"{again}; {down}; {shown}")
	run.shot(item, "super-down")
	item.check(again and down and shown, "Super+Down did not leave full screen")
	item.passed()


@run.define("apps.terminal.many-windows")
def terminal_many(item):
	mark = run.mark()
	for _ in range(20):
		run.as_user("/bin/terminal")
		time.sleep(0.3)
	deadline = time.monotonic() + 90
	started = []
	while time.monotonic() < deadline:
		started = run.lines(r"ZTERM START ", mark)
		if len(started) >= 20:
			break
		time.sleep(2)
	maps = run.lines(r"KWL MAP client=", mark)
	failed = run.lines(r"ZTERM FAILED|errno=[1-9]", mark)
	item.step(f"started Terminal 20 times as {aatlib.USER}", f"{len(maps)} maps, {len(started)} starts, {len(failed)} failures")
	run.shot(item, "twenty")
	item.check(len(started) >= 20 and len(maps) >= 20, f"only {len(started)} Terminals started ({len(maps)} windows)")
	item.check(not failed, f"{failed[0] if failed else ''}")
	clients = {aatlib.number(line, "client") for line in maps}
	ended = time.monotonic()
	run.stop_programs()
	# The compositor lets them go one by one (T1-232: 23 s for twenty, its keys waiting meanwhile, so that the next
	# scenario's Windows key came too late): the scenario ends when each window's client is gone.
	gone = set()
	while time.monotonic() < ended + 90 and not clients <= gone:
		gone = {aatlib.number(line, "client") for line in run.lines(r"KWL CLIENT gone client=\d+", mark)}
		time.sleep(2)
	took = time.monotonic() - ended
	item.step("ended them all", f"{len(clients & gone)} of {len(clients)} clients gone in {took:.0f} s")
	item.check(clients <= gone, f"{len(clients - gone)} clients were not gone after 90 s")
	item.passed(f"the compositor let them go in {took:.0f} s")


@run.define("apps.terminal.japanese-history")
def terminal_history(item):
	old = common.current_method(run)
	common.set_method(run, item, 1)
	try:
		terminal(item)
		run.type("echo ")
		mark = run.mark()
		common.to_language(run, item, "ja")
		run.type("nihongonobunshouwonagakuutsu")
		run.key("space")
		time.sleep(0.6)
		run.key("enter")
		commit = run.wait(r"ZTERM IME commit bytes=\d+", mark, 10)
		run.key("alt+space")
		run.key("enter")
		time.sleep(0.8)
		item.step("echo with a long Japanese phrase", commit)
		item.check(commit, "no ZTERM IME commit")
		run.key("up")
		run.key(*(["left"] * 5), *(["backspace"] * 2), *(["right"] * 3))
		time.sleep(0.8)
		item.step("Up, Left x5, Backspace x2, Right x3")
		run.shot(item, "history")
		run.key("ctrl+c")
	finally:
		if common.current_method(run) != old:
			common.set_method(run, item, old)
	item.person("the prompt stays and the line is not broken in the screenshot (BUG-173)")


@run.define("apps.emacs.edit-save")
def emacs(item):
	terminal(item)
	# -nw as GNU Emacs users type it: REmacs is a terminal editor and accepts it, changing nothing (BUG-238).
	run.type(f"emacs -nw {aatlib.WORK}/emacs.txt")
	run.key("enter")
	time.sleep(4.0)
	run.shot(item, "emacs")
	run.type("hello from emacs")
	run.key("ctrl+x", "ctrl+s")
	time.sleep(1.0)
	run.key("ctrl+x", "ctrl+c")
	time.sleep(1.5)
	_, text = run.sh(f"cat {aatlib.WORK}/emacs.txt")
	item.step("typed, C-x C-s, C-x C-c", repr(text))
	run.shot(item, "shell")
	item.check(text.strip() == "hello from emacs", f"the file is {text!r}")
	item.passed()


# Settings.

PAGES = ("wifi", "ethernet", "network", "appearance", "wallpaper", "sound", "display", "languages", "storage",
	"keyboard", "mouse", "touchpad", "sharing", "users", "about")


@run.define("apps.settings.pages")
def settings_pages(item):
	run.launch(item, "Settings")
	bad = []
	for page in PAGES:
		mark = run.mark()
		run.as_user(f"/bin/settings {page}")
		layout = run.wait(rf"ZSETTINGS LAYOUT page={page} controls=", mark, 15)
		if page == "wallpaper":
			# The tiles are made one by one by keiland-preview's child; the shot waits for all (T1-320 showed three
			# empty).
			run.wait(r"ZSETTINGS LOOK pictures ready count=\d+", mark, 20)
		time.sleep(0.8)
		failed = run.lines(r"ZSETTINGS FAILED", mark)
		item.step(f"page {page}", layout or "no layout")
		run.shot(item, page)
		if not layout or failed:
			bad.append(f"{page}: {failed[0] if failed else 'no layout'}")
	item.check(not bad, "; ".join(bad))
	item.person(f"{len(PAGES)} pages drawn; their look in the screenshots")


@run.define("apps.settings.single-instance")
def settings_single(item):
	run.launch(item, "Settings")
	mark = run.mark()
	run.as_user("/bin/settings about")
	handed = run.wait(r"ZSETTINGS DONE reason=handed-over page=about", mark, 15)
	page = run.wait(r"ZSETTINGS PAGE about", mark, 10)
	time.sleep(1.0)
	maps = run.lines(r"KWL MAP client=", mark)
	item.step("started settings about as kei", f"{handed}; {page}; {len(maps)} new windows")
	run.shot(item, "about")
	item.check(handed and page, "the second start did not hand its page over")
	item.check(not maps, "a second window was mapped")
	item.passed()


@run.define("apps.settings.users-page")
def settings_users(item):
	window, since = run.settings(item, "users")
	account = run.lines(rf"ZSETTINGS USERS account name={aatlib.USER}\b", None)
	listed = run.lines(r"ZSETTINGS USERS list count=\d+", None)
	item.step("read the Users page", f"{account[-1] if account else 'no account'}; {listed[-1] if listed else 'no list'}")
	run.shot(item, "users")
	item.check(account, f"no account line for {aatlib.USER}")
	item.check(listed and aatlib.number(listed[-1], "count") >= 1, "no users listed")
	item.person("Your account, the three password fields and the list in the screenshot")


def shadow_line() -> str:
	"""kei's line of /etc/shadow."""
	_, text = run.sh(f"grep '^{aatlib.USER}:' /etc/shadow")
	return text.strip()


def fill(fields: list[str]) -> None:
	"""Types fields one after another with Tab between them, then Enter."""
	for index, value in enumerate(fields):
		if index:
			run.key("tab")
		run.type(value)
	run.key("enter")


@run.define("apps.settings.change-password")
def settings_password(item):
	common.keep_shadow(run)
	before = shadow_line()
	window, since = run.settings(item, "users")
	controls = run.controls(since, "users")
	cases = (
		("a wrong current password", ["wrong-pass", "aat-pass-1", "aat-pass-1"], "wrong"),
		("a short new password", [aatlib.PASSWORD, "short", "short"], "short"),
		("the right change", [aatlib.PASSWORD, "aat-pass-1", "aat-pass-1"], "right"),
	)
	for what, fields, name in cases:
		# The field is at the page's foot, half under the window's edge (T1-232: a click there missed it, and the
		# typing went to the sidebar): the page is scrolled until the field is whole.
		controls = reveal_control(window, "users", 1)
		mark = run.mark()
		run.click_control(item, window, controls, 1, "the current password's field")
		fill(fields)
		time.sleep(3.0)
		result = run.lines(r"ZSETTINGS USERS result request=\d+ errno=\d+", mark)
		now = shadow_line()
		item.step(what, f"{result[-1] if result else 'no request'}; shadow {'changed' if now != before else 'kept'}")
		run.shot(item, name)
		if name == "right":
			item.check(result and aatlib.number(result[-1], "errno") == 0 and now != before, "the right change was not made")
		else:
			item.check(now == before, f"{what} changed the password")
			if name == "wrong":
				item.check(result and aatlib.number(result[-1], "errno") != 0, "no refused request for the wrong password")
		# Esc empties the fields, or with nothing typed goes back from the page (T1-202c: the cases after the first
		# typed into the Settings overview): the Users page is asked for again, its controls read anew.
		run.key("esc")
		controls = users_page_again(controls)
	common.restore_shadow(run)
	item.check(shadow_line() == before, "/etc/shadow was not put back")
	item.step("put /etc/shadow back")
	item.person("the messages under the fields in the screenshots (wrong, not accepted, changed)")


def users_page_again(controls: dict) -> dict:
	"""Shows Settings' Users page again (the page asked for once more re-logs its controls) and gives its controls
	(those given when no new list comes)."""
	mark = run.mark()
	run.as_user("/bin/settings users")
	layout = run.wait(r"ZSETTINGS LAYOUT page=users controls=", mark, 15)
	time.sleep(0.5)
	if layout is None:
		return controls
	return run.controls(mark, "users")


def reveal_control(window, page: str, index: int) -> dict:
	"""Scrolls a Settings page down (the wheel over the window) until a control is inside the window (T1-202c: the
	users' list's last row was 1294 pixels down); gives the page's controls then."""
	controls = run.controls(None, page)
	for _ in range(8):
		place = controls.get(index)
		if place is not None and 0 <= place[1] and place[1] + place[3] <= window.height:
			return controls
		run.aat("wheel", str(window.x + window.width // 2), str(window.y + window.height // 2), "-5")
		time.sleep(0.6)
		controls = run.controls(None, page)
	return controls


def remove_aatuser() -> None:
	"""Removes aatuser when a run left it (as kei, through account-admin), and its home (a removal from Settings keeps
	it unless its switch says otherwise, and a home left makes the next addition fail with home-exists)."""
	run.sh("grep -q '^aatuser:' /etc/passwd && { printf 'kei\\nremove\\naatuser\\nremove-home\\n' | su kei -c /usr/libexec/account-admin; }; "
		"grep -q '^aatuser:' /etc/passwd || rm -rf /home/aatuser; true")


@run.define("apps.settings.manage-users")
def settings_manage(item):
	remove_aatuser()
	window, since = run.settings(item, "users")
	controls = run.controls(since, "users")
	# ws177-p004: the name field takes no more than account-admin does (32 bytes); Esc closes the form.
	mark = run.mark()
	run.click_control(item, window, controls, 21, "Add User")
	started = run.wait(r"ZSETTINGS USERS admin start mode=1", mark, 10)
	time.sleep(0.5)
	run.type("abcdefghijklmnopqrstuvwxyzabcdefghijklmn")
	full = run.wait(r"ZSETTINGS USERS admin field=0 length=32$", mark, 10)
	over = run.lines(r"ZSETTINGS USERS admin field=0 length=(3[3-9]|40)$", mark)
	item.step("typed 40 letters into User name", f"{started}; {full}; {len(over)} longer")
	run.shot(item, "name-32")
	item.check(started and full and not over, f"the name field did not stop at 32 ({full}; {len(over)} longer)")
	run.key("esc")
	time.sleep(0.5)
	for what, password, expected in (("a wrong own password", "wrong-pass", "bad-password"), ("the right one", aatlib.PASSWORD, None)):
		mark = run.mark()
		run.click_control(item, window, controls, 21, "Add User")
		started = run.wait(r"ZSETTINGS USERS admin start mode=1", mark, 10)
		time.sleep(0.5)
		fill(["aatuser", "AAT User", "aat-pass-1", password])
		result = run.wait(r"ZSETTINGS USERS admin result request=\d+ ", mark, 20)
		_, count = run.sh("grep -c '^aatuser:' /etc/passwd")
		item.step(f"Add User with {what}", f"{started}; {result}; passwd count {count.strip()}")
		run.shot(item, "add-wrong" if expected else "added")
		if expected:
			item.check(result and f"reason={expected}" in result and count.strip() == "0", f"{what}: {result}")
		else:
			item.check(result and "errno=0" in result and count.strip() == "1", f"{what}: {result}")
	time.sleep(1.0)
	since = run.mark()
	run.as_user("/bin/settings users")
	time.sleep(1.5)
	controls = run.controls(None, "users")
	# The rows of the users' list are 100 to 199; 200 and on are the PIN fields (T1-232 clicked one as a row).
	rows = sorted(index for index in controls if 100 <= index < 200)
	item.check(rows, "no rows in the users' list")
	controls = reveal_control(window, "users", rows[-1])
	run.click_control(item, window, controls, rows[-1], "aatuser's row")
	time.sleep(0.5)
	controls = reveal_control(window, "users", 23)
	mark = run.mark()
	run.click_control(item, window, controls, 23, "Remove")
	started = run.wait(r"ZSETTINGS USERS admin start mode=3", mark, 10)
	time.sleep(0.5)
	run.type(aatlib.PASSWORD)
	run.key("enter")
	result = run.wait(r"ZSETTINGS USERS admin result request=\d+ ", mark, 20)
	_, count = run.sh("grep -c '^aatuser:' /etc/passwd")
	item.step("Remove aatuser", f"{started}; {result}; passwd count {count.strip()}")
	run.shot(item, "removed")
	remove_aatuser()
	item.check(result and "errno=0" in result and count.strip() == "0", f"the removal: {result}")
	item.person("the form and the answer lines in the screenshots")


@run.define("apps.settings.about")
def settings_about(item):
	# Settings reads the system's names once as it starts, before any page (T1-232): the line is looked for from the
	# launch on.
	start = run.mark()
	window, since = run.settings(item, "about")
	about = run.wait(r"ZSETTINGS ABOUT system=", start, 10)
	_, release = run.sh(". /etc/os-release; echo \"$PRETTY_NAME\"; uname -a")
	pretty, uname = (release.splitlines() + ["", ""])[:2]
	item.step("read the About page", about)
	item.step("read /etc/os-release and uname -a", f"{pretty} | {uname}")
	run.shot(item, "about")
	item.check(about, "no ZSETTINGS ABOUT line")
	system = re.search(r"system=(.*?) kernel=", about)
	item.check(system and system.group(1).strip() == pretty.strip(), f"system {system.group(1) if system else '-'} is not {pretty!r}")
	kernel = re.search(r"kernel=(.*?) machine=", about)
	item.check(kernel and kernel.group(1).strip() and kernel.group(1).strip() in uname, f"kernel {kernel.group(1) if kernel else '-'} is not in uname")
	item.passed(pretty)


@run.define("apps.settings.sound-page")
def settings_sound(item):
	# Settings opens its sound connection once as it starts, before any page (T1-232): the line is looked for from the
	# launch on.
	start = run.mark()
	window, since = run.settings(item, "sound")
	line = run.wait(r"ZSETTINGS SOUND open live=", start, 10)
	item.step("read the Sound page", line)
	run.shot(item, "sound")
	item.check(line, "no ZSETTINGS SOUND open line")
	if aatlib.number(line, "reachable") != 1:
		item.person(f"audiod is not reachable here: {line}")
	volume = run.lines(r"KWL VOLUME (restored|set) value=", None)
	value = aatlib.number(line, "value")
	bar = aatlib.number(volume[-1], "value") if volume else None
	item.check(bar is None or bar == value, f"Settings says {value}, the bar {bar}")
	item.passed(f"value {value}")


@run.define("apps.settings.display-language")
def settings_display_language(item):
	# The display language (ws158-p004): Japanese on the Languages page, Settings and Files follow at once, English again.
	window, since = run.settings(item, "languages")
	controls = run.controls(since, "languages")
	mark = run.mark()
	run.click_control(item, window, controls, 5, "the switch of 日本語")
	chosen = run.wait(r"ZSETTINGS LANGUAGES ui language=ja", mark, 10)
	followed = run.wait(r"ZSETTINGS LOOK language=ja", mark, 10)
	time.sleep(1.0)
	item.step("chose 日本語", f"{chosen}; {followed}")
	run.shot(item, "settings-ja")
	item.check(chosen and followed, "Settings did not follow the display language")
	files = run.launch(item, "Files")
	time.sleep(1.5)
	item.step("opened Files", run.lines(r"ZFILES LANGUAGE language=\w+", mark)[-1:] or "no LANGUAGE line")
	run.shot(item, "files-ja")
	run.close(item, files)
	# Settings still runs: its page asked for again brings it to the front and logs its controls anew.
	mark = run.mark()
	run.as_user("/bin/settings languages")
	run.wait(r"ZSETTINGS LAYOUT page=languages controls=", mark, 15)
	time.sleep(0.5)
	controls = run.controls(mark, "languages")
	mark = run.mark()
	run.click_control(item, window, controls, 4, "the switch of English")
	back = run.wait(r"ZSETTINGS LOOK language=en", mark, 10)
	item.step("chose English again", back)
	item.check(back, "Settings did not come back to English")
	item.person("the Japanese words of Settings and Files in settings-ja.png and files-ja.png")


@run.define("apps.settings.login-language")
def settings_login_language(item):
	# The login screen's language (ws158-p004): an administrator sets the system's language with the password.
	_, before = run.sh("cat /etc/keiland/language 2>/dev/null || echo none")
	window, since = run.settings(item, "languages")
	# The Login screen card is an administrator's: its password field (control 8; Apply, 9, takes clicks only once a
	# change and the password are there, T1-239).
	controls = reveal_control(window, "languages", 8)
	item.check(8 in controls, "no password field (control 8): the Login screen card is not shown, not an administrator here")
	results = []
	for code, switch in (("ja", 7), ("en", 6)):
		controls = reveal_control(window, "languages", 8)
		run.click_control(item, window, controls, switch, f"the login screen's switch of {code}")
		time.sleep(0.4)
		run.click_control(item, window, controls, 8, "the password's field")
		mark = run.mark()
		run.type(aatlib.PASSWORD)
		run.key("enter")
		result = run.wait(r"ZSETTINGS LANGUAGES system result request=\d+ errno=\d+", mark, 20)
		_, now = run.sh("cat /etc/keiland/language 2>/dev/null || echo none")
		item.step(f"set the login screen to {code}", f"{result}; file {now.strip()!r}")
		run.shot(item, f"login-{code}")
		results.append((code, result, now.strip()))
	for code, result, now in results:
		item.check(result and aatlib.number(result, "errno") == 0 and now == code, f"{code}: {result}; the file says {now!r}")
	item.passed(f"before {before.strip()!r}, ja then en (en is kept)")


# The other applications.
# The other applications.

@run.define("apps.calendar.navigate")
def calendar(item):
	mark = run.mark()
	window = run.launch(item, "Calendar")
	today = run.wait(r"CALENDAR READY today=\S+", mark, 10)
	_, date = run.sh("date +%Y-%m-%d")
	item.step("read CALENDAR READY and the target's date", f"{today}; {date.strip()}")
	item.check(today and aatlib.field(today, "today") == date.strip(), f"today is {aatlib.field(today, 'today')}, not {date.strip()}")
	run.click(*window.middle())
	time.sleep(0.5)
	picked = run.lines(r"CALENDAR SELECT date=", mark)
	mark = run.mark()
	run.key("right")
	selected = run.wait(r"CALENDAR SELECT date=\S+", mark, 10)
	item.step("clicked the month, then Right", f"{picked[-1] if picked else 'no pick'}; {selected}")
	run.shot(item, "selected")
	item.check(selected, "Right selected no date")
	mark = run.mark()
	run.key("pagedown")
	flipped = run.wait(r"CALENDAR FLIP from=", mark, 10)
	time.sleep(0.8)
	item.step("Page Down", flipped)
	run.shot(item, "next-month")
	item.check(flipped, "Page Down did not turn the month")
	mark = run.mark()
	run.key("t")
	back = run.wait(rf"CALENDAR (SELECT date|FLIP from=\S+ to)={re.escape(date.strip())}", mark, 10)
	item.step("T", back)
	item.check(back, "T did not go back to today")
	item.person("the month's grid and the turning in the screenshots")


@run.define("apps.videoplayer.play")
def videoplayer(item):
	mark = run.mark()
	run.open_as_user(item, f"/bin/videoplayer {aatlib.SAMPLES}/sample.mp4")
	opened = run.wait(r"VIDEOPLAYER OPEN path=\S*sample.mp4 width=640 height=360", mark, 15)
	item.step("opened sample.mp4", opened)
	item.check(opened, "sample.mp4 did not open at 640x360")
	time.sleep(1.0)
	run.shot(item, "playing-1")
	time.sleep(0.6)
	run.shot(item, "playing-2")
	# FRAMES is logged at the first picture and every 100th only; how many were shown is Pause's (T1-202c).
	frames = [aatlib.number(line, "shown") for line in run.lines(r"VIDEOPLAYER FRAMES shown=\d+", mark)]
	mark = run.mark()
	run.key("space")
	paused = run.wait(r"VIDEOPLAYER PAUSE shown=", mark, 5)
	shown = aatlib.number(paused, "shown")
	item.step("played for 1.6 s, then Space", f"frames {frames[-3:]}; {paused}")
	item.check(shown is not None and shown > 1, f"the frames did not advance: {frames}, {paused}")
	run.key("space")
	played = run.wait(r"VIDEOPLAYER PLAY shown=", mark, 5)
	item.step("Space again", f"{paused}; {played}")
	item.check(paused and played, "Space did not pause and play")
	mark = run.mark()
	run.key("right")
	seek = run.wait(r"VIDEOPLAYER SEEK to_ms=", mark, 5)
	item.step("Right", seek)
	item.check(seek, "Right did not seek")
	ended = run.wait(r"VIDEOPLAYER ENDED shown=", mark, 12)
	item.step("waited for the end", ended)
	item.check(ended, "the video did not end")
	item.person("the moving picture in the two screenshots; the sound is the UAT's")


@run.define("apps.imageview.open-png-jpeg")
def imageview(item):
	for name, kind in (("sample.png", "png"), ("sample.jpg", "jpeg")):
		mark = run.mark()
		run.open_as_user(item, f"/bin/imageview {aatlib.SAMPLES}/{name}")
		line = run.wait(rf"IMAGEVIEW IMAGE path=\S*{re.escape(name)} format=\S+ width=320 height=200", mark, 10)
		item.step(f"opened {name}", line)
		run.shot(item, kind)
		item.check(line, f"{name} did not open at 320x200")
		run.stop_programs()
	item.person("the gradient with a white square in both screenshots")


@run.define("apps.pdfviewer.open-turn")
def pdfviewer(item):
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/pdfviewer {aatlib.SAMPLES}/sample.pdf")
	ready = run.wait(r"PDFVIEWER (READY|OPEN) .*pages=2", mark, 10)
	item.step("opened sample.pdf", ready)
	run.shot(item, "page-1")
	item.check(ready, "the PDF did not open with 2 pages")
	run.click(*window.middle())
	shown = run.lines(r"PDFVIEWER PAGE shown=\d+", mark)
	mark = run.mark()
	run.key("pagedown")
	turned = run.wait(r"PDFVIEWER PAGE shown=\d+", mark, 10)
	time.sleep(0.8)
	item.step("Page Down", f"{shown[-1] if shown else '-'} -> {turned}")
	second = run.shot(item, "page-2")
	if turned:
		item.check(not shown or turned != shown[-1], "Page Down did not turn the page")
		item.person("AAT page 1, then AAT page 2 in the screenshots")
	# The scroll mode (the viewer's first) scrolls a screen and logs no PAGE (T1-202c): the view must have moved.
	first = run.outdir / "png" / f"{item.ident}-page-1.png"
	moved = first.read_bytes() != (run.outdir / second).read_bytes()
	item.check(moved, "Page Down moved nothing (the two screenshots are the same)")
	item.person("the scroll mode: the view moved down a screen from AAT page 1 towards AAT page 2 in the screenshots")


@run.define("apps.textedit.type-save")
def textedit(item):
	run.sh(f"rm -f {aatlib.WORK}/textedit.txt")
	window = run.open_as_user(item, f"/bin/textedit {aatlib.WORK}/textedit.txt")
	run.click(*window.middle())
	run.type("hello from aat")
	time.sleep(0.5)
	item.step("typed hello from aat")
	run.shot(item, "typed")
	mark = run.mark()
	run.key("ctrl+s")
	saved = run.wait(rf"TEXTEDIT SAVE path={aatlib.WORK}/textedit.txt", mark, 10)
	_, text = run.sh(f"cat {aatlib.WORK}/textedit.txt")
	item.step("Ctrl+S", f"{saved}; {text!r}")
	item.check(saved and text.strip() == "hello from aat", f"the file is {text!r}")
	item.passed()


@run.define("apps.videoplayer.fullscreen")
def videoplayer_fullscreen(item):
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/videoplayer {aatlib.SAMPLES}/sample.mp4")
	item.check(run.wait(r"VIDEOPLAYER OPEN path=", mark, 15), "sample.mp4 did not open")
	point = window.middle()
	run.click(*point)
	time.sleep(0.5)

	def toggled(what: str, keys: tuple, on: int, extra: str = "") -> str:
		since = run.mark()
		if keys:
			run.key(*keys)
		else:
			run.click(*point, "--count", "2")
		line = run.wait(rf"VIDEOPLAYER FULLSCREEN on={on}" + (f"|{extra}" if extra else ""), since, 10)
		# The player's first frame of the new size (a new swapchain first; T1-235's screenshot 0.8 s after the line
		# still had the window's size), when the size changed; then the compositor's frame.
		configure = run.wait(rf"KWL CONFIGURE client={window.client} surface={window.surface} serial=\d+ width=\d+ height=\d+ fullscreen={on}", since, 10)
		width, height = aatlib.number(configure, "width"), aatlib.number(configure, "height")
		presented = None
		if width and height:
			presented = run.wait(rf"VIDEOPLAYER PRESENTED width={width} height={height}|VIDEOPLAYER FAILED operation=frame", since, 10)
		time.sleep(0.8)
		item.step(what, f"{line}; {configure}; {presented or 'no new size shown'}")
		item.check(line, f"{what}: full screen did not turn {'on' if on else 'off'}")
		# Out of full screen the window comes back to its own size, not the output's (T1-235: Alt+Enter soon after Esc
		# saved the last fullscreen image's size).
		output = (int(run.ready().get("width", 0)), int(run.ready().get("height", 0)))
		if not on:
			item.check((width, height) != output, f"{what}: the window came back at the output's size {width}x{height}")
		return line

	mark = run.mark()
	toggled("F11", ("f11",), 1)
	# The game mode (ws122-p005b): the video shown straight once the pointer has been still 2 s (direct=1), or the
	# reason the display would not (backend: the QEMU display shows no shared images; the UAT's i915 should).
	scanout = run.wait(r"KWL SCANOUT direct=1 |KWL SCANOUT direct=0 reason=(backend|refused)", mark, 8)
	item.step("the game mode after 2 s of a still pointer", scanout or "no KWL SCANOUT line")
	item.check(scanout, "the fullscreen video neither went straight to the display nor was refused by it")
	run.shot(item, "f11")
	left = run.lines(r"KWL SCANOUT direct=0 reason=shot", mark)
	item.step("the screenshot is composed (the game mode leaves for it)", left[-1] if left else "no shot reason")
	toggled("Esc", ("esc",), 0)
	toggled("Alt+Enter", ("alt+enter",), 1)
	toggled("F11 again", ("f11",), 0, r"KWL GLASS fullscreen-leave surface=\d+ via=f11")
	time.sleep(0.5)
	toggled("a double click", (), 1)
	run.shot(item, "double-click")
	time.sleep(2.5)
	run.shot(item, "bar-hidden")
	toggled("a double click again", (), 0)
	item.person("the picture over the whole screen in f11.png and double-click.png, the bar gone in bar-hidden.png; the game mode's line (direct=1 on the UAT's i915)")


@run.define("apps.files.devices")
def files_devices(item):
	mark = run.mark()
	window = run.launch(item, "Files")
	time.sleep(2.0)
	count = run.lines(r"ZFILES DEVICES count=", mark)
	devices = run.lines(r"ZFILES DEVICE id=", mark)
	_, root = run.sh("df / | tail -1 | awk '{print $1}'")
	disk = re.sub(r"(p|s)?\d+$", "", root.strip().split("/")[-1])
	item.step("read the Devices", f"{count[-1] if count else 'no count'}; root on {root.strip()}")
	run.shot(item, "devices")
	item.check(count, "no ZFILES DEVICES line")
	boot = [line for line in devices if disk and disk in line]
	item.check(not boot, f"the boot disk is listed: {boot[0] if boot else ''}")
	rows = [line for line in run.lines(r"ZFILES DEVICE row id=", mark)]
	unmounted = [line for line in devices if "mounted=0" in line]
	if not rows or not unmounted:
		item.passed(f"{len(devices)} devices, the boot disk ({disk}) not among them; none to mount here")
	ident = aatlib.field(unmounted[0], "id")
	row = next((line for line in rows if aatlib.field(line, "id") == ident), None)
	item.check(row, f"no row for {ident}")
	x, y, width, height = (aatlib.number(row, name) for name in ("x", "y", "width", "height"))
	mark = run.mark()
	run.click(window.x + x + width // 2, window.y + y + height // 2, "--count", "2")
	confirm = run.wait(r"ZFILES DEVICE mount confirm id=", mark, 10)
	time.sleep(0.5)
	item.step(f"double-clicked {ident}", confirm)
	run.shot(item, "confirm")
	item.check(confirm, "no confirmation before the mount")
	run.key("esc")
	answer = run.wait(r"ZFILES DEVICE mount answer id=\S+ confirmed=0", mark, 10)
	item.step("Esc", answer)
	item.check(answer, "the confirmation was not cancelled")
	item.person("the confirmation card (name, size, file system, Cancel and Mount) in the screenshot")


@run.define("apps.monitor.frame-rate")
def monitor(item):
	mark = run.mark()
	run.launch(item, "System Monitor")
	time.sleep(10.0)
	ready = run.lines(r"ZMON READY ", mark)
	rates = [float(value) for value in re.findall(r"ZMON FRAME fps=([0-9.]+)", "\n".join(run.lines(r"ZMON FRAME fps=", mark)))]
	middle = statistics.median(rates) if rates else None
	item.step("ran for 10 s", f"{ready[-1] if ready else 'no READY'}; median fps {middle}")
	run.shot(item, "monitor")
	item.check(ready, "no ZMON READY")
	item.check(rates, "no ZMON FRAME lines")
	if not run.qemu and middle >= 15:
		item.passed(f"median {middle:.1f} fps")
	if not run.qemu:
		item.failed(f"median {middle:.1f} fps (under 15)")
	item.person(f"QEMU: median {middle:.1f} fps; the values in the screenshot")


@run.define("apps.browser.long-url-selection")
def browser(item):
	mark = run.mark()
	window = run.launch(item, "Browser")
	time.sleep(1.5)
	controls = [line for line in run.lines(rf"KWL TITLEBAR control client={window.client} surface={window.surface} where=floating id=\d+ ", mark)
		if aatlib.number(line, "width") and aatlib.number(line, "width") >= 200]
	item.check(controls, "no wide control (the URL field) in the title bar's log")
	field = controls[-1]
	x, y, width, height = (aatlib.number(field, name) for name in ("x", "y", "width", "height"))
	focus_mark = run.mark()
	run.click(x + width // 2, y + height // 2)
	focus = run.wait(rf"KWL TITLEBAR focus client={window.client} surface=\d+ id=\d+ edit=1", focus_mark, 10)
	item.step("clicked the URL field", focus)
	item.check(focus, "the URL field did not take the keyboard")
	run.key("ctrl+a")
	run.type("https://example.com/" + "aat-long-path-" * 15)
	run.key("ctrl+a")
	time.sleep(0.8)
	item.step("typed a long URL and selected it all")
	run.shot(item, "selected")
	run.key("esc")
	item.person("the selection stays inside the field, not over the title or the buttons (BUG-181)")


@run.define("apps.notes.draw-stroke")
def notes(item):
	mark = run.mark()
	window = run.launch(item, "Notes")
	left, top = window.x + window.width // 4, window.y + window.height // 3
	run.press_path([(left + step * 30, top + step * 15) for step in range(10)], pause=0.03)
	stroke = run.wait(r"NOTES STROKE page=\S+ id=\d+ tool=\d+ points=\d+", mark, 10)
	item.step("dragged across the page", stroke)
	run.shot(item, "stroke")
	item.check(stroke and aatlib.number(stroke, "points") > 1, "no NOTES STROKE with points")
	item.person("the line on the page in the screenshot")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
