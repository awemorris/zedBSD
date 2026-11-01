#!/bin/sh
# usage: cmpgap.sh NEW PREV   (dev numbers)
cd /home/user/zedBSD
python3 plan/ws074/tests/interop2025-chrome-gap.py compare --chrome build/ws074-cmp/chrome --ours build/ws074-cmp/dev$1 --write build/ws074-cmp/gap$1.json
python3 -I - $1 $2 <<'P'
import json,sys
a=json.load(open('build/ws074-cmp/gap%s.json'%sys.argv[2]))['gap']; b=json.load(open('build/ws074-cmp/gap%s.json'%sys.argv[1]))['gap']
for k in a:
    print(k,'fixed',sorted(set(a[k])-set(b[k])),'REGRESS',sorted(set(b[k])-set(a[k])))
P
