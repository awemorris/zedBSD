#!/usr/bin/env python3
"""Summarizes plan/tools/ssh-rtt/qemu-ssh-rtt.sh's rounds against the guest's capture.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    rtt-summary.py PCAP ROUNDS LIMIT TCPSTATS_BEFORE TCPSTATS_AFTER

ROUNDS has one line per round: its number, the host's time before and after
it, ssh's status, and the boot disk's counts before and after it (reads,
writes, flushes, and the emulator's ms in each).  Each round is matched to the SSH connection whose SYN
the capture shows inside it.  The capture is read with tshark.
"""
from __future__ import annotations

import subprocess
import sys

FIELDS = ("frame.time_epoch", "ip.src", "ip.dst", "ipv6.src", "ipv6.dst",
	  "tcp.stream", "tcp.srcport", "tcp.dstport", "tcp.flags.str", "tcp.len",
	  "tcp.analysis.retransmission", "tcp.payload", "udp.srcport",
	  "udp.dstport", "arp.opcode", "_ws.col.Protocol", "tcp.seq_raw", "tcp.ack_raw",
	  "tcp.window_size_value")


def read_capture(path: str) -> list[dict]:
	"""Returns each frame's fields."""
	command = ["tshark", "-r", path, "-T", "fields", "-E", "separator=\t",
		   "-E", "occurrence=f"]
	for field in FIELDS:
		command += ["-e", field]
	output = subprocess.run(command, capture_output=True, text=True,
				check=True).stdout
	frames = []
	for line in output.splitlines():
		values = line.split("\t")
		values += [""] * (len(FIELDS) - len(values))
		frames.append(dict(zip(FIELDS, values)))
	return frames


def stats_lines(path: str) -> list[str]:
	"""Returns the net.tcpstats lines of one reading."""
	try:
		with open(path, encoding="utf-8", errors="replace") as handle:
			return [line.rstrip("\r\n") for line in handle
				if line.startswith("net.tcpstats")]
	except OSError:
		return []


def state_counts(lines: list[str]) -> str:
	"""Counts the sockets of a reading by their state."""
	counts: dict[str, int] = {}
	for line in lines[1:]:
		for word in line.split():
			if word.startswith("state="):
				counts[word] = counts.get(word, 0) + 1
	return " ".join(f"{key}:{value}" for key, value in sorted(counts.items())) or "none"


