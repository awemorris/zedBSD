from pathlib import Path
import subprocess,sys,os,hashlib,json
repo=Path(__file__).resolve().parents[3];out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
s=(repo/'src/hal/amd64/space.c').read_text()
names={'valid_space_range':'static int','leaf_flags':'static uint64_t','system_query':'static int','hal_space_map':'int','hal_space_prot_query':'int','hal_space_prot':'int','hal_space_unmap':'int','hal_space_query':'int','hal_space_clear_flags':'int','hal_space_get_kernel_range':'void'}
parts=[]
for name,kind in names.items():
 a=s.index('\n'+name+'(')+1;b=s.index('{',a);i=b+1;depth=1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 parts.append(kind+'\n'+s[a:i])
(out/'system-space-extracted.h').write_text('\n'.join(parts)+'\n')
paths=['src/hal/amd64/space.c','include/hal/hal.h','plan/ws025-io-memory-cache/tests/system-space-host.c']
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
for mode in ['ordinary','sanitize']:
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'] if mode=='sanitize' else []
 with (out/(mode+'.log')).open('w') as log:
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Iinclude','-Iinclude/uapi','-I.','-I'+str(out),*flags,paths[2],'-o',str(out/mode)],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
  subprocess.run([str(out/mode)],stdout=log,stderr=subprocess.STDOUT,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
 print(mode+': PASS')
