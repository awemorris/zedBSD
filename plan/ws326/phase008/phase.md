<!-- awesome-plan project=zedbsd record=ws326-p008 -->

# ws326-p008 macroblock の型と neighbour

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、e1a70942e）。
- 依存: p001（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-mb.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 表 7-11・7-13・7-14・7-17・7-18、6.4.3・6.4.7・6.4.8・6.4.9・6.4.11.1・6.4.11.2・6.4.11.4・6.4.11.5・6.4.12.1（表 6-3）・6.4.13.1。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-mb.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-mb.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- mb_type: I の 0（I_NxN）・1〜24（I_16x16 の mode・cbp_luma・cbp_chroma を表 7-11 の全行で）・25（I_PCM）、P の 0〜4 と 5〜30、B の 0〜22 と 23〜48、範囲外は EINVAL。skip の 2 種。sub_mb_type の P 0〜3・B 0〜12。
- blk4x4 の x・y（6.4.3）の 16 個と h264sw_blk4x4_at の往復。
- neighbours: 4x3 の picture で、角・端・中の macroblock、別 slice の隣、未 decode（slice -1）の隣。locate の表 6-3 の 7 行（xN・yN の負・中・超え）と xW・yW。
- 4x4 の neighbour A・B を 16 block 全部で（同じ macroblock の中と隣の macroblock）、8x8、chroma 4x4。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- struct h264sw_mb の配列を test で作り、slice の番号で availability を作る。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh mb` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 日付: 2026-10-30（host の `date`）。
- 作った物: `userland/desktop/libmedia/h264sw-mb.c`（全関数と表。表 7-11・7-13・7-14・7-17・7-18 と 6.4.3 の block 位置を block ごとの `Values referenced:` の comment で出典付き）、単体試験 `plan/ws326/tests/unit/test-mb.c`。header は変えていない。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh mb` → `test-mb: PASS (1938 checks)`、`run-unit: PASS`（ASan・UBSan の報告 0）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → test-bitwriter PASS (14 checks)、test-mb PASS (1938 checks)、`run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の warning 0（`grep -ci warning` が 0）。
  - `git diff --check` → 問題なし。
- header が決めていない点で担当が決めた物（Q1 の確認用）:
  - B_Direct_16x16 と B_Skip の `parts` は表が na なので 4（MbPartWidth・Height は表の 8、8x8 の kind と同じ扱い）。
  - 範囲外の入力: `h264sw_blk4x4_at` は各座標を 16 で割った余りに、`h264sw_mb_locate` は (xN + maxW) % maxW を非負に正規化。返す物が NULL の時 `*xw`・`*yw`・`*blk_n` は 0。
  - `h264sw_mb_neighbours` は current の slice が -1 の時、隣を全て NULL にする。`width_mbs` 0 や `mbs` NULL は current も NULL。
  - `h264sw-mb.c` は `H264SW_SLICE_*` のために `h264sw-slice.h` を include する（`h264sw-mb.h` は include しない）。
- 未実施: QEMU・実機の試験（この Phase の範囲外）。規約の全文の見直しの Phase（WS の最後）。
