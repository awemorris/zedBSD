#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# -*- coding: utf-8 -*-
#
# expand.py - Build the remacs SKK dictionary (SKK-JISYO.remacs) from
# hand-authored, clean-room source lists.
#
# The linguistic content (readings, kanji, conjugation class) is authored
# by hand in src/*.tsv.  This script only performs the *mechanical* string
# work of turning that content into SKK dictionary format:
#   - merging okuri-nasi candidates that share a reading,
#   - deriving okuri-ari headwords (reading-stem + okurigana consonant)
#     from a word's conjugation class,
#   - sorting and emitting the two SKK sections in UTF-8.
#
# No entry is copied from any existing dictionary (SKK-JISYO.L is GPL and
# is never read); the readings are original.  Therefore the output carries
# only the remacs license, with no third-party attribution requirement.
#
# Source format (TAB-separated), '#' starts a comment line:
#   nouns/misc :  reading <TAB> kanji[,kanji2,...] [<TAB> annotation]
#   verb godan :  V5 <TAB> reading(dict-form) <TAB> kanji-stem
#   verb ichidan: V1 <TAB> reading(dict-form) <TAB> kanji-stem <TAB> stem-reading
#   adjective   :  ADJ <TAB> reading(dict-form ...i) <TAB> kanji-stem <TAB> stem-reading
#
# The verb/adjective rows carry the *reading* only; the kanji stem is the
# part shown before the okurigana.  Godan derives the stem reading by
# dropping the last kana; ichidan/adjective give it explicitly because the
# okurigana length varies (見る=み+る vs 食べる=た+べる).

import sys, os, glob

# First romaji consonant letter of a kana (as typed in SKK okurigana).
KANA_CONS = {}
def _fill(rows):
    for kana, c in rows:
        KANA_CONS[kana] = c
_fill([
    ("か","k"),("き","k"),("く","k"),("け","k"),("こ","k"),
    ("さ","s"),("し","s"),("す","s"),("せ","s"),("そ","s"),
    ("た","t"),("ち","t"),("つ","t"),("て","t"),("と","t"),
    ("な","n"),("に","n"),("ぬ","n"),("ね","n"),("の","n"),
    ("は","h"),("ひ","h"),("ふ","h"),("へ","h"),("ほ","h"),
    ("ま","m"),("み","m"),("む","m"),("め","m"),("も","m"),
    ("や","y"),("ゆ","y"),("よ","y"),
    ("ら","r"),("り","r"),("る","r"),("れ","r"),("ろ","r"),
    ("わ","w"),("を","w"),
    ("が","g"),("ぎ","g"),("ぐ","g"),("げ","g"),("ご","g"),
    ("ざ","z"),("じ","j"),("ず","z"),("ぜ","z"),("ぞ","z"),
    ("だ","d"),("ぢ","j"),("づ","z"),("で","d"),("ど","d"),
    ("ば","b"),("び","b"),("ぶ","b"),("べ","b"),("ぼ","b"),
    ("ぱ","p"),("ぴ","p"),("ぷ","p"),("ぺ","p"),("ぽ","p"),
    # う as the FIRST okurigana kana only occurs for godan-u verbs (買う→買わ
    # ない), whose SKK consonant is 'w'; other vowels map to themselves.
    ("あ","a"),("い","i"),("う","w"),("え","e"),("お","o"),
    ("ん","n"),
])

def cons_of(kana):
    if kana not in KANA_CONS:
        raise ValueError("no consonant mapping for kana: %r" % kana)
    return KANA_CONS[kana]

def okuri_cons(okuri):
    # Consonant of the first typed romaji letter of the okurigana.  A leading
    # っ (sokuon) is typed as a doubled consonant, so the first letter is the
    # consonant of the following kana (引っかかる -> っか -> 'k' -> ひk).
    if okuri and okuri[0] == "っ" and len(okuri) > 1:
        return cons_of(okuri[1])
    return cons_of(okuri[0])

# reading -> set of okuri-nasi candidates (ordered, de-duped)
nasi = {}
# headword -> ordered list of kanji stems (okuri-ari)
ari = {}
ann = {}   # (reading, kanji) -> annotation

def add_nasi(reading, kanji, annotation=None):
    lst = nasi.setdefault(reading, [])
    if kanji not in lst:
        lst.append(kanji)
    if annotation:
        ann[(reading, kanji)] = annotation

def add_ari(headword, kanji):
    lst = ari.setdefault(headword, [])
    if kanji not in lst:
        lst.append(kanji)

def trailing_kana(surface):
    # length of the trailing hiragana run (the okurigana) of a surface form.
    k = len(surface)
    while k > 0 and "ぁ" <= surface[k - 1] <= "ゟ":
        k -= 1
    return len(surface) - k

