<!-- awesome-plan project=zedbsd record=ws326-p016 -->

# ws326-p016 MV の予測と P_Skip

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、67a62dfba）。
- 依存: p008（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-mvpred.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 6.4.11.7、8.4.1.1、8.4.1.3・8.4.1.3.1・8.4.1.3.2（式 8-203〜8-216）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-mvpred.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-mvpred.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- median の 3 例、refIdx が 1 つだけ一致の 3 例、B と C が無く A だけの時、C が無く D で代える時。
- 16x8 の上下・8x16 の左右の方向の予測（一致と不一致）。
- macroblock の中の未 decode の C（decoded_4x4 の bit）: 8x8 の sub の 4x4 の 4 個の各々の C の可否。
- P_Skip: A か B が無い、A が ref 0 で mv 0、B が同じ、どれでもない時の median。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- struct h264sw_slice_ctx と mb の配列を test で作る（picture 3x3 の macroblock）。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh mvpred` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 日付: 2026-10-30（host の `date`）。状態: 実装済み・試験 PASS（cleared は Q1 が受け入れを見て決める）。
- 作った物: `userland/desktop/libmedia/h264sw-mvpred.c`（header は変えていない）、`plan/ws326/tests/unit/test-mvpred.c`。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh mvpred` → `test-mvpred: PASS (83 checks)`、`run-unit: PASS`（ASan・UBSan 黙る）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 全 10 試験 PASS、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の `warning` は 0 件。
  - `git diff --check` → 問題なし。
- 試験の中身: median（3 例と、極値 32767・-32768）、refIdx が 1 つだけ一致（A・B・C の各々）・2 つ一致・0 一致、A だけ（B と C が無い、A の refIdx が不一致でも A の値）、
  B が無く C が有る（A による置換が起きない）、C が無く D で代える（最後の列）、16x8 の上下・8x16 の左右（一致と不一致、D を使う右の 8x16）、
  1 つの macroblock の中の 16 個の 4x4 block 全部の A・B・C/D（decoded_4x4 を block 番号の順で与え、各 block の vector がその block の id を持つので、返った値がどの block から来たか分かる。期待は表 6-2・6-3 から手で追った）、
  8x8・8x4・4x8 の partition、decoded_4x4 の bit の有無での C の可否、intra の隣・list を使わない隣・list 1、
  P_Skip（A か B が無い・A が ref 0 で mv 0・B が同じ・median・A が ref 1 で mv 0・A が intra・別 slice）、
  誤りの入力（list>1・負の refIdx・空か macroblock を出る矩形・巨大な値・NULL）。
- 判断（header が書かない所）:
  - 誤りの入力（NULL の ctx・出力、`ctx->n.current` が NULL、list が 2 以上、ref_idx が負、width か height が 0、x か y が 16 以上、矩形が macroblock を出る）は、mvp（P_Skip は mv）に 0 と 0 を書いて戻る。ref_idx が負なのは「list を使わない」を表す -1 と区別が付かず、仕様は予測を求めない値なので拒む。
  - 16x8 と 8x16 の向き付き予測は、矩形が 16x8（y が 0 か 8）または 8x16（x が 0 か 8）の時だけ。y・x がそれ以外の値なら median へ進む。
  - 隣の partition が「利用可能」とは、macroblock が利用可能（slice が同じ）で、現在の macroblock の中なら decoded_4x4 の bit が立っていること。intra でも利用可能（median の手順 1 の A の判定に効く）。intra は ref_idx 範囲の外ではなく `motion.intra` で見る。
  - 隣の refIdx は 8x8 quadrant の `ref_idx[list][blk / 4]`（blk は luma4x4BlkIdx。quadrant 内の 4 個が連続する番号）、vector は 4x4 block の `mv[list][blk]`。ref_idx が負なら vector が保存されていても 0 とする（8.4.1.3.2）。
  - median は式 5-15（x + y + z - Min - Max）。frame の macroblock だけ（MBAFF の式 8-217〜8-220 は範囲外）。
- 仕様と phase.md の食い違い: 無し。
- 未実施: 実機・QEMU の試験は対象外（host の単体試験のみ）。突然変異による試験の検出力の確認は未実施。
