#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Prints the buttons the compositor saw, what took them, the pages Settings opened, and the receiver's dumped mouse reports.
L=/run/user/1000/session.log
grep -n -E "KWL (BTN|FRAME|PRESS|PING pong)|ZSETTINGS (PAGE|WELCOME)" $L | grep -v "KWL FRAME dev=/dev/input/event[4-7]" | tail -${1:-80}
echo "--- dmesg interface 1 reports"
dmesg | grep "usb-hid-report: usb0 device 1 interface 1" | tail -${1:-80}
