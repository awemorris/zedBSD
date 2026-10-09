#!/usr/bin/env python3
"""The automatic helpers of tests/scenarios/desktop/ (WS173 p004): each does its scenario's steps by the same id.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_desktop.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

desktop.touchpad.gestures needs a person's fingers and has no helper.
"""
import re
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_desktop")

# The system bar's launcher: 10 pixels from the left, 26 square, in the middle of the 44-pixel bar (shell.c
# BAR_LAUNCHER_*); the log does not name it.
LAUNCHER = (10 + 13, 22)

# The on-screen keyboard (keyboard.c): the flick panel's keys (a side of the screen's height / 11, 64 to 96, 6 apart,
# 4 x 4 at the bottom of the panel), its title band (36) and the tools' rows (44, the tabs' row 30).
KEY_GAP = 6
FLICK_ROWS = 4
BAND = 36
TOOL_ROW = 44
TOOL_TABS = 30


@run.define("desktop.home.super-key")
def home_super_key(item):
	since = run.mark()
	run.key("super")
	opened = run.wait(r"KWL HOME open via=super", since, 10)
	item.step("Windows key pressed and let go", opened)
	run.shot(item, "open")
	item.check(opened and run.lines(r"KWL SUPER home", since), "App Home did not open by the Windows key")
	since = run.mark()
	run.key("super")
	closed = run.wait(r"KWL HOME close", since, 10)
	item.step("Windows key again", closed)
	item.check(closed, "App Home did not close")
	item.passed()


def power_dialog(item):
	"""Opens App Home's Power Off dialog: Home by the Windows key, "power" typed, the Power Off icon clicked."""
	since = run.home_open(item)
	run.type("power")
	item.check(run.wait(r'KWL HOME search query="power"', since, 10), "Home did not search for power")
	icons = run.lines(r'KWL HOME icon name="Power Off" x=-?\d+ y=-?\d+', since)
	item.step("typed 'power' in App Home", icons[-1] if icons else "no icon")
	item.check(icons, "Home shows no Power Off icon")
	x, y = (int(value) for value in re.search(r"x=(-?\d+) y=(-?\d+)", icons[-1]).groups())
	mark = run.mark()
	run.click(x, y)
	opened = run.wait(r"KWL POWER dialog open source=home ", mark, 10)
	time.sleep(0.6)
	item.step(f"clicked the Power Off icon at {x},{y}", opened)
	item.check(opened, "no KWL POWER dialog open")
	item.check(not run.lines(r"KWL SESSION logout", mark), "the session ended without asking")
	return opened


@run.define("desktop.home.power-off-dialog")
def home_power_off(item):
	opened = power_dialog(item)
	run.shot(item, "dialog")
	mark = run.mark()
	run.key("esc")
	cancelled = run.wait(r"KWL POWER choice=cancel via=escape", mark, 10)
	time.sleep(0.5)
	item.step("Esc", cancelled)
	run.shot(item, "cancelled")
	item.check(cancelled, "Esc did not cancel")
	power_dialog(item)
	mark = run.mark()
	run.click(20, 400)
	outside = run.wait(r"KWL POWER choice=cancel via=outside", mark, 10)
	item.step("clicked outside the card", outside)
	item.check(outside, "a click outside did not cancel")
	item.person(f"the darkened desktop and the card in the first screenshot ({opened}); Log Out by hand (step 4)")


@run.define("desktop.home.switch-running")
def home_switch_running(item):
	files = run.launch(item, "Files")
	run.launch(item, "Terminal")
	since = run.home_open(item)
	run.type("files")
	item.check(run.wait(r'KWL HOME search query="files"', since, 10), "Home did not search for files")
	icons = run.lines(r'KWL HOME icon name="Files" x=-?\d+ y=-?\d+', since)
	item.check(icons, "Home shows no Files icon")
	x, y = (int(value) for value in re.search(r"x=(-?\d+) y=(-?\d+)", icons[-1]).groups())
	mark = run.mark()
	run.click(x, y)
	switched = run.wait(rf"KWL HOME switch name=Files surface={files.surface} client={files.client}\b", mark, 10)
	time.sleep(1.0)
	launched = run.lines(r"KWL HOME launch name=Files ", mark)
	mapped = run.lines(r"KWL MAP client=", mark)
	item.step(f"clicked the Files icon at {x},{y} with Files running", f"{switched}; launches {len(launched)}; maps {len(mapped)}")
	item.check(switched, "Home did not switch to the running Files")
	item.check(not launched and not mapped, "a second Files was started")
	run.home_open(item)
	run.shot(item, "running-marks")
	run.key("esc")
	item.person("the short lines under Files and Terminal in the screenshot")


@run.define("desktop.home.open-latency")
def home_open_latency(item):
	since = run.mark()
	run.key("super")
	cover = run.wait(r"KWL HOME layer=cover after_ms=\d+", since, 10)
	content = run.wait(r"KWL HOME layer=content after_ms=\d+", since, 10)
	time.sleep(0.4)
	item.step("Windows key pressed and let go", f"{cover}; {content}")
	run.shot(item, "open")
	run.key("super")
	item.check(cover and content, "no KWL HOME layer=cover or layer=content")
	item.person(f"cover after_ms={aatlib.number(cover, 'after_ms')} (16 or less), content after_ms={aatlib.number(content, 'after_ms')} (150 or less); the icons rising in")


@run.define("desktop.home.launcher-button")
def home_launcher(item):
	since = run.mark()
	run.click(*LAUNCHER)
	opened = run.wait(r"KWL HOME open via=launcher", since, 10)
	item.step(f"clicked the launcher at {LAUNCHER[0]},{LAUNCHER[1]}", opened)
	run.shot(item, "open")
	item.check(opened, "App Home did not open from the launcher")
	since = run.mark()
	run.key("esc")
	closed = run.wait(r"KWL HOME close", since, 10)
	item.step("Esc", closed)
	item.check(closed, "App Home did not close")
	item.passed()


