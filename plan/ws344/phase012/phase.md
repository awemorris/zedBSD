<!-- awesome-plan project=zedbsd record=ws344-p012 -->

# ws344-p012 VP8 の macroblock header（vp8-modes.c）

- Status: in-progress（実装済み・試験 PASS、Sonnet q1952-i01、2026-11-02。cleared は Q1）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p009, p010（vp8-header.h の struct だけを使う。test が struct を手で埋める）。
- 目的: segment・skip・luma の mode・16 の subblock の mode（context）・chroma の mode。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8-modes.h`（仕様）、`vp8-header.h`、`vp8-tables.h`。
- RFC 6386 §10、§11.1〜§11.5、§19.3。

## 2. 作る物

- `vp8-modes.c`、`unit/test-vp8_modes.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- boolwriter（vp8-tables の tree・確率を使って書く）で macroblock の header の列を組む: segment（map の更新の有無）、skip（有無）、各 ymode、B_PRED の 16 の mode（context の A と L を test が前の書いた mode から決めて kf_bmode_prob[A][L] で書く）、uvmode。読んだ結果と above・left の context の更新（B_PRED でない時は implied の mode）。frame の端の context は B_DC_PRED。
- 3x2 macroblock の並びで、上と左の macroblock から context が正しく来ること（test が context の配列を持ち回る）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- kf_bmode_prob の添字は [above][left]。
- B_PRED でない macroblock の subblock の mode（後の macroblock の context）: DC_PRED → B_DC_PRED、V_PRED → B_VE_PRED、H_PRED → B_HE_PRED、TM_PRED → B_TM_PRED（§11.3）。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8_modes
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

2026-11-02（host の `date`）、Sonnet q1952-i01。

- 作った物: `userland/desktop/libwebp-compat/vp8-modes.c`、`plan/ws344/tests/unit/test-vp8_modes.c`。header・道具は変えていない。
- 実行した command と最後の行:
  - `sh plan/ws344/tests/run-unit.sh vp8_modes` -> `test-vp8_modes: PASS (627570 checks)` / `run-unit: PASS`
  - `sh plan/ws344/tests/run-unit.sh all` -> 8 試験（bool 8376、common 32、helpers 26、lbits 205384、prefix 1430293、riff 42324、vp8_modes 627570、vp8_tables 37674）全部 PASS、`run-unit: PASS`
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` -> `check-skeleton: PASS`
  - `sh plan/ws344/tests/target-build.sh` -> `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`
  - `git diff --check` -> 出力なし（終了 0）
- 試験の内容: boolwriter で tree・確率（vp8-tables）を使って macroblock header を書き、subblock の context は test が全 subblock の mode の grid（frame の外は border）から自分で求める。単独の macroblock（ymode 0..4 × uvmode 0..3、B_PRED の 10 mode、segment・skip の全組合せ、非対称の above/left の context）、3x2 の frame（B_PRED の割合 0〜100%、border は B_DC_PRED）、乱数の frame 1000 本（乱数の確率・border）。ended（空・全ての切り方・全体）、乱数 byte の 2000 partition × 6 macroblock（範囲と context の更新）、segment・skip を読まない header で 0 になること、範囲外の context（10・11・200・255）。
- 自分で決めた点: header が黙っている「呼び出し側の context が 0..9 の外」は B_DC_PRED として扱う（確率表を範囲外に引かないため）。segment は header の `update_map` だけで読むかを決める（read_header が enabled でない時 update_map を 0 にする約束）。
- mutation（git の外の複写 `build/tmp/p012-mut/orig.c` から戻し、`cmp` で戻りを確認）: 23 通り。A と L の入れ替え、上・左の添字のずれ、top 行・left 列の境界、V/H/TM の implied の取り違え、context の guard の境界と無効化、segment・skip・uv・ymode の確率の取り違え、skip・segment の条件の反転、context の更新の添字、segment・skip の初期値。22 通りが FAIL（ASan の範囲外 index の検出を含む）。生き残り 1: `modes->ymode == B_PRED` を `>= B_PRED` にする変更。B_PRED が ymode の最大値（4）で kf_ymode_tree に 5 以上の leaf が無いので同値の変更で、見逃しではない。
- 未実施: QEMU・実機・fixture の全体の試験（この Phase の範囲外）。`mutate.sh`・fuzz は未実行（fixture が無い）。
- 仕様・header との食い違い: なし。
