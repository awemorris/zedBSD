#!/usr/bin/env python3
"""Run a zedBSD guest that can be typed at, copied to, and debugged.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

plan/tools/boot-test.sh answers one question -- did it boot -- by reading
the screen.  Anything past that was being done by injecting keystrokes into
the console, which drops characters under load and cannot carry a file or a
debugger.  This gives the guest a network instead:

  - the emulator forwards a host port to the guest's SSH port, so commands
    run over a connection rather than a keyboard;
  - files go both ways with scp;
  - the guest's own lldb debugs a program there, and the emulator's gdb
    stub debugs the kernel from here;
  - the screen is still readable, because some things (the console itself)
    can only be seen that way.

The key pair is made once under plan/tmp/guest and is for these guests
only.  The public half has to be in the image, which is what `extra-files`
prints the make arguments for.

    plan/tools/guest/guest.sh keys
    eval "make ... $(plan/tools/guest/guest.sh extra-files) disk-image"
    plan/tools/guest/guest.sh start build/x/hdd-image.img
    plan/tools/guest/guest.sh run 'net show'
    plan/tools/guest/guest.sh put a.txt /tmp/a.txt
    plan/tools/guest/guest.sh lldb /bin/which -o run -o quit
    plan/tools/guest/guest.sh stop
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
STATE = ROOT / "plan/tmp/guest"
KEY = STATE / "id_ed25519"

# What one running guest leaves behind -- its copy of the disk, the log, the
# sockets and the session record -- lives under build/, which git ignores.
# Only the keys stay in plan/tmp/guest, because images are built with them.
# The running guest's state; GUEST_RUNTIME lets several guests run side by side.
RUNTIME = Path(os.environ.get("GUEST_RUNTIME", str(ROOT / "build/guest"))).resolve()
SESSION = RUNTIME / "session.json"

# Every kind of host key the guest's sshd_config names.  All of them are put
# into the image: the server makes any that is missing, and making an RSA key
# on an emulated machine takes most of a minute and writes over the console
# while it does.
HOST_KEY_KINDS = ("rsa", "ecdsa", "ed25519")

# The emulator options that make a guest reachable.  The network is a USB
# CDC-ECM adapter because that is the one the guest has a driver for; it
# appears there as ue0, which plan/tools/guest/net.conf addresses by DHCP.
SSH_GUEST_PORT = 22


def fail(message: str) -> "NoReturn":
	"""Stops with one line on the standard error."""
	print(f"guest: {message}", file=sys.stderr)
	raise SystemExit(1)


def free_port() -> int:
	"""Returns a port nothing is listening on."""
	with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
		probe.bind(("127.0.0.1", 0))
		return probe.getsockname()[1]


def read_session() -> dict:
	"""Returns what start recorded, or stops if no guest is running."""
	if not SESSION.exists():
		fail("no guest is running (start one first)")
	return json.loads(SESSION.read_text())


def make_keys() -> None:
	"""Makes the key pair and the guest's host key, if they are not there."""
	STATE.mkdir(parents=True, exist_ok=True)
	wanted = [(KEY, "ed25519", "zedbsd-guest-harness")]
	for kind in HOST_KEY_KINDS:
		wanted.append((STATE / f"ssh_host_{kind}_key", kind,
			       f"zedbsd-guest-host-{kind}"))

	for path, kind, comment in wanted:
		if not path.exists():
			subprocess.run(["ssh-keygen", "-q", "-t", kind, "-N", "",
					"-C", comment, "-f", str(path)], check=True)

		# The private halves are readable only by whoever made them;
		# ssh refuses a key anyone else could have read.
		path.chmod(0o600)


