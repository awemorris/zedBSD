#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Decodes a saved USB HID report ring (/dev/usbhid-ring) beside the session's log.

usage: python3 -I ring-decode.py DIR [--mouse BUS:ADDR:IF] [--at EPOCH_SECONDS] [--window SECONDS] [--dump]

DIR is what ring-save.sh left: ring.bin, session.log and info.txt.

The ring's records are 64 bytes (src/drivers/usb/usb-hid-ring.h):
counter_us, monotonic_us, sequence (u64 each), length (u16), bus, address,
interface, endpoint, kept, flags (u8 each), bytes[32].  monotonic_us is
CLOCK_MONOTONIC, the clock of the compositor's at_ms and of the input
events' time (the event time in ms is monotonic_us // 1000 of the same
report), so the report of a press is matched to its KWL FRAME and KWL BTN
lines by that time.

The mouse is the interface whose reports are a mouse's report 2 (the Bolt
receiver: id 2, buttons u16, X and Y s16); --mouse names it instead.

For each click (a report that presses the left button and the report that
releases it) it prints whether the compositor saw both (KWL FRAME code0=272),
gave them to the seat (KWL BTN), kept the press (KWL PRESS), and whether
Settings changed its page (ZSETTINGS PAGE, which has no time of its own: it is
the click's when it comes after the release's KWL BTN line and before the next
click's press line).  A click with no page change is classed:

  report-no-frame   the ring has the report but the compositor has no FRAME
  frame-no-btn      FRAME but no KWL BTN (the input thread or the seat dropped it)
  kept              KWL PRESS: the compositor took the press or the release
  same-row          FRAME and BTN, no page: pressed and released in one row
  already-shown     the same, on the row of the page already shown
  different-row     pressed in one row, released in another (not a click)
  row-unknown       the rows are learned from clicks that changed the page;
                    the point is in none of them

A click that never reached the ring (no report) cannot be listed this way:
--at with the wall time (date +%s) the user saw it fail prints every record
and log line --window seconds around it, and says whether any left-button
report is there (info.txt gives the offset between the wall clock and
CLOCK_MONOTONIC, to about the receiver's 0.5 s idle report).
"""

import argparse
import os
import re
import struct
import sys

RECORD = struct.Struct("<QQQHBBBBBB32s")
FLAGS = [(0x01, "raw"), (0x02, "drain"), (0x04, "stall"), (0x08, "error"), (0x10, "attach"), (0x20, "detach")]
LEFT = 272


def read_ring(path):
	"""The ring's records, oldest first, as dicts."""
	data = open(path, "rb").read()
	records = []
	for offset in range(0, len(data) - len(data) % RECORD.size, RECORD.size):
		fields = RECORD.unpack_from(data, offset)
		records.append({
			"counter_us": fields[0], "monotonic_us": fields[1], "sequence": fields[2], "length": fields[3],
			"bus": fields[4], "address": fields[5], "interface": fields[6], "endpoint": fields[7],
			"kept": fields[8], "flags": fields[9], "bytes": fields[10][:fields[8]],
		})
	return records, len(data) % RECORD.size


def flag_names(flags):
	"""The record's flags as words."""
	names = [name for bit, name in FLAGS if flags & bit]
	return ",".join(names) if names else "input"


def mouse_report(record):
	"""(buttons, dx, dy) of a mouse's report 2, or None."""
	data = record["bytes"]
	if record["flags"] != 0 or len(data) < 7 or data[0] != 0x02:
		return None
	buttons, dx, dy = struct.unpack_from("<Hhh", data, 1)
	return buttons, dx, dy


def find_mouse(records):
	"""The (bus, address, interface) with the most mouse reports."""
	counts = {}
	for record in records:
		if mouse_report(record) is not None:
			key = (record["bus"], record["address"], record["interface"])
			counts[key] = counts.get(key, 0) + 1
	if not counts:
		return None
	return max(counts, key=counts.get)


def read_log(path):
	"""The session log's lines that matter, in file order, with their kind and fields."""
	lines = []
	if not os.path.exists(path):
		return lines
	for number, text in enumerate(open(path, "r", errors="replace"), 1):
		text = text.rstrip("\n")
		entry = None
		if "KWL FRAME " in text:
			entry = {"kind": "frame"}
		elif "KWL BTN " in text:
			entry = {"kind": "btn"}
		elif "KWL PRESS " in text:
			entry = {"kind": "press"}
		elif "ZSETTINGS PAGE " in text:
			match = re.search(r"ZSETTINGS PAGE (\S+)(.*)", text)
			entry = {"kind": "page", "page": match.group(1), "back": "back" in match.group(2)}
		else:
			continue
		for key, value in re.findall(r"(\w+)=(\S+)", text):
			entry.setdefault(key, value)
		entry["line"] = number
		entry["text"] = text
		lines.append(entry)
	return lines


def as_int(entry, key, default=None):
	"""A numeric field of a log line."""
	try:
		return int(entry.get(key))
	except (TypeError, ValueError):
		return default


def clicks_of(records, mouse):
	"""The left-button presses and releases of the mouse's reports, paired, with the motion between."""
	clicks = []
	held = None
	state = 0
	for record in records:
		if (record["bus"], record["address"], record["interface"]) != mouse:
			continue
		report = mouse_report(record)
		if report is None:
			continue
		buttons, dx, dy = report
		left = buttons & 1
		if held is not None:
			held["moved"] += abs(dx) + abs(dy)
			held["reports"] += 1
		if left and not state:
			held = {"press": record, "release": None, "moved": 0, "reports": 0, "press_motion": abs(dx) + abs(dy)}
			clicks.append(held)
		elif not left and state and held is not None:
			held["release"] = record
			held["reports"] -= 1
			held = None
		state = left
	return clicks


def near(entries, kind, time_ms, value=None, field="state"):
	"""The first log line of a kind whose time= is the report's (within a millisecond)."""
	for entry in entries:
		if entry["kind"] != kind:
			continue
		if kind == "frame" and as_int(entry, "code0") != LEFT:
			continue
		if value is not None and as_int(entry, field) != value:
			continue
		event_time = as_int(entry, "time")
		if event_time is not None and abs(event_time - (time_ms & 0xffffffff)) <= 1:
			return entry
	return None


def learn_rows(results):
	"""The y range each page's row covered in the clicks that opened it."""
	rows = {}
	for result in results:
		if result["page"] is None or result["page"]["back"]:
			continue
		for y in (result["press_y"], result["release_y"]):
			if y is None:
				continue
			low, high = rows.get(result["page"]["page"], (y, y))
			rows[result["page"]["page"]] = (min(low, y), max(high, y))
	return rows


def row_of(rows, y):
	"""The page whose learned row holds y, or None."""
	if y is None:
		return None
	for page, (low, high) in rows.items():
		if low <= y <= high:
			return page
	return None


def main():
	parser = argparse.ArgumentParser(description="Decodes a saved USB HID report ring beside the session's log.")
	parser.add_argument("dir")
	parser.add_argument("--mouse", help="BUS:ADDR:IF of the mouse (default: the interface with the most mouse reports)")
	parser.add_argument("--at", type=float, help="wall time (seconds since the epoch) a click was seen to fail")
	parser.add_argument("--window", type=float, default=3.0, help="seconds around --at to list")
	parser.add_argument("--dump", action="store_true", help="print every record")
	options = parser.parse_args()

	records, tail = read_ring(os.path.join(options.dir, "ring.bin"))
	entries = read_log(os.path.join(options.dir, "session.log"))
	if not records:
		print("ring-decode: no records")
		return 1

	# The ring's span, and the records it lost (overwritten while the reader read).
	gaps = sum(1 for before, after in zip(records, records[1:]) if after["sequence"] != before["sequence"] + 1)
	print("records=%d first-sequence=%d last-sequence=%d monotonic=%.3f..%.3f s gaps=%d partial-bytes=%d" % (
		len(records), records[0]["sequence"], records[-1]["sequence"],
		records[0]["monotonic_us"] / 1e6, records[-1]["monotonic_us"] / 1e6, gaps, tail))
	interfaces = {}
	for record in records:
		key = (record["bus"], record["address"], record["interface"], flag_names(record["flags"] & 0x03))
		interfaces[key] = interfaces.get(key, 0) + 1
	for (bus, address, interface, kind), count in sorted(interfaces.items()):
		print("  usb%d device %d interface %d (%s): %d records" % (bus, address, interface, kind, count))
	for record in records:
		if record["flags"] & 0x3c:
			print("  event %s usb%d device %d interface %d at %.3f s length=%d" % (
				flag_names(record["flags"]), record["bus"], record["address"], record["interface"],
				record["monotonic_us"] / 1e6, record["length"]))

	if options.dump:
		for record in records:
			print("%10d %12.6f %12.6f usb%d/%d/%d ep%02x %-6s len=%-3d %s" % (
				record["sequence"], record["monotonic_us"] / 1e6, record["counter_us"] / 1e6,
				record["bus"], record["address"], record["interface"], record["endpoint"],
				flag_names(record["flags"]), record["length"], record["bytes"].hex(" ")))

	# The mouse and its clicks.
	if options.mouse:
		mouse = tuple(int(part) for part in options.mouse.split(":"))
	else:
		mouse = find_mouse(records)
	if mouse is None:
		print("ring-decode: no mouse reports")
		return 1
	clicks = clicks_of(records, mouse)
	print("mouse usb%d device %d interface %d: %d presses" % (mouse[0], mouse[1], mouse[2], len(clicks)))

	# Each click's lines: FRAME, BTN and PRESS by time; the page by order.
	results = []
	for click in clicks:
		press_ms = click["press"]["monotonic_us"] // 1000
		release_ms = click["release"]["monotonic_us"] // 1000 if click["release"] else None
		result = {"click": click, "press_ms": press_ms, "release_ms": release_ms}
		result["frame_press"] = near(entries, "frame", press_ms, 1, "value0")
		result["frame_release"] = near(entries, "frame", release_ms, 0, "value0") if release_ms is not None else None
		result["btn_press"] = near(entries, "btn", press_ms, 1)
		result["btn_release"] = near(entries, "btn", release_ms, 0) if release_ms is not None else None
		result["kept"] = [entry for entry in entries if entry["kind"] == "press" and
				  as_int(entry, "time") in (press_ms & 0xffffffff, (release_ms or -1) & 0xffffffff)]
		result["press_y"] = as_int(result["btn_press"], "y") if result["btn_press"] else None
		result["release_y"] = as_int(result["btn_release"], "y") if result["btn_release"] else None
		result["press_x"] = as_int(result["btn_press"], "x") if result["btn_press"] else None
		result["release_x"] = as_int(result["btn_release"], "x") if result["btn_release"] else None
		result["page"] = None
		results.append(result)

	# The page a click opened: the first PAGE line after its release's BTN line and before the next press's.
	for index, result in enumerate(results):
		if result["btn_release"] is None:
			continue
		start = result["btn_release"]["line"]
		end = None
		for later in results[index + 1:]:
			if later["btn_press"] is not None:
				end = later["btn_press"]["line"]
				break
		for entry in entries:
			if entry["kind"] == "page" and entry["line"] > start and (end is None or entry["line"] < end):
				result["page"] = entry
				break

	# The page shown before each click, and the rows learned from the clicks that worked.
	rows = learn_rows(results)
	shown = None
	failed = 0
	for result in results:
		click = result["click"]
		if result["btn_press"] is not None:
			for entry in entries:
				if entry["kind"] == "page" and entry["line"] < result["btn_press"]["line"]:
					shown = entry["page"]
		if result["page"] is not None:
			verdict = "page=%s" % result["page"]["page"]
		elif click["release"] is None:
			verdict = "no-release-report"
		elif result["frame_press"] is None or result["frame_release"] is None:
			verdict = "report-no-frame (press-frame=%s release-frame=%s)" % (
				"yes" if result["frame_press"] else "no", "yes" if result["frame_release"] else "no")
		elif result["btn_press"] is None or result["btn_release"] is None:
			verdict = "frame-no-btn"
		elif result["kept"]:
			verdict = "kept " + " / ".join(entry["text"].split("KWL PRESS ", 1)[1].split(" button=")[0] for entry in result["kept"])
		else:
			press_row = row_of(rows, result["press_y"])
			release_row = row_of(rows, result["release_y"])
			if press_row is None or release_row is None:
				verdict = "row-unknown (dx=%d dy=%d px)" % (result["release_x"] - result["press_x"], result["release_y"] - result["press_y"])
			elif press_row != release_row:
				verdict = "different-row (%s -> %s)" % (press_row, release_row)
			elif press_row == shown:
				verdict = "already-shown (%s)" % press_row
			else:
				verdict = "same-row (%s)" % press_row
		if result["page"] is None:
			failed += 1
		held_ms = (result["release_ms"] - result["press_ms"]) if result["release_ms"] is not None else -1
		print("click press=%.3f s held=%d ms moved=%d counts in %d reports at (%s,%s)->(%s,%s): %s" % (
			result["press_ms"] / 1000.0, held_ms, click["moved"], click["reports"],
			result["press_x"], result["press_y"], result["release_x"], result["release_y"], verdict))
	print("clicks=%d without-page-change=%d rows-learned=%s" % (len(results), failed,
	      " ".join("%s:%d-%d" % (page, low, high) for page, (low, high) in sorted(rows.items(), key=lambda item: item[1]))))

	# Around a time the user saw a click fail.
	if options.at is not None:
		info = {}
		info_path = os.path.join(options.dir, "info.txt")
		if os.path.exists(info_path):
			for text in open(info_path):
				if "=" in text:
					key, value = text.strip().split("=", 1)
					info[key] = value
		saved_epoch = float(info.get("saved_epoch", "nan"))
		newest_us = records[-1]["monotonic_us"]
		at_us = newest_us - (saved_epoch - options.at) * 1e6
		low = at_us - options.window * 1e6
		high = at_us + options.window * 1e6
		print("around %.0f (monotonic %.3f s, +-%.1f s; the offset is good to about 0.5 s):" % (options.at, at_us / 1e6, options.window))
		lefts = 0
		for record in records:
			if low <= record["monotonic_us"] <= high:
				report = mouse_report(record)
				if report is not None and (record["bus"], record["address"], record["interface"]) == mouse and report[0] & 1:
					lefts += 1
				print("  %12.6f usb%d/%d/%d %-6s len=%-3d %s" % (record["monotonic_us"] / 1e6, record["bus"], record["address"],
				      record["interface"], flag_names(record["flags"]), record["length"], record["bytes"].hex(" ")))
		for entry in entries:
			at_ms = as_int(entry, "at_ms")
			if at_ms is not None and low / 1000 <= at_ms <= high / 1000:
				print("  log %d: %s" % (entry["line"], entry["text"]))
		print("left-button-down reports around it: %d%s" % (lefts, "  -> no report (the receiver or the mouse)" if lefts == 0 else ""))
	return 0


if __name__ == "__main__":
	sys.exit(main())
