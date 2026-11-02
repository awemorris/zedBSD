<!-- awesome-plan project=zedbsd record=ws344-p015 -->

# ws344-p015 VP8 の intra 予測（vp8-predict.c）

- Status: 実装済み・試験 PASS（担当 Sonnet q1960-i01、2026-11-02。cleared は Q1 が決める）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p001。
- 目的: 16x16・8x8 の DC・V・H・TM と 4x4 の 10 mode。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8-predict.h`（仕様）。
- RFC 6386 §12.2、§12.3（subblock の mode の code と説明）、§20.14（predict.c の DC の丸め）。

## 2. 作る物

- `vp8-predict.c`、`unit/test-vp8_predict.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- predict_block: 16 と 8 の各 mode を決めた edge で、期待は手で（V・H は写し、TM の clamp の両端、DC の 4 つの場合: 両方・上だけ・左だけ・無し（128）、丸め）。
- predict_subblock: 10 の mode を決めた edge（above[-1]〜above[7]、left[0]〜[3]）で、期待は §12.3 の各 mode の式を画素ごとに手で追った値（comment に式）。B_VE_PRED・B_HE_PRED は平滑化した値（単なる写しではない）、B_HE_PRED の最後の行は L[3] を 2 回、B_LD_PRED・B_VL_PRED は above-right を使う、B_DC_PRED は 8 つの和 + 4 >> 3。
- dst の stride が 4・16 以外でも正しい。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- subblock の mode は neighbour の有無を見ない（edge の値は呼び手が 127・129 で埋める）。
- 16x16 と 8x8 の DC だけが have_above・have_left を見る。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8_predict
sh plan/ws344/tests/run-unit.sh all
BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh
sh plan/ws344/tests/target-build.sh
git diff --check
```

## 7. 終わりの条件

- 上の command が全部 PASS（最後の行を自分で読んで確かめる）。`check-skeleton` は `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2` で header と道具が変わっていないこと。
- 試験が通った後、実装をわざと数通りに壊し（git の外の複写で。例: 境界の比較を 1 つずらす、符号を 1 つ逆にする、表の値を 1 つ変える）、単体試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block ごとに出典の comment がある。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）が無い。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <作った path>...`。push しない。rm しない。
- 結果の節（下）: 日付（host の `date`）、実行した command と最後の行、件数、未実施、自分で決めた点、仕様と phase.md・header の食い違い。ws.md のこの行を「実装済み・試験 PASS（担当、日付）」に（cleared は Q1 が決める）。

## 結果

2026-11-02 14:18（host の `date`）。

- 作った物: `userland/desktop/libwebp-compat/vp8-predict.c`、`plan/ws344/tests/unit/test-vp8_predict.c`。header・道具・他の module は変えていない。
- 実行した command と最後の行:
  - `sh plan/ws344/tests/run-unit.sh vp8_predict` → `test-vp8_predict: PASS (5281 checks)` / `run-unit: PASS`
  - `sh plan/ws344/tests/run-unit.sh all` → 13 試験すべて PASS、最後 `run-unit: PASS`
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`
  - `git diff --check` → 出力なし
- 試験の中身: 16・8 の V・H（pixel ごとに違う edge）、TM の clamp の両端と丁度 0・255、DC の 4 つの場合と丸めの両側（16・8 の各 mode、無視される edge は 255 などの別の値）、
  subblock の 10 mode を 1 組の edge（P = 50、A = 10 20 40 80 90 100 200 250、L = 30 60 120 240）で手で追った 16 画素（comment に式）、
  平坦な edge（0・127・129・255）で全 mode が平坦、stride 4・16・17・40 と block の stride 16・17・40 で隣の画素が不変、範囲の外の size・mode は何も書かない、
  malloc の大きさちょうどの edge・dst で 3000 回の subblock と 2000 回の block の乱数（§12.3 の式を画素ごとに写した reference と比較）。
- 変異試験（git の外の複写 `build/tmp/p015/mut/`、script `build/tmp/p015/mut.py`）: 35 通り（DC の shift・丸め・have_* の無視・128、TM の符号・clamp、V・H の写し、
  size の許可、avg2・avg3 の丸め、subblock の DC・TM・VE・HE の下端と上端・LD の最後・RD の index・VR・VL・HD・HU の各画素、mode の上限、stride）を入れ、全部 FAIL（killed）。見逃しは 0。
  source は変異の後も commit と同一（`cmp` で確認）。
- 自分で決めた点（header が黙っている所）:
  - 範囲の外の size（8・16 以外）と mode（block は DC・V・H・TM 以外、subblock は 10 以上）は何も書かずに返る（戻り値は void。呼び手が検査済みの前提）。
  - subblock の edge は 13 画素の 1 本の線（左の列の下から上、corner、above[0..7]）に並べ直して読む。B_HE_PRED の最上行は corner（above[-1]）を使う（§12.3 の L[-1]）。
- 仕様と header・phase.md の食い違い: なし。§12.3 の narrative と §20.14 の predict.c は subblock の 10 mode の全画素で一致することを手で確かめた
  （narrative の誤記 `svg2p` は avg2p。実装は avg2）。header の DC の shift の記述（3 + have_above + have_left、8 は 1 少ない）は実装と一致。
- 未実施: ASan・UBSan 以外の実行（QEMU・実機・fixture の全体の比較。依存する module が揃ってから）。
