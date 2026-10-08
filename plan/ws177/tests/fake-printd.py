#!/usr/bin/env python3
"""ws177-p023: a stand-in keiland-printd for the backend's life test (host-print-life.c).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    fake-printd.py FOLDER       (started by the backend through a wrapper, its socket on descriptor 3)

Each start appends "START" to FOLDER/log and takes the next word of FOLDER/modes (one a line; the last repeats):

    normal         ACCEPTED, STATE sending, STATE done for each JOB; CANCEL answered STATE cancelled
    crash          ends at the first JOB without a word
    fatal          says FATAL runtime and ends
    fd             answers the first JOB with a descriptor (the protocol broken), then waits for its socket's end
    hold           takes the JOB's document but answers nothing; CANCEL answered REJECTED <job> cancelled
    hold-end       takes the JOB but answers nothing; CANCEL ends the daemon

Every line it gets is appended to FOLDER/log.
"""
import os
import socket
import sys
from pathlib import Path

FOLDER = Path(sys.argv[1])


def main():
	log = open(FOLDER / "log", "a")
	starts = (FOLDER / "starts").read_text().count("x") if (FOLDER / "starts").exists() else 0
	with open(FOLDER / "starts", "a") as out:
		out.write("x")
	modes = [line.strip() for line in (FOLDER / "modes").read_text().splitlines() if line.strip()]
	mode = modes[min(starts, len(modes) - 1)]
	log.write(f"START {mode}\n")
	log.flush()
	control = socket.socket(fileno=3)
	if mode == "fatal":
		control.sendall(b"FATAL runtime\n")
		return 1
	control.sendall(b"SPOOL " + str(FOLDER / "spool").encode() + b"\n")
	buffer = b""
	while True:
		data, fds, _, _ = socket.recv_fds(control, 4096, 8)
		for fd in fds:
			os.close(fd)
		if not data:
			return 0
		buffer += data
		while b"\n" in buffer:
			line, buffer = buffer.split(b"\n", 1)
			text = line.decode()
			log.write(text + "\n")
			log.flush()
			words = text.split(" ")
			if words[0] == "JOB":
				if mode == "crash":
					return 1
				if mode == "fd":
					socket.send_fds(control, [b"ACCEPTED " + words[1].encode() + b"\n"], [os.open("/dev/null", os.O_RDONLY)])
					continue
				if mode in ("hold", "hold-end"):
					continue
				control.sendall(f"ACCEPTED {words[1]}\nSTATE {words[1]} sending\nSTATE {words[1]} done\n".encode())
			elif words[0] == "CANCEL":
				if mode == "hold":
					control.sendall(f"REJECTED {words[1]} cancelled\n".encode())
				elif mode == "hold-end":
					return 0
				else:
					control.sendall(f"STATE {words[1]} cancelled\n".encode())


if __name__ == "__main__":
	sys.exit(main())
