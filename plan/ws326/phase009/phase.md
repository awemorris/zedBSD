<!-- awesome-plan project=zedbsd record=ws326-p009 -->

# ws326-p009 CAVLC

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-30 p001 で作成）。
- 依存: p002、p008（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-cavlc.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.3.5.3.2、9.2・9.2.1（表 9-5）・9.2.2・9.2.2.1（表 9-6）・9.2.3（表 9-7〜9-10）・9.2.4、9.1.2（表 9-4）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-cavlc.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-cavlc.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- coeff_token: 表 9-5 の 5 列（0 <= nC < 2、2 <= nC < 4、4 <= nC < 8、8 <= nC、nC = -1）の各々から 10 個以上の符号語（最短と最長、TrailingOnes 0〜3、TotalCoeff の小と大を含む）を PDF の表から手で選んで bitwriter で書き、TotalCoeff・TrailingOnes が表どおり。実装の表を test に写して比べることはしない（同じ写し間違いが通る）。
- level: suffixLength 0 からの増え方、level_prefix 14・15（escape）・16 以上、TrailingOnes < 3 の +2、手で計算した block 3 つ（係数の列 → bit 列は 9.2 を逆に追って作る）。
- total_zeros（表 9-7・9-8・9-9 a）と run_before（表 9-10）、zerosLeft が 0 で止まる、maxNumCoeff 15（AC）と 4（chroma DC）の配置。
- nC: 隣が無い・片方・両方・skip・I_PCM（16）。cbp: 表 9-4 の codeNum 0〜47 の intra と inter の全部、48 は EINVAL。
- 誤り: 表に無い符号、TotalCoeff が maxNumCoeff を超える、run が start より前、終わりで切れた入力。
- 表の形（写し間違いを捕まえる、実装の表を読む小さな検査を test に書く）: 表 9-5 の各列、表 9-7・9-8 の各 tzVlcIndex、表 9-9 a の各 tzVlcIndex、表 9-10 の各 zerosLeft の符号語の集合が prefix-free で Kraft の和 sum 2^-len が 1（9-5 の nC >= 8 の固定長の列も 1）。符号語の数が行の数どおり（9-5 は列ごとに 62、ただし nC = -1 は 14）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- 表 9-5 の符号語は長さと値の組で持つ（peek して長さで飛ばす方式を推す、h264sw_bits_peek）。block ごとに `Values referenced: ITU-T H.264 (08/2021) Table 9-5`。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh cavlc` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。
