#!/usr/bin/env python3
"""Boot a clone of the accepted graphical coexistence installation without USB."""
import json
from pathlib import Path
import runpy
import shutil
import sys

HERE=Path(__file__).resolve().parent
B=runpy.run_path(str(HERE/'run-installed-boot-qemu.py'))
F,REPO=B['F'],B['REPO']

def main():
    accepted=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
    for path in [accepted,out]:path.relative_to(REPO/'plan/ws019/temp')
    prior=json.loads((accepted/'result.json').read_text())
    assert prior['result']=='PASS graphic coexistence'
    source=accepted/'gpt.img';before=F['digest'](source)
    out.mkdir(parents=True,exist_ok=False)
    F['copy_image'](source,out/'installed.img')
    shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',out/'vars.fd')
    result={'result':'FAIL graphic installed boot','accepted_run':str(accepted),'source_before':before,'cases':[]}
    guest=None
    try:
        for attempt in range(2):
            guest=B['InstalledGuest'](out/f'boot{attempt}',out/'installed.img',out/'vars.fd')
            guest.login();guest.check_root()
            if attempt==0:
                guest.run('echo graphical-coexistence-persistence > /root/graphic-persistence')
                guest.run('sync /root/graphic-persistence')
            guest.run('cat /root/graphic-persistence','graphical-coexistence-persistence')
            guest.halt_checked();guest.stop();guest=None
            result['cases'].append(f'boot {attempt}: overlay root, active file swap, persistence, halt')
        result['result']='PASS graphic installed boot'
    except BaseException as error:
        result['failure']=str(error);raise
    finally:
        if guest is not None:guest.stop()
        result['source_after']=F['digest'](source)
        if result['source_after']!=before:result['result']='FAIL accepted reference changed'
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['result']=='PASS graphic installed boot',result
    print(result['result'],flush=True)

if __name__=='__main__':main()