def extra_files() -> str:
	"""Prints the make arguments that put the harness into an image."""
	make_keys()
	files = [
		f"--file /root/.ssh/authorized_keys={KEY}.pub",
		f"--file /etc/net.conf={ROOT}/plan/tools/guest/net.conf",
		"--mode /root/.ssh/authorized_keys=0600",
		# sshd refuses keys under a directory others can write to, and
		# the image makes directories group-writable by default.
		"--mode /root/.ssh=0700",
	]
	for kind in HOST_KEY_KINDS:
		key = STATE / f"ssh_host_{kind}_key"
		files.append(f"--file /etc/ssh/ssh_host_{kind}_key={key}")
		files.append(f"--file /etc/ssh/ssh_host_{kind}_key.pub={key}.pub")
		files.append(f"--mode /etc/ssh/ssh_host_{kind}_key=0600")

	# The host key is put in so that the guest does not have to make one at
	# every boot: doing that takes tens of seconds on an emulated disk and
	# writes over the console while it does.  These keys are for harness
	# guests and are in the repository's build tree, not in a release.
	return "ZEDBSD_TEST_EXTRA_FILES=" + shlex.quote(" ".join(files))


def ssh_options(session: dict, port_flag: str = "-p") -> list[str]:
	"""Returns the ssh arguments every connection to this guest uses.

	scp takes the same options except the port, which it spells -P: its
	-p means to keep file times.
	"""
	return [
		"-i", str(KEY),
		port_flag, str(session["ssh_port"]),
		"-o", "StrictHostKeyChecking=no",
		"-o", "UserKnownHostsFile=/dev/null",
		"-o", "LogLevel=ERROR",
		"-o", "ConnectTimeout=5",
		"-o", "BatchMode=yes",
	]


def start(arguments: argparse.Namespace) -> int:
	"""Boots one guest and records how to reach it."""
	image = Path(arguments.image).resolve()
	if not image.is_file():
		fail(f"no such image: {image}")
	make_keys()
	RUNTIME.mkdir(parents=True, exist_ok=True)

	# The guest writes to the disk it booted from, so it is given a copy:
	# a test that changes the image would change what the next test boots.
	disk = RUNTIME / "disk.img"
	subprocess.run(["cp", "--reflink=auto", str(image), str(disk)], check=True)
	nvram = RUNTIME / "uefi-vars.fd"
	subprocess.run(["cp", arguments.ovmf_vars, str(nvram)], check=True)
	monitor = RUNTIME / "qmp.sock"
	monitor.unlink(missing_ok=True)
	serial = RUNTIME / "serial.sock"
	serial.unlink(missing_ok=True)
	ssh_port = free_port()
	debug_port = free_port()
	# KVM when the host offers it: an emulated CPU takes minutes to boot
	# the guest and makes every timeout in it a question of luck.
	acceleration = []
	if not arguments.no_kvm and os.access("/dev/kvm", os.R_OK | os.W_OK):
		acceleration = ["-accel", "kvm"]
	# The boot disk is a USB stick, the way the physical machines are
	# tried, or an NVMe drive, the way they are installed (2026-09-25 user
	# direction: the USB model is slow and is not to be used for measuring).
	if arguments.disk == "nvme":
		boot_device = ["-device",
		    "nvme,serial=zedbsd-boot,drive=boot,bootindex=1"]
	else:
		boot_device = ["-device",
		    "usb-storage,bus=xhci.0,port=1,drive=boot,"
		    "id=rootstick,bootindex=1"]
	command = [
		arguments.qemu, *acceleration,
		"-machine", "q35", "-m", str(arguments.memory),
		"-smp", str(arguments.cpus), "-cpu", "host" if acceleration else "max",
		"-drive", f"if=pflash,format=raw,readonly=on,file={arguments.ovmf_code}",
		"-drive", f"if=pflash,format=raw,file={nvram}",
		"-device", "qemu-xhci,id=xhci",
		"-drive", f"if=none,id=boot,file={disk},format=raw",
		*boot_device,
		"-netdev", "user,id=net0,net=10.0.2.0/24,host=10.0.2.2,"
			   f"dhcpstart=10.0.2.15,dns=10.0.2.3,"
			   f"hostfwd=tcp:127.0.0.1:{ssh_port}-:{SSH_GUEST_PORT}",
		# The network adapter and the keyboard share the boot disk's
		# controller, as they would on a laptop.
		#
		# They were once kept on a second controller because the boot
		# disk stopped answering ("usb-storage: BOT CBW error") when a
		# full-speed device joined it.  ws035-p039 found the cause in the
		# xHCI driver: the interrupt handler acknowledged IMAN by reading
		# it back and writing it, which could put back the IE=0 that a
		# polling command had just lifted, leaving the event ring without
		# interrupts.  With that fixed, the shared arrangement booted in
		# every run.
		"-device", "usb-net,bus=xhci.0,port=2,id=ecm,netdev=net0,"
			   "mac=52:54:00:33:00:01,msos-desc=on",
		"-device", "usb-kbd,bus=xhci.0,port=3",
		"-vga", "std", "-display", "none", "-no-reboot",
		"-qmp", f"unix:{monitor},server,nowait",

		# The console as text, for plan/tools/guest/serial.py, when SSH is
		# not up yet or is what is being investigated.  A kernel built
		# with CONFIG_PCAT_SERIAL_MIRROR=y writes it and reads from it.
		"-serial", f"unix:{serial},server,nowait",

		# The stub is offered but not waited for: a guest that nobody
		# is debugging must still boot.
		"-gdb", f"tcp:127.0.0.1:{debug_port}",
	]
	command += shlex.split(arguments.qemu_extra or "")
	log = open(RUNTIME / "qemu.log", "wb")
	process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
				   stdin=subprocess.DEVNULL, start_new_session=True)
	SESSION.write_text(json.dumps({
		"pid": process.pid,
		"ssh_port": ssh_port,
		"debug_port": debug_port,
		"monitor": str(monitor),
		"serial": str(serial),
		"image": str(image),
		"disk": str(disk),
		"symbols": arguments.symbols,
	}, indent=1) + "\n")
	print(f"guest: pid {process.pid}, ssh 127.0.0.1:{ssh_port}, "
	      f"gdb 127.0.0.1:{debug_port}")
	return 0


