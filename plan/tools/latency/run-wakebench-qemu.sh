#!/bin/sh
# ws041: boots IMG on SMP CPUs (1 by default, so every process shares one
# CPU), logs in on the serial console and runs the wakebench modes.
set -u
cd /home/awe/zedBSD-rpi4
IMG=${IMG:?image}
SMP=${SMP:-1}
D=${RUN:-/tmp/ws041-run}
mkdir -p $D; rm -f $D/*.sock
for pid in $(pgrep qemu-system); do kill -9 $pid; done; sleep 1
cp $IMG $D/stick.img; cp /usr/share/OVMF/OVMF_VARS_4M.fd $D/vars.fd
qemu-system-x86_64 -accel kvm -machine q35 -m 512 -smp $SMP -cpu host \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=$D/vars.fd -device qemu-xhci,id=xhci \
  -drive if=none,id=boot,file=$D/stick.img,format=raw \
  -device usb-storage,bus=xhci.0,port=1,drive=boot,bootindex=1 \
  -serial unix:$D/serial.sock,server,nowait -vga std -display none -no-reboot \
  -qmp unix:$D/qmp.sock,server,nowait > $D/qemu.log 2>&1 &
sleep 5
G="python3 plan/tools/guest/serial.py --socket $D/serial.sock"
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12; do
	timeout 60 $G --timeout 40 login >/dev/null 2>&1 && break
done
$G --timeout 300 run 'wakebench quiet 2000; wakebench pingpong 2000; wakebench sleep 500; wakebench fair 5; wakebench starve 5'
python3 plan/tools/qmp.py $D/qmp.sock quit > /dev/null 2>&1
