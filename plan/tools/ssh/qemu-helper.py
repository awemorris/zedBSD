#!/usr/bin/python3 -I
# The host side helpers of qemu-interop.sh:
#   owner PORT PID           exits 0 when the socket listening on 127.0.0.1:PORT belongs to PID or a process under it
#   wait-port PORT SECONDS   exits 0 once 127.0.0.1:PORT takes a connection
#   hold PORT SECONDS        connects, reads the server's identification line, sends one of its own and keeps the
#                            connection open for SECONDS without a key exchange (a client still before
#                            authentication); prints "held" once it has sent its line
#   pty OUT ROWS COLS SECONDS -- COMMAND...   runs COMMAND on a new terminal of ROWS x COLS, writes what it printed
#                            to OUT and exits with its status (124 when it ran out of time)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import fcntl, os, pty, select, signal, socket, struct, sys, termios, time


def owner(port, root):
	inodes = set()
	for line in open("/proc/net/tcp").read().splitlines()[1:]:
		fields = line.split()
		if fields[3] == "0A" and fields[1] == "0100007F:%04X" % port:
			inodes.add("socket:[%s]" % fields[9])
	if not inodes:
		return 1
	children = {}
	for entry in os.listdir("/proc"):
		if entry.isdigit():
			try:
				parent = int(open("/proc/%s/stat" % entry).read().rsplit(")", 1)[1].split()[1])
			except OSError:
				continue
			children.setdefault(parent, []).append(int(entry))
	pending = [root]
	while pending:
		pid = pending.pop()
		pending.extend(children.get(pid, []))
		try:
			links = set(os.readlink("/proc/%d/fd/%s" % (pid, fd)) for fd in os.listdir("/proc/%d/fd" % pid))
		except OSError:
			continue
		if inodes & links:
			return 0
	return 1


def wait_port(port, seconds):
	end = time.time() + seconds
	while time.time() < end:
		try:
			socket.create_connection(("127.0.0.1", port), 1).close()
			return 0
		except OSError:
			time.sleep(0.2)
	return 1


def hold(port, seconds):
	connection = socket.create_connection(("127.0.0.1", port), 10)
	connection.settimeout(30)
	data = b""
	while b"\n" not in data:
		chunk = connection.recv(256)
		if not chunk:
			return 1
		data += chunk
	if not data.startswith(b"SSH-2.0-"):
		return 1
	connection.sendall(b"SSH-2.0-held_client\r\n")
	print("held", flush=True)
	time.sleep(seconds)
	connection.close()
	return 0


def run_pty(args):
	out, rows, cols, seconds = args[0], int(args[1]), int(args[2]), float(args[3])
	command = args[5:]
	pid, master = pty.fork()
	if pid == 0:
		fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
		os.execv(command[0], command)
	data = b""
	end = time.time() + seconds
	status = None
	while True:
		left = end - time.time()
		if left <= 0:
			os.kill(pid, signal.SIGKILL)
			os.waitpid(pid, 0)
			status = 124
			break
		ready, _, _ = select.select([master], [], [], min(left, 0.2))
		if ready:
			try:
				chunk = os.read(master, 65536)
			except OSError:
				chunk = b""
			if not chunk:
				break
			data += chunk
	if status is None:
		_, raw = os.waitpid(pid, 0)
		status = os.WEXITSTATUS(raw) if os.WIFEXITED(raw) else 128 + os.WTERMSIG(raw)
	open(out, "wb").write(data)
	return status


def main():
	verb, args = sys.argv[1], sys.argv[2:]
	if verb == "owner":
		return owner(int(args[0]), int(args[1]))
	if verb == "wait-port":
		return wait_port(int(args[0]), float(args[1]))
	if verb == "hold":
		return hold(int(args[0]), float(args[1]))
	if verb == "pty":
		return run_pty(args)
	return 2


sys.exit(main())
