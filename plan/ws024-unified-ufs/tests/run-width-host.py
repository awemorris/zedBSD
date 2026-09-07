#!/usr/bin/env python3
from pathlib import Path
import subprocess,sys,json
repo=Path(__file__).resolve().parents[3];out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws024-unified-ufs/temp');out.mkdir(parents=True,exist_ok=False)
commands=[]
for arch,abi in [('amd64',['-m64','-DZEDBSD_USER_ABI_LP64']),('i386',['-m32'])]:
 binary=str(out/arch)
 args=['cc',*abi,'-std=c11','-O1','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-nostdlib','-static','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include','plan/ws024-unified-ufs/tests/ufs-width-host.c','src/drivers/fs/ufs/ufs-endian.c','-Wl,--gc-sections,-e,_start','-o',binary]
 for name,cmd in [(arch+'-build',args),(arch,['timeout','30s',binary])]:
  commands.append(dict(name=name,argv=cmd));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  r=subprocess.run(cmd,cwd=repo,capture_output=True,text=True);(out/(name+'.log')).write_text(r.stdout+r.stderr);print(name,r.returncode,r.stderr[-1400:],flush=True)
  if r.returncode:sys.exit(1)
print('LP64 and ILP32 high-address/size-boundary production gates PASS')
