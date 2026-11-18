# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Phase 1 (collection): discover vocabulary from an input corpus (the user's
# own diary) via MeCab, and emit a "collection" TSV in the same authored
# format expand.py consumes.  MeCab is used only to ENUMERATE which words the
# corpus uses and their standard (factual) readings; the SKK headword split
# is done by our own unified rule.  Selection is driven by the user's text.
import subprocess, collections, sys, unicodedata
KATA="ァアィイゥウェエォオカガキギクグケゲコゴサザシジスズセゼソゾタダチヂッツヅテデトドナニヌネノハバパヒビピフブプヘベペホボポマミムメモャヤュユョヨラリルレロワヲンヴー"
HIRA="ぁあぃいぅうぇえぉおかがきぎくぐけげこごさざしじすずせぜそぞただちぢっつづてでとどなにぬねのはばぱひびぴふぶぷへべぺほぼぽまみむめもゃやゅゆょよらりるれろわをんゔー"
K2H=str.maketrans(KATA,HIRA)
def is_hira(c): return 'ぁ'<=c<='ゟ'
def is_kanji(c): return '一'<=c<='鿿' or c=='々'
def mecab(text):
    return subprocess.run(['mecab'],input=text,capture_output=True,text=True).stdout

import os
corpus_path=sys.argv[3] if len(sys.argv)>3 else "/home/awe/text"
corpus=open(corpus_path,encoding="utf-8",errors="replace").read()
noun=collections.Counter()          # (reading, kanji) -> freq
verbadj={}                          # base_surface -> [pos, freq]
for line in mecab(corpus).splitlines():
    if line=="EOS" or "\t" not in line: continue
    surf,feat=line.split("\t",1); f=feat.split(",")
    pos=f[0]
    if pos=="名詞":
        if f[1] in ("非自立","接尾","代名詞","数","副詞可能"): continue
        yomi=f[7] if len(f)>7 and f[7]!="*" else ""
        if not yomi or not any(is_kanji(c) for c in surf): continue
        if not any(is_kanji(c) for c in surf): continue
        if any(not (is_kanji(c) or is_hira(c) or "ァ"<=c<="ヿ") for c in surf): continue
        noun[(yomi.translate(K2H),surf)]+=1
    elif pos in ("動詞","形容詞"):
        if f[1] in ("非自立","接尾"): continue
        base=f[6] if len(f)>6 and f[6]!="*" else surf
        if not any(is_kanji(c) for c in base): continue
        if not is_hira(base[-1]): continue        # must end in okurigana kana
        if any(not (is_kanji(c) or is_hira(c)) for c in base): continue
        e=verbadj.setdefault(base,[pos,0]); e[1]+=1

# base-form readings for verbs/adjectives (re-analyze the base forms)
bases=list(verbadj)
basejoin="\n".join(bases)
read={}
i=0
for line in mecab(basejoin).splitlines():
    if line=="EOS" or "\t" not in line: continue
    surf,feat=line.split("\t",1); f=feat.split(",")
    # a base may tokenize to >1 token; accumulate until it matches a known base
    # simple path: reconstruct per line index is unreliable, so map by surface
    y=f[7] if len(f)>7 and f[7]!="*" else surf
    read.setdefault(surf, y.translate(K2H))

def base_reading(base):
    # analyze this single base alone to get its whole reading
    out=mecab(base)
    y=""
    for ln in out.splitlines():
        if ln=="EOS" or "\t" not in ln: continue
        f=ln.split("\t")[1].split(",")
        y+=(f[7] if len(f)>7 and f[7]!="*" else ln.split("\t")[0])
    return y.translate(K2H)

# emit
NOUN_MIN=int(sys.argv[1]) if len(sys.argv)>1 else 2
VB_MIN=int(sys.argv[2]) if len(sys.argv)>2 else 2
OUT_N=sys.argv[4] if len(sys.argv)>4 else "src/50-diary-nouns.tsv"
OUT_V=sys.argv[5] if len(sys.argv)>5 else "src/51-diary-verbs.tsv"
with open(OUT_N,"w",encoding="utf-8") as w:
    w.write("# collected from ~/text (user diary) via MeCab enumeration.\n")
    by=collections.defaultdict(list)
    for (yomi,kanji),c in noun.items():
        if c>=NOUN_MIN and all(is_hira(ch) for ch in yomi):
            by[yomi].append((c,kanji))
    for yomi in sorted(by):
        ks=[k for _,k in sorted(by[yomi],reverse=True)]
        w.write(yomi+"\t"+",".join(dict.fromkeys(ks))+"\n")
badv=0
with open(OUT_V,"w",encoding="utf-8") as w:
    w.write("# collected verbs/adjectives from ~/text via MeCab.  W <reading> <surface>.\n")
    for base,(pos,c) in sorted(verbadj.items()):
        if c<VB_MIN: continue
        k=len(base)
        while k>0 and is_hira(base[k-1]): k-=1
        okuri=base[k:]; kanji=base[:k]
        if not kanji or not okuri: continue
        br=base_reading(base)
        if len(br)<=len(okuri) or not br.endswith(okuri):
            badv+=1; continue
        w.write("W\t%s\t%s\n"%(br,base))  # unified rule derives the split
print("nouns:",len([1 for v in noun.values() if v>=NOUN_MIN]),"verb/adj bases:",len(verbadj),"skipped:",badv)
