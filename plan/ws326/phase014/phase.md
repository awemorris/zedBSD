<!-- awesome-plan project=zedbsd record=ws326-p014 -->

# ws326-p014 intra 予測

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、a04b5a2d7）。
- 依存: p008（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-intra.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.3.1.1・8.3.1.2.1〜8.3.1.2.9、8.3.2.1（式 8-72）・8.3.2.2.1〜8.3.2.2.10、8.3.3.1〜8.3.3.4、8.3.4.1〜8.3.4.4。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-intra.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-intra.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 4x4 の 9 mode を、決めた端の sample（例 top = 10,20,…,80、left = 5,15,25,35、top_left 0）で、手で計算した 16 値と比べる。top-right が無い時の置き換え。必要な sample が無い mode は EINVAL と 128。
- 8x8: 端の filter（8.3.2.2.1）の結果を手で計算した値で、続いて 9 mode の 1 つ以上ずつの 64 値（少なくとも DC・vertical・diagonal down left・horizontal up）。
- 16x16 の 4 mode（plane は手で計算した 3 点以上）、chroma の 4 mode（DC の 4 block の位置ごとの規則、plane）。
- mode の導出: 隣が無い・inter と constrained_intra_pred・I_16x16（2）・I_8x8 の隣、prev_flag と rem の 3 例。8x8 の n（A は 1、B は 2）。
- edge: 4x4 block 3・5・7・11・13 の top-right の可否、constrained_intra_pred の inter の隣、picture の端。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh intra` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果（2026-10-30）

- 実装: `userland/desktop/libmedia/h264sw-intra.c`（骨格を置き換え。header は変えていない）。単体試験: `plan/ws326/tests/unit/test-intra.c`。
- 実行した command と結果（worktree の root）:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh intra` → `test-intra: PASS (593 checks)`、`run-unit: PASS`。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → test-bits 169、test-bitwriter 14、test-cabac_init 4159、test-intra 593、test-mb 1938、test-nal 95 の全部 PASS、最後の行 `run-unit: PASS`（ASan・UBSan の報告 0）。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so`（指定の command）→ exit 0、出力の warning 0（`-Wall -Wextra -Werror`）。
  - `git diff --check` → 出力なし。
- 試験の中身: 4x4 の 9 mode の 16 値、top-right の置き換え、必要な sample の無い mode の EINVAL と 128。8x8 の 9 mode の 64 値（端の filter の結果を手で計算して V・H に出る）、DC の 4 通り、corner 無しの V・H、top-right 無しの置き換え。
  16x16 の V・H・DC の 4 通り・plane の 3 例（手で計算した点、上と下の clip、勾配の切り下げの点）。chroma の DC（4 block × 4 通り）・H・V・plane（64 値）。
  mode の導出（隣が無い・別の slice・inter と constrained_intra_pred・I_16x16 と I_PCM の隣・I_8x8 の隣・現在の macroblock の中の隣・prev_flag と rem の例・n が A は 1、B は 2）。
  edge（4x4 の block 3・5・7・11・13 の top-right、右端・左端・上端・角の macroblock、別の slice、inter の A・B・C・D と constrained_intra_pred、8x8、16x16、chroma の Cb・Cr、frame の外）。誤りの入力（NULL・範囲外の mode・size・block・plane・巨大な値）。
  期待の値は仕様の式から手で計算した値で、他の decoder の出力や source は使っていない。実装に変異（rounding・index・順序の誤り）を 20 通りほど入れた別の複写（git には入れない）で、観測できる物は試験が FAIL になることを確かめた（下の「観測できない物」を除く）。
- 判断（header が黙っている所）:
  - 端の sample と `available`: gatherer は edge を 0 で埋めてから集める（使われない entry も 0）。predictor は `available` の既知の 4 bit（TOP・LEFT・TOP_LEFT・TOP_RIGHT）以外の bit を見ない。
    top-right の bit が立っても TOP が無い時は、置き換えの元が無いので使わない（DDL・VL は EINVAL）。8x8 の filter は TOP がある時（置き換え後は p[ 0..15, -1 ] が全部ある）に top を filter する。
  - mode の導出の誤りの入力: ctx が NULL か block が範囲外なら 2（DC）。rem が 7 を超える時（3 bit の syntax element なので呼び手の誤り）は結果を 8 に clamp する。neighbour の intra4x4_mode の entry が 8 を超える時と kind が I_4X4・I_8X8 でない時は 2。
    kind は entry の値だけに頼らず、`H264SW_MB_I_4X4`・`H264SW_MB_I_8X8` かどうかを見る（I_16x16 に古い値が残っていても 2）。
  - 「inter」は kind が `H264SW_MB_P_SKIP` 以上（P_Skip から B_8x8）。constrained_intra_pred の判定は `ctx->picture->pps->constrained_intra_pred`。
  - gatherer の誤りの入力: ctx・edge が NULL、size が 4・8・16 以外、block が範囲外、plane が 1・2 以外、picture か frame か plane が NULL、frame の外にはみ出す run は、その部分（か全部）を available にしない。size 16 の blk は無視。
    sample の位置は frame の座標（macroblock の位置 × 大きさ + 位置）で、MBAFF は無い。
  - 8x8 の top-right の「後の block」の検査は header の通りに実装したが、4 つの 8x8 block では起きない（block 3 は Table 6-3 で外れ、block 2 の右上は先に復号した block 1）。この検査は試験で観測できない。
  - 4x4 と 8x8 の方向の mode は 1 つの実装（size を引数）で書いた。8-60・8-64 の 4x4 の式は 8x8 の式（8-103・8-107）に x = 0 か y = 0 で一致するので同じ式を使う。
  - 仕様の `>>` は算術 shift で、負の値は C では処理系定義なので、plane の b・c・値は `intra_shift_floor`（切り下げの割り算）で計算した（h264sw.h の算術の規則）。
  - 仕様の PDF の式は 8.3.1.1〜8.3.4.4 を pdftotext と、plane の式（8-126・8-127・8-148・8-149）は頁の画で確かめた。
- 観測できない物（実装はあるが試験で値を確かめられない）: 8x8 の p'[ -1, -1 ] の 8-82・8-83（top か left が無い時の corner。corner を使う mode は top・left・corner の 3 つを要るので、この時は使われない）。8x8 の「後の block」の検査（上）。
- 未実施: QEMU・実機の試験（この Phase は host の単体試験まで）。recon との結合（p019）は後の Phase。規約の全文の見直しはこの WS の最後の Phase。
- 注: 試験の中の `h264sw_mb_neighbours`・`h264sw_mb_locate` などは p008 の実装を使っている（run-unit.sh が全 module を link する）。
