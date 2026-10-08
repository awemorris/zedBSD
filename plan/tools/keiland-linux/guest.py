#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Controls an isolated Debian QEMU guest through loopback SSH and QMP."""
import importlib.util
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
GUEST_DIR = Path(os.environ.get('GUEST_DIR', 'build/keiland-linux/guest')).resolve()
GUEST_IMAGE = Path(os.environ.get('GUEST_IMAGE', GUEST_DIR / 'guest.img')).resolve()
GUEST_RUN = Path(os.environ.get('GUEST_RUN', 'build/keiland-linux/run')).resolve()
PORT = os.environ.get('SSH_PORT', '2225')
PIDFILE = GUEST_RUN / 'qemu.pid'
SSH_USER = os.environ.get('SSH_USER', 'root')
SSH_OPTIONS = ['-i', str(GUEST_DIR / 'id_ed25519'), '-o', 'StrictHostKeyChecking=no',
	'-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR', '-o', 'ConnectTimeout=2',
	'-o', 'BatchMode=yes']


def run(arguments, **kwargs):
	return subprocess.run(arguments, check=True, timeout=kwargs.pop('timeout', 60), **kwargs)


def ssh(arguments, **kwargs):
	return run(['ssh', *SSH_OPTIONS, '-p', PORT, SSH_USER + '@127.0.0.1', *arguments], **kwargs)


def qmp(command, arguments=None):
	return run(['python3', str(TOOLS.parent / 'qmp.py'), str(GUEST_RUN / 'qmp.sock'),
		command, json.dumps(arguments or {})], stdout=subprocess.PIPE, text=True)


def alive():
	if not PIDFILE.exists():
		return False
	pid = int(PIDFILE.read_text())
	try:
		# Refuse stale PID files that now refer to a different program.
		command = Path(f'/proc/{pid}/cmdline').read_bytes()
		return str(GUEST_RUN / 'qmp.sock').encode() in command
	except FileNotFoundError:
		return False