def wait(arguments: argparse.Namespace) -> int:
	"""Waits until the guest answers on SSH."""
	session = read_session()
	deadline = time.monotonic() + arguments.timeout
	while time.monotonic() < deadline:
		if not alive(session["pid"]):
			fail("the guest stopped before it answered")
		result = subprocess.run(
			["ssh", *ssh_options(session), f"root@127.0.0.1", "true"],
			capture_output=True, text=True)
		if result.returncode == 0:
			print("guest: ready")
			return 0
		time.sleep(2.0)
	fail(f"no answer on SSH within {arguments.timeout:g}s")


def alive(pid: int) -> bool:
	"""Reports whether the emulator is still running."""
	try:
		os.kill(pid, 0)
	except OSError:
		return False
	return True


def run(arguments: argparse.Namespace) -> int:
	"""Runs one command in the guest and passes its output and status on."""
	session = read_session()
	result = subprocess.run(
		["ssh", *ssh_options(session), "root@127.0.0.1", "--",
		 " ".join(arguments.words)])
	return result.returncode


def put(arguments: argparse.Namespace) -> int:
	"""Copies one file into the guest."""
	session = read_session()
	return subprocess.run(
		["scp", *ssh_options(session, "-P"), arguments.source,
		 f"root@127.0.0.1:{arguments.destination}"]).returncode


def get(arguments: argparse.Namespace) -> int:
	"""Copies one file out of the guest."""
	session = read_session()
	return subprocess.run(
		["scp", *ssh_options(session, "-P"),
		 f"root@127.0.0.1:{arguments.source}",
		 arguments.destination]).returncode


def lldb(arguments: argparse.Namespace) -> int:
	"""Runs the guest's own lldb, over the connection.

	The debugger runs where the program does.  Its commands are given as
	arguments so that a caller that is not a person can use it.
	"""
	session = read_session()
	command = ["/usr/bin/lldb", "--batch"] + list(arguments.arguments)
	return subprocess.run(
		["ssh", *ssh_options(session), "root@127.0.0.1", "--",
		 " ".join(shlex.quote(word) for word in command)]).returncode


def kgdb(arguments: argparse.Namespace) -> int:
	"""Attaches the host's debugger to the emulator, for the kernel.

	The kernel is not a process the guest can debug: it is what would be
	stopped.  The emulator's stub is the way in, and the symbols come from
	the vmunix the image was built from.
	"""
	session = read_session()
	symbols = arguments.symbols or session.get("symbols")
	if not symbols:
		fail("no kernel symbols (pass --symbols build/<x>/vmunix)")
	script = [f"target remote 127.0.0.1:{session['debug_port']}"]
	script += list(arguments.commands)
	command = ["gdb", "-q", "-batch"]
	for line in script:
		command += ["-ex", line]
	command.append(symbols)
	return subprocess.run(command).returncode