@run.define("desktop.wiseview.super-tab")
def wiseview(item):
	run.launch(item, "Files")
	since = run.mark()
	run.key("super+tab")
	opening = run.wait(r"KWL WISEVIEW opening", since, 10)
	time.sleep(0.6)
	home = run.lines(r"KWL HOME open", since)
	item.step("Windows+Tab", opening)
	run.shot(item, "wiseview")
	item.check(opening, "Wiseview did not open")
	item.check(not home, "App Home opened as well")
	since = run.mark()
	run.key("esc")
	closed = run.wait(r"KWL WISEVIEW close", since, 10)
	item.step("Esc", closed)
	item.check(closed, "Wiseview did not close")
	item.passed()


@run.define("desktop.keyboard.super-not-to-client")
def super_not_to_client(item):
	window = run.launch(item, "Terminal")
	run.click(*window.middle())
	run.type(f"cat > {aatlib.WORK}/super.txt")
	run.key("enter")
	time.sleep(0.8)
	since = run.mark()
	run.key("super")
	opened = run.wait(r"KWL HOME open", since, 10)
	time.sleep(0.5)
	run.key("super")
	closed = run.wait(r"KWL HOME close", since, 10)
	item.step("Windows key twice while cat waits", f"{opened}; {closed}")
	item.check(opened and closed, "App Home did not open and close")
	time.sleep(0.8)
	run.type("x")
	run.key("enter", "ctrl+d")
	time.sleep(1.0)
	_, text = run.sh(f"cat {aatlib.WORK}/super.txt")
	item.step("x, Enter, Ctrl+D", repr(text))
	run.shot(item, "terminal")
	item.check(text == "x\n", f"the file is {text!r}, not 'x'")
	item.passed()


@run.define("desktop.windows.move-by-title")
def move_by_title(item):
	window = run.launch(item, "Files")
	x, y = window.title_point()
	since = run.mark()
	run.drag(x, y, x + 120, y + 80)
	moved = run.wait(rf"KWL GLASS moved surface={window.surface} ", since, 10)
	now = run.window(window.client, window.surface)
	item.step(f"dragged the title bar from {x},{y} by 120,80", moved)
	run.shot(item, "moved")
	item.check(moved and now, "no KWL GLASS moved")
	dx, dy = now.x - window.x, now.y - window.y
	item.check(abs(dx - 120) <= 8 and abs(dy - 80) <= 8, f"moved by {dx},{dy}, not 120,80")
	item.passed(f"moved by {dx},{dy}")


def maximize(item, window):
	"""Double-clicks a window's title bar; returns the dock line and the resize line."""
	since = run.mark()
	run.click(*window.title_point(), "--count", "2")
	dock = run.wait(rf"KWL GLASS dock surface={window.surface} via=double-click", since, 10)
	resized = run.wait(rf"KWL GLASS resized surface={window.surface} docked=1 ", since, 10)
	item.step("double-clicked the title bar", f"{dock}; {resized}")
	return dock, resized


@run.define("desktop.windows.maximize-double-click")
def maximize_double_click(item):
	window = run.launch(item, "Files")
	dock, resized = maximize(item, window)
	run.shot(item, "maximized")
	item.check(dock, "no KWL GLASS dock ... via=double-click")
	item.check(resized, "no KWL GLASS resized ... docked=1")
	after = aatlib.number(resized, "after_ms")
	if after is not None and after <= 200:
		item.passed(f"after_ms={after}")
	item.person(f"after_ms={after} (the goal is 200 or less, BUG-179)")


@run.define("desktop.windows.unmaximize-drag")
def unmaximize_drag(item):
	window = run.launch(item, "Files")
	dock, _ = maximize(item, window)
	item.check(dock, "the window did not dock")
	time.sleep(0.8)
	match = re.search(r"title=(-?\d+)", dock)
	start = (int(match.group(1)) + 40 if match else 160, 22)
	since = run.mark()
	run.aat("move", str(start[0]), str(start[1]))
	run.aat("down")
	for step in range(1, 7):
		run.aat("move", str(start[0] + step * 10), str(start[1] + step * 50))
		if step == 3:
			run.shot(item, "during")
	run.aat("up")
	undock = run.wait(rf"KWL GLASS undock surface={window.surface} ", since, 10)
	item.step(f"dragged the docked title at {start[0]},{start[1]} down by 300", undock)
	run.shot(item, "after")
	item.check(undock, "no KWL GLASS undock")
	item.person("the window keeps its own size under the pointer, without going back to maximized first (BUG-180)")


@run.define("desktop.windows.open-maximized")
def open_maximized(item):
	window = run.launch(item, "Files")
	dock, _ = maximize(item, window)
	item.check(dock, "Files did not dock")
	time.sleep(0.8)
	since = run.mark()
	run.launch(item, "Text Editor")
	docked = run.lines(r"KWL GLASS open-docked ", since)
	item.step("opened Text Editor over the maximized Files", docked[0] if docked else "no open-docked")
	run.shot(item, "opened")
	item.check(docked, "Text Editor did not open maximized (no KWL GLASS open-docked)")
	item.passed()


