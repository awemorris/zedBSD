#!/usr/bin/env python3
"""Boot a current-kernel installed image with NVMe controllers in both orders."""
import hashlib, json, runpy, shutil, subprocess, sys
from pathlib import Path
R=Path(__file__).resolve().parents[3]
B=runpy.run_path(str(R/'plan/ws019/tests/run-installed-boot-qemu.py'))
def main():
 out=Path(sys.argv[1]).resolve();out.relative_to(R/'plan/ws004/temp');out.mkdir(parents=True)
 accepted=R/'plan/ws019/temp/q184-graphic-coexist1'
 assert json.loads((accepted/'result.json').read_text())['result']=='PASS graphic coexistence'
 result={'status':'FAIL','cases':[],'kernel_sha256':hashlib.sha256((R/'build/amd64/vmunix').read_bytes()).hexdigest()}
 g=None
 try:
  for first in [True,False]:
   case=out/('aux-first' if first else 'installed-first');case.mkdir()
   disk=case/'installed.img';aux=case/'aux.img'
   subprocess.run(['cp','--reflink=auto',str(accepted/'gpt.img'),str(disk)],check=True)
   subprocess.run(['mcopy','-o','-i',str(disk)+'@@269484032',str(R/'build/amd64/vmunix'),'::/vmunix'],check=True)
   installed=subprocess.check_output(['mtype','-i',str(disk)+'@@269484032','::/vmunix'])
   assert hashlib.sha256(installed).hexdigest()==result['kernel_sha256']
   with aux.open('xb') as f:
    f.truncate(256*1024*1024);f.write(bytes([0x7d])*65536);f.seek(1048576);f.write(b'q187-protected')
   variables=case/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
   extra=['-drive',f'file={aux},format=raw,if=none,id=aux','-device','nvme,drive=aux,serial=q187aux']
   g=B['InstalledGuest'](case/'boot',disk,variables,extra_args=extra,auxiliary_first=first)
   g.login();g.check_root()
   g.run('diskpart --machine list',r'nvme0n1')
   assert 'nvme1n1' in g.text()
   assert 'additional controller rejected' not in g.text()
   auxiliary='nvme0n1' if first else 'nvme1n1'
   g.run('echo q187-installed-root > /root/q187-persist')
   g.run('sync /root/q187-persist')
   g.run(f'/bin/dd if=/dev/zero of=/dev/{auxiliary} bs=512 count=128')
   g.run(f'/bin/dd if=/dev/{auxiliary} of=/tmp/q187-readback bs=512 count=128')
   g.run('/bin/cksum -a sha256 -- /tmp/q187-readback',hashlib.sha256(bytes(65536)).hexdigest())
   g.run('cat /root/q187-persist','q187-installed-root')
   g.halt_checked();g.stop();g=None
   with aux.open('rb') as f:
    assert f.read(65536)==bytes(65536);f.seek(1048576);assert f.read(14)==b'q187-protected'
   result['cases'].append({'auxiliary_first':first,'status':'PASS boot/login, both namespaces, separate writes/readback/flush, halt'})
   print(result['cases'][-1],flush=True)
  result['status']='PASS'
 finally:
  if g is not None:g.stop()
  (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
