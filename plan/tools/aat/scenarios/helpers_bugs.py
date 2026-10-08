#!/usr/bin/env python3
"""The automatic helpers of tests/scenarios/bugs/ (q911, the UI bugs' reproduction sweep of 2026-10-08).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_bugs.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

Each scenario tries to reproduce one open UI bug (or a few of the same
place) on the AAT image and says what it saw: pass (the bug did not show:
every check held), fail (the bug showed, or a step could not be done: the
note says which), needs-person (the checks held and a screenshot is for a
person's eye).  Which bugs and how to read each verdict:
plan/agents/bug-ui-sweep-20261008.md.

bugs.settings-wifi-click-tap needs /bin/network-probe and bugs.x11-docked-close
/bin/glxtest: the image of plan/tools/aat/config-amd64-aat-bugs.mk has both
(AAT_CONFIG=... plan/tools/aat/build-image.sh BUILD); on the plain AAT image
they say needs-person with the reason.
"""
from __future__ import annotations

import re
import shlex
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_bugs")

INPUT = "/bin/aat-input"

# The on-screen keyboard's flick keys (keyboard.c, as helpers_desktop.py): 6 apart, 4 x 4 at the panel's bottom.
KEY_GAP = 6
FLICK_ROWS = 4

# Phone's store and its places (helpers_phone.py): the contacts a third of the window, the field at the bottom.
PHONE_FOLDER = "/home/kei/Documents/Phone"
PHONE_COMPOSER = 60
PHONE_SHARE = 0.34

DESKTOP_FOLDER = "/home/kei/Desktop"


# Shared steps.

def inputs(*commands: str, timeout: float = 60.0) -> None:
	"""Runs aat-input commands one after another in one shell on the target (no SSH round between them, so that a
	key held, a quick press or a drag keeps its timing).  "sleep MS" is aat-input's own wait."""
	script = "; ".join(f"{INPUT} {command} >/dev/null" for command in commands)
	status, output = run.sh(script, timeout=timeout)
	if status != 0:
		raise RuntimeError(f"aat-input: {output.strip()[-200:]}")


def setting(key: str) -> str | None:
	"""A desktop setting's value now (keiland-settings get)."""
	_, output = run.as_user(f"/bin/keiland-settings get {key}", wait=True)
	match = re.search(rf"KEILAND-SETTINGS value key={re.escape(key)} value=(\S+)", output)
	return match.group(1) if match else None


def set_setting(key: str, value: str) -> str:
	"""Sets a desktop setting (keiland-settings set); returns its answer line."""
	_, output = run.as_user(f"/bin/keiland-settings set {key} {value}", wait=True)
	match = re.search(rf"KEILAND-SETTINGS set key={re.escape(key)} .*", output)
	return match.group(0) if match else output.strip()[-120:]


def write_file(path: str, text: str) -> None:
	"""Writes a text file on the target (readable by everyone)."""
	run.sh(f"cat > {shlex.quote(path)} <<'AAT_EOF'\n{text}\nAAT_EOF\nchmod 644 {shlex.quote(path)}")


def program_running(name: str) -> bool:
	"""Whether a program of that name runs on the target."""
	_, output = run.sh(f"ps -A -o args | grep -c '[/]bin/{name}'")
	return output.strip().splitlines()[-1:] != ["0"] if output.strip() else False


def kill_program(name: str) -> None:
	"""Ends every process of a program (by its /bin/ path in ps)."""
	run.sh(f"for p in $(ps -A -o pid,args | grep '[/]bin/{name}' | awk '{{print $1}}'); do kill $p; done; sleep 1; true")


def maximize(item, window) -> str:
	"""Double-clicks a floating window's title bar; returns its dock line."""
	since = run.mark()
	run.click(*window.title_point(), "--count", "2")
	dock = run.wait(rf"KWL GLASS dock surface={window.surface} via=double-click", since, 10)
	item.step("double-clicked the title bar", dock)
	item.check(dock, "the window did not dock (no KWL GLASS dock ... via=double-click)")
	time.sleep(0.8)
	return dock


def bar_icons(since: str | None) -> list[tuple[str, int]]:
	"""The apps bar's applications with a window, left to right (their last KWL APPS icon line after a mark)."""
	last: dict[str, tuple[int, int]] = {}
	for line in run.lines(r"KWL APPS icon app=\S+ x=-?\d+ ", since):
		app = aatlib.field(line, "app")
		last[app] = (aatlib.number(line, "x"), aatlib.number(line, "windows") or 0)
	return sorted(((app, x) for app, (x, windows) in last.items() if windows >= 1), key=lambda pair: pair[1])


def switch_to(item, pattern: str, mark: str) -> str | None:
	"""Alt held, Tab until the switcher's selection is the application whose key matches pattern, Alt let go;
	returns the commit line."""
	inputs("key-down alt", "key tab", "sleep 400")
	selected = run.wait(r"KWL SWITCH open via=keys index=\d+ app=\S+", mark, 5)
	app = aatlib.field(selected, "app") or ""
	for _ in range(5):
		if re.search(pattern, app, re.I):
			break
		step = run.mark()
		inputs("key tab", "sleep 300")
		line = run.wait(r"KWL SWITCH step index=\d+ app=\S+", step, 5)
		app = aatlib.field(line, "app") or ""
	inputs("sleep 400", "key-up alt")
	commit = run.wait(r"KWL SWITCH commit app=\S+", mark, 5)
	item.step(f"Alt+Tab to {pattern}", f"{selected}; {commit}")
	return commit


def editor(item, name: str):
	"""Text Editor on a new file of /tmp/aat-work, the pointer in its text."""
	run.sh(f"rm -f {aatlib.WORK}/{name}")
	window = run.open_as_user(item, f"/bin/textedit {aatlib.WORK}/{name}")
	run.click(*window.middle())
	time.sleep(0.5)
	return window


def save(item, name: str) -> str:
	"""Ctrl+S in Text Editor, and the file's text once TEXTEDIT SAVE says so."""
	mark = run.mark()
	run.key("ctrl+s")
	line = run.wait(rf"TEXTEDIT SAVE path={aatlib.WORK}/{re.escape(name)}", mark, 10)
	item.step("Ctrl+S", line)
	item.check(line, "no TEXTEDIT SAVE")
	_, text = run.sh(f"cat {aatlib.WORK}/{name}")
	return text


def corner_swipe(kind: str) -> None:
	"""The diagonal drag from a bottom corner (right: the flick panel, left: QWERTY)."""
	width, height = run.screen()
	if kind == "flick":
		run.drag(width - 3, height - 3, width - 203, height - 203, steps=12)
	else:
		run.drag(2, height - 3, 202, height - 203, steps=12)


