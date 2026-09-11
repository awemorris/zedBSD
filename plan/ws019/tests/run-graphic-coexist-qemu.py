#!/usr/bin/env python3
"""Accept the public graphical coexistence path without changing the layout."""
import json
from pathlib import Path
import runpy
import sys
import time

HERE=Path(__file__).resolve().parent
N=runpy.run_path(str(HERE/'run-native-installer-qemu.py'))
G=runpy.run_path(str(HERE/'graphic-framebuffer.py'))
I,F,REPO,key=N['I'],N['F'],N['REPO'],N['key']
BASE=I['BASE'];wait_title=G['wait_title']

def enter(guest,label):
    start=len(guest.text())
    guest.send('/sbin/zedinst-graphic; echo storage-result-$?')
    wait_title(guest,'Installation source',label+'-source');key(guest,'ret')
    wait_title(guest,'Installation mode',label+'-mode');key(guest,'ret')
    wait_title(guest,'Choose your disk',label+'-disk');key(guest,'ret')
    wait_title(guest,'Choose existing FAT payload',label+'-payload')
    key(guest,'down');key(guest,'ret')
    return start

def main():
    out=Path(sys.argv[1]).resolve()
    out.relative_to(REPO/'plan/ws019/temp')
    out.mkdir(parents=True,exist_ok=False)
    result=F['prepare_boot'](out,False,False,REPO/'build/arch-images/amd64.ufs')
    disk=out/'gpt.img'
    F['create_nvme'](disk,sectors=10485760)
    BASE['add_discovery_payload'](disk)
    protected=F['protected_bytes'](disk)
    payload=BASE['payload_marker'](disk)
    result.update(result='FAIL graphic coexistence',cases=[],screenshots=[])
    guest=I['PublicInstallGuest'](out,usb_boot=True)
    guest.deadline=time.monotonic()+7200
    try:
        guest.login()
        before=F['digest'](disk)
        for case in ['cancel','install','rerun']:
            start=enter(guest,case)
            result['screenshots'].append(wait_title(guest,'Review coexistence installation',case+'-review'))
            if case!='cancel': key(guest,'down')
            key(guest,'ret')
            expected='Installation cancelled' if case=='cancel' else 'Installation complete'
            result['screenshots'].append(wait_title(guest,expected,case+'-result',3600))
            key(guest,'ret')
            assert expected+'.' in guest.command_status(start)
            if case=='cancel': assert F['digest'](disk)==before,'default NO modified destination'
            result['cases'].append(case)
        # A conflicting managed file must be rejected and preserved by both frontends.
        guest.run('mkdir /run/graphic-check')
        guest.run('mount -t fat nvme0n1p2 /run/graphic-check')
        guest.run('/bin/cp -T --update=none-fail /run/graphic-check/vmunix /run/kernel-backup')
        guest.run('dd if=/dev/zero of=/run/graphic-check/vmunix bs=1 count=1 conv=notrunc')
        guest.run('/bin/cp -T --update=none-fail /run/graphic-check/vmunix /run/corrupt-copy')
        guest.run('umount /run/graphic-check')
        start=enter(guest,'conflict')
        result['screenshots'].append(wait_title(guest,'Installation incomplete','conflict-result'))
        key(guest,'ret')
        guest.command_status(start,status=1)
        guest.run('mount -t fat nvme0n1p2 /run/graphic-check')
        guest.run('cmp /run/corrupt-copy /run/graphic-check/vmunix')
        guest.run('/bin/cp -T /run/kernel-backup /run/graphic-check/vmunix')
        guest.run('cmp /run/kernel-backup /run/graphic-check/vmunix')
        guest.run('sync /run/graphic-check/vmunix')
        guest.run('umount /run/graphic-check')
        guest.run('rmdir /run/graphic-check')
        guest.run('rm /run/kernel-backup /run/corrupt-copy')
        result['cases'].append('single-byte conflict refused and preserved; original restored')
        result['result']='PASS graphic coexistence'
    except BaseException as error:
        result['failure']=str(error)
        raise
    finally:
        guest.stop()
        result['protected_before']=protected;result['protected_after']=F['protected_bytes'](disk)
        result['payload_before']=payload;result['payload_after']=BASE['payload_marker'](disk)
        result['production_after']=F['digest'](REPO/'build/amd64/hdd-image.img')
        if result['protected_after']!=protected or result['payload_after']!=payload:
            result['result']='FAIL unrelated destination data changed'
        if result['production_after']!=result['production_sha256']: result['result']='FAIL production image changed'
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['result']=='PASS graphic coexistence',result
    print(result['result'],flush=True)

if __name__=='__main__': main()