def screenshot(arguments: argparse.Namespace) -> int:
	"""Photographs the screen, which is the only view of the console."""
	session = read_session()
	boot_test = ROOT / "plan/tools/boot-test.py"
	return subprocess.run(
		[sys.executable, str(boot_test), "--monitor", session["monitor"],
		 "--screenshot", arguments.output, "--timeout", "1",
		 "--pattern", arguments.pattern]).returncode


def stop(arguments: argparse.Namespace) -> int:
	"""Stops the guest and forgets it."""
	if not SESSION.exists():
		return 0
	session = json.loads(SESSION.read_text())
	if alive(session["pid"]):
		os.kill(session["pid"], 15)
		deadline = time.monotonic() + 10.0
		while time.monotonic() < deadline and alive(session["pid"]):
			time.sleep(0.2)
		if alive(session["pid"]):
			os.kill(session["pid"], 9)
	SESSION.unlink(missing_ok=True)
	print("guest: stopped")
	return 0


def main() -> int:
	"""Runs one harness command."""
	parser = argparse.ArgumentParser(prog="guest")
	commands = parser.add_subparsers(dest="command", required=True)

	commands.add_parser("keys", help="make the harness key pair")
	commands.add_parser("extra-files", help="print the make arguments")

	start_parser = commands.add_parser("start", help="boot a guest")
	start_parser.add_argument("image")
	start_parser.add_argument("--qemu", default="qemu-system-x86_64")
	# 8 GiB, the memory amd64 is tested with (2026-09-24 user decision).
	start_parser.add_argument("--memory", type=int, default=8192)
	start_parser.add_argument("--cpus", type=int, default=4)
	start_parser.add_argument("--symbols", default=None)
	start_parser.add_argument("--qemu-extra", default=None)
	start_parser.add_argument("--disk", choices=("usb", "nvme"), default="usb",
	    help="how the boot disk is attached (default usb)")
	start_parser.add_argument("--no-kvm", action="store_true")
	start_parser.add_argument(
		"--ovmf-code", default="/usr/share/OVMF/OVMF_CODE_4M.fd")
	start_parser.add_argument(
		"--ovmf-vars", default="/usr/share/OVMF/OVMF_VARS_4M.fd")

	wait_parser = commands.add_parser("wait", help="wait for SSH")
	wait_parser.add_argument("--timeout", type=float, default=240.0)

	run_parser = commands.add_parser("run", help="run a command in the guest")
	run_parser.add_argument("words", nargs=argparse.REMAINDER)

	put_parser = commands.add_parser("put", help="copy a file in")
	put_parser.add_argument("source")
	put_parser.add_argument("destination")

	get_parser = commands.add_parser("get", help="copy a file out")
	get_parser.add_argument("source")
	get_parser.add_argument("destination")

	lldb_parser = commands.add_parser("lldb", help="debug in the guest")
	lldb_parser.add_argument("arguments", nargs=argparse.REMAINDER)

	kgdb_parser = commands.add_parser("kgdb", help="debug the kernel")
	kgdb_parser.add_argument("--symbols", default=None)
	kgdb_parser.add_argument("commands", nargs="*")

	shot_parser = commands.add_parser("screenshot", help="photograph the screen")
	shot_parser.add_argument("output")
	shot_parser.add_argument("--pattern", default="$^")

	commands.add_parser("stop", help="stop the guest")
	arguments = parser.parse_args()

	handlers = {
		"keys": lambda _: (make_keys(), print(f"guest: {KEY}"))[1] or 0,
		"extra-files": lambda _: (print(extra_files()), 0)[1],
		"start": start,
		"wait": wait,
		"run": run,
		"put": put,
		"get": get,
		"lldb": lldb,
		"kgdb": kgdb,
		"screenshot": screenshot,
		"stop": stop,
	}
	return handlers[arguments.command](arguments)


if __name__ == "__main__":
	sys.exit(main())
