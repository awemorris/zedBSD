#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Controls the isolated FreeBSD 15.1 QEMU guest of WS137 through loopback SSH and QMP.

The guest is reached only through 127.0.0.1's forwarded SSH port and the QMP socket.  Its serial port is not
connected and no console or serial log is read (AGENTS.md, the WS109 FreeBSD guest exception).  No host device is
passed through; the display is an emulated virtio-vga used only for QMP screendumps.
"""
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[2]


def absolute(value, default):
	path = Path(os.environ.get(value, default))
	return (path if path.is_absolute() else ROOT / path).resolve()


GUEST_DIR = absolute('GUEST_DIR', 'build/keiland-freebsd/guest')
GUEST_IMAGE = absolute('GUEST_IMAGE', GUEST_DIR / 'guest.qcow2')
GUEST_KEY = absolute('GUEST_KEY', GUEST_DIR / 'id_ed25519')
GUEST_RUN = absolute('GUEST_RUN', 'build/keiland-freebsd/run')
PORT = os.environ.get('SSH_PORT', '2235')
MEMORY = os.environ.get('GUEST_MEMORY', '8G')
CPUS = os.environ.get('GUEST_CPUS', '8')
PIDFILE = GUEST_RUN / 'qemu.pid'
SSH_USER = os.environ.get('SSH_USER', 'root')
SSH_OPTIONS = ['-i', str(GUEST_KEY), '-o', 'StrictHostKeyChecking=no',
	'-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR', '-o', 'ConnectTimeout=3',
	'-o', 'BatchMode=yes', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4']


def run(arguments, **kwargs):
	return subprocess.run(arguments, check=True, timeout=kwargs.pop('timeout', 60), **kwargs)


def ssh(arguments, **kwargs):
	return run(['ssh', *SSH_OPTIONS, '-p', PORT, SSH_USER + '@127.0.0.1', *arguments], **kwargs)


def qmp(command, arguments=None):
	return run(['python3', str(TOOLS.parent / 'qmp.py'), str(GUEST_RUN / 'qmp.sock'),
		command, json.dumps(arguments or {})], stdout=subprocess.PIPE, text=True, timeout=30)


def alive():
	if not PIDFILE.exists():
		return False
	try:
		pid = int(PIDFILE.read_text())
		# Refuse stale PID files that now refer to a different program.
		command = Path(f'/proc/{pid}/cmdline').read_bytes()
		return str(GUEST_RUN / 'qmp.sock').encode() in command
	except (FileNotFoundError, ValueError):
		return False


def ready():
	try:
		ssh(['true'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=8)
		return True
	except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
		return False


def accelerator():
	# The accelerator rule of all amd64 test guests (KVM when /dev/kvm is usable, TCG otherwise).
	script = '. "$1"; qemu_accel_args max'
	output = run(['sh', '-c', script, 'sh', str(TOOLS.parent / 'guest/qemu-accel.sh')],
		stdout=subprocess.PIPE, text=True).stdout
	return output.split()


def screenshot(path):
	path = Path(path).resolve()
	path.parent.mkdir(parents=True, exist_ok=True)
	qmp('screendump', {'filename': str(path), 'format': 'png'})


def wait_ready(limit):
	deadline = time.monotonic() + limit
	while time.monotonic() < deadline:
		if ready():
			return
		if not alive():
			raise RuntimeError('QEMU exited before SSH became ready')
		time.sleep(2)
	raise RuntimeError(f'SSH did not become ready within {limit} seconds')


def launch(seed=None):
	"""Starts QEMU.  With a seed the image itself is written (preparation); otherwise a fresh overlay is."""
	if alive():
		if not ready():
			raise RuntimeError('guest process exists but SSH is unavailable')
		print('guest: ready (already running)')
		return False
	if not GUEST_IMAGE.is_file():
		raise RuntimeError(f'{GUEST_IMAGE} is missing; run plan/tools/keiland-freebsd/build-guest.sh first')
	if not GUEST_KEY.is_file():
		raise RuntimeError(f'{GUEST_KEY} is missing')
	GUEST_RUN.mkdir(parents=True, exist_ok=True)
	for name in ('overlay.qcow2', 'qmp.sock', 'qemu.pid'):
		(GUEST_RUN / name).unlink(missing_ok=True)
	drives = []
	if seed is None:
		# The base image stays read-only; it may be another checkout's prepared image.
		run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-b', str(GUEST_IMAGE), '-F', 'qcow2',
			str(GUEST_RUN / 'overlay.qcow2')])
		drives += ['-drive', f'file={GUEST_RUN}/overlay.qcow2,format=qcow2,if=virtio']
	else:
		drives += ['-drive', f'file={GUEST_IMAGE},format=qcow2,if=virtio',
			'-drive', f'file={Path(seed).resolve()},format=raw,media=cdrom,readonly=on']
	# A sound device only for the sound tests (WS191 p004): GUEST_AUDIO_WAV names the WAV the guest's playback goes to.
	sound = []
	wav = os.environ.get('GUEST_AUDIO_WAV')
	if wav:
		sound = ['-audiodev', f'wav,id=snd0,path={Path(wav).resolve()},out.frequency=48000,out.channels=2,out.format=s16',
			'-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0']
	run(['qemu-system-x86_64', '-machine', 'q35', *accelerator(), '-m', MEMORY, '-smp', CPUS,
		*drives, *sound, '-device', 'virtio-vga,id=video0', '-device', 'qemu-xhci,id=xhci',
		'-device', 'usb-tablet,bus=xhci.0', '-device', 'usb-kbd,bus=xhci.0',
		'-netdev', f'user,id=n0,hostfwd=tcp:127.0.0.1:{PORT}-:22', '-device', 'virtio-net-pci,netdev=n0',
		'-display', 'none', '-serial', 'null', '-monitor', 'none',
		'-qmp', f'unix:{GUEST_RUN}/qmp.sock,server=on,wait=off',
		'-pidfile', str(PIDFILE), '-daemonize'])
	return True


def start():
	if launch():
		wait_ready(int(os.environ.get('GUEST_BOOT_TIMEOUT', '240')))
		print('guest: ready')


def boot_time():
	result = ssh(['test ! -e /firstboot && test ! -e /firstboot-reboot && sysctl -n kern.boottime'],
		stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=15)
	return result.stdout.strip()


def prepare(seed):
	"""First boot of the official image: growfs, nuageinit and the first-boot package upgrade with its reboot.

	Returns when SSH answers, the first-boot sentinels are gone and the boot time stays the same for 30 s.
	"""
	launch(seed)
	deadline = time.monotonic() + int(os.environ.get('GUEST_PREPARE_TIMEOUT', '1500'))
	previous = None
	while time.monotonic() < deadline:
		if not alive():
			raise RuntimeError('QEMU exited during the first boot')
		try:
			current = boot_time()
		except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
			current = None
			previous = None
		if current and current == previous:
			print('guest: first boot finished (' + current + ')')
			return
		previous = current
		time.sleep(30 if current else 5)
	raise RuntimeError('the first boot did not finish in time')


def stop():
	if alive():
		pid = int(PIDFILE.read_text())
		try:
			ssh(['shutdown -p now'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
		except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
			pass
		deadline = time.monotonic() + 90
		while alive() and time.monotonic() < deadline:
			time.sleep(1)
		if alive():
			try:
				qmp('quit')
			except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
				pass
			time.sleep(2)
		if alive():
			os.kill(pid, signal.SIGTERM)
			time.sleep(2)
		if alive():
			raise RuntimeError('QEMU is still alive; overlay retained')
	(GUEST_RUN / 'overlay.qcow2').unlink(missing_ok=True)
	print('guest: stopped')


def copy(destination, paths):
	"""Copies the working tree's tracked and unignored files under PATHS to the guest's DESTINATION."""
	if not destination.startswith('/') or destination.rstrip('/') in ('', '/root', '/usr', '/tmp'):
		raise ValueError('copy needs an absolute, dedicated guest directory')
	listed = run(['git', '-C', str(ROOT), 'ls-files', '-z', '-co', '--exclude-standard', '--', *paths],
		stdout=subprocess.PIPE).stdout.split(b'\0')
	deleted = set(run(['git', '-C', str(ROOT), 'ls-files', '-z', '-d', '--', *paths],
		stdout=subprocess.PIPE).stdout.split(b'\0'))
	files = sorted({name for name in listed if name and name not in deleted})
	if not files:
		raise ValueError('nothing to copy')
	archive = subprocess.Popen(['tar', '-C', str(ROOT), '--null', '-T', '-', '-cf', '-'],
		stdin=subprocess.PIPE, stdout=subprocess.PIPE)
	feeder = archive.stdin
	remote = subprocess.Popen(['ssh', *SSH_OPTIONS, '-p', PORT, SSH_USER + '@127.0.0.1',
		f'rm -rf {destination} && mkdir -p {destination} && tar -xf - -C {destination}'], stdin=archive.stdout)
	archive.stdout.close()
	feeder.write(b'\0'.join(files) + b'\0')
	feeder.close()
	if remote.wait(timeout=600) != 0 or archive.wait(timeout=60) != 0:
		raise RuntimeError('copy failed')
	print(f'guest: copied {len(files)} files to {destination}')


def main():
	if len(sys.argv) < 2:
		raise ValueError('usage: guest.sh start|stop|status|ssh|put|get|copy|shot ...')
	command, *arguments = sys.argv[1:]
	if command == 'start':
		start()
	elif command == 'prepare':
		prepare(arguments[0])
	elif command == 'stop':
		stop()
	elif command == 'status':
		print('guest: running' if alive() else 'guest: stopped')
		if alive():
			print('ssh: ready' if ready() else 'ssh: unavailable')
	elif command == 'ssh':
		ssh(arguments, timeout=int(os.environ.get('GUEST_COMMAND_TIMEOUT', '120')))
	elif command in ('put', 'get'):
		source, destination = arguments
		remote = SSH_USER + '@127.0.0.1:'
		if command == 'put':
			destination = remote + destination
		else:
			source = remote + source
		run(['scp', '-r', *SSH_OPTIONS, '-P', PORT, source, destination], timeout=300)
	elif command == 'copy':
		copy(arguments[0], arguments[1:] or ['.'])
	elif command in ('shot', 'screenshot'):
		screenshot(arguments[0])
	else:
		raise ValueError('unknown command: ' + command)


if __name__ == '__main__':
	try:
		main()
	except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as error:
		print('guest: ' + str(error), file=sys.stderr)
		sys.exit(1)
