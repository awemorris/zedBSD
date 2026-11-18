#!/bin/sh
# ws095-p017: makes SKK-JISYO.ja, the one dictionary file of zedBSD's input method, from the two it was made of
# (2026-10-05, the user's decision "辞書を 1 つにまとめる"): zedBSD's own supplement (SKK-JISYO.kei, looked in
# first) and REmacs's dictionary (SKK-JISYO.X).  They are not merged headword by headword: the segmenter weighs a
# headword of the supplement above one of the system dictionary (ja-segment.c), so the file keeps them as two
# parts, the supplement's first, and the line ";; ==== part: system ====" begins the system dictionary's part
# (ja_dict_load_parts reads the file once and indexes each part).  Both parts keep their own headers.
# The two files were taken out of the tree once the result was committed; their last revision is in git
# (userland/desktop/ime/dict/SKK-JISYO.kei and SKK-JISYO.X before ws095-p017).
#
#   sh userland/desktop/ime/dict/merge.sh SKK-JISYO.kei SKK-JISYO.X > SKK-JISYO.ja
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
supplement=$1
system=$2
cat <<'HEADER'
;; -*- mode: fundamental; coding: utf-8 -*-
;; SKK-JISYO.ja - the dictionary of zedBSD's input method (Japanese).
;;
;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;
;; One file of two parts (ws095-p017), read in this order:
;;
;;   1. zedBSD's supplement (formerly SKK-JISYO.kei): about a thousand everyday
;;      words written for zedBSD (WS095 decision D3), with the annotations of
;;      how verbs and adjectives conjugate.
;;   2. REmacs's dictionary (formerly SKK-JISYO.X), from the line
;;      ";; ==== part: system ====" on.  Its copyright holder, Awe Morris,
;;      relicensed it under zlib for zedBSD on 2026-09-29 (WS095 decision D1;
;;      its own header below is kept as it was published).
;;
;; Both parts are SKK dictionaries; an SKK tool reads the file as one.  The
;; input method keeps them apart because it weighs the supplement's words
;; above the system dictionary's when it splits a sentence.
;;
;; ==== part: supplement ====
HEADER
cat "$supplement"
printf ';; ==== part: system ====\n'
cat "$system"
