#!/usr/bin/env python3
"""Run one native scalar-input cell on a disposable USB boot image."""
from pathlib import Path
import argparse,hashlib,json,re,runpy,shutil,struct,subprocess,time
REPO=Path(__file__).resolve().parents[3]
helpers=runpy.run_path(str(Path(__file__).with_name('capture-uas-descriptors.py')))
QMP,digest=helpers['QMP'],helpers['digest']
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--image',type=Path,required=True);p.add_argument('--view',action='store_true');p.add_argument('--output-view',action='store_true');o=p.parse_args()
tag='OUTPUT-VIEW' if o.output_view else 'INPUT-VIEW'
out=o.output.resolve();out.relative_to(REPO/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
source=o.image.resolve();record={'source_sha256':digest(source),'view_expected':o.view,'direction':'output' if o.output_view else 'input'}
subprocess.run(['cp','--reflink=auto','--sparse=always',str(source),str(out/'boot.img')],check=True)
sysroot=REPO/'build/amd64/sysroot/usr';obj=out/'guest.o';binary=out/'ivtest'
subprocess.run([str(REPO/'build/llvm/bin/clang'),'--target=x86_64-unknown-zedbsd','-nostdinc','-isystem',str(sysroot/'include'),'-I'+str(REPO/'include/uapi'),'-DZEDBSD_USER_ABI_LP64=1','-ffreestanding','-fno-pie','-O1','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-c',str(Path(__file__).with_name('output-view-guest.c' if o.output_view else 'input-view-guest.c')),'-o',str(obj)],check=True)
subprocess.run(['ld','-m','elf_x86_64','--gc-sections','-nostdlib','-static','-z','max-page-size=4096','-z','stack-size=0x100000','-T',str(REPO/'platform/amd64/user.ld'),str(sysroot/'lib/crt0.o'),str(sysroot/'lib/libc.o'),str(obj),'-o',str(binary)],check=True)
with source.open('rb') as f:
 f.seek(512);header=f.read(512);assert header[:8]==b'EFI PART'
 lba,slots,width=struct.unpack_from('<QII',header,72);assert slots<=4096 and 128<=width<=4096
 f.seek(lba*512);entries=f.read(slots*width)
candidates=[]
for i in range(slots):
 entry=entries[i*width:(i+1)*width]
 if entry[:16]==bytes(16):continue
 first=struct.unpack_from('<Q',entry,32)[0]
 if subprocess.run(['mdir','-i',f'{source}@@{first*512}','::/rootfs.img'],capture_output=True).returncode==0:candidates.append((i+1,first))
assert len(candidates)==1
partition,first=candidates[0]
subprocess.run(['mcopy','-i',f'{out / "boot.img"}@@{first*512}',str(binary),'::/ivtest'],check=True)
shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',out/'vars.fd')
args=['qemu-system-x86_64','-machine','q35,usb=off','-m','512','-smp','4','-display','none','-serial','none','-nic','none','-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,file={out}/vars.fd','-device','qemu-xhci,id=xhci','-device','usb-kbd,bus=xhci.0,port=3','-drive',f'if=none,format=raw,id=boot,file={out}/boot.img','-device','usb-storage,bus=xhci.0,port=1,drive=boot,bootindex=1','-debugcon',f'file:{out}/guest.log','-qmp',f'unix:{out}/qmp.sock,server=on,wait=off']
(out/'argv.json').write_text(json.dumps(args,indent=2));record['guest_sha256']=digest(binary)
qmp=None
with (out/'qemu.log').open('w') as log:
 process=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT)
 def wait(pattern,start=0,seconds=150):
  deadline=time.monotonic()+seconds
  while time.monotonic()<deadline:
   if process.poll() is not None:raise RuntimeError('QEMU exited')
   text=(out/'guest.log').read_text(errors='replace') if (out/'guest.log').exists() else ''
   if re.search(r'panic:|fatal:|'+tag+r' FAIL',text,re.I):raise RuntimeError('guest failure')
   match=re.search(pattern,text[start:])
   if match:return match,text
   time.sleep(.2)
  raise TimeoutError(pattern)
 def line(value):
  keys={' ':'spc','/':'slash','-':'minus','.':'dot','\n':'ret'}
  for c in value+'\n':
   qmp.call('human-monitor-command',{'command-line':'sendkey '+('shift-'+c.lower() if c.isupper() else keys.get(c,c))});time.sleep(.06)
 try:
  wait('login:');qmp=QMP(out/'qmp.sock');line('root');wait('Password:');line('');_,text=wait(r'root[^\n]*[$#]')
  for cmd in ['mkdir -p /run/ivboot',f'mount -t fat -r sda{partition} /run/ivboot','cp /run/ivboot/ivtest /run/ivtest','chmod 700 /run/ivtest','umount /run/ivboot']:
   start=len(text);line(cmd);_,text=wait(r'root[^\n]*[$#]',start)
  start=len(text);line('/run/ivtest');_,text=wait(tag+' PASS',start,180)
  matches=re.findall(tag+r' METRIC copy=(\d+) view=(\d+) cpu_us=(\d+) wall_ns=(\d+)',text)
  assert len(matches)==8
  record['samples']=[dict(zip(['copy','view','cpu_us','wall_ns'],map(int,row))) for row in matches]
  for metric in record['samples']:
   assert metric['view']==(256*65536 if o.view else 0)
   assert metric['copy']==(0 if o.view else 256*65536)
  record['metric']={key:sum(row[key] for row in record['samples']) for key in ['copy','view','cpu_us','wall_ns']}
  record['status']='PASS'
 except BaseException as e:
  record['status']='FAIL';record['error']=repr(e)
  if qmp:
   try:record['registers']=qmp.call('human-monitor-command',{'command-line':'info registers'})
   except Exception:pass
  raise
 finally:
  if qmp:qmp.close()
  if process.poll() is None:
   process.terminate()
   try:process.wait(timeout=5)
   except subprocess.TimeoutExpired:process.kill();process.wait()
  record['source_unchanged']=digest(source)==record['source_sha256']
  (out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record),flush=True)
