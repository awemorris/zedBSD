#!/usr/bin/env python3
"""Capture the public BeUI wizard and verify non-destructive keyboard navigation."""
import json
from pathlib import Path
import runpy
import sys
import time

HERE=Path(__file__).resolve().parent
N=runpy.run_path(str(HERE/'run-native-installer-qemu.py'))
I,F,REPO,key=N['I'],N['F'],N['REPO'],N['key']

def main():
    out=Path(sys.argv[1]).resolve()
    out.relative_to(REPO/'plan/ws019-installation/temp')
    out.mkdir(parents=True,exist_ok=False)
    result=F['prepare_boot'](out,False,False,REPO/'build/arch-images/amd64.ufs')
    disk=out/'gpt.img'
    with disk.open('xb') as stream: stream.truncate(1024**3)
    before=F['digest'](disk)
    result.update(result='FAIL graphic navigation',screenshots=[])
    guest=I['PublicInstallGuest'](out,usb_boot=True)
    try:
        guest.login()
        guest.run('/sbin/zedinst-graphic < /dev/null','interactive terminal',status=1)
        start=len(guest.text())
        guest.send('/sbin/zedinst-graphic; echo storage-result-$?')
        for name,keys in [('source',[]),('mode',['ret']),('disk',['down','ret'])]:
            for stroke in keys: key(guest,stroke)
            time.sleep(15)
            screen=guest.capture_screen('graphic-'+name)
            result['screenshots'].append(screen)
            print(screen,flush=True)
            assert b'640 480\n' in Path(screen).with_suffix('.ppm').read_bytes()[:40], 'wrong physical mode'
        key(guest,'esc')
        time.sleep(3)
        result['screenshots'].append(guest.capture_screen('graphic-cancelled'))
        key(guest,'ret')
        text=guest.command_status(start)
        assert 'Installation cancelled.' in text,text
        guest.run('echo graphic-console-restored','graphic-console-restored')
        assert F['digest'](disk)==before,'navigation modified target'
        result['result']='PASS graphic navigation'
    except BaseException as error:
        result['failure']=str(error)
        raise
    finally:
        guest.stop()
        result['production_after']=F['digest'](REPO/'build/amd64/hdd-image.img')
        if result['production_after']!=result['production_sha256']: result['result']='FAIL production image changed'
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(result['result'],flush=True)

if __name__=='__main__': main()
