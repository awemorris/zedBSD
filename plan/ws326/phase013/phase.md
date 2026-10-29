<!-- awesome-plan project=zedbsd record=ws326-p013 -->

# ws326-p013 変換と逆量子化

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-30。cleared は Q1 が受け入れを見て決める）。
- 依存: p003（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-transform.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.5.6〜8.5.14（式 8-313〜8-326、8-330〜8-354、8.5.13 の 8x8）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-transform.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-transform.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- levelscale: flat 16 の list で LevelScale4x4(0,0,0) = 160、(0,1,1) = 256、(0,0,1) = 208 など手で計算した値（v × 16）。8x8 も 6 位置の種類ごとに 1 つ。
- 4x4: DC だけ（c00 = k）の逆変換が平らな block（( k' + 32 ) >> 6 の値）、1 つの AC の係数の手で計算した 16 値、qP 0・23・24・51 の scale の丸めの分かれ目。
- 8x8: DC だけと、手で計算した 1 係数の例。
- Intra16x16 DC: 16 の DC の Hadamard と scale（qP 35 と 36 の分かれ目）。chroma DC 2x2: 手で計算した 4 値。
- add_residual: 0・255 での clip。巨大な係数（2^15-1 の全部）で UBSan が黙ること（int32 の中で収まる、溢れない計算）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh transform` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
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
- 作った物: `userland/desktop/libmedia/h264sw-transform.c`（全関数）、単体試験 `plan/ws326/tests/unit/test-transform.c`。header は変えていない。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh transform` → `test-transform: PASS (33158 checks)`、`run-unit: PASS`（ASan・UBSan の報告 0）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → bits 169、bitwriter 14、cabac 195654、cabac_init 4159、mb 1938、nal 95、ps 41887、tables 846、transform 33158 の各 PASS、`run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の warning 0（`grep -ci warning` が 0）。
  - `git diff --check` → 問題なし。
  - 試験の感度の確認（一時的に実装を壊して FAIL するかを見た。壊した物は戻した）: Hadamard の符号、shift の補数計算、8x8 の normAdjust の場合分けは FAIL。8x8 の e3 の符号は最初の試験では PASS したため、8x8 の係数 1 つずつ（水平・垂直 各 8 通り）の試験を足し、その後 FAIL することを確認した。
- 期待値: 4x4・8x8 の逆変換、Hadamard、chroma DC は式から手で計算した値（試験の comment に手順）。scale の式は、floor の除算で書いた素直な写しと qP 0..51 × 水平の level で比べる。手計算の値は別に python で式を解いて照合した（期待値の出所は手計算）。
- header が決めていない点で担当が決めた物（Q1 の確認用）:
  - header の関数は全て void で、返す誤りが無い。誤りの入力は、NULL の引数なら何もしない、範囲外の値なら端に丸める、で扱う。
  - qP は 0..51 に丸める（8 bit の QP'Y・QP'C の範囲。範囲外は shift の回数が未定義になるため）。4:2:2 の qPDC は対象外。
  - 入力の level・係数は、各関数の最初に -32768..32767 に丸める（header の「clause が縛る値」）。scale の引数（LevelScale）は任意の int32_t を受け、int64_t で積を取る。
  - 右 shift は、負の値で処理系定義を避けるため、補数を使った floor の shift で書く（算術 shift と同じ結果）。
  - `h264sw_unscan4x4`・`h264sw_unscan8x8` は入力と出力が同じ配列でもよい（一度複写する）。
  - `h264sw_add_residual` の size が 4・8 以外なら出力を変えない。residual は size 列の raster（行 stride = size）。和は int64_t で取り、巨大な residual でも溢れない。
  - 8x8 の `h264sw_scale8x8` に dc_done は無い（header どおり）。
- 未実施: QEMU・実機の試験（この Phase の範囲外）。規約の全文の見直しの Phase（WS の最後）。
