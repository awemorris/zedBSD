#!/bin/sh
# ws036-p012: boots the Raspberry Pi 4 image in QEMU raspi4b, waits for the
# login prompt on the serial console, logs in and runs a few commands.
#   BUILD=/tmp/rpi4-k sh plan/ws036/tests/boot-rpi4.sh
# BUILD is a directory made by
#   make BUILD=$BUILD ZEDBSD_CONFIG=config/ci/config-rpi4.mk disk-image
set -e
BUILD=${BUILD:-build/arm64}
D=${RUN:-/tmp/rpi4-run}; mkdir -p $D; rm -f $D/serial.sock
DTB=vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb
qemu-system-aarch64 -M raspi4b -m 2G -kernel $BUILD/vmunix -dtb $DTB \
  -drive file=$BUILD/hdd-image.img,format=raw,if=sd,snapshot=on \
  -display none -serial unix:$D/serial.sock,server=on,wait=off -serial null \
  -monitor none > $D/qemu.log 2>&1 &
echo $! > $D/qemu.pid
sleep 2
S="python3 plan/tools/guest/serial.py --socket $D/serial.sock --timeout 120"
$S expect 'login: ' > /dev/null
echo "rpi4: login prompt"
$S login
$S run 'uname -a; id; df; /bin/dyntest | tail -n 1; echo rpi4-commands-ok'
kill "$(cat $D/qemu.pid)"