@run.define("desktop.windows.close-minimize")
def close_minimize(item):
	window = run.launch(item, "Files")
	since = run.mark()
	run.click(*window.button_point(2))
	minimized = run.wait(rf"KWL GLASS minimize surface={window.surface}", since, 10)
	item.step("clicked the minimize button", minimized)
	run.shot(item, "minimized")
	item.check(minimized, "no KWL GLASS minimize")
	icons = [line for line in run.lines(r"KWL APPS icon app=", None) if re.search(r"app=\S*files", line, re.I)]
	item.check(icons, "no apps bar icon for Files (KWL APPS icon)")
	x, y, width, height = (aatlib.number(icons[-1], name) for name in ("x", "y", "width", "height"))
	run.click(x + width // 2, y + height // 2)
	time.sleep(1.0)
	item.step(f"clicked Files' icon in the apps bar at {x + width // 2},{y + height // 2}", icons[-1])
	run.shot(item, "restored")
	run.close(item, window)
	item.person("the restored window shows again in the second screenshot")


@run.define("desktop.bar.status-icons")
def status_icons(item):
	network = run.lines(r"KWL NETWORK icon x=", None)
	volume = run.lines(r"KWL VOLUME icon x=", None)
	item.step("read the bar's icons in the log", f"{network[-1] if network else 'no network'}; {volume[-1] if volume else 'no volume'}")
	run.shot(item, "bar")
	item.check(network and volume, "the bar's icons are not in the log")
	item.person("the clock, the battery (with a battery), the network and the volume side by side")


def icon_middle(line: str) -> tuple[int, int]:
	"""The middle of an icon's x y width height."""
	x, y, width, height = (aatlib.number(line, name) for name in ("x", "y", "width", "height"))
	return x + width // 2, y + height // 2


@run.define("desktop.bar.network-details")
def network_details(item):
	icons = run.lines(r"KWL NETWORK icon x=", None)
	item.check(icons, "no KWL NETWORK icon line")
	point = icon_middle(icons[-1])
	since = run.mark()
	run.click(*point, "--with", "alt")
	opened = run.wait(r"KWL NETWORK info open", since, 10)
	time.sleep(1.5)
	rows = run.lines(r"KWL NETWORK info row label=", since)
	item.step(f"Alt+click on the network icon at {point[0]},{point[1]}", f"{opened}; {len(rows)} rows")
	run.shot(item, "details")
	item.check(opened, "the details did not open")
	values = {}
	for row in rows:
		match = re.search(r"label=(.*?) value=(.*)$", row)
		if match:
			values[match.group(1).strip()] = match.group(2).strip()
	for label in ("Interface", "IPv4 address", "MAC address"):
		item.check(label in values, f"no {label} row (rows: {', '.join(values)})")
	_, ifconfig = run.sh("ifconfig -a")
	item.step("ifconfig -a", f"address {values['IPv4 address']}, MAC {values['MAC address']}")
	item.check(values["IPv4 address"] in ifconfig, f"{values['IPv4 address']} is not in ifconfig")
	item.check(values["MAC address"].lower() in ifconfig.lower(), f"{values['MAC address']} is not in ifconfig")
	since = run.mark()
	run.key("esc")
	closed = run.wait(r"KWL NETWORK info close", since, 10)
	item.step("Esc", closed)
	item.check(closed, "the details did not close")
	# A plain click opens the status pill's panel (WS192), whose Wi-Fi row opens the menu.
	since = run.mark()
	run.click(*point)
	panel = run.wait(r"KWL STATUS panel open ", since, 10)
	row = run.wait(r"KWL STATUS item name=wifi x=", since, 5)
	item.check(panel and row, "a plain click did not open the status panel")
	run.click(aatlib.number(row, "x") + 30, aatlib.number(row, "y") + aatlib.number(row, "height") // 2)
	menu = run.wait(r"KWL NETWORK open", since, 10)
	run.shot(item, "menu")
	run.key("esc")
	item.step("a plain click (the panel), its Wi-Fi row, then Esc", f"{panel}; {menu}")
	item.check(menu, "the panel's Wi-Fi row did not open the menu")
	item.passed(f"{values['Interface']} {values['IPv4 address']}")


@run.define("desktop.bar.volume-slider")
def volume_slider(item):
	icons = run.lines(r"KWL VOLUME icon x=", None)
	item.check(icons, "no KWL VOLUME icon line")
	before = run.lines(r"KWL VOLUME (restored|set) value=", None)
	old = aatlib.number(before[-1], "value") if before else None
	# A click on the icon opens the status pill's panel (WS192), whose sound row has the slider.
	since = run.mark()
	run.click(*icon_middle(icons[-1]))
	popup = run.wait(r"KWL STATUS panel open ", since, 10)
	slider = run.wait(r"KWL STATUS item name=volume x=", since, 5)
	item.step("clicked the volume icon", f"{popup}; {slider}")
	item.check(popup and slider, "the panel did not open")
	# Without a sound device (QEMU without audio) the controls do nothing by design (volume.c, T1-202c).
	if aatlib.number(popup, "sound") == 0:
		run.shot(item, "no-sound")
		run.key("esc")
		item.person("no sound device here (sound=0): the slider is shown inert; drag it on a machine with sound")
	# The knob's middle travels the track less the knob (28): 0% at the track's left + 14 (status-panel.c).
	left = aatlib.number(slider, "x") + 14
	width = aatlib.number(slider, "width") - 28
	y = aatlib.number(slider, "y") + aatlib.number(slider, "height") // 2
	since = run.mark()
	points = [(left + width * (20 + 60 * step / 10) / 100, y) for step in range(11)]
	run.press_path(points, pause=0.08)
	set_line = run.wait(r"KWL VOLUME set value=\d+ .*final=1", since, 10)
	feedback = run.lines(r"KWL VOLUME feedback", since)
	began = time.monotonic()
	run.shot(item, "after-drag")
	answered = time.monotonic() - began
	item.step("dragged the slider from 20% to 80%", f"{set_line}; {len(feedback)} feedback sounds; shot in {answered:.1f} s")
	item.check(set_line, "no final KWL VOLUME set")
	value = aatlib.number(set_line, "value")
	item.check(value is not None and 70 <= value <= 90, f"the volume is {value}, not about 80")
	item.check(len(feedback) <= 1, f"{len(feedback)} feedback sounds (BUG-170)")
	item.check(answered < 5.0, f"the screen took {answered:.1f} s after the drag")
	if old is not None:
		run.press_path([(left + width * value / 100, y), (left + width * old / 100, y)], pause=0.1)
		item.step(f"put the volume back to {old}")
	run.key("esc")
	item.passed(f"value {value}, {len(feedback)} feedback sound(s)")


def island_middle(item) -> tuple[int, int]:
	"""The middle of the system bar's status pill (shell.c logs its left and width)."""
	lines = run.lines(r"KWL GLASS status left=-?\d+ width=\d+", None)
	item.check(lines, "no KWL GLASS status line")
	return aatlib.number(lines[-1], "left") + aatlib.number(lines[-1], "width") // 2, 22


@run.define("desktop.bar.status-panel")
def status_panel(item):
	# WS192: a click or a tap anywhere on the status pill opens the glass control panel.
	island = island_middle(item)
	since = run.mark()
	run.click(*island)
	opened = run.wait(r"KWL STATUS panel open ", since, 10)
	items = run.lines(r"KWL STATUS item name=", since)
	item.step(f"clicked the status pill at {island[0]},{island[1]}", f"{opened}; {len(items)} items")
	run.shot(item, "mouse")
	item.check(opened, "a click on the status pill did not open the panel")
	since = run.mark()
	run.key("esc")
	item.check(run.wait(r"KWL STATUS panel close via=key", since, 5), "Esc did not close the panel")
	since = run.mark()
	run.aat("tap", str(island[0]), str(island[1]))
	opened = run.wait(r"KWL STATUS panel open ", since, 10)
	run.shot(item, "touch")
	item.step("tapped the status pill", opened)
	item.check(opened, "a tap on the status pill did not open the panel")
	since = run.mark()
	run.aat("tap", "200", "600")
	item.check(run.wait(r"KWL STATUS panel close via=outside", since, 5), "a tap outside did not close the panel")
	since = run.mark()
	run.aat("tap", str(island[0]), str(island[1]))
	opened = run.wait(r"KWL STATUS panel open ", since, 10)
	item.check(opened, "the panel did not open again")
	rows = {aatlib.field(line, "name"): line for line in run.lines(r"KWL STATUS item name=", since)}
	done = []
	if "input" in rows:
		row = rows["input"]
		point = (str(aatlib.number(row, "x") + 40), str(aatlib.number(row, "y") + aatlib.number(row, "height") // 2))
		# The input method's language before the tap, put back after it (T1-494: Japanese left on turned the
		# next scenarios' typing in App Home into kana).
		said = run.lines(r"KWL IME (app key=\S+ )?language=\S+", None)
		old = re.search(r"language=(\S+)", said[-1]).group(1) if said else "direct"
		mark = run.mark()
		run.aat("tap", *point)
		next_line = run.wait(r"KWL IME indicator next via=panel", mark, 5)
		item.step("tapped the input row", next_line)
		item.check(next_line, "the input row did not ask for the next language")
		back = run.lines(rf"KWL IME language={re.escape(old)}$", mark)
		for _ in range(4):
			if back:
				break
			mark = run.mark()
			run.aat("tap", *point)
			back = run.wait(rf"KWL IME language={re.escape(old)}$", mark, 3)
		item.step(f"tapped the input row until {old} again", back[-1] if isinstance(back, list) and back else back)
		item.check(back, f"the input row did not come back to {old}")
		done.append("input")
	if aatlib.number(opened, "sound") == 1 and "mute" in rows:
		row = rows["mute"]
		point = (aatlib.number(row, "x") + aatlib.number(row, "width") // 2, aatlib.number(row, "y") + aatlib.number(row, "height") // 2)
		mark = run.mark()
		run.aat("tap", str(point[0]), str(point[1]))
		muted = run.wait(r"KWL VOLUME set value=\d+ muted=\d+ via=panel-mute", mark, 5)
		run.aat("tap", str(point[0]), str(point[1]))
		item.step("tapped Mute twice", muted)
		item.check(muted, "the panel's Mute did nothing")
		done.append("mute")
	run.shot(item, "rows")
	row = rows.get("wifi")
	item.check(row, "no Wi-Fi row")
	mark = run.mark()
	run.aat("tap", str(aatlib.number(row, "x") + 30), str(aatlib.number(row, "y") + aatlib.number(row, "height") // 2))
	menu = run.wait(r"KWL NETWORK open", mark, 10)
	closed = run.lines(r"KWL STATUS panel close via=item", mark)
	run.shot(item, "network-menu")
	run.key("esc")
	item.step("tapped the Wi-Fi row", f"{menu}; panel closed {len(closed)}")
	item.check(menu and closed, "the Wi-Fi row did not open the network's menu in the panel's place")
	done.append("wifi")
	item.person("the panel on the glass at the top right, under the clock's pill, with large rows (mouse.png, touch.png, rows.png); the network's menu at the top right (network-menu.png)")
	item.passed(f"opened by click and tap; {', '.join(done)}")


@run.define("desktop.startup.wallpaper-time")
def wallpaper_time(item):
	line = run.lines(r"KWL STARTUP step=wallpaper ms=", None)
	item.step("read KWL STARTUP step=wallpaper", line[0] if line else "none")
	item.check(line, "no KWL STARTUP step=wallpaper line")
	ms = aatlib.number(line[0], "ms")
	if ms is not None and ms <= 2000:
		item.passed(f"{ms} ms")
	item.person(f"the wallpaper took {ms} ms (over 2000)")


# Appearance (Settings).

@run.define("desktop.appearance.dark-mode")
def dark_mode(item):
	files = run.launch(item, "Files")
	window, since = run.settings(item, "appearance")
	controls = run.controls(since, "appearance")
	for value in (1, 0):
		mark = run.mark()
		run.click_control(item, window, controls, 2, "the Dark appearance switch")
		theme = run.wait(rf"KWL THEME appearance={value}", mark, 10)
		settings = run.wait(rf"ZSETTINGS APPEARANCE appearance={value}", mark, 10)
		time.sleep(1.0)
		item.step(f"clicked the Dark appearance switch ({'dark' if value else 'light'})", f"{theme}; {settings}")
		run.shot(item, "dark" if value else "light")
		item.check(theme and settings, f"appearance {value} did not reach the compositor and Settings")
	item.person("the desktop, Settings and Files are dark in the first screenshot, light in the second")


@run.define("desktop.appearance.accent")
def accent(item):
	files = run.launch(item, "Files")
	window, since = run.settings(item, "appearance")
	controls = run.controls(since, "appearance")
	item.check(90 in controls and 91 in controls, "no accent swatches (controls 90 and 91)")
	for index, name in ((1, "purple"), (0, "blue")):
		mark = run.mark()
		run.click_control(item, window, controls, 90 + index, f"the {name} accent swatch")
		settings = run.wait(rf"ZSETTINGS ACCENT index={index}\b", mark, 10)
		theme = run.wait(rf"KWL THEME appearance=\d+ accent={index}\b", mark, 10)
		files_line = run.wait(rf"ZFILES ACCENT index={index}\b", mark, 3)
		time.sleep(1.0)
		item.step(f"clicked the {name} accent swatch", f"{settings}; {theme}; Files: {files_line or 'no line in the log'}")
		run.shot(item, name)
		item.check(settings and theme, f"accent {index} did not reach Settings and the compositor")
		item.check(files_line, f"accent {index} did not reach the desktop's Files (ZFILES ACCENT index={index}, BUG-262)")
	item.person("Settings' and Files' highlights are purple in the first screenshot, blue in the second")


@run.define("desktop.appearance.window-opacity")
def window_opacity(item):
	window, since = run.settings(item, "appearance")
	opened = run.lines(r"ZSETTINGS LOOK open opacity=", None)
	old = aatlib.number(opened[-1], "opacity") if opened else None
	controls = run.controls(since, "appearance")
	item.check(1 in controls, "no opacity slider (control 1)")
	x, y, width, height = controls[1]
	left, right, middle = window.x + x + 2, window.x + x + width - 2, window.y + y + height // 2
	if old == 100:
		# Opaque already (an earlier scenario left it so, T1-320): the compositor applies only a change, so first
		# the slider's left end.
		mark = run.mark()
		run.drag((left + right) // 2, middle, left - 40, middle, steps=10)
		lowered = run.wait(r"ZSETTINGS LOOK set key=window.opacity value=\d+ error=0", mark, 10)
		item.step("dragged the opacity slider to its left end first", lowered or "")
		time.sleep(0.5)
	mark = run.mark()
	run.drag((left + right) // 2, middle, right + 40, middle, steps=10)
	saved = run.wait(r"ZSETTINGS LOOK set key=window.opacity value=100 error=0", mark, 10)
	applied = run.wait(r"KWL PREFERENCES key=window.opacity applied value=100", mark, 10)
	item.step("dragged the opacity slider to its right end", f"{saved}; {applied}")
	time.sleep(1.0)
	run.shot(item, "opaque")
	item.check(saved and applied, "Opaque was not saved and applied")
	if old is not None and old != 100:
		target = left + (right - left) * (old - 85) / 15
		run.drag(right - 2, middle, target, middle, steps=10)
		item.step(f"put the opacity back to {old}")
	item.person("no wallpaper shows through the windows in the screenshot (BUG-171)")


@run.define("desktop.appearance.wallpaper")
def wallpaper(item):
	window, since = run.settings(item, "wallpaper")
	ready = run.wait(r"ZSETTINGS LOOK pictures ready count=\d+", since, 20)
	opened = run.lines(r"ZSETTINGS LOOK open opacity=", None)
	old = aatlib.field(opened[-1], "wallpaper") if opened else None
	time.sleep(1.0)
	controls = run.controls(since, "wallpaper")
	tiles = sorted(index for index in controls if index >= 100)
	item.step("waited for the pictures", f"{ready}; tiles {tiles}")
	run.shot(item, "page")
	item.check(len(tiles) >= 2, "fewer than two pictures")
	chosen = None
	for index in tiles:
		mark = run.mark()
		began = time.monotonic()
		run.click_control(item, window, controls, index, f"picture {index - 100}")
		saved = run.wait(r"ZSETTINGS LOOK set key=wallpaper value=\S+ error=0", mark, 10)
		if saved and aatlib.field(saved, "value") != old:
			applied = run.wait(r"KWL PREFERENCES key=wallpaper applied", mark, 10)
			took = time.monotonic() - began
			chosen = (index, saved, applied, took)
			break
	item.check(chosen, "no picture other than the current one was taken")
	index, saved, applied, took = chosen
	item.step(f"clicked picture {index - 100}", f"{saved}; {applied}; {took:.1f} s")
	time.sleep(0.5)
	run.shot(item, "changed")
	item.check(applied and took <= 3.0, f"the wallpaper was not applied within 2 s ({took:.1f} s with the tools' delay)")
	for back in tiles:
		mark = run.mark()
		run.click_control(item, window, controls, back, "a picture")
		again = run.wait(r"ZSETTINGS LOOK set key=wallpaper value=\S+ error=0", mark, 10)
		if again and aatlib.field(again, "value") == old:
			item.step("put the wallpaper back", again)
			break
	item.person("the new wallpaper shows in the second screenshot")


# The input method.

@run.define("desktop.input-method.choose-method")
def choose_method(item):
	old = common.current_method(run)
	window, since = run.settings(item, "languages")
	controls = run.controls(since, "languages")
	order = [method for method in (2, 1, 0) if method != old] + [old]
	for method in order:
		index = common.METHOD_SWITCHES[method]
		mark = run.mark()
		run.click_control(item, window, controls, index, f"method {method}")
		chosen = run.wait(rf"ZSETTINGS LANGUAGES ime method={method}", mark, 10)
		desktop = run.wait(rf"KWL IME method={method}", mark, 10)
		item.step(f"clicked the switch of method {method}", f"{chosen}; {desktop}")
		run.shot(item, f"method-{method}")
		item.check(chosen and desktop, f"method {method}: not taken")
	item.passed(f"went through 2, 1, 0 and back to {old}")


def editor(item, name: str):
	"""Opens Text Editor on a new file of /tmp/aat-work as the session's user, the pointer in its text."""
	run.sh(f"rm -f {aatlib.WORK}/{name}")
	window = run.open_as_user(item, f"/bin/textedit {aatlib.WORK}/{name}")
	run.click(*window.middle())
	time.sleep(0.5)
	return window


def save(item, name: str) -> str:
	"""Ctrl+S, and the file's text once TEXTEDIT SAVE says so."""
	mark = run.mark()
	run.key("ctrl+s")
	line = run.wait(rf"TEXTEDIT SAVE path={aatlib.WORK}/{re.escape(name)}", mark, 10)
	item.step("Ctrl+S", line)
	item.check(line, "no TEXTEDIT SAVE")
	_, text = run.sh(f"cat {aatlib.WORK}/{name}")
	return text


def japanese_in_editor(item, method: int, name: str, keys: str) -> None:
	"""The steps of japanese-textedit and skk-textedit."""
	old = common.current_method(run)
	common.set_method(run, item, method)
	try:
		editor(item, name)
		# The Japanese engine's language is "ja", SKK's "skk" (userland/desktop/ime, T1-202c).
		language = "skk" if method == 2 else "ja"
		common.to_language(run, item, language)
		run.type(keys)
		run.key("space")
		time.sleep(0.8)
		run.shot(item, "converting")
		run.key("enter")
		time.sleep(0.5)
		item.step(f"typed {keys!r}, Space, Enter")
		run.shot(item, "committed")
		data = save(item, name)
		text = data
		item.step("read the file", repr(text))
		item.check(any(ord(character) > 0x7f for character in text), f"no Japanese in the file: {text!r}")
		# Back to direct input for the scenarios after this one.
		run.key("alt+space")
	finally:
		if common.current_method(run) != old:
			common.set_method(run, item, old)
	item.person(f"saved {text.strip()!r}; the conversion's look in the screenshots (BUG-139)")


@run.define("desktop.input-method.japanese-textedit")
def japanese_textedit(item):
	japanese_in_editor(item, 1, "ja.txt", "nihongo")


@run.define("desktop.input-method.skk-textedit")
def skk_textedit(item):
	japanese_in_editor(item, 2, "skk.txt", "Nihongo")


# The on-screen keyboard.

def corner_swipe(item, corner: str) -> None:
	"""The diagonal drag from a bottom corner (right: the flick panel, left: QWERTY)."""
	width, height = run.screen()
	if corner == "right":
		run.drag(width - 3, height - 3, width - 203, height - 203, steps=12)
	else:
		run.drag(2, height - 3, 202, height - 203, steps=12)


def open_panel(item, kind: str) -> tuple[int, int, int, int]:
	"""Opens a panel; returns its x, y, width and height."""
	mark = run.mark()
	corner_swipe(item, "right" if kind == "flick" else "left")
	line = run.wait(rf"KWL OSK open kind={kind} ", mark, 10)
	item.step(f"swiped from the bottom {'right' if kind == 'flick' else 'left'} corner", line)
	item.check(line, f"the {kind} panel did not open")
	time.sleep(0.6)
	return tuple(aatlib.number(line, name) for name in ("x", "y", "width", "height"))


def close_panel(item, kind: str) -> None:
	"""Closes a panel with the same swipe."""
	mark = run.mark()
	corner_swipe(item, "right" if kind == "flick" else "left")
	line = run.wait(rf"KWL OSK close kind={kind}", mark, 10)
	item.step(f"the same swipe again", line)
	item.check(line, f"the {kind} panel did not close")


def flick_key(panel, row: int, column: int) -> tuple[int, int]:
	"""The middle of a flick key."""
	px, py, pw, ph = panel
	key = min(96, max(64, run.screen()[1] // 11))
	x = px + KEY_GAP + column * (key + KEY_GAP)
	y = py + ph - (FLICK_ROWS - row) * (key + KEY_GAP)
	return x + key // 2, y + key // 2


def tap(point) -> None:
	"""A short press and release at a point."""
	run.press_path([point, point], pause=0.06)
	time.sleep(0.4)


def kana_face(item, panel) -> None:
	"""Turns the flick panel to its kana face (the face key, row 4 column 4), from the faces the log said."""
	for _ in range(3):
		faces = run.lines(r"KWL OSK face name=\S+", None)
		face = aatlib.field(faces[-1], "name") if faces else "kana"
		if face == "kana":
			return
		tap(flick_key(panel, 3, 3))
	item.failed("the flick panel did not turn to kana")


@run.define("desktop.osk.open-close")
def osk_open_close(item):
	for kind in ("flick", "qwerty"):
		open_panel(item, kind)
		run.shot(item, kind)
		if kind == "qwerty":
			item.check(run.lines(r"KWL OSK qrect ", None), "no KWL OSK qrect lines")
		close_panel(item, kind)
	item.passed()


@run.define("desktop.osk.qwerty-type")
def osk_qwerty(item):
	editor(item, "qwerty.txt")
	mark = run.mark()
	open_panel(item, "qwerty")
	rects = {}
	for line in run.lines(r"KWL OSK qrect ", mark):
		label = aatlib.field(line, "label")
		rects[label] = tuple(aatlib.number(line, name) for name in ("x", "y", "width", "height"))
	for letter in "abc":
		item.check(letter in rects, f"no key {letter} in KWL OSK qrect")
		x, y, width, height = rects[letter]
		tap((x + width // 2, y + height // 2))
	sent = run.lines(r"KWL OSK send ", mark)
	item.step("tapped a, b and c", f"{len(sent)} sends")
	run.shot(item, "typed")
	close_panel(item, "qwerty")
	text = save(item, "qwerty.txt")
	item.step("read the file", repr(text))
	item.check(text.strip() == "abc", f"the file is {text!r}")
	item.passed()


@run.define("desktop.osk.prediction")
def osk_prediction(item):
	old = common.current_method(run)
	common.set_method(run, item, 1)
	editor(item, "predict.txt")
	panel = open_panel(item, "flick")
	kana_face(item, panel)
	mark = run.mark()
	tap(flick_key(panel, 0, 1))
	wa = flick_key(panel, 3, 1)
	run.press_path([wa, (wa[0], wa[1] - 15), (wa[0], wa[1] - 30)], pause=0.05)
	predictions = run.wait(r"KWL OSK predictions .*reading=かん count=[1-9]", mark, 10)
	item.step("tapped か, flicked わ up (ん)", predictions)
	run.shot(item, "predictions")
	item.check(predictions, "no predictions for かん")
	slots = run.lines(r"KWL OSK crect slot=0 ", mark)
	item.check(slots, "no KWL OSK crect slot=0")
	x, y, width, height = (aatlib.number(slots[-1], name) for name in ("x", "y", "width", "height"))
	mark = run.mark()
	tap((x + width // 2, y + height // 2))
	commit = run.wait(r"KWL OSK candidate commit sent=1 slot=0 word=\S+", mark, 10)
	item.step("tapped the first candidate", commit)
	run.shot(item, "chosen")
	item.check(commit, "no candidate commit")
	word = aatlib.field(commit, "word")
	close_panel(item, "flick")
	text = save(item, "predict.txt")
	item.step("read the file", repr(text))
	if common.current_method(run) != old:
		common.set_method(run, item, old)
	item.check(word and word in text, f"{word!r} is not in the file {text!r}")
	item.person(f"chose {word}; the candidates' tab in the screenshot")


@run.define("desktop.osk.emoji")
def osk_emoji(item):
	editor(item, "emoji.txt")
	panel = open_panel(item, "flick")
	px, py, pw, _ = panel
	column = (pw - 5 * KEY_GAP) // 4 + KEY_GAP
	tab = (px + KEY_GAP + 3 * column + (column - KEY_GAP) // 2, py + BAND + KEY_GAP + TOOL_ROW + KEY_GAP + TOOL_TABS // 2)
	mark = run.mark()
	tap(tab)
	face = run.wait(r"KWL OSK tool face=emoji", mark, 10)
	item.step(f"tapped the emoji tab at {tab[0]},{tab[1]}", face)
	run.shot(item, "emoji")
	item.check(face, "the emoji face did not open")
	cells = run.lines(r"KWL OSK erect category=\d+ index=0 ", mark)
	item.check(cells, "no KWL OSK erect index=0")
	x, y, width, height = (aatlib.number(cells[-1], name) for name in ("x", "y", "width", "height"))
	mark = run.mark()
	tap((x + width // 2, y + height // 2))
	commit = run.wait(r"KWL OSK emoji commit sent=1 text=\S+", mark, 10)
	item.step("tapped the first emoji", commit)
	item.check(commit, "no emoji commit")
	emoji = aatlib.field(commit, "text")
	# The emoji in the editor, for its colour (the shot above is from before the tap, T1-320).
	time.sleep(0.8)
	run.shot(item, "typed")
	close_panel(item, "flick")
	text = save(item, "emoji.txt")
	item.step("read the file", repr(text))
	item.check(emoji and emoji in text, f"{emoji!r} is not in the file {text!r}")
	item.person(f"typed {emoji}; its colour in the screenshot")


@run.define("desktop.lock.lock-unlock")
def lock_unlock(item):
	mark = run.mark()
	run.key("super+l")
	locked = run.wait(r"KWL LOCK locked ", mark, 10)
	time.sleep(1.0)
	item.step("Super+L", locked)
	run.shot(item, "locked")
	item.check(locked, "the screen did not lock")
	run.type(aatlib.PASSWORD)
	run.key("enter")
	unlocked = run.wait(r"KWL LOCK unlocked", mark, 15)
	time.sleep(1.0)
	item.step("typed the password and Enter", unlocked)
	run.shot(item, "unlocked")
	item.check(unlocked, "the screen did not unlock")
	item.passed()


@run.define("desktop.session.release-clients")
def release_clients(item):
	# BUG-239: 20 clients killed at once took the compositor 23 s to release in QEMU; each release logs its parts.
	mark = run.mark()
	for _ in range(20):
		run.as_user("/bin/terminal")
	started = time.time()
	while len(run.lines(r"ZTERM START ", mark)) < 20 and time.time() - started < 60.0:
		time.sleep(1.0)
	opened = len(run.lines(r"ZTERM START ", mark))
	item.step("started 20 Terminals", f"{opened} started")
	item.check(opened == 20, f"only {opened} Terminals started")
	time.sleep(2.0)
	killed = run.mark()
	# zedBSD has no pkill (T1-388: the Terminals lived on): each /bin/terminal's pid from ps, then kill.
	_, listed = run.sh("ps -A -o pid,args | grep '[/]bin/terminal' | awk '{print $1}'")
	pids = listed.split()
	item.step("Terminals to kill", f"{len(pids)}")
	run.sh("kill " + " ".join(pids) + "; true")
	began = time.time()
	while len(run.lines(r"KWL CLEANUP done ", killed)) < 20 and time.time() - began < 60.0:
		time.sleep(0.5)
	elapsed = time.time() - began
	done = run.lines(r"KWL CLEANUP done ", killed)
	for line in done:
		item.step("released", line)
	# The gone clients' buffers are released one a pass after the frames; the list empties later (BUG-239).
	drained = run.wait(r"KWL RETIRE drained ", killed, 120)
	drain_elapsed = time.time() - began
	for line in run.lines(r"KWL RETIRE slow ", killed):
		item.step("slow release", line)
	frame_ms = []
	for line in run.lines(r"KWL PERF ", killed):
		item.step("perf", line)
		found = re.search(r"frame_ms=([0-9.]+)", line)
		if found:
			frame_ms.append(float(found.group(1)))
	# The releases wait for an idle compositor (BUG-239, T1-392 saw 510 to 604 ms frames while they ran).
	worst = max(frame_ms) if frame_ms else 0.0
	item.step("all released", f"{len(done)} in {elapsed:.1f} s; buffers: {drained} after {drain_elapsed:.1f} s; frame_ms at most {worst:.1f} over {len(frame_ms)} PERF windows")
	item.check(len(done) == 20, f"only {len(done)} clients were released in 60 s")
	item.check(elapsed <= 10.0, f"the release took {elapsed:.1f} s (target 10 s)")
	item.check(drained, "the gone clients' buffers were not all released in 120 s")
	item.check(worst <= 100.0, f"a frame took {worst:.1f} ms on average in a PERF window while the buffers were released (target 100 ms)")
	item.passed()


def lock_choose_password(item, mark) -> None:
	"""On a lock card that offers its styles side by side (a PIN or a key enrolled, ws187-p003), presses Password first:
	the PIN is the field's style by default (T1-520), so a password typed as it is would go to the PIN."""
	pattern = r"KWL GREETER style-at style=1 x=-?\d+ y=-?\d+ width=\d+ height=\d+"
	places = run.lines(pattern, mark) or run.lines(pattern, None)
	if not places:
		item.step("styles side by side", "none (the password alone)")
		return
	line = places[-1]
	x = int(aatlib.field(line, "x")) + int(aatlib.field(line, "width")) // 2
	y = int(aatlib.field(line, "y")) + int(aatlib.field(line, "height")) // 2
	run.click(x, y)
	chosen = run.wait(r"KWL GREETER style=1 via=choice", mark, 5)
	item.step("pressed Password among the styles", chosen or "the field took the password already")


@run.define("desktop.lock.swipe-card")
def lock_swipe_card(item):
	# ws187-p001..p003: the clock and the hint alone, a swipe up from the lower part brings the card (a manual lock: no grace).
	width, height = run.screen()
	mark = run.mark()
	run.key("super+l")
	locked = run.wait(r"KWL LOCK locked reason=key .*manual=1", mark, 10)
	time.sleep(1.0)
	item.step("Super+L", locked)
	run.shot(item, "clock")
	item.check(locked, "the screen did not lock as a manual lock")
	run.drag(width // 2, height * 85 // 100, width // 2, height * 40 // 100, steps=20)
	swiped = run.wait(r"KWL LOCK swipe via=pointer grace=0 manual=1", mark, 10)
	time.sleep(1.0)
	item.step("dragged up from the lower part", swiped)
	run.shot(item, "card")
	item.check(swiped, "the swipe was not taken")
	lock_choose_password(item, mark)
	run.type(aatlib.PASSWORD)
	run.key("enter")
	unlocked = run.wait(r"KWL LOCK unlocked", mark, 15)
	item.step("typed the password and Enter", unlocked)
	item.check(unlocked, "the screen did not unlock")
	item.person("clock.png: the large clock above the middle, the hint at the foot, no card; card.png: the card under the clock, not touching it")


@run.define("desktop.lock.wheel-card")
def lock_wheel_card(item):
	# ws187-p002: the wheel turned up two notches is a swipe (machines without a touch pad or a touch screen).
	width, height = run.screen()
	mark = run.mark()
	run.key("super+l")
	locked = run.wait(r"KWL LOCK locked reason=key", mark, 10)
	time.sleep(1.0)
	item.step("Super+L", locked)
	item.check(locked, "the screen did not lock")
	run.aat("wheel", str(width // 2), str(height // 2), "2")
	swiped = run.wait(r"KWL LOCK swipe via=wheel grace=0 manual=1", mark, 10)
	time.sleep(1.0)
	item.step("turned the wheel up two notches", swiped)
	run.shot(item, "card")
	item.check(swiped, "the wheel was not taken")
	lock_choose_password(item, mark)
	run.type(aatlib.PASSWORD)
	run.key("enter")
	unlocked = run.wait(r"KWL LOCK unlocked", mark, 15)
	item.step("typed the password and Enter", unlocked)
	item.check(unlocked, "the screen did not unlock")
	item.passed()


@run.define("desktop.language.lock-japanese")
def lock_japanese(item):
	# The lock screen in Japanese (ws158-p003, q809: a zdesktop of a session sessiond started, which can unlock it).
	mark = run.mark()
	run.as_user("/bin/keiland-settings set ui.language 1")
	japanese = run.wait(r"KWL LANGUAGE language=ja ", mark, 10)
	item.step("ui.language 1", japanese)
	item.check(japanese, "the desktop did not change to Japanese")
	mark = run.mark()
	run.key("super+l")
	locked = run.wait(r"KWL LOCK locked ", mark, 10)
	time.sleep(1.0)
	item.step("Super+L", locked)
	run.shot(item, "ja-lock")
	run.type(aatlib.PASSWORD)
	run.key("enter")
	unlocked = run.wait(r"KWL LOCK unlocked", mark, 15)
	item.step("typed the password and Enter", unlocked)
	mark = run.mark()
	run.as_user("/bin/keiland-settings set ui.language 0")
	english = run.wait(r"KWL LANGUAGE language=en ", mark, 10)
	item.step("ui.language 0", english)
	item.check(locked and unlocked, "the screen did not lock and unlock")
	item.check(english, "the desktop did not come back to English")
	item.person("the lock screen's Japanese words (パスワード) in ja-lock.png")


@run.define("desktop.session.logout-login")
def logout_login(item):
	before = run.lines(r"KWL READY socket=\S+ .* role=normal", None)
	old_pid = aatlib.field(before[-1], "pid") if before else None
	# Log Out is in App Home's Power Off dialog (ws099-p037, BUG-235): the keys start on Cancel, Up is Log Out.
	power_dialog(item)
	greeter_mark = run.mark("/var/log/greeter.log")
	mark = run.mark()
	run.key("up")
	run.key("enter")
	opened = run.wait(r"KWL GREETER open users=", greeter_mark, 30, log="/var/log/greeter.log")
	time.sleep(1.5)
	chosen = run.lines(r"KWL POWER choice=logout via=key error=0", mark)
	item.step("Up and Enter in the dialog (Log Out)", f"{chosen[-1] if chosen else 'no choice line'}; {opened}")
	run.shot(item, "greeter")
	item.check(opened, "the login screen did not come (greeter.log)")
	run.type(aatlib.PASSWORD)
	run.key("enter")
	auth = run.wait(r"KWL GREETER auth user=", greeter_mark, 20, log="/var/log/greeter.log")
	ready = None
	deadline = time.monotonic() + 60
	while time.monotonic() < deadline and ready is None:
		found = [line for line in run.lines(r"KWL READY socket=\S+ .* role=normal", None) if aatlib.field(line, "pid") != old_pid]
		ready = found[-1] if found else None
		time.sleep(2)
	run._ready = None
	item.step("typed the password and Enter", f"{auth}; {ready}")
	time.sleep(2.0)
	run.shot(item, "desktop")
	item.check(auth and ready, "no new session after the login")
	item.passed()


sys.exit(run.go(before=common.before(run), after=common.after(run)))