def open_panel(item, kind: str) -> tuple[int, int, int, int]:
	"""Opens an on-screen keyboard panel; returns its x, y, width and height."""
	mark = run.mark()
	corner_swipe(kind)
	line = run.wait(rf"KWL OSK open kind={kind} ", mark, 10)
	item.step(f"swiped from the bottom {'right' if kind == 'flick' else 'left'} corner", line)
	item.check(line, f"the {kind} panel did not open")
	time.sleep(0.6)
	return tuple(aatlib.number(line, name) for name in ("x", "y", "width", "height"))


def close_panel(kind: str) -> None:
	"""Closes a panel with the same swipe (no check: the scenario's end)."""
	corner_swipe(kind)
	time.sleep(0.5)


def flick_key(panel, row: int, column: int) -> tuple[int, int]:
	"""The middle of a flick key."""
	px, py, pw, ph = panel
	key = min(96, max(64, run.screen()[1] // 11))
	x = px + KEY_GAP + column * (key + KEY_GAP)
	y = py + ph - (FLICK_ROWS - row) * (key + KEY_GAP)
	return x + key // 2, y + key // 2


def tap(point) -> None:
	"""A short press and release of the pointer at a point."""
	run.press_path([point, point], pause=0.06)
	time.sleep(0.4)


def kana_face(item, panel) -> None:
	"""Turns the flick panel to its kana face (the face key, row 4 column 4)."""
	for _ in range(3):
		faces = run.lines(r"KWL OSK face name=\S+", None)
		face = aatlib.field(faces[-1], "name") if faces else "kana"
		if face == "kana":
			return
		tap(flick_key(panel, 3, 3))
	item.failed("the flick panel did not turn to kana")


def qwerty_rects(mark: str) -> dict[str, tuple[int, int, int, int]]:
	"""The QWERTY panel's keys by their labels (KWL OSK qrect after a mark)."""
	rects = {}
	for line in run.lines(r"KWL OSK qrect ", mark):
		label = aatlib.field(line, "label")
		rects[label] = tuple(aatlib.number(line, name) for name in ("x", "y", "width", "height"))
	return rects


def phone_store(item) -> None:
	"""Two contacts in kei's Documents/Phone (Ben first), as helpers_phone.py."""
	now = int(time.time())
	script = f"""set -e
rm -rf {PHONE_FOLDER}
mkdir -p {PHONE_FOLDER}/contacts {PHONE_FOLDER}/messages/aat-ben {PHONE_FOLDER}/messages/aat-aiko
printf 'BEGIN:VCARD\\r\\nVERSION:3.0\\r\\nFN:Ben Carter\\r\\nTEL:+1 415 555 0142\\r\\nEND:VCARD\\r\\n' > {PHONE_FOLDER}/contacts/aat-ben.vcf
printf 'BEGIN:VCARD\\r\\nVERSION:3.0\\r\\nFN:Aiko Tanaka\\r\\nTEL:+81 90 1234 5678\\r\\nEND:VCARD\\r\\n' > {PHONE_FOLDER}/contacts/aat-aiko.vcf
printf 'Kind: text\\nChannel: sms\\nDirection: in\\nDate: {now - 60}\\nState: unread\\n\\nCall me when you can.\\n' > {PHONE_FOLDER}/messages/aat-ben/1-1-in.txt
printf 'Kind: text\\nChannel: rcs\\nDirection: in\\nDate: {now - 7200}\\nState: read\\n\\nGood morning!\\n' > {PHONE_FOLDER}/messages/aat-aiko/1-2-in.txt
chown -R kei {PHONE_FOLDER}
"""
	status, output = run.sh(script)
	item.step("two contacts in ~/Documents/Phone", output.strip()[-200:])
	item.check(status == 0, "the Phone store could not be written")


def phone_forget() -> None:
	"""Takes the Phone store away and the backend back to none."""
	run.sh(f"rm -rf {PHONE_FOLDER}")
	run.as_user("/bin/keiland-settings set phone.backend 0", wait=True)


def phone_field(window) -> tuple[int, int]:
	"""The middle of Phone's message field."""
	share = min(max(int(window.width * PHONE_SHARE), 260), 340)
	return window.x + (share + 8 + window.width) // 2, window.y + window.height - PHONE_COMPOSER // 2


def browser_field(item, window, since: str) -> tuple[int, int]:
	"""The middle of Browser's URL field (the title bar's wide control)."""
	controls = [line for line in run.lines(rf"KWL TITLEBAR control client={window.client} surface={window.surface} where=floating id=\d+ ", since)
		if aatlib.number(line, "width") and aatlib.number(line, "width") >= 200]
	item.check(controls, "no wide control (the URL field) in the title bar's log")
	x, y, width, height = (aatlib.number(controls[-1], name) for name in ("x", "y", "width", "height"))
	return x + width // 2, y + height // 2


# BUG-182: a tap in Browser presses an HTML button.

BUTTON_PAGE = (
	'<!doctype html><html><head><title>aat182</title></head><body style="margin:0">'
	'<button style="display:block;width:100%;height:420px;font-size:40px" '
	"onclick=\"console.log('aat-bug182-click')\">Tap me</button>"
	'<div style="height:400px"></div></body></html>'
)


@run.define("bugs.browser-tap-button")
def browser_tap_button(item):
	page = f"{aatlib.WORK}/b182.html"
	write_file(page, BUTTON_PAGE)
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/browser {page}", timeout=30)
	loaded = run.wait(r"ZBROWSER (READY|NAVIGATE) ", mark, 20)
	item.step("opened the page with one large button", loaded)
	item.check(loaded, "the page did not load")
	time.sleep(1.0)
	point = (window.x + 200, window.y + 150)
	clicked = run.mark()
	run.click(*point)
	by_mouse = run.wait(r"ZBROWSER CONSOLE level=\d+ aat-bug182-click", clicked, 10)
	item.step(f"clicked the button at {point[0]},{point[1]}", by_mouse)
	item.check(by_mouse, "a mouse click did not press the button either (the page, not the touch)")
	tapped = run.mark()
	run.aat("tap", str(point[0]), str(point[1]))
	touch = run.wait(r"ZBROWSER TOUCH tap x=", tapped, 10)
	by_tap = run.wait(r"ZBROWSER CONSOLE level=\d+ aat-bug182-click", tapped, 10)
	item.step(f"tapped the button at {point[0]},{point[1]}", f"{touch}; {by_tap}")
	run.shot(item, "tapped")
	item.check(touch, "Browser saw no tap (no ZBROWSER TOUCH tap)")
	item.check(by_tap, "BUG-182 reproduced: the tap did not press the HTML button (no console line after the tap)")
	item.passed("a tap pressed the button as a click does")


# BUG-184 and BUG-188: Settings' Wi-Fi switch turns off from on, and one tap on a network joins it.

@run.define("bugs.settings-wifi-click-tap")
def settings_wifi(item):
	status, _ = run.sh("test -x /bin/network-probe")
	if status != 0:
		item.person("no /bin/network-probe in this image: build it with config-amd64-aat-bugs.mk (QEMU has no Wi-Fi)")
	# The compositor watches networkd on one long connection (SUBSCRIBE) made when it started, so networkd itself is
	# stopped for the scenario: the watch ends, the compositor watches again within a second and finds network-probe
	# (T1-481: with only the socket moved aside, the old watch kept "wifi=absent" and the page had no Wi-Fi switch).
	# Stopping networkd leaves the wired interface as it is (it retires only the Wi-Fi radios).
	mark = run.mark()
	run.sh("service networkd stop >/dev/null 2>&1; i=0; while [ -S /run/networkd.sock ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done; "
		"[ -S /run/networkd.sock.aat ] || [ ! -S /run/networkd.sock ] || mv /run/networkd.sock /run/networkd.sock.aat; "
		"nohup /bin/network-probe 300 > /tmp/aat-probe.log 2>&1 </dev/null & sleep 1; true")
	try:
		listening = run.wait(r"NETPROBE listening", None, 10, log="/tmp/aat-probe.log")
		watched = run.wait(r"KWL NETWORK state reachable=1 .* wifi=(off|searching|connecting|connected|disconnected) ", mark, 10)
		item.step("networkd stopped, network-probe in its place (three networks, Wi-Fi on)", f"{listening}; {watched}")
		item.check(listening, "network-probe did not start")
		item.check(watched, "the compositor did not watch network-probe (no KWL NETWORK state with a Wi-Fi radio)")
		window, since = run.settings(item, "wifi")
		scanned = run.wait(r"ZSETTINGS NETWORK scan count=3", since, 15)
		item.step("Settings on the Wi-Fi page", scanned)
		item.check(scanned, "Settings did not list the three networks")
		for on in (0, 1, 0):
			controls = run.controls(since, "wifi")
			mark = run.mark()
			run.click_control(item, window, controls, 1, "the Wi-Fi switch")
			shows = run.wait(rf"ZSETTINGS NETWORK switch shows on={on}", mark, 10)
			settled = run.wait(r"ZSETTINGS NETWORK switch settled wifi=" + ("1" if on == 0 else "[2-5]"), mark, 10)
			item.step(f"clicked the switch ({'off' if on == 0 else 'on'})", f"{shows}; {settled}")
			run.shot(item, "off" if on == 0 else "on")
			item.check(shows and settled, f"BUG-184 reproduced: the switch did not turn {'off' if on == 0 else 'on'}")
			if on:
				item.check(run.wait(r"ZSETTINGS NETWORK scan count=3", mark, 15), "the networks did not come back after on")
				time.sleep(1.0)
		mark = run.mark()
		run.click_control(item, window, run.controls(since, "wifi"), 1, "the Wi-Fi switch")
		item.check(run.wait(r"ZSETTINGS NETWORK switch settled wifi=[2-5]", mark, 10), "the switch did not turn on again")
		item.check(run.wait(r"ZSETTINGS NETWORK scan count=3", mark, 15), "the networks did not come back")
		time.sleep(1.0)
		controls = run.controls(since, "wifi")
		item.check(101 in controls, "no second network's row (control 101)")
		x, y, width, height = controls[101]
		point = (window.x + x + width // 3, window.y + y + height // 2)
		probe = run.mark("/tmp/aat-probe.log")
		mark = run.mark()
		run.aat("tap", str(point[0]), str(point[1]))
		request = run.wait(r"NETPROBE request op=\d+ ssid=\S.*", probe, 8, log="/tmp/aat-probe.log")
		joined = run.wait(r"ZSETTINGS NETWORK state reachable=1 connected=1 .*wifi=4 ssid=\S", mark, 10)
		item.step(f"one tap on the second network's row at {point[0]},{point[1]}", f"{request}; {joined}")
		run.shot(item, "tapped")
		item.check(request, "BUG-188 reproduced: one tap asked for no join (network-probe got no request)")
		item.check(joined, "the tap's join did not end connected")
		item.passed(f"off from on by one click; one tap joined ({aatlib.field(joined, 'ssid')})")
	finally:
		kill_program("network-probe")
		run.sh("if [ -S /run/networkd.sock.aat ]; then rm -f /run/networkd.sock; mv /run/networkd.sock.aat /run/networkd.sock; fi; "
			"service networkd start >/dev/null 2>&1; true")


# BUG-203 and BUG-204: Japanese in Phone's field, by the input method and by the flick panel's kana.

@run.define("bugs.phone-ime-flick")
def phone_ime_flick(item):
	old = common.current_method(run)
	phone_store(item)
	try:
		common.set_method(run, item, 1)
		run.as_user("/bin/keiland-settings set phone.backend 1", wait=True)
		window = run.launch(item, "Phone")
		mark = run.mark()
		run.click(window.x + window.width // 6, window.y + 96 + 34)
		item.check(run.wait(r"PHONE SELECT contact=0", mark, 10), "Ben was not chosen")
		run.click(*phone_field(window))
		time.sleep(0.3)
		common.to_language(run, item, "ja")
		mark = run.mark()
		run.type("nihon")
		run.key("space")
		time.sleep(0.8)
		run.shot(item, "converting")
		run.key("enter")
		time.sleep(0.5)
		run.key("alt+space")
		time.sleep(0.3)
		run.key("enter")
		sent = run.wait(r"PHONE SEND contact=0 channel=0 length=\d+ error=0", mark, 10)
		time.sleep(1.0)
		_, kept = run.sh(f"cat {PHONE_FOLDER}/messages/aat-ben/*.txt")
		japanese = any(ord(character) > 0x7f for character in kept)
		item.step("Japanese on, nihon, Space, Enter (commit), Alt+Space, Enter (send)", f"{sent}; Japanese kept {japanese}")
		run.shot(item, "ime-sent")
		item.check(sent and japanese, "BUG-203 reproduced: the input method's Japanese did not go into the message")
		panel = open_panel(item, "flick")
		kana_face(item, panel)
		mark = run.mark()
		tap(flick_key(panel, 0, 1))
		tap(flick_key(panel, 3, 0))
		voiced = run.wait(r"KWL OSK send via=commit text=が before=3", mark, 10)
		refused = run.lines(r"KWL OSK refused ", mark)
		item.step("か, then the voice key (が)", f"{voiced}; refused {refused[-1] if refused else 'none'}")
		run.shot(item, "flick")
		close_panel("flick")
		send = run.mark()
		run.click(*phone_field(window))
		run.key("enter")
		flicked = run.wait(r"PHONE SEND contact=0 channel=0 length=3 error=0", send, 10)
		item.step("Enter (send)", flicked)
		item.check(voiced and not refused, "BUG-204 reproduced: the flick panel's kana did not reach Phone's field")
		item.check(flicked, "the kana が was not sent (no PHONE SEND length=3)")
		item.passed("Japanese by the input method and by the flick panel went into Phone's field")
	finally:
		phone_forget()
		if common.current_method(run) != old:
			common.set_method(run, item, old)


# BUG-205: the bold text of the applications (a look).

@run.define("bugs.bold-text")
def bold_text(item):
	phone_store(item)
	old = setting("ui.language")
	try:
		run.launch(item, "Settings")
		run.shot(item, "settings")
		run.launch(item, "Phone")
		run.shot(item, "phone")
		run.launch(item, "Files")
		run.shot(item, "files")
		run.stop_programs()
		mark = run.mark()
		item.step("ui.language 1", set_setting("ui.language", "1"))
		item.check(run.wait(r"KWL LANGUAGE language=ja ", mark, 10), "the desktop did not change to Japanese")
		# App Home names the applications in Japanese now: Settings by its program.
		run.open_as_user(item, "/bin/settings", r"ZSETTINGS READY ")
		run.shot(item, "settings-ja")
	finally:
		set_setting("ui.language", old or "0")
		phone_forget()
	item.person("the bold headings and names: stems even, edges as soft as the regular text, the spacing natural, in Latin and Japanese (BUG-205)")


# BUG-206: Browser's URL field keeps a URL's own scheme.

@run.define("bugs.browser-url-scheme")
def browser_url_scheme(item):
	page = f"{aatlib.WORK}/b206.html"
	write_file(page, "<!doctype html><title>aat206</title><p>aat206</p>")
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/browser {page}", timeout=30)
	item.check(run.wait(r"ZBROWSER (READY|NAVIGATE) ", mark, 20), "the file page did not load")
	time.sleep(1.0)
	checked = []
	for target in (None, "https://example.com/", "data:text/html,<p>aat206</p>"):
		if target is not None:
			go = run.mark()
			run.click(*browser_field(item, window, mark))
			run.key("ctrl+a")
			run.type(target)
			run.key("enter")
			scheme_name = target.split(":", 1)[0]
			arrived = run.wait(rf"ZBROWSER TITLEBAR back=\d+ forward=\d+ path={scheme_name}:\S+", go, 25)
			failed = run.lines(r"ZBROWSER ERROR load ", go)
			item.step(f"opened {target}", f"{arrived}; {failed[-1] if failed else ''}")
			if not arrived or failed:
				continue
			time.sleep(1.0)
		paths = run.lines(r"ZBROWSER TITLEBAR back=\d+ forward=\d+ path=\S+", mark)
		path = aatlib.field(paths[-1], "path") if paths else None
		if path in checked:
			continue
		item.check(path, "no ZBROWSER TITLEBAR path line")
		scheme = re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", path) is not None and not path.startswith("/")
		expected = len(path) if scheme else len("file://") + len(path)
		edit = run.mark()
		run.click(*browser_field(item, window, mark))
		focus = run.wait(r"KWL TITLEBAR focus client=\d+ surface=\d+ id=\d+ edit=1", edit, 10)
		time.sleep(0.5)
		run.shot(item, f"editing-{len(checked)}")
		run.key("end")
		run.key("enter")
		text = run.wait(r"KWL TITLEBAR text client=\d+ id=\d+ done=1 how=\d+ length=\d+", edit, 10)
		length = aatlib.number(text, "length")
		loading = run.wait(r"ZBROWSER (LOADING url=|TITLEBAR back=)\S*", edit, 10)
		item.step(f"the field on {path}: Enter", f"{focus}; {text}; expected length {expected}; {loading}")
		item.check(text, "the field gave no text on Enter")
		item.check(length == expected, f"BUG-206 reproduced: the field held {length} characters, not the {expected} of {'the URL' if scheme else 'file:// and the path'} ({path})")
		item.check(not (loading and "file://" in loading and scheme), f"BUG-206 reproduced: {loading}")
		checked.append(path)
	item.check(len(checked) >= 2, f"no page with a scheme could be opened (checked {checked}): no network and no data: URL")
	item.passed(f"the field kept {', '.join(checked)}")


# BUG-207: Browser answers while a page loads.

TALL_PAGE = "<!doctype html><title>aat207</title><body>" + "".join(f"<p>line {n}</p>" for n in range(300)) + "</body>"


@run.define("bugs.browser-busy-scroll")
def browser_busy(item):
	page = f"{aatlib.WORK}/b207.html"
	write_file(page, TALL_PAGE)
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/browser {page}", timeout=30)
	item.check(run.wait(r"ZBROWSER (READY|NAVIGATE) ", mark, 20), "the tall page did not load")
	time.sleep(1.0)
	run.click(*browser_field(item, window, mark))
	run.key("ctrl+a")
	run.type("https://www.wikipedia.org/")
	go = run.mark()
	run.key("enter")
	middle = (window.x + 200, window.y + 200)
	inputs(f"move-to {middle[0]} {middle[1]}", "wheel -3", "sleep 150", "wheel -3", "sleep 150", "wheel -3", "sleep 150", "wheel -3")
	run.shot(item, "loading")
	arrived = run.wait(r"ZBROWSER (NAVIGATE path=\S*wikipedia|ERROR load )", go, 40)
	lines = run.lines(r"ZBROWSER (LOADING|FRAME scroll=|NAVIGATE|ERROR load )", go)
	item.step("Enter on https://www.wikipedia.org/, then the wheel down 12 notches at once", f"{arrived}; {len(lines)} lines")
	run.shot(item, "loaded")
	if not arrived or "ERROR load" in arrived:
		item.person(f"the page did not come ({arrived}): the guest has no network to the Internet; nothing judged")
	before = []
	for line in lines:
		if "NAVIGATE" in line:
			break
		if "FRAME scroll=" in line and float(re.search(r"scroll=([0-9.]+)", line).group(1)) > 0:
			before.append(line)
	item.step("frames scrolled before the new page", f"{len(before)}: {before[-1] if before else ''}")
	item.check(before, "BUG-207 reproduced: the wheel moved nothing until the page had loaded")
	item.passed(f"{len(before)} scrolled frames while the page loaded")


# BUG-208: F11 on a docked Terminal fills the screen and comes back docked.

@run.define("bugs.docked-f11")
def docked_f11(item):
	window = run.launch(item, "Terminal")
	run.click(*window.middle())
	maximize(item, window)
	width, height = run.screen()
	mark = run.mark()
	run.key("f11")
	full = run.wait(rf"KWL CONFIGURE client={window.client} surface={window.surface} serial=\d+ width={width} height={height} fullscreen=1", mark, 10)
	time.sleep(1.5)
	item.step("F11 on the docked Terminal", full)
	run.shot(item, "docked-full")
	item.check(full, f"BUG-208 reproduced: no full-screen configure ({width}x{height} fullscreen=1)")
	mark = run.mark()
	run.key("f11")
	back = run.wait(rf"KWL WINDOW unfullscreen surface={window.surface} .*docked=1", mark, 10)
	configure = run.wait(rf"KWL CONFIGURE client={window.client} surface={window.surface} serial=\d+ width=\d+ height=\d+ fullscreen=0", mark, 10)
	time.sleep(1.5)
	item.step("F11 again", f"{back}; {configure}")
	run.shot(item, "docked-back")
	item.check(back and configure, "the window did not come back docked")
	item.passed(f"full {width}x{height}, back docked {aatlib.number(configure, 'width')}x{aatlib.number(configure, 'height')}")


# BUG-209: Alt+Tab starts on the current application and goes right in the bar's order.

@run.define("bugs.alt-tab-order")
def alt_tab_order(item):
	start = run.mark()
	run.launch(item, "Files")
	run.launch(item, "Terminal")
	run.launch(item, "Text Editor")
	time.sleep(1.0)
	order = [app for app, _ in bar_icons(start)]
	item.step("the bar's applications left to right", ", ".join(order))
	item.check(len(order) >= 3, f"fewer than three applications in the bar: {order}")
	current = next((app for app in order if re.search(r"text", app, re.I)), None)
	item.check(current, f"no Text Editor in the bar: {order}")
	mark = run.mark()
	inputs("key-down alt", "key tab", "sleep 600")
	opened = run.wait(r"KWL SWITCH open via=keys index=\d+ app=\S+", mark, 5)
	run.shot(item, "switcher")
	inputs("key tab", "sleep 400")
	step = run.wait(r"KWL SWITCH step index=\d+ app=\S+ via=\S+", mark, 5)
	inputs("key esc", "sleep 200", "key-up alt")
	cancel = run.wait(r"KWL SWITCH cancel via=escape", mark, 5)
	expected = order[(order.index(current) + 1) % len(order)]
	item.step("Alt held, Tab, Tab, Esc", f"{opened}; {step}; {cancel}; expected the second on {expected}")
	item.check(opened and aatlib.field(opened, "app") == current, f"BUG-209 reproduced: the switcher opened on {aatlib.field(opened, 'app')}, not the current {current}")
	item.check(step and aatlib.field(step, "app") == expected, f"BUG-209 reproduced: Tab went to {aatlib.field(step, 'app')}, not {expected} (one right in the bar)")
	mark = run.mark()
	inputs("key-down alt", "key tab", "key-up alt", "sleep 600")
	stay = run.wait(r"KWL SWITCH stay via=quick-alt ", mark, 5)
	inputs("key esc")
	item.step("a quick Alt+Tab, then Esc", stay)
	item.check(stay, "a quick Alt+Tab did not leave the switcher open (KWL SWITCH stay via=quick-alt)")
	item.passed(f"opened on {current}, Tab to {expected}")


# BUG-214: the opacity slider starts at 100 and the frosted glass is its own switch.

@run.define("bugs.opacity-frosted")
def opacity_frosted(item):
	old = setting("window.frosted")
	window, since = run.settings(item, "appearance")
	opened = run.lines(r"ZSETTINGS LOOK open opacity=\d+", None)
	item.step("the Appearance page", opened[-1] if opened else "no LOOK open")
	run.shot(item, "page")
	controls = run.controls(since, "appearance")
	item.check(3 in controls, "no Frosted glass switch (control 3)")
	try:
		for value, panels in ((0, "opaque"), (1, "glass")):
			mark = run.mark()
			run.click_control(item, window, controls, 3, "the Frosted glass switch")
			saved = run.wait(rf"ZSETTINGS LOOK set key=window.frosted value={value} error=0", mark, 10)
			applied = run.wait(rf"KWL PREFERENCES key=window.opacity applied value=\d+ panels={panels}", mark, 10)
			time.sleep(1.0)
			item.step(f"clicked the Frosted glass switch ({'off' if value == 0 else 'on'})", f"{saved}; {applied}")
			run.shot(item, panels)
			item.check(saved and applied, f"the switch did not make the panels {panels}")
	finally:
		if old is not None:
			set_setting("window.frosted", old)
	item.person(f"the slider at {aatlib.number(opened[-1], 'opacity') if opened else '?'}% (100 by default) in page.png; the panels solid in opaque.png, frosted in glass.png (BUG-214)")


# BUG-217: the maximized state is the session's: a switch docks or floats the window switched to.

@run.define("bugs.layout-session-switch")
def layout_session(item):
	files = run.launch(item, "Files")
	maximize(item, files)
	mark = run.mark()
	run.launch(item, "Terminal")
	docked = run.lines(r"KWL GLASS open-docked ", mark)
	item.step("Terminal opened over the docked Files", docked[-1] if docked else "no open-docked")
	item.check(docked, "Terminal did not open docked (the session's state)")
	mark = run.mark()
	run.back_to_windowed()
	windowed = run.wait(r"KWL LAYOUT mode=windowed", mark, 5)
	item.step("the bar's restore button (windowed)", windowed)
	item.check(windowed, "the layout did not turn windowed")
	time.sleep(0.8)
	mark = run.mark()
	switch_to(item, "files", mark)
	# Leaving the docked mode by the bar's button floats every window quietly (KWL LAYOUT float-quiet), so Files is
	# floating already and the switch keeps it so ("action=keep"); one still docked would be floated ("action=float").
	floated = run.wait(rf"KWL LAYOUT switch surface={files.surface} action=(float|keep) mode=windowed", mark, 5)
	time.sleep(1.0)
	run.shot(item, "files-floating")
	item.check(floated, "BUG-217 reproduced: Files, docked before, did not float when switched to in the windowed mode")
	now = run.window(files.client, files.surface) or files
	maximize(item, now)
	mark = run.mark()
	switch_to(item, "term", mark)
	docks = run.wait(r"KWL LAYOUT switch surface=\d+ action=dock mode=docked", mark, 5)
	time.sleep(1.0)
	run.shot(item, "terminal-docked")
	item.check(docks, "BUG-217 reproduced: Terminal, floating, did not dock when switched to in the docked mode")
	item.passed("the switched-to window followed the session's mode both ways")


# BUG-218: Phone's body has no padding (as Settings).

@run.define("bugs.phone-padding")
def phone_padding(item):
	phone_store(item)
	try:
		run.launch(item, "Phone")
		run.shot(item, "phone")
		run.stop_programs()
		run.launch(item, "Settings")
		run.shot(item, "settings")
	finally:
		phone_forget()
	item.person("Phone's body reaches the window's edges with the same gap under the title bar as Settings' (BUG-218); the scroll's start is the touch pad's (UAT)")


# BUG-229: the on-screen keyboard comes back after App Home.

@run.define("bugs.osk-home-restore")
def osk_home(item):
	editor(item, "osk229.txt")
	open_panel(item, "flick")
	mark = run.mark()
	run.key("super")
	away = run.wait(r"KWL OSK put-away kind=flick reason=home", mark, 10)
	time.sleep(0.8)
	run.key("esc")
	restored = run.wait(r"KWL OSK restore kind=flick", mark, 10)
	time.sleep(0.8)
	item.step("App Home (the Windows key), then Esc back to Text Editor", f"{away}; {restored}")
	run.shot(item, "restored")
	close_panel("flick")
	item.check(away, "App Home did not put the keyboard away")
	item.check(restored, "BUG-229 reproduced: the keyboard did not come back after App Home")
	item.passed()


# BUG-230: the hint of the keyboard's corner (a look).

@run.define("bugs.osk-pull-hint")
def osk_hint(item):
	editor(item, "osk230.txt")
	width, height = run.screen()
	x, y = width - 8, height - 8
	mark = run.mark()
	inputs(f"move-to {x} {y}", "sleep 150", "down left", "sleep 80", f"move-to {x - 20} {y - 20}", "sleep 60",
		f"move-to {x - 40} {y - 40}", "sleep 400")
	try:
		armed = run.wait(r"KWL OSK armed corner=flick", mark, 5)
		run.shot(item, "hint-short")
		inputs(f"move-to {x - 80} {y - 80}", "sleep 400")
		run.shot(item, "hint-label")
		inputs(f"move-to {x - 130} {y - 130}", "sleep 400")
		run.shot(item, "hint-ready")
	finally:
		inputs("up left", "sleep 700")
	opened = run.wait(r"KWL OSK open kind=flick", mark, 5)
	item.step("pressed in the bottom right corner and pulled 40, 80 and 130 pixels, then let go", f"{armed}; {opened}")
	close_panel("flick")
	item.check(armed and opened, "the corner's pull did not arm and open the flick panel")
	item.person("the quarter disc of glass with \"Keyboard\" as Notes' pull, not a white square (BUG-230), in the three screenshots")


# BUG-231: the full keyboard goes through the input method (a: あ, Space converts).

@run.define("bugs.osk-qwerty-ime")
def osk_qwerty_ime(item):
	old = common.current_method(run)
	common.set_method(run, item, 1)
	try:
		editor(item, "osk231.txt")
		common.to_language(run, item, "ja")
		mark = run.mark()
		open_panel(item, "qwerty")
		rects = qwerty_rects(mark)
		for label in ("a", "space", "Enter"):
			item.check(label in rects, f"no key {label} in KWL OSK qrect")
		sends = []
		for label, code in (("a", 30), ("space", 57), ("Enter", 28)):
			key = run.mark()
			rx, ry, rw, rh = rects[label]
			tap((rx + rw // 2, ry + rh // 2))
			line = run.wait(rf"KWL OSK send via=ime code={code}", key, 5)
			sends.append(line)
			if label == "space":
				time.sleep(0.6)
				run.shot(item, "converting")
		item.step("tapped a, space and Enter", "; ".join(str(line) for line in sends))
		close_panel("qwerty")
		run.key("alt+space")
		text = save(item, "osk231.txt")
		item.step("read the file", repr(text))
		item.check(all(sends), "BUG-231 reproduced: the full keyboard's keys did not go through the input method")
		item.check(any(ord(character) > 0x7f for character in text), f"no Japanese in the file: {text!r}")
	finally:
		if common.current_method(run) != old:
			common.set_method(run, item, old)
	item.passed(f"the full keyboard typed {text.strip()!r} through the input method")


# BUG-233 and BUG-234: Files opens a video with x bits in Video Player, and a command line program in a Terminal
# that stays.

def desktop_icon(item, name: str, since: str) -> tuple[int, int]:
	"""The middle of a desktop icon (the desktop's place on the screen plus the item's cell)."""
	place = run.wait(rf"ZFILES DESKTOP place name={re.escape(name)} column=\d+ row=\d+ x=-?\d+ y=-?\d+", since, 15)
	item.check(place, f"the desktop did not show {name}")
	configure = run.lines(r"ZFILES DESKTOP configure x=-?\d+ y=-?\d+", None)
	dx, dy = (aatlib.number(configure[-1], "x"), aatlib.number(configure[-1], "y")) if configure else (0, 0)
	return dx + aatlib.number(place, "x") + 48, dy + aatlib.number(place, "y") + 40


@run.define("bugs.files-open-programs")
def files_open_programs(item):
	mark = run.mark()
	run.sh(f"mkdir -p {DESKTOP_FOLDER} && cp {aatlib.SAMPLES}/sample.mp4 {DESKTOP_FOLDER}/aat-clip.mp4 && "
		f"chmod 755 {DESKTOP_FOLDER}/aat-clip.mp4 && cp /bin/ls {DESKTOP_FOLDER}/aat-ls && chmod 755 {DESKTOP_FOLDER}/aat-ls && "
		f"chown kei {DESKTOP_FOLDER} {DESKTOP_FOLDER}/aat-clip.mp4 {DESKTOP_FOLDER}/aat-ls")
	try:
		clip = desktop_icon(item, "aat-clip.mp4", mark)
		ls = desktop_icon(item, "aat-ls", mark)
		run.shot(item, "desktop")
		opened = run.mark()
		run.click(*clip, "--count", "2")
		chosen = run.wait(r"ZFILES DESKTOP open name=aat-clip.mp4 via=double-click", opened, 10)
		video = run.wait(r"VIDEOPLAYER (READY|OPEN) ", opened, 20)
		terminal = run.lines(r"ZTERM START ", opened)
		ways = run.lines(r"ZFILES (OPEN|LAUNCH|SPAWN) ", opened)
		item.step(f"double-clicked aat-clip.mp4 (x bits) at {clip[0]},{clip[1]}", f"{chosen}; {ways[-1] if ways else ''}; {video}; terminals {len(terminal)}")
		time.sleep(1.0)
		run.shot(item, "video")
		item.check(chosen, "the desktop did not open the clip")
		item.check(video and not terminal, "BUG-233 reproduced: the video was not opened in Video Player (or a Terminal ran it)")
		run.stop_programs()
		opened = run.mark()
		run.click(*ls, "--count", "2")
		started = run.wait(r"ZTERM START ", opened, 15)
		window = run.mapped_after(opened, 15, size_wait=2.0)
		time.sleep(3.0)
		gone = run.lines(rf"KWL CLIENT gone client={window.client}\b", opened) if window else []
		ways = run.lines(r"ZFILES (OPEN|LAUNCH|SPAWN) ", opened)
		item.step(f"double-clicked aat-ls (a command line program) at {ls[0]},{ls[1]}", f"{ways[-1] if ways else ''}; {started}; window {window}; gone {len(gone)}")
		run.shot(item, "ls")
		item.check(started and window, "BUG-234 reproduced: no Terminal came for the program")
		item.check(not gone, "BUG-234 reproduced: the Terminal went away when the program ended")
	finally:
		run.sh(f"rm -f {DESKTOP_FOLDER}/aat-clip.mp4 {DESKTOP_FOLDER}/aat-ls")
	item.person("the clip plays in Video Player (video.png); ls's output and the end's note stay in the Terminal (ls.png)")


# BUG-237: the applications' icons show what is behind their symbols (a look).

@run.define("bugs.icon-holes")
def icon_holes(item):
	old = setting("appearance.dark")
	try:
		run.home_open(item)
		run.shot(item, "home-light")
		run.key("esc")
		run.launch(item, "Files")
		run.launch(item, "Terminal")
		mark = run.mark()
		inputs("key-down alt", "key tab", "sleep 600")
		try:
			run.wait(r"KWL SWITCH open ", mark, 5)
			run.shot(item, "switcher")
		finally:
			inputs("key esc", "key-up alt")
		mark = run.mark()
		item.step("appearance.dark 1", set_setting("appearance.dark", "1"))
		item.check(run.wait(r"KWL THEME appearance=1", mark, 10), "the desktop did not turn dark")
		run.stop_programs()
		run.home_open(item)
		run.shot(item, "home-dark")
		run.key("esc")
	finally:
		set_setting("appearance.dark", old or "0")
	item.person("the symbols of the icons show the wallpaper (App Home, the bar) or the blurred scene (Alt+Tab), not white (BUG-237)")


# BUG-242: Emacs's M-x shell lays ls / out in columns (a look).

@run.define("bugs.emacs-shell-ls")
def emacs_shell(item):
	window = run.launch(item, "Terminal")
	run.click(*window.middle())
	time.sleep(0.5)
	run.type("emacs -nw")
	run.key("enter")
	time.sleep(4.0)
	run.key("alt+x")
	run.type("shell")
	run.key("enter")
	time.sleep(3.0)
	run.type("stty size; ls /")
	run.key("enter")
	time.sleep(2.0)
	item.step("emacs -nw, M-x shell, stty size; ls /")
	run.shot(item, "shell-ls")
	_, listing = run.sh("ls / | wc -l")
	run.key("ctrl+x", "ctrl+c")
	time.sleep(1.0)
	run.type("yes")
	run.key("enter")
	time.sleep(1.0)
	item.person(f"ls / in the shell buffer: {listing.strip()} names in even columns as in a terminal, nothing broken; stty size above it 0 0 (the shell's pty has no size, BUG-242)")


# BUG-259: PDF Viewer draws its pages again only when a resize ends.

@run.define("bugs.pdf-resize-drag")
def pdf_resize(item):
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/pdfviewer --width=900 --height=600 {aatlib.SAMPLES}/sample.pdf", r"PDFVIEWER READY")
	width, height = run.screen()
	cx, cy = window.x + 900 + 3, window.y + 600 + 3
	item.check(cx < width and cy < height, f"the window's corner {cx},{cy} is off the screen")
	moves = []
	for index in range(1, 31):
		moves += [f"move-to {cx - 10 * index} {cy - 6 * index}", "sleep 30"]
	drag = run.mark()
	inputs(f"move-to {cx} {cy}", "sleep 300", "down left", "sleep 80", *moves, "sleep 700")
	try:
		run.shot(item, "held")
	finally:
		inputs("up left", "sleep 800")
	started = run.wait(r"KWL RESIZE start", drag, 5)
	settled = run.wait(r"PDFVIEWER RESIZE settled width=\d+ height=\d+", drag, 10)
	rasters = run.lines(r"PDFVIEWER RASTER index=0 ", mark)
	item.step("dragged the bottom right corner in by 300x180 over about a second, held, let go",
		f"{started}; {settled}; page 1 drawn {len(rasters)} times")
	run.shot(item, "after")
	item.check(started, "the drag did not start a resize")
	item.check(settled, "no PDFVIEWER RESIZE settled")
	item.check(len(rasters) <= 4, f"BUG-259 reproduced: page 1 drawn {len(rasters)} times (at most 4: at the start and when the size settles)")
	item.passed(f"page 1 drawn {len(rasters)} times")


# BUG-265: a tap and drag on a floating title bar moves the window and does not maximize it.

def title_tap_drag(item, window, touch: bool) -> None:
	"""A first press and release on the title bar, then a second press held and moved 80 pixels (the mouse, or a
	finger), then let go: the window moves, and does not dock."""
	x, y = window.title_point()
	steps = [(x + 10 * n, y + 10 * n) for n in range(1, 9)]
	mark = run.mark()
	if touch:
		inputs(f"tap {x} {y}", "sleep 120", f"touch-down 0 {x} {y}", "sleep 60",
			*[command for px, py in steps for command in (f"touch-move 0 {px} {py}", "sleep 30")], "sleep 100", "touch-up 0")
	else:
		inputs(f"move-to {x} {y}", "click left", "sleep 120", "down left", "sleep 60",
			*[command for px, py in steps for command in (f"move-to {px} {py}", "sleep 30")], "sleep 100", "up left")
	time.sleep(1.0)
	how = "a finger" if touch else "the mouse"
	wait = run.lines(rf"KWL GLASS double-click wait surface={window.surface}\b", mark)
	moved = run.lines(rf"KWL GLASS (double-click moved|moved) surface={window.surface}\b", mark)
	docked = run.lines(rf"KWL GLASS dock surface={window.surface} ", mark)
	now = run.window(window.client, window.surface)
	item.step(f"{how}: a tap, then the second press held and moved by 80,80",
		f"wait {len(wait)}; {moved[-1] if moved else 'no move'}; docks {len(docked)}; now {now}")
	run.shot(item, "touch" if touch else "mouse")
	item.check(not docked, f"BUG-265 reproduced ({how}): the tap and drag maximized the window")
	item.check(moved and now and (now.x - window.x) >= 40, f"the window did not move with {how}'s drag")


@run.define("bugs.title-tap-drag")
def title_drag(item):
	window = run.launch(item, "Files")
	title_tap_drag(item, window, False)
	window = run.window(window.client, window.surface) or window
	title_tap_drag(item, window, True)
	window = run.window(window.client, window.surface) or window
	dock = maximize(item, window)
	item.passed(f"moved by the mouse and a finger without docking; a double click still docks ({aatlib.field(dock, 'via')})")


# BUG-270: a finger from the top edge down opens Wiseview; the bar's taps still work.

@run.define("bugs.touch-top-edge-wiseview")
def top_edge(item):
	run.launch(item, "Files")
	width, height = run.screen()
	mark = run.mark()
	inputs(f"touch-drag {width // 2} 3 {width // 2} 360 20")
	opened = run.wait(r"KWL WISEVIEW (gesture via=top-edge|opening)", mark, 10)
	time.sleep(0.8)
	band = run.lines(r"KWL EDGE band press ", mark)
	item.step("a finger from the top edge down 360 pixels", f"{band[-1] if band else 'no band press'}; {opened}")
	run.shot(item, "wiseview")
	item.check(opened, "BUG-270 reproduced: the swipe from the top edge did not open Wiseview")
	mark = run.mark()
	run.key("esc")
	item.check(run.wait(r"KWL WISEVIEW close", mark, 10), "Wiseview did not close")
	icons = run.lines(r"KWL VOLUME icon x=", None)
	item.check(icons, "no KWL VOLUME icon line")
	x, y, w, h = (aatlib.number(icons[-1], name) for name in ("x", "y", "width", "height"))
	mark = run.mark()
	run.aat("tap", str(x + w // 2), str(y + h // 2))
	popup = run.wait(r"KWL VOLUME popup open", mark, 10)
	item.step("a tap on the bar's volume icon", popup)
	run.key("esc")
	item.check(popup, "a tap on the bar no longer opens its menu")
	item.passed()


# BUG-273: the bar's close button of a docked X11 window ends the X client.

@run.define("bugs.x11-docked-close")
def x11_close(item):
	status, _ = run.sh("test -x /bin/glxtest && test -x /bin/xserver")
	if status != 0:
		item.person("no /bin/glxtest (or /bin/xserver) in this image: build it with config-amd64-aat-bugs.mk")
	try:
		run.as_user("env DISPLAY=:0 /bin/xserver")
		up = 1
		for _ in range(20):
			up, _ = run.sh("test -S /tmp/.X11-unix/X0")
			if up == 0:
				break
			time.sleep(0.5)
		item.step("xserver as kei", f"socket {'there' if up == 0 else 'missing'}")
		item.check(up == 0, "the X server did not start")
		mark = run.mark()
		window = run.open_as_user(item, "env DISPLAY=:0 /bin/glxtest --frames=6000 --delay-ms=30 --token=aat", timeout=30)
		run.shot(item, "glxtest")
		dock = maximize(item, window)
		match = re.search(r"buttons=(-?\d+),", dock)
		item.check(match, "the dock line has no buttons")
		run.shot(item, "docked")
		close = run.mark()
		run.click(int(match.group(1)), aatlib.TITLE_HEIGHT // 2)
		asked = run.wait(rf"KWL GLASS close surface={window.surface} ", close, 5)
		left = True
		for _ in range(10):
			left = program_running("glxtest")
			if not left:
				break
			time.sleep(0.5)
		item.step(f"clicked the bar's close button at {match.group(1)},{aatlib.TITLE_HEIGHT // 2}", f"{asked}; glxtest running {left}")
		run.shot(item, "closed")
		item.check(asked, "BUG-273 reproduced: the press did not reach the close button (no KWL GLASS close)")
		item.check(not left, "BUG-273 reproduced: the close was asked but glxtest did not end")
		item.passed("the docked X11 window closed and glxtest ended")
	finally:
		kill_program("glxtest")
		kill_program("xserver")


# BUG-172 and BUG-191: the key repeat follows the Settings' rate.

def repeat_count(item, rate: int) -> int:
	"""Holds a in a Terminal for 3 s at a repeat rate; returns how many a's came."""
	mark = run.mark()
	item.step(f"keyboard.repeat.rate {rate}", set_setting("keyboard.repeat.rate", str(rate)))
	run.wait(rf"KWL PREFERENCES key=keyboard.repeat.rate applied value={rate}", mark, 5)
	window = run.launch(item, "Terminal")
	run.click(*window.middle())
	time.sleep(0.5)
	name = f"{aatlib.WORK}/repeat{rate}.txt"
	run.type(f"cat > {name}")
	run.key("enter")
	time.sleep(0.8)
	inputs("key-down a", "sleep 3000", "key-up a", "sleep 300", timeout=30)
	run.key("enter", "ctrl+d")
	time.sleep(0.8)
	_, text = run.sh(f"cat {name}")
	count = text.count("a")
	item.step(f"held a for 3 s at {rate} a second", f"{count} a's")
	run.shot(item, f"rate{rate}")
	run.stop_programs()
	return count


@run.define("bugs.key-repeat-rate")
def key_repeat(item):
	old_rate, old_delay = setting("keyboard.repeat.rate"), setting("keyboard.repeat.delay")
	try:
		set_setting("keyboard.repeat.delay", "400")
		slow = repeat_count(item, 5)
		fast = repeat_count(item, 40)
	finally:
		set_setting("keyboard.repeat.rate", old_rate or "25")
		set_setting("keyboard.repeat.delay", old_delay or "400")
	# 3 s held, 0.4 s delay: about 1 + 2.6 x rate (14 at 5, 105 at 40).
	item.check(6 <= slow <= 22, f"BUG-191 reproduced: {slow} a's at 5 a second (about 14 expected)")
	item.check(fast >= 3 * slow, f"BUG-191 reproduced: {fast} a's at 40 a second against {slow} at 5: the rate did not take")
	if 70 <= fast <= 130:
		item.passed(f"{slow} a's at 5/s, {fast} at 40/s")
	item.person(f"{slow} a's at 5/s, {fast} at 40/s (about 105 expected; QEMU may cap it): the rate takes; the steadiness is the UAT's (BUG-172)")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