def ready():
	try:
		ssh(['true'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=4)
		return True
	except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
		return False


def screenshot(path):
	path = Path(path).resolve()
	path.parent.mkdir(parents=True, exist_ok=True)
	qmp('screendump', {'filename': str(path), 'format': 'png'})


def input_events(events):
	qmp('input-send-event', {'device': 'video0', 'head': 0, 'events': events})


def key(keys):
	for down, sequence in ((True, keys), (False, reversed(keys))):
		input_events([{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': k}}}
			for k in sequence])


def start():
	if alive():
		if not ready():
			raise RuntimeError('guest process exists but SSH is unavailable')
		print('guest: ready (already running)')
		return
	if not GUEST_IMAGE.is_file():
		raise RuntimeError('build the guest image first')
	GUEST_RUN.mkdir(parents=True, exist_ok=True)
	for name in ('overlay.qcow2', 'qmp.sock', 'qemu.pid'):
		(GUEST_RUN / name).unlink(missing_ok=True)
	run(['qemu-img', 'create', '-f', 'qcow2', '-b', str(GUEST_IMAGE), '-F', 'raw',
		str(GUEST_RUN / 'overlay.qcow2')])
	# The sound device's output: nowhere, or for the sound tests (WS191 p004) the WAV GUEST_AUDIO_WAV names.
	audiodev = 'none,id=snd0'
	wav = os.environ.get('GUEST_AUDIO_WAV')
	if wav:
		audiodev = f'wav,id=snd0,path={Path(wav).resolve()},out.frequency=48000,out.channels=2,out.format=s16'
	run(['qemu-system-x86_64', '-machine', 'q35,vmport=off', '-accel', 'kvm', '-cpu', 'host',
		'-m', '4G', '-smp', '4', '-display', 'none', '-kernel', str(GUEST_DIR / 'vmlinuz'),
		'-initrd', str(GUEST_DIR / 'initrd.img'), '-append', 'root=/dev/vda rw console=ttyS0 quiet',
		'-drive', f'file={GUEST_RUN}/overlay.qcow2,format=qcow2,if=virtio',
		'-device', 'virtio-vga,id=video0', '-device', 'virtio-keyboard-pci,display=video0',
		'-device', 'virtio-tablet-pci,display=video0', '-audiodev', audiodev,
		'-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0',
		'-netdev', f'user,id=n0,hostfwd=tcp:127.0.0.1:{PORT}-:22', '-device', 'virtio-net-pci,netdev=n0',
		'-qmp', f'unix:{GUEST_RUN}/qmp.sock,server=on,wait=off',
		'-serial', f'file:{GUEST_RUN}/serial.log', '-pidfile', str(PIDFILE), '-daemonize'])
	deadline = time.monotonic() + 180
	while time.monotonic() < deadline:
		if ready():
			print('guest: ready')
			return
		if not alive():
			raise RuntimeError('QEMU exited before SSH became ready')
		time.sleep(1)
	raise RuntimeError('SSH did not become ready within 180 seconds')


def stop():
	if alive():
		pid = int(PIDFILE.read_text())
		try:
			ssh(['poweroff'], timeout=5)
		except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
			pass
		deadline = time.monotonic() + 30
		while alive() and time.monotonic() < deadline:
			time.sleep(1)
		if alive():
			try:
				qmp('quit')
			except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
				pass
			time.sleep(1)
		if alive():
			os.kill(pid, signal.SIGTERM)
			time.sleep(1)
		if alive():
			raise RuntimeError('QEMU is still alive; overlay retained')
	(GUEST_RUN / 'overlay.qcow2').unlink(missing_ok=True)
	print('guest: stopped')


def main():
	command, *arguments = sys.argv[1:]
	if command == 'start':
		start()
	elif command == 'stop':
		stop()
	elif command == 'gdm-setup':
		# AccountsService selects the existing session for the isolated guest user.
		ssh(['busctl call org.freedesktop.Accounts /org/freedesktop/Accounts org.freedesktop.Accounts FindUserByName s kei'], stdout=subprocess.PIPE, text=True)
		identity = ssh(['id -u kei'], stdout=subprocess.PIPE, text=True).stdout.strip()
		if not identity.isdecimal():
			raise RuntimeError('guest user has no numeric uid')
		path = '/org/freedesktop/Accounts/User' + identity
		ssh(['busctl call org.freedesktop.Accounts ' + path + ' org.freedesktop.Accounts.User SetSession s keiland'])
		ssh(['busctl call org.freedesktop.Accounts ' + path + ' org.freedesktop.Accounts.User SetSessionType s wayland'])
		ssh(['systemctl restart gdm'])
	elif command == 'ssh':
		ssh(arguments, timeout=int(os.environ.get('GUEST_COMMAND_TIMEOUT', '120')))
	elif command in ('put', 'get'):
		source, destination = arguments
		remote = SSH_USER + '@127.0.0.1:'
		if command == 'put':
			destination = remote + destination
		else:
			source = remote + source
		run(['scp', *SSH_OPTIONS, '-P', PORT, source, destination], timeout=120)
	elif command == 'screenshot':
		screenshot(arguments[0])
	elif command == 'key':
		key(arguments)
	elif command == 'type':
		text = ' '.join(arguments)
		mapping = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash'}
		if any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789 -./' for c in text):
			raise ValueError('type supports lowercase ASCII, digits, space, dash, dot and slash')
		for c in text:
			key([mapping.get(c, c)])
	elif command in ('move', 'click'):
		x, y = int(arguments[0]), int(arguments[1])
		screenshot(GUEST_RUN / 'input-size.png')
		spec = importlib.util.spec_from_file_location('png_probe', TOOLS / 'png-probe.py')
		probe = importlib.util.module_from_spec(spec)
		spec.loader.exec_module(probe)
		width, height, _, _ = probe.read_png(GUEST_RUN / 'input-size.png')
		if not (0 <= x < width and 0 <= y < height):
			raise ValueError('pointer outside screen')
		input_events([{'type': 'abs', 'data': {'axis': 'x', 'value': x * 32767 // (width - 1)}},
			{'type': 'abs', 'data': {'axis': 'y', 'value': y * 32767 // (height - 1)}}])
		if command == 'click':
			button = arguments[2] if len(arguments) > 2 else 'left'
			if button not in ('left', 'right', 'middle'):
				raise ValueError('invalid pointer button')
			for down in (True, False):
				input_events([{'type': 'btn', 'data': {'down': down, 'button': button}}])
	elif command == 'status':
		print('guest: running' if alive() else 'guest: stopped')
		if alive():
			print('ssh: ready' if ready() else 'ssh: unavailable')
	else:
		raise ValueError('unknown command: ' + command)


if __name__ == '__main__':
	try:
		main()
	except (ValueError, RuntimeError, subprocess.SubprocessError) as error:
		print('guest: ' + str(error), file=sys.stderr)
		sys.exit(1)
