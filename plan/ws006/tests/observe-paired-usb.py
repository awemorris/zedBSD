#!/usr/bin/env python3
"""Bounded QMP stack/register observations on a disposable paired-controller boot."""
from pathlib import Path
import json
import re
import socket
import subprocess
import sys
import time

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws006/temp')
out.mkdir(parents=True, exist_ok=False)
source = Path(sys.argv[2]).resolve()
source.relative_to(repo / 'plan/ws006/temp')
subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(out / 'boot.img')], check=True)
subprocess.run(['cp', '/usr/share/OVMF/OVMF_VARS_4M.fd', str(out / 'vars.fd')], check=True)
args = ['qemu-system-x86_64', '-machine', 'q35,usb=off,i8042=off', '-m', '512', '-smp', '4',
 '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
 '-drive', f'if=pflash,format=raw,unit=1,file={out}/vars.fd', '-device', 'VGA,id=video0',
 '-drive', f'if=none,id=boot,file={out}/boot.img,format=raw',
 '-device', 'ich9-usb-ehci1,id=ehci',
 '-device', 'ich9-usb-uhci1,id=uhci1,masterbus=ehci.0,firstport=0',
 '-device', 'ich9-usb-uhci2,id=uhci2,masterbus=ehci.0,firstport=2',
 '-device', 'ich9-usb-uhci3,id=uhci3,masterbus=ehci.0,firstport=4',
 '-device', 'usb-storage,bus=ehci.0,port=6,drive=boot,id=rootstick,bootindex=1',
 '-device', 'usb-kbd,bus=ehci.0,port=1,id=kbd,serial=in-t41-kbd,usb_version=1,display=video0',
 '-display', 'none', '-serial', 'none', '-debugcon', f'file:{out}/guest.log',
 '-qmp', f'unix:{out}/qmp.sock,server=on,wait=off', '-no-reboot']
(out / 'argv.json').write_text(json.dumps(args, indent=2))
log = (out / 'qemu.log').open('w')
proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
conn = socket.socket(socket.AF_UNIX)
channel = None
observations = []
try:
 for _ in range(100):
  try:
   conn.connect(str(out / 'qmp.sock'))
   break
  except (FileNotFoundError, ConnectionRefusedError):
   time.sleep(.05)
 conn.settimeout(5)
 channel = conn.makefile('rwb', buffering=0)
 json.loads(channel.readline())
 sequence = 0
 def command(name, arguments=None):
  global sequence
  sequence += 1
  request = {'execute': name, 'id': sequence}
  if arguments is not None:
   request['arguments'] = arguments
  channel.write((json.dumps(request) + '\n').encode())
  while True:
   response = json.loads(channel.readline())
   if response.get('id') == sequence:
    return response
 command('qmp_capabilities')
 deadline = time.monotonic() + 60
 while time.monotonic() < deadline:
  text = (out / 'guest.log').read_text(errors='replace') if (out / 'guest.log').exists() else ''
  if 'boot: platform devices detected:' in text or 'login:' in text:
   break
  if proc.poll() is not None:
   raise RuntimeError('guest exited')
  time.sleep(.2)
 for _ in range(3):
  time.sleep(2)
  sample = {}
  command('stop')
  for query in ['info registers -a', 'info usb', 'info pci', 'xp /4wx 0xf0800000', 'xp /24wx 0xf0800020']:
   sample[query] = command('human-monitor-command', {'command-line': query})
  sample['stacks'] = []
  registers = sample['info registers -a']['return']
  for cpu, block in enumerate(re.split(r'CPU#\d+', registers)[1:]):
   command('human-monitor-command', {'command-line': f'cpu {cpu}'})
   frame = int(re.search(r'RBP=([0-9a-f]+)', block).group(1), 16)
   frames = []
   for depth in range(14):
    reply = command('human-monitor-command', {'command-line': f'x /2gx 0x{frame:x}'})
    frames.append(reply)
    values = re.findall(r'0x([0-9a-f]{16})', reply.get('return', ''))
    if len(values) != 2:
     break
    next_frame = int(values[0], 16)
    if next_frame <= frame or next_frame - frame > 65536:
     break
    frame = next_frame
   sample['stacks'].append({'cpu': cpu, 'frames': frames})
  if len(sys.argv) > 3:
   layout = json.loads(Path(sys.argv[3]).read_text())
   symbols = subprocess.check_output(['nm', str(source.parents[1] / 'vmunix')], text=True)
   process0 = int(re.search(r'^([0-9a-f]+) B process0$', symbols, re.M).group(1), 16)
   def read_word(address):
    reply = command('human-monitor-command', {'command-line': f'x /1gx 0x{address:x}'})
    values = re.findall(r'0x([0-9a-f]{16})', reply.get('return', ''))
    if len(values) != 1:
     raise RuntimeError(reply)
    return int(values[0], 16)
   pointer = read_word(process0 + layout['process_threads'])
   sample['threads'] = []
   seen = set()
   while pointer:
    if pointer in seen or len(seen) > 100:
     raise RuntimeError('invalid thread list')
    seen.add(pointer)
    record = {'pointer': pointer}
    for field in ['state', 'tid', 'task', 'entry']:
     record[field] = read_word(pointer + layout['thread_' + field])
     if field in ['state', 'tid']:
      record[field] &= 0xffffffff
    for field in ['cpu', 'priority', 'queue']:
     record[field] = read_word(pointer + layout['thread_sched'] + layout['sched_' + field]) & 0xffffffff
    if record['task'] and (record['state'] != 2 or record['tid'] == 0):
     sp = read_word(record['task'] + layout['task_rsp'])
     record['saved_rsp'] = sp
     frame = read_word(sp + 32)
     record['returns'] = [read_word(sp + 56)]
     for depth in range(12):
      if frame < 0xffff800000000000:
       break
      next_frame = read_word(frame)
      record['returns'].append(read_word(frame + 8))
      if next_frame <= frame or next_frame - frame > 65536:
       break
      frame = next_frame
    sample['threads'].append(record)
    pointer = read_word(pointer + layout['thread_next'])
  command('cont')
  observations.append(sample)
 (out / 'observations.json').write_text(json.dumps(observations, indent=2))
 print('observations captured')
finally:
 if channel is not None:
  try:
   command('quit')
  except (OSError, ValueError):
   pass
  channel.close()
 conn.close()
 try:
  proc.wait(timeout=5)
 except subprocess.TimeoutExpired:
  proc.kill()
  proc.wait()
 log.close()
