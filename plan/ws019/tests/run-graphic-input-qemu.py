#!/usr/bin/env python3
"""Public BeUI held-key/pointer navigation and missing-asset recovery."""
import json
from pathlib import Path
import runpy
import sys
import time

HERE=Path(__file__).resolve().parent
N=runpy.run_path(str(HERE/'run-native-installer-qemu.py'))
G=runpy.run_path(str(HERE/'graphic-framebuffer.py'))
I,F,REPO,key=N['I'],N['F'],N['REPO'],N['key'];wait_title=G['wait_title']

def monitor(guest,command):
    guest.proc.stdin.write(command+'\n');guest.proc.stdin.flush();time.sleep(.15)

def click(guest,x,y):
    # Small relative packets work with the PS/2 pointer, without assuming its origin.
    for _ in range(8): monitor(guest,'mouse_move -100 -100')
    while x>0 or y>0:
        dx,dy=min(x,100),min(y,100)
        monitor(guest,f'mouse_move {dx} {dy}');x-=dx;y-=dy
    monitor(guest,'mouse_button 1');monitor(guest,'mouse_button 0')

def main():
    out=Path(sys.argv[1]).resolve();out.relative_to(REPO/'plan/ws019/temp')
    out.mkdir(parents=True,exist_ok=False)
    result=F['prepare_boot'](out,False,False,REPO/'build/arch-images/amd64.ufs')
    disk=out/'gpt.img'
    with disk.open('xb') as stream: stream.truncate(1024**3)
    before=F['digest'](disk)
    result.update(result='FAIL graphical input',screenshots=[],cases=[])
    guest=I['PublicInstallGuest'](out,usb_boot=True)
    guest.deadline=time.monotonic()+1800
    try:
        guest.login()
        start=len(guest.text());guest.send('/sbin/zedinst-graphic; echo storage-result-$?')
        wait_title(guest,'Installation source','held-source')
        monitor(guest,'sendkey ret 2000')
        wait_title(guest,'Installation mode','held-mode')
        time.sleep(3)
        screen=guest.capture_screen('held-still-mode')
        assert G['text_at'](Path(screen).with_suffix('.ppm'))=='Installation mode','held Enter crossed two screens'
        result['screenshots'].append(screen)
        key(guest,'esc');wait_title(guest,'Installation cancelled','held-cancel');key(guest,'ret')
        assert 'Installation cancelled.' in guest.command_status(start)
        result['cases'].append('held Enter cannot cross the next screen')
        start=len(guest.text());guest.send('/sbin/zedinst-graphic; echo storage-result-$?')
        wait_title(guest,'Installation source','pointer-source')
        click(guest,505,406)
        wait_title(guest,'Installation mode','pointer-mode')
        click(guest,230,269) # Dedicated disk row.
        click(guest,505,406)
        result['screenshots'].append(wait_title(guest,'Choose your disk','pointer-disk'))
        click(guest,110,406)
        wait_title(guest,'Installation cancelled','pointer-cancel')
        click(guest,505,406)
        assert 'Installation cancelled.' in guest.command_status(start)
        result['cases'].append('pointer source, mode, continue and cancellation')
        guest.run('mv /lib/zedinst/assets/menu.bmp /lib/zedinst/assets/menu.saved')
        guest.run('/sbin/zedinst-graphic',status=1)
        guest.run('mv /lib/zedinst/assets/menu.saved /lib/zedinst/assets/menu.bmp')
        guest.run('echo restored-after-missing-asset','restored-after-missing-asset')
        result['cases'].append('missing asset refuses and restores console')
        assert F['digest'](disk)==before,'input/refusal changed destination'
        result['result']='PASS graphical input'
    except BaseException as error:
        result['failure']=str(error);raise
    finally:
        guest.stop()
        result['production_after']=F['digest'](REPO/'build/amd64/hdd-image.img')
        if result['production_after']!=result['production_sha256']: result['result']='FAIL production image changed'
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['result']=='PASS graphical input',result
    print(result['result'],flush=True)

if __name__=='__main__':main()
