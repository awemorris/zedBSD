#!/bin/sh
# ws035-p007: boots the HDA test image under KVM with the serial console and
# logs in.  IMG is the disk image, RUN the run directory, QEMU_EXTRA the audio
# devices.
cd /home/awe/zedBSD-rpi4
for pid in $(pgrep qemu-system); do kill -9 $pid; done; sleep 1
IMG=${IMG:-build/p007-img/hdd-image.img}
D=${RUN:-/tmp/p007-run}; mkdir -p $D; rm -f $D/*.sock $D/*.wav
cp $IMG $D/stick.img; cp /usr/share/OVMF/OVMF_VARS_4M.fd $D/vars.fd
qemu-system-x86_64 -accel kvm -machine q35 -m 512 -smp 4 -cpu host \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=$D/vars.fd -device qemu-xhci,id=xhci \
  -drive if=none,id=boot,file=$D/stick.img,format=raw \
  -device usb-storage,bus=xhci.0,port=1,drive=boot,id=rootstick,bootindex=1 \
  -serial unix:$D/serial.sock,server,nowait -vga std -display none -no-reboot \
  -qmp unix:$D/qmp.sock,server,nowait $QEMU_EXTRA > $D/qemu.log 2>&1 &
sleep 5
G=plan/tools/guest/serial.py; S=$D/serial.sock
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12; do
	timeout 60 python3 $G --socket $S --timeout 40 login >/dev/null 2>&1 && { echo "serial: logged in"; exit 0; }
done
echo "serial: login failed"; exit 1
