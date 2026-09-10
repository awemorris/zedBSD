#!/usr/bin/env python3
"""Exercise actual x86 fixed syscall/fault entries on a disposable image."""
from pathlib import Path
import argparse,hashlib,json,re,runpy,shutil,struct,subprocess,time
REPO=Path(__file__).resolve().parents[3]
helpers=runpy.run_path(str(Path(__file__).with_name('capture-uas-descriptors.py')))
QMP,digest=helpers['QMP'],helpers['digest']
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--image',type=Path,required=True);p.add_argument('--platform',choices=['amd64','pcat'],default='amd64');o=p.parse_args()
arch='i386' if o.platform=='pcat' else 'amd64'
target='i386-unknown-zedbsd' if o.platform=='pcat' else 'x86_64-unknown-zedbsd'
abi='-DZEDBSD_USER_ABI_ILP32=1' if o.platform=='pcat' else '-DZEDBSD_USER_ABI_LP64=1'
tag='HAL-ENTRY'
softfloat=[str(REPO/'build/i386/sysroot/usr/lib/libzedbsd-compiler-rt.o')] if o.platform=='pcat' else []
cpu_flags=['-msoft-float','-mno-80387','-mno-mmx','-mno-sse','-mno-sse2'] if o.platform=='pcat' else []
out=o.output.resolve();out.relative_to(REPO/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
source=o.image.resolve();record={'source_sha256':digest(source),'purpose':'fixed HAL syscall/fault runtime'}
subprocess.run(['cp','--reflink=auto','--sparse=always',str(source),str(out/'boot.img')],check=True)
sysroot=REPO/f'build/{arch}/sysroot/usr';obj=out/'guest.o';binary=out/'ivtest'
subprocess.run([str(REPO/'build/llvm/bin/clang'),'--target='+target,'-nostdinc','-isystem',str(sysroot/'include'),'-I'+str(REPO/'include/uapi'),abi,*cpu_flags,'-ffreestanding','-fno-pie','-O1','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-c',str(Path(__file__).with_name('fixed-entry-guest.c')),'-o',str(obj)],check=True)
subprocess.run(['ld','-m','elf_i386' if o.platform=='pcat' else 'elf_x86_64','--gc-sections','-nostdlib','-static','-z','max-page-size=4096','-z','stack-size=0x100000','-T',str(REPO/f'platform/{o.platform}/user.ld'),str(sysroot/'lib/crt0.o'),str(sysroot/'lib/libc.o'),str(obj),*softfloat,'-o',str(binary)],check=True)
candidates=[]
with source.open('rb') as f:
 f.seek(512);header=f.read(512)
 if header[:8]==b'EFI PART':
  lba,slots,width=struct.unpack_from('<QII',header,72);assert slots<=4096 and 128<=width<=4096
  f.seek(lba*512);entries=f.read(slots*width)
  partitions=[(i+1,struct.unpack_from('<Q',entries,i*width+32)[0])
              for i in range(slots) if entries[i*width:i*width+16]!=bytes(16)]
 else:
  assert o.platform=='pcat'
  f.seek(0);mbr=f.read(512);assert mbr[510:]==b'\x55\xaa'
  partitions=[(i+1,struct.unpack_from('<I',mbr,446+i*16+8)[0])
              for i in range(4) if mbr[446+i*16+4]!=0]
for number,first in partitions:
 if subprocess.run(['mdir','-i',f'{source}@@{first*512}','::/rootfs.img'],capture_output=True).returncode==0:
  candidates.append((number,first))
assert len(candidates)==1
partition,first=candidates[0]
subprocess.run(['mcopy','-i',f'{out / "boot.img"}@@{first*512}',str(binary),'::/ivtest'],check=True)
shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',out/'vars.fd')
args=['qemu-system-x86_64','-machine','q35,usb=off','-m','512','-smp','4','-display','none','-serial','none','-nic','none','-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,file={out}/vars.fd','-device','qemu-xhci,id=xhci','-device','usb-kbd,bus=xhci.0,port=3','-drive',f'if=none,format=raw,id=boot,file={out}/boot.img','-device','usb-storage,bus=xhci.0,port=1,drive=boot,bootindex=1','-debugcon',f'file:{out}/guest.log','-qmp',f'unix:{out}/qmp.sock,server=on,wait=off']
if o.platform=='pcat':
 args=['qemu-system-i386','-machine','pc','-m','512','-smp','1',
       '-display','none','-serial','none','-nic','none','-no-reboot',
       '-drive',f'if=ide,index=0,format=raw,file={out}/boot.img',
       '-debugcon',f'file:{out}/guest.log',
       '-qmp',f'unix:{out}/qmp.sock,server=on,wait=off']
(out/'argv.json').write_text(json.dumps(args,indent=2));record['guest_sha256']=digest(binary)
qmp=None
with (out/'qemu.log').open('w') as log:
 process=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT)
 def wait(pattern,start=0,seconds=150):
  deadline=time.monotonic()+seconds
  while time.monotonic()<deadline:
   if process.poll() is not None:raise RuntimeError('QEMU exited')
   text=(out/'guest.log').read_text(errors='replace') if (out/'guest.log').exists() else ''
   if re.search(r'panic:|fatal:|Segmentation fault|'+tag+r' FAIL',text,re.I):raise RuntimeError('guest failure')
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
  record['status']='PASS'
 except BaseException as e:
  record['status']='FAIL';record['error']=repr(e)
  if qmp:
   try:record['registers']=qmp.call('human-monitor-command',{'command-line':'info registers'})
   except Exception:pass
   try:
    symbols=subprocess.check_output(['nm',str(REPO/f'build/{o.platform}/vmunix')],text=True)
    address=next(row.split()[0] for row in symbols.splitlines() if row.endswith(' user_fault_probe'))
    record['fault_probe']=qmp.call('human-monitor-command',{'command-line':'x /12gx 0x'+address})
   except Exception as error:record['fault_probe_error']=repr(error)
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
