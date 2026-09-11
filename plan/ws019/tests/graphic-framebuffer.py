"""Read actual BeUI text pixels using the public kernel's fixed UEFI VGA font."""
from pathlib import Path
import re
import shutil
import time

REPO=Path(__file__).resolve().parents[3]
FONT_SOURCE=REPO/'src/drivers/platform/pcat/graphics/vgafont.c'
FONT=bytes(int(token,16) for token in re.findall(r'0x([0-9a-fA-F]{2})',FONT_SOURCE.read_text()))
assert len(FONT)==4096
GLYPHS={FONT[code*16:(code+1)*16]:chr(code) for code in range(32,127)}

def text_at(ppm,x=60,y=118,count=65,color=0xffffff):
    magic,dimensions,maximum,pixels=Path(ppm).read_bytes().split(b'\n',3)
    width,height=map(int,dimensions.split())
    assert (magic,width,height,maximum)==(b'P6',640,480,b'255')
    assert len(pixels)==width*height*3
    foreground=color.to_bytes(3,'big')
    text=''
    for column in range(count):
        glyph=[]
        for row in range(16):
            bits=0
            for bit in range(8):
                offset=((y+row)*width+x+column*8+bit)*3
                if pixels[offset:offset+3]==foreground: bits|=0x80>>bit
            glyph.append(bits)
        text+=GLYPHS.get(bytes(glyph),'?')
    return text.rstrip()

def wait_title(guest,expected,name,timeout=1200):
    deadline=time.monotonic()+timeout
    latest=''
    while time.monotonic()<deadline:
        for suffix in ['.png','.ppm']:
            (guest.output/('frame-poll'+suffix)).unlink(missing_ok=True)
        image=Path(guest.capture_screen('frame-poll'))
        latest=text_at(image.with_suffix('.ppm'))
        if latest=='Installing zedBSD':
            label=text_at(image.with_suffix('.ppm'),y=231)
            if label.startswith('Files copied;') and not (guest.output/'install-file-progress.png').exists():
                for suffix in ['.png','.ppm']:
                    shutil.copyfile(image.with_suffix(suffix),guest.output/('install-file-progress'+suffix))
                print('file progress: '+text_at(image.with_suffix('.ppm'),y=271),flush=True)
        if latest==expected:
            for suffix in ['.png','.ppm']:
                shutil.copyfile(image.with_suffix(suffix),guest.output/(name+suffix))
            print(f'{name}: {latest}',flush=True)
            time.sleep(.5) # Wait for the new screen to observe released input.
            return str(guest.output/(name+'.png'))
        if latest in ['Installation incomplete','Installation could not continue']:
            raise AssertionError('graphical refusal: '+text_at(image.with_suffix('.ppm'),y=151,color=14873573))
        if guest.proc.poll() is not None: raise RuntimeError('QEMU stopped')
        time.sleep(2)
    raise TimeoutError(f'waiting for {expected!r}; framebuffer title {latest!r}')