def main() -> int:
	pcap, rounds_path, limit, before, after = sys.argv[1:6]
	limit_seconds = float(limit)
	frames = read_capture(pcap)
	streams: dict[str, list[dict]] = {}
	for frame in frames:
		if frame["tcp.stream"] and "22" in (frame["tcp.srcport"], frame["tcp.dstport"]):
			streams.setdefault(frame["tcp.stream"], []).append(frame)

	failed = 0
	with open(rounds_path, encoding="utf-8") as handle:
		rounds = [line.split() for line in handle if line.strip()]
	# The emulator stamps the capture with its own clock, so the rounds are
	# matched by their order: the last sessions but one (the reading of
	# net.tcpstats after them) are the rounds', one each.
	# Only whole sessions count: something on the host now and then opens
	# the forwarded port, sends its banner and closes at once.
	ordered = sorted((stream for stream in streams.values()
			  if "S" in stream[0]["tcp.flags.str"] and
			  sum(int(frame["tcp.len"] or 0) for frame in stream
			      if frame["tcp.dstport"] == "22") >= 1000),
			 key=lambda stream: float(stream[0]["frame.time_epoch"]))
	matched = ordered[-(len(rounds) + 1):-1] if len(ordered) > len(rounds) else []
	offset = None
	for index, words in enumerate(rounds):
		number, begin, end, status = words[:4]
		disk = ""
		if len(words) >= 16 and "-" not in words[4:16]:
			a = [int(word) for word in words[4:10]]
			b = [int(word) for word in words[10:16]]
			d = [y - x for x, y in zip(a, b)]
			disk = (f" disk r{d[0]}/{d[3]}ms w{d[1]}/{d[4]}ms f{d[2]}/{d[5]}ms")
		start, finish = float(begin), float(end)
		seconds = finish - start
		mine = matched[index] if matched else None
		if mine is not None and offset is None:
			offset = start - float(mine[0]["frame.time_epoch"])
		detail = "no connection in the capture"
		if mine is not None:
			syn = float(mine[0]["frame.time_epoch"])
			marks = {}
			retransmissions = 0
			for frame in mine:
				at = float(frame["frame.time_epoch"]) - syn
				flags = frame["tcp.flags.str"]
				to_guest = frame["tcp.dstport"] == "22"
				if frame["tcp.analysis.retransmission"]:
					retransmissions += 1
				if "S" in flags and "A" in flags.replace("·", ""):
					pass
				if "S" in flags and not to_guest:
					marks.setdefault("synack", at)
				payload = frame["tcp.payload"]
				if payload and not to_guest:
					marks.setdefault("banner", at)
				if payload and to_guest and payload.startswith("5353482d"):
					marks.setdefault("cbanner", at)
				if payload and to_guest and "banner" in marks and not payload.startswith("5353482d"):
					marks.setdefault("ckex", at)
				if "F" in flags:
					marks.setdefault("fin-" + ("host" if to_guest else "guest"), at)
				if "R" in flags:
					marks.setdefault("rst-" + ("host" if to_guest else "guest"), at)
			marks["last"] = float(mine[-1]["frame.time_epoch"]) - syn
			# The longest the guest took to answer the host's data with
			# its own (where a slow round waits), and the size of what
			# it was answering.
			longest = 0.0
			after = 0
			asked_at = None
			asked_length = 0
			for frame in mine:
				length = int(frame["tcp.len"] or 0)
				if length == 0:
					continue
				at = float(frame["frame.time_epoch"])
				if frame["tcp.dstport"] == "22":
					if asked_at is None:
						asked_at, asked_length = at, length
				elif asked_at is not None:
					if at - asked_at > longest:
						longest, after = at - asked_at, asked_length
					asked_at = None
			marks["answer-max"] = longest
			lead = syn + offset - start
			detail = (f"syn +{lead:.2f} " +
				  " ".join(f"{key} {value:.2f}" for key, value in marks.items()) +
				  f" (after {after} bytes) frames {len(mine)} rexmit {retransmissions}")
		detail += disk
		verdict = "ok" if status == "0" and seconds < limit_seconds else "SLOW"
		if verdict != "ok":
			failed += 1
		print(f"round {number}: {seconds:.2f} s status {status} {verdict}: {detail}")

	# What else the guest sent while the rounds ran: names asked of the DNS
	# server, and anything that is neither SSH nor the DHCP renewals.
	if rounds and offset is not None:
		first_begin = float(rounds[0][1]) - offset
		last_end = float(rounds[-1][2]) - offset
		others: dict[str, int] = {}
		for frame in frames:
			at = float(frame["frame.time_epoch"])
			if not first_begin <= at <= last_end:
				continue
			if frame["tcp.stream"] and "22" in (frame["tcp.srcport"], frame["tcp.dstport"]):
				continue
			key = frame["_ws.col.Protocol"] + " " + frame["udp.dstport"] + frame["tcp.dstport"]
			others[key] = others.get(key, 0) + 1
		print("other frames during the rounds: " +
		      (" ".join(f"[{key}]x{value}" for key, value in sorted(others.items())) or "none"))

	print("net.tcpstats before: " + state_counts(stats_lines(before)))
	print("net.tcpstats after: " + state_counts(stats_lines(after)))
	print(f"rounds: {len(rounds)}, slow or failed: {failed}")
	print("qemu-ssh-rtt: " + ("PASS" if failed == 0 and rounds else "FAIL"))
	return 0 if failed == 0 and rounds else 1


if __name__ == "__main__":
	raise SystemExit(main())
