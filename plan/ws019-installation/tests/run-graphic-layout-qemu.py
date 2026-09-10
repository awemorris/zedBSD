#!/usr/bin/env python3
"""Presentation-only multi-option/long-label/empty-list corpus in actual BeUI."""
import json
from pathlib import Path
import runpy
import shlex
import sys
import time

HERE=Path(__file__).resolve().parent
N=runpy.run_path(str(HERE/'run-native-installer-qemu.py'))
G=runpy.run_path(str(HERE/'graphic-framebuffer.py'))
I,F,REPO,key=N['I'],N['F'],N['REPO'],N['key'];wait_title=G['wait_title']

def main():
    out=Path(sys.argv[1]).resolve();out.relative_to(REPO/'plan/ws019-installation/temp')
    out.mkdir(parents=True,exist_ok=False)
    result=F['prepare_boot'](out,False,False,REPO/'build/arch-images/amd64.ufs')
    disk=out/'gpt.img'
    with disk.open('xb') as stream: stream.truncate(1024**3)
    before=F['digest'](disk)
    result.update(result='FAIL graphic layout fixture',screenshots=[])
    guest=I['PublicInstallGuest'](out,usb_boot=True)
    guest.deadline=time.monotonic()+1800
    try:
        guest.login()
        lines=(HERE/'graphic-layout-fixture.noct').read_text().splitlines()
        guest.run('printf '+shlex.quote('%s\n')+' '+ ' '.join(shlex.quote(line) for line in lines)+' > /run/graphic-layout.noct')
        start=len(guest.text())
        guest.send('/bin/noct --path=/lib/zedinst /run/graphic-layout.noct; echo storage-result-$?')
        result['screenshots'].append(wait_title(guest,'Fixture: many long labels','fixture-many'))
        for _ in range(7): key(guest,'down')
        time.sleep(1)
        result['screenshots'].append(guest.capture_screen('fixture-last-page'))
        key(guest,'ret')
        result['screenshots'].append(wait_title(guest,'Fixture: no destinations','fixture-empty'))
        key(guest,'ret');time.sleep(1)
        image=guest.capture_screen('fixture-empty-after-enter')
        assert G['text_at'](Path(image).with_suffix('.ppm'))=='Fixture: no destinations'
        key(guest,'esc')
        assert 'graphic layout fixture PASS' in guest.command_status(start)
        assert F['digest'](disk)==before,'presentation fixture modified storage'
        result['result']='PASS graphic layout fixture (not a multiple-controller acceptance)'
    except BaseException as error:
        result['failure']=str(error);raise
    finally:
        guest.stop()
        result['production_after']=F['digest'](REPO/'build/amd64/hdd-image.img')
        if result['production_after']!=result['production_sha256']: result['result']='FAIL production image changed'
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['result'].startswith('PASS'),result
    print(result['result'],flush=True)

if __name__=='__main__':main()
