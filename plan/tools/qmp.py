#!/usr/bin/env python3
"""Sends one QMP command to a QEMU and prints the reply.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    qmp.py SOCKET COMMAND [JSON-ARGUMENTS]

WS040 p005 uses it to throttle the guest's boot disk while the guest runs:
    qmp.py S block_set_io_throttle '{"device":"boot","iops":1,...}'
"""
import json
import socket
import sys


def main() -> int:
	if len(sys.argv) not in (3, 4):
		print(__doc__, file=sys.stderr)
		return 2
	connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	connection.connect(sys.argv[1])
	stream = connection.makefile("rw")
	stream.readline()
	stream.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
	stream.flush()
	stream.readline()
	request = {"execute": sys.argv[2]}
	if len(sys.argv) == 4:
		request["arguments"] = json.loads(sys.argv[3])
	stream.write(json.dumps(request) + "\n")
	stream.flush()
	# Events may arrive before the reply; the reply is the line with "return" or "error".
	while True:
		line = stream.readline()
		if not line:
			return 1
		reply = json.loads(line)
		if "return" in reply or "error" in reply:
			print(json.dumps(reply))
			return 0 if "return" in reply else 1


if __name__ == "__main__":
	sys.exit(main())
