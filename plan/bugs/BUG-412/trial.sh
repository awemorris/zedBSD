#!/bin/sh
# Runs one click variant on the board (root) and prints the Settings page changes it caused.
# usage: trial.sh NAME COMMANDS... (each word sequence separated by ';' runs through aat-input)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
A=/tmp/aat-input2
L=/run/user/1000/session.log
name=$1; shift
# Back to Bluetooth first, with the absolute pointer.
$A move-to 494 300 >/dev/null; $A sleep 200 >/dev/null; $A down left >/dev/null; $A sleep 40 >/dev/null; $A up left >/dev/null; $A sleep 500 >/dev/null
n=$(wc -l < $L)
echo "$*" | tr ';' '\n' | while read -r c; do [ -n "$c" ] && $A $c >/dev/null; done
sleep 1
printf '%s: %s\n' "$name" "$(tail -n +$n $L | grep -o -E 'ZSETTINGS PAGE [a-z]+' | tr '\n' ' ')"
