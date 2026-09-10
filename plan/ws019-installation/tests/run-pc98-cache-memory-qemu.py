#!/usr/bin/env python3
# Focused diagnostic: two disposable IDE disks, 64 MiB RAM, active graphical UI.
import importlib.util, subprocess,time,struct,sys
from pathlib import Path
r=Path(__file__).resolve().parents[3];s=importlib.util.spec_from_file_location('pc',r/'plan/ws025-io-memory-cache/tests/run-p032-pc98-session.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
p=Path(sys.argv[1]).resolve();p.relative_to(r/'plan/ws019-installation/temp');p.mkdir(parents=True)
for name in ['source.img','target.img']:subprocess.run(['cp','--reflink=auto',str(r/'build/pc98/hdd-image.img'),str(p/name)],check=True)
with (p/'target.img').open('r+b') as f:f.seek(1048576+39);f.write(struct.pack('<I',0x18500001))
subprocess.run(['mcopy','-i',str(p/'source.img')+'@@1048576',str(Path(sys.argv[2]).resolve()) if len(sys.argv)>2 else str(r/'plan/ws019-installation/tests/pc98-cache-memory.noct'),'::check.nct'],check=True)
original=m.Guest.send
def send(g,text):
 if '=' not in text:return original(g,text)
 for part in text.split('=')[:-1]:
  for c in part:g.socket.sendall(('sendkey '+({'/':'slash','-':'minus',' ':'spc'}.get(c,c))+' 10\n').encode());time.sleep(.04)
  g.socket.sendall(b'sendkey shift-minus 10\n');time.sleep(.1)
 original(g,text.split('=')[-1])
m.Guest.send=send
g=m.Guest(p/'boot',p/'source.img',[p/'target.img']);g.deadline=time.monotonic()+1200
try:
 g.login()
 for cmd in ['mkdir /tmp/source','/sbin/mount -t fat -r sda1 /tmp/source','mkdir /tmp/target','/sbin/mount -t fat sdb1 /tmp/target']:g.command(cmd)
 g.send('/bin/noct --path=/lib/zedinst /tmp/source/check.nct')
 g.wait('PROBE-END',1000);print(g.screen(),flush=True)
finally:g.close()
