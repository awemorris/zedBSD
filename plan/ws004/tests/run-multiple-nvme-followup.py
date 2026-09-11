#!/usr/bin/env python3
"""Check retained writes and isolated namespace-probe failure on cloned images."""
import json,runpy,shutil,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[3]
B=runpy.run_path(str(R/'plan/ws019/tests/run-installed-boot-qemu.py'))
prior=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
for p in [prior,out]:p.relative_to(R/'plan/ws004/temp')
assert json.loads((prior/'result.json').read_text())['status']=='PASS'
out.mkdir(parents=True);result={'status':'FAIL','cases':[]};g=None
try:
 for name,first,empty in [('aux-first',True,False),('installed-first',False,False),('empty-first',True,True)]:
  case=out/name;case.mkdir();old=prior/('aux-first' if empty else name)
  disk=case/'installed.img';subprocess.run(['cp','--reflink=auto',str(old/'installed.img'),str(disk)],check=True)
  if empty:
   extra=['-device','nvme,serial=q187empty']
  else:
   aux=case/'aux.img';subprocess.run(['cp','--reflink=auto',str(old/'aux.img'),str(aux)],check=True)
   extra=['-drive',f'file={aux},format=raw,if=none,id=aux','-device','nvme,drive=aux,serial=q187aux']
  variables=case/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
  g=B['InstalledGuest'](case/'boot',disk,variables,extra_args=extra,auxiliary_first=first)
  g.login();g.check_root();g.run('cat /root/q187-persist','q187-installed-root')
  if empty:
   assert 'namespace probe failed' in g.text() and 'quarantined' in g.text()
   g.run('echo q187-after-probe-failure > /root/q187-after-failure')
   g.run('sync /root/q187-after-failure')
   g.run('cat /root/q187-after-failure','q187-after-probe-failure')
  else:
   auxname='nvme0n1' if first else 'nvme1n1'
   g.run(f'/bin/dd if=/dev/{auxname} of=/tmp/retained bs=512 count=128')
   g.run('/bin/cksum /tmp/retained',r'4215202376 65536')
  g.halt_checked();g.stop();g=None
  result['cases'].append(name+' PASS persisted root/aux data or isolated probe failure; halt')
  print(result['cases'][-1],flush=True)
 result['status']='PASS'
finally:
 if g is not None:g.stop()
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
