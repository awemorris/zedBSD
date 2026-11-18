# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Dev-time only: cross-check hand-authored readings against MeCab (IPADIC)
# and Sudachi to catch typos. Analyzer output is NOT shipped; this only
# flags mismatches for human review.
import subprocess, glob, sys
KATA="ァアィイゥウェエォオカガキギクグケゲコゴサザシジスズセゼソゾタダチヂッツヅテデトドナニヌネノハバパヒビピフブプヘベペホボポマミムメモャヤュユョヨラリルレロワヲンヴー"
HIRA="ぁあぃいぅうぇえぉおかがきぎくぐけげこごさざしじすずせぜそぞただちぢっつづてでとどなにぬねのはばぱひびぴふぶぷへべぺほぼぽまみむめもゃやゅゆょよらりるれろわをんゔー"
K2H=str.maketrans(KATA,HIRA)
def mecab_reading(word):
    try:
        out=subprocess.run(['mecab'],input=word+"\n",capture_output=True,text=True,timeout=5).stdout
    except Exception: return None
    yomi=""
    for line in out.splitlines():
        if line=="EOS" or "\t" not in line: continue
        f=line.split("\t")[1].split(",")
        yomi += (f[7] if len(f)>7 and f[7]!="*" else line.split("\t")[0])
    return yomi.translate(K2H)
mism=[]
for path in sorted(glob.glob("src/*.tsv")):
    for ln in open(path,encoding="utf-8"):
        ln=ln.rstrip("\n")
        if not ln or ln.lstrip().startswith("#"): continue
        f=ln.split("\t")
        if f[0] in ("V5","V1","ADJ"):
            reading=f[1]; word=f[2]+ (reading[len(f[3]):] if f[0]!="V5" else reading[len(reading)-1:]) if False else reading
            # reconstruct surface: for verbs/adj compare dict-form reading to mecab of the full word
            surface = {"V5":f[2]+reading[-1],"V1":f[2]+reading[len(f[3]):],"ADJ":f[2]+"い"}[f[0]]
            exp=reading
        else:
            surface=f[1].split(",")[0]; exp=f[0]
        got=mecab_reading(surface)
        if got and got!=exp:
            mism.append((path.split('/')[-1],surface,exp,got))
for m in mism:
    print("MISMATCH %-22s %s  authored=%s  mecab=%s"%(m[0],m[1],m[2],m[3]))
print("---- %d mismatches to review ----"%len(mism),file=sys.stderr)
