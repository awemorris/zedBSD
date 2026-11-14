#!/bin/sh
# Prints agy's final answer of round N and writes the commit's diff to rst-diffN.diff.
S=$(cd "$(dirname "$0")" && pwd)
N=$1
tail -2 "$S/r-rst$N.jsonl" | head -1 | /usr/bin/python3 -I -c '
import json,sys
e=json.loads(sys.stdin.read())
def find(o):
    if isinstance(o,dict):
        for k,v in o.items():
            if isinstance(v,str) and "DONE" in v and len(v)>200: print(v); return True
            if find(v): return True
    elif isinstance(o,list):
        return any(find(v) for v in o)
find(e)'
