#!/usr/bin/env python3
import json,runpy,shutil,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[3]
B=runpy.run_path(str(R/'plan/ws019-installation/tests/run-installed-boot-qemu.py'))
prior=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
for p in [prior,out]:p.relative_to(R/'plan/ws004-hardware/temp')
assert json.loads((prior/'result.json').read_text())['status']=='PASS'
out.mkdir(parents=True);sysroot=R/'build/amd64/sysroot/usr';obj=out/'probe.o';binary=out/'probe'
subprocess.run([str(R/'build/llvm/bin/clang'),'--target=x86_64-unknown-zedbsd','-nostdinc','-isystem',str(sysroot/'include'),'-I'+str(R/'include/uapi'),'-DZEDBSD_USER_ABI_LP64=1','-ffreestanding','-fno-pie','-O1','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-c',str(R/'plan/ws004-hardware/tests/multiple-nvme-io.c'),'-o',str(obj)],check=True)
subprocess.run(['ld','-m','elf_x86_64','--gc-sections','-nostdlib','-static','-z','max-page-size=4096','-z','stack-size=0x100000','-T',str(R/'platform/amd64/user.ld'),str(sysroot/'lib/crt0.o'),str(sysroot/'lib/libc.o'),str(obj),'-o',str(binary)],check=True)
for name in ['installed.img','aux.img']:subprocess.run(['cp','--reflink=auto',str(prior/'aux-first'/name),str(out/name)],check=True)
subprocess.run(['mcopy','-i',str(out/'installed.img')+'@@1048576',str(binary),'::/q187-probe'],check=True)
variables=out/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
extra=['-drive',f'file={out / "aux.img"},format=raw,if=none,id=aux','-device','nvme,drive=aux,serial=q187aux']
g=B['InstalledGuest'](out/'boot',out/'installed.img',variables,extra_args=extra)
result={'status':'FAIL'}
try:
 g.login();g.check_root();g.run('mkdir /run/probe-esp')
 g.run('mount -t fat -r nvme1n1p1 /run/probe-esp')
 g.run('/bin/cp /run/probe-esp/q187-probe /run/q187-probe')
 g.run('chmod 700 /run/q187-probe');g.run('umount /run/probe-esp')
 g.run('/run/q187-probe /dev/nvme0n1 /root/q187-concurrent','MULTIPLE NVME concurrent write/flush/readback PASS')
 g.halt_checked();result['status']='PASS'
finally:
 g.stop();(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
