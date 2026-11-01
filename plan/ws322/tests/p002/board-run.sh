#!/bin/sh
# Runs on the VisionFive 2 as root (ws322-p002): reads /dev/vf2-vpu-probe with the buildroot firmware
# (wave511_dec_fw.bin) installed, then with soft_3rdpart's chagall.bin installed under the same name, and puts the
# first back.  run-vf2.sh copies this file and the firmware files to /var/vpu-probe.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
d=/var/vpu-probe
f=/lib/firmware/wave511/wave511_dec_fw.bin
echo "=== probe firmware wave511_dec_fw.bin"
cp $d/wave511_dec_fw.bin $f && sync
cat /dev/vf2-vpu-probe
echo "=== probe firmware chagall.bin"
cp $d/chagall.bin $f && sync
cat /dev/vf2-vpu-probe
cp $d/wave511_dec_fw.bin $f && sync
echo "=== end"
