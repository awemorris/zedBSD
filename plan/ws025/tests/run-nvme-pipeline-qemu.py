#!/usr/bin/env python3
"""Depth 1/2/4/8, private builds and raw NVMe read/write/flush/restart gates."""
from pathlib import Path
import hashlib, importlib.util, json, re, shutil, socket, struct, subprocess, sys, time
REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('storage', REPO/'plan/ws019/tests/run-storage-qemu.py')
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)

def snapshot(guest, output, kernel, layout, label):
    symbols = subprocess.check_output(['nm', '-n', str(kernel)], text=True)
    address = int(re.search(r'^([0-9a-f]+) [bBdD] nvme_primary$', symbols, re.M)[1], 16)
    sock = socket.socket(socket.AF_UNIX); sock.settimeout(10)
    sock.connect(str(output/'observe.sock'))
    stream = sock.makefile('rwb', buffering=0)
    json.loads(stream.readline())
    def qmp(name, args=None):
        obj = {'execute': name}
        if args is not None: obj['arguments'] = args
        stream.write((json.dumps(obj)+'\n').encode())
        while True:
            reply = json.loads(stream.readline())
            if 'error' in reply: raise RuntimeError(reply)
            if 'return' in reply: return reply['return']
    qmp('qmp_capabilities'); qmp('stop')
    def save(addr, size, path):
        reply = qmp('human-monitor-command', {'command-line': f'memsave 0x{addr:x} {size} "{path}"'})
        if not path.exists() or path.stat().st_size != size: raise RuntimeError((reply, str(path)))
        return path.read_bytes()
    try:
        pointer = struct.unpack('<Q', save(address, 8, output/(label+'-pointer.bin')))[0]
        assert pointer != 0
        raw = save(pointer, layout['size'], output/(label+'-controller.bin'))
        result = {'posted': struct.unpack_from('<Q', raw, layout['posted'])[0], 'high_water': struct.unpack_from('<I', raw, layout['high_water'])[0]}
        (output/(label+'-counters.json')).write_text(json.dumps(result, indent=2)+'\n')
        return result
    finally:
        qmp('cont'); stream.close(); sock.close()

def main():
    out = Path(sys.argv[1]).resolve(); out.relative_to(REPO/'plan/ws025/temp')
    out.mkdir(parents=True, exist_ok=False)
    layout = json.loads(Path(sys.argv[2]).read_text())
    depths = tuple(int(x) for x in sys.argv[3].split(',')) if len(sys.argv) > 3 else (1,2,4,8)
    assert depths and len(set(depths)) == len(depths) and all(x in (1,2,4,8) for x in depths)
    source = REPO/'build/amd64/hdd-image.img'; original = base.digest(source)
    private = out/'build'; arches = out/'arch-images'
    with source.open('rb') as f:
        f.seek(512); hdr=f.read(512); assert hdr[:8] == b'EFI PART'
        lba, count, width = struct.unpack_from('<QII', hdr, 72)
        assert 0 < count <= 4096 and 128 <= width <= 4096
        f.seek(lba*512); entries=f.read(count*width)
    candidates=[]
    for i in range(count):
        entry=entries[i*width:(i+1)*width]
        if entry[:16] == bytes(16): continue
        first,last=struct.unpack_from('<QQ',entry,32)
        assert 34 <= first <= last < source.stat().st_size//512
        if subprocess.run(['mdir','-i',f'{source}@@{first*512}','::/rootfs.img'],capture_output=True).returncode == 0: candidates.append(first)
    assert len(candidates)==1
    metadata = {'source_sha256': original, 'kernel_source_sha256': base.digest(REPO/'src/drivers/pci/pci-nvme.c'), 'depths': {}}
    try:
        for depth in depths:
            cell=out/f'd{depth}'; cell.mkdir()
            command=['make','-j16','-f','Makefile','-f','plan/ws025/tests/nvme-pipeline.mk','ZEDBSD_CONFIG=config/ci/config-amd64.mk',f'BUILD={private}', f'ARCH_IMAGE_DIR={arches}', f'ZEDBSD_TEST_CPPFLAGS=-DNVME_IO_PIPELINE_DEPTH={depth}', str(private/'vmunix'), 'ws025-nvme-fixture']
            (cell/'build-command.json').write_text(json.dumps(command,indent=2)+'\n')
            with (cell/'build.log').open('w') as log:
                subprocess.run(command,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
            kernel=cell/'vmunix'; shutil.copyfile(private/'vmunix',kernel)
            subprocess.run(['cp','--reflink=auto','--sparse=always',str(source),str(cell/'boot.img')],check=True)
            for src,dst in [(kernel,'vmunix'),(arches/'amd64-ws025-nvme.ufs','rootfs.img')]:
                subprocess.run(['mcopy','-o','-i',f'{cell/"boot.img"}@@{candidates[0]*512}',str(src),'::/'+dst],check=True)
            with (cell/'gpt.img').open('xb') as f: f.truncate(5*1024**3)
            base.fixture(cell/'mbr.img',False)
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',cell/'vars.fd')
            results=[]
            for boot in range(2):
                if boot:
                    for name in ('guest.log','qemu.log','commands.log'):
                        (cell/name).rename(cell/('boot1-'+name))
                guest=base.Guest(cell,usb_boot=True,extra_args=['-qmp',f'unix:{cell/"observe.sock"},server=on,wait=off'])
                try:
                    guest.login()
                    before=snapshot(guest,cell,kernel,layout,f'b{boot}-before')
                    mode = 'write' if boot == 0 else 'verify'
                    guest.run('nvme-bio '+mode, expected='NVME BIO PASS mode='+mode)
                    native = snapshot(guest,cell,kernel,layout,f'b{boot}-native')
                    assert native['posted'] - before['posted'] == (1056 if boot == 0 else 512), (before,native)
                    # Hardware can finish before the submission window is filled.
                    assert 1 <= native['high_water'] <= depth, (depth,native)
                    if depth > 1:
                        assert native['high_water'] > 1, ('no observed overlap',depth,native)
                    pairs = [('write',8388608),('write',4294971392),('stress-write',4296015872),('concurrent-write',4311744512)] if boot == 0 else [('verify',8388608),('verify',4294971392),('stress-verify',4296015872),('concurrent-verify',4311744512)]
                    for mode,offset in pairs:
                        start=time.monotonic()
                        guest.run(f'nvme-pipeline {mode} /dev/nvme0n1 {offset}',expected=f'HW-T20 NVME-IO PASS mode={mode} offset={offset}')
                        results.append({'boot':boot,'mode':mode,'offset':offset,'seconds':time.monotonic()-start})
                    after=snapshot(guest,cell,kernel,layout,f'b{boot}-after')
                    assert after['posted'] > before['posted'] + 100
                    assert native['high_water'] <= after['high_water'] <= 63, (depth,after)
                    text=guest.text()
                    assert not re.search(r'PANIC|page fault|invalid free|NVME-IO FAIL',text,re.I)
                finally: guest.stop()
            metadata['depths'][str(depth)]={'status':'PASS','results':results,'kernel_sha256':base.digest(kernel)}
            (out/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
            print(f'depth {depth} QEMU PASS',flush=True)
    finally:
        metadata['source_unchanged']=base.digest(source)==original
        (out/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
        assert metadata['source_unchanged']
if __name__ == '__main__': main()
