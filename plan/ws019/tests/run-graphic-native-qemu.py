#!/usr/bin/env python3
"""Accept the public graphical dedicated installer, including default NO and boot."""
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
N=runpy.run_path(str(HERE/'run-native-installer-qemu.py'))
G=runpy.run_path(str(HERE/'graphic-framebuffer.py'))
I,B,F,REPO,key=N['I'],N['B'],N['F'],N['REPO'],N['key']
wait_title=G['wait_title']

def review(guest,result,label):
    start=len(guest.text())
    guest.send('/sbin/zedinst-graphic; echo storage-result-$?')
    wait_title(guest,'Installation source',label+'-source')
    key(guest,'ret')
    wait_title(guest,'Installation mode',label+'-mode')
    key(guest,'down');key(guest,'ret')
    result['screenshots'].append(wait_title(guest,'Choose your disk',label+'-disk'))
    key(guest,'ret')
    result['screenshots'].append(wait_title(guest,'Review dedicated installation',label+'-review'))
    return start

def main():
    out=Path(sys.argv[1]).resolve()
    out.relative_to(REPO/'plan/ws019/temp')
    out.mkdir(parents=True,exist_ok=False)
    result=F['prepare_boot'](out,False,False,REPO/'build/arch-images/amd64.ufs')
    disk=out/'gpt.img'
    with disk.open('xb') as stream: stream.truncate(1024**3)
    before=F['digest'](disk)
    result.update(result='FAIL graphic native installer',screenshots=[],cases=[])
    guest=None
    try:
        guest=I['PublicInstallGuest'](out,usb_boot=True)
        guest.deadline=time.monotonic()+7200
        guest.login()
        guest.run('/sbin/zedinst-graphic < /dev/null','interactive terminal',status=1)
        start=review(guest,result,'cancel')
        assert F['digest'](disk)==before,'review modified destination'
        key(guest,'ret')
        wait_title(guest,'Installation cancelled','cancel-result')
        key(guest,'ret')
        assert 'Installation cancelled.' in guest.command_status(start)
        assert F['digest'](disk)==before,'default NO modified destination'
        result['cases'].append('default NO preserves the complete blank disk')
        start=review(guest,result,'install')
        key(guest,'down');key(guest,'ret')
        result['screenshots'].append(wait_title(guest,'Installing zedBSD','install-start'))
        result['screenshots'].append(wait_title(guest,'Installation complete','install-complete',3600))
        key(guest,'ret')
        assert 'Installation complete.' in guest.command_status(start)
        guest.run('mount');guest.run('ls /run')
        result['cases'].append('graphical dedicated installation completes through shared backend')
        guest.stop();guest=None
        variables=out/'native-vars.fd'
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        for attempt in range(2):
            guest=B['InstalledGuest'](out/f'native{attempt}',disk,variables)
            guest.login()
            guest.run('mount',r' on / type ufs ')
            guest.run('truncate -s 0 /swapfile','Device or resource busy',status=1)
            if attempt==0:
                guest.run('echo graphic-installer-persistence > /etc/installer-check')
                guest.run('sync /etc/installer-check')
            else: guest.run('cat /etc/installer-check','graphic-installer-persistence')
            guest.halt_checked();guest.stop();guest=None
            result['cases'].append(f'source-free native boot {attempt}, active swap, persistence and halt')
        result['result']='PASS graphic native installer'
    except BaseException as error:
        result['failure']=str(error)
        raise
    finally:
        if guest is not None: guest.stop()
        result['production_after']=F['digest'](REPO/'build/amd64/hdd-image.img')
        source_start=F['boot_payload'](out/'boot.img')
        source=subprocess.check_output(['mtype','-i',f'{out / "boot.img"}@@{source_start*512}','::/rootfs.img'])
        result['source_after']=hashlib.sha256(source).hexdigest()
        if result['source_after']!=result['rootfs_sha256']: result['result']='FAIL installation source changed'
        if result['production_after']!=result['production_sha256']: result['result']='FAIL production image changed'
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['result']=='PASS graphic native installer',result
    print(result['result'],flush=True)

if __name__=='__main__': main()