def parse_row(row, lineno, path):
    f = row.split("\t")
    tag = f[0]
    if tag == "W":
        # Unified verb/adjective:  W <reading> <surface>
        # okurigana = surface's trailing hiragana run; the kanji candidate is
        # the rest; the stem reading is the reading minus the okurigana.  The
        # headword is stem + the consonant the okurigana is typed with.
        #   書く   -> 書 +く   -> かk       上がる -> 上 +がる -> あg
        #   食べる -> 食 +べる -> たb       買う   -> 買 +う   -> かw
        #   難しい -> 難 +しい -> むずかs   高い   -> 高 +い   -> たかi (+たかk)
        reading, surface = f[1], f[2]
        n = trailing_kana(surface)
        if n == 0 or n >= len(reading):
            raise ValueError("no okurigana boundary: %r %r" % (reading, surface))
        okuri = reading[len(reading) - n:]
        kanji = surface[:len(surface) - n]
        stem = reading[:len(reading) - n]
        add_ari(stem + okuri_cons(okuri), kanji)
        # A pure い-adjective (okurigana == い) also inflects to く/かっ/けれ,
        # so add the k-headword (高い->たかk for 高く/高かった).
        if okuri == "い":
            add_ari(stem + "k", kanji)
    else:                                  # okuri-nasi: reading <TAB> kanji[,..]
        reading = f[0]
        for k in f[1].split(","):
            add_nasi(reading, k, f[2] if len(f) > 2 else None)
    return

def load(path):
    with open(path, encoding="utf-8") as fp:
        for i, line in enumerate(fp, 1):
            line = line.rstrip("\n")
            if not line or line.lstrip().startswith("#"):
                continue
            try:
                parse_row(line, i, path)
            except Exception as e:
                sys.stderr.write("%s:%d: %s  (%r)\n" % (path, i, e, line))
                raise

def cand_block(cands, reading):
    out = []
    for k in cands:
        a = ann.get((reading, k))
        out.append(k + (";" + a if a else ""))
    return "/" + "/".join(out) + "/"

def make_header(out_path):
    name = os.path.basename(out_path)
    if name.endswith(".X"):
        tagline = ("an extended dictionary covering the maintainer's full "
                   "writing corpus")
    else:
        tagline = "a compact everyday-Japanese dictionary for remacs"
    return (
";; -*- mode: fundamental; coding: utf-8 -*-\n"
";; %s - %s.\n"
";;\n"
";; Copyright (C) 2026 remacs authors.\n"
";;\n"
";; This dictionary is an original work built from hand-authored word lists\n"
";; plus vocabulary collected from the maintainer's own writing.  No entry is\n"
";; copied from SKK-JISYO.L (GPL), which is never read by the build.  The\n"
";; okurigana headword construction is entirely our own.  Word readings are\n"
";; standard Japanese readings (facts); during the build they were enumerated\n"
";; and cross-checked with MeCab + IPADIC and Sudachi (SudachiDict), which are\n"
";; development tools only and ship no data of their own into this file.\n"
";;   MeCab/IPADIC: BSD-style license.   SudachiDict: Apache License 2.0.\n"
";; This dictionary is distributed under the same license as remacs.\n"
";;\n"
";; SKK-JISYO.remacs is the compact bundled default; SKK-JISYO.X is the fuller\n"
";; version.  For an even larger dictionary, download SKK-JISYO.L separately\n"
";; and point SkkDictPath at it.\n"
";;\n" % (name, tagline))

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        here, "..", "..", "dict", "SKK-JISYO.remacs")
    # Optional source globs (argv[2:]) select which word lists to include;
    # default is the compact set in src/.  SKK-JISYO.X adds srcx/.
    if len(sys.argv) > 2:
        patterns = sys.argv[2:]
    else:
        patterns = [os.path.join(here, "src", "*.tsv")]
    srcs = []
    for p in patterns:
        srcs += sorted(glob.glob(p))
    for s in srcs:
        load(s)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as w:
        w.write(make_header(out))
        w.write(";; okuri-ari entries.\n")
        for hw in sorted(ari.keys(), reverse=True):     # descending, SKK convention
            w.write(hw + " " + cand_block(ari[hw], hw) + "\n")
        w.write(";; okuri-nasi entries.\n")
        for rd in sorted(nasi.keys()):                  # ascending
            w.write(rd + " " + cand_block(nasi[rd], rd) + "\n")
    n_ari = len(ari); n_nasi = len(nasi)
    sz = os.path.getsize(out)
    sys.stderr.write("wrote %s: %d okuri-ari + %d okuri-nasi = %d entries, %d bytes (%.1f KB)\n"
                     % (out, n_ari, n_nasi, n_ari + n_nasi, sz, sz / 1024.0))

if __name__ == "__main__":
    main()
