#!/usr/bin/env python3
"""Normal PC98 FAT install on two disposable IDE disks, then source-free boot."""
import hashlib, importlib.util, json, re, struct, subprocess, sys, time, zlib
from pathlib import Path
R=Path(__file__).resolve().parents[3]
s=importlib.util.spec_from_file_location('pc98',R/'plan/ws025-io-memory-cache/tests/run-p032-pc98-session.py')
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
font=(R/'build/qemu-pc98/pc-bios/pc98font.bin').read_bytes()[0x800:0x1800]
glyphs={font[c*16:(c+1)*16]:chr(c) for c in range(32,127)}
def png(p):
    _,d,_,b=p.read_bytes().split(b'\n',3);w,h=map(int,d.split())
    def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b))
    p.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(b'\0'+b[i*w*3:(i+1)*w*3] for i in range(h))))+chunk(b'IEND',b''))
def title(p,x=60,y=118,color=0xffffff):
    _,d,_,b=p.read_bytes().split(b'\n',3);w,h=map(int,d.split())
    if (w,h)!=(640,480):return 'not graphical'
    line=''
    for col in range(65):
        data=[]
        for row in range(16):
            bits=0
            for bit in range(8):
                at=((y+row)*w+x+col*8+bit)*3
                if b[at:at+3]==color.to_bytes(3,'big'):bits|=128>>bit
            data.append(bits)
        line+=glyphs.get(bytes(data),'?')
    return line.rstrip()
def capture(g,label):
    p=g.directory/(label+'.ppm');p.unlink(missing_ok=True)
    g.socket.sendall(f'screendump "{p}"\n'.encode());g.drain()
    return p
def wait(g,expected,label,seconds=180):
    end=time.monotonic()+seconds;previous=None;stable=None;settled=0
    while time.monotonic()<end:
        p=capture(g,'current');text=title(p)
        if text!=previous:print(label+': '+text,flush=True);previous=text
        if text==expected:
            frame=p.read_bytes()
            settled=settled+1 if frame==stable else 0
            stable=frame
            if settled<2:
                time.sleep(.5);continue
            target=g.directory/(label+'.ppm');target.write_bytes(p.read_bytes());png(target);return
        if text in ('Installation incomplete','Installation could not continue'):
            png(p);key(g,'ret');time.sleep(3);print(g.screen(),flush=True);raise RuntimeError('installer error: '+str(p.with_suffix('.png')))
        if g.proc.poll() is not None:raise RuntimeError('QEMU exited')
        time.sleep(2)
    png(p);raise RuntimeError('timeout '+expected+'\n'+g.screen())
def key(g,key):
    time.sleep(.5)
    g.socket.sendall(f'sendkey {key} 250\n'.encode());g.drain();time.sleep(.5)
def main():
    out=Path(sys.argv[1]).resolve();out.relative_to(R/'plan/ws019-installation/temp');out.mkdir()
    source=out/'source.img';target=out/'target.img'
    for disk in (source,target):subprocess.run(['cp','--reflink=auto','--sparse=always',str(R/'build/pc98/hdd-image.img'),str(disk)],check=True)
    # Fixture: preformatted PC98 FAT with its bootstrap, no installed OS payload.
    names=['BOOTZBSD.EXE','VMUNIX','ROOTFS.IMG','DATA.IMG','SWAPFILE','BOOTZBSD.CFG']
    # Clones initially share the source FAT UUID; assign a distinct destination.
    with target.open('r+b') as stream:
        stream.seek(1048576+39);stream.write(struct.pack('<I',0x18501998))
    spec=str(target)+'@@1048576'
    subprocess.run(['mattrib','-i',spec,'-r','-h','-s','::*'],check=True)
    for name in names:subprocess.run(['mdel','-i',spec,'::'+name],check=True)
    marker=out/'KEEP.TXT';marker.write_text('existing FAT file\n')
    subprocess.run(['mcopy','-i',spec,str(marker),'::KEEP.TXT'],check=True)
    result={'status':'FAIL','topology':'PC98 486 64 MiB; two IDE HDDs; preformatted FAT16 with existing bootstrap'}
    g=None
    try:
        g=m.Guest(out/'install',source,[target],sys.argv[2] if len(sys.argv)>2 else '64M');g.deadline=time.monotonic()+3600;g.login()
        g.command('uname -a',r'pc98 i386')
        g.send('/sbin/zedinst-graphic')
        for expected,label in [('Installation source','source'),('Installation mode','mode'),('Choose your disk','disk'),('Choose existing FAT partition','partition'),('Review PC98 FAT installation','review')]:
            wait(g,expected,label);key(g,'ret') if label!='review' else None
        key(g,'down');key(g,'ret')
        wait(g,'Installation complete','complete',1800)
        key(g,'ret');g.wait(r'root@[^\n]*\$ *$',60);g.halt();g.close();g=None
        subprocess.run(['mcopy','-i',spec,'::KEEP.TXT',str(out/'kept.txt')],check=True)
        assert (out/'kept.txt').read_bytes()==marker.read_bytes()
        for name in ['VMUNIX','ROOTFS.IMG']:
            a=out/('source-'+name);b=out/('installed-'+name)
            subprocess.run(['mcopy','-i',str(source)+'@@1048576','::'+name,str(a)],check=True)
            subprocess.run(['mcopy','-i',spec,'::'+name,str(b)],check=True)
            assert hashlib.sha256(a.read_bytes()).digest()==hashlib.sha256(b.read_bytes()).digest()
        g=m.Guest(out/'installed-boot',target);g.login();g.command('uname -a',r'pc98 i386');g.command('ls /sbin/zedinst-graphic',r'/sbin/zedinst-graphic');g.halt()
        result['status']='PASS';print('PASS PC98 graphical FAT installation and source-free boot',flush=True)
    finally:
        if g:g.close()
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
