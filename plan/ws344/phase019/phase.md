<!-- awesome-plan project=zedbsd record=ws344-p019 -->

# ws344-p019 interface（decode.c）

- Status: planned
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Opus を推す（phase-runner）。依存: p002, p008, p017, p018。
- 目的: `<compat/webp/decode.h>`・`types.h` の全呼び出し、buffer の検査と確保、状態の写像。libwebp と報告が一致。
- 基点の SHA（header と道具の凍結）: `@BASE@`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `include/libc/compat/webp/decode.h`・`types.h`（仕様）、`riff.h`・`vp8l.h`・`vp8.h`・`alpha.h`・`output.h`。
- design.md §3（互換の interface、状態の規則）。

## 2. 作る物

- `decode.c`、`unit/test-decode.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- test-decode.c: 引数の誤り（NULL、版の違い、colorspace の範囲の外、外部の buffer の小ささ・stride の短さ・負の stride・NULL の plane）、範囲の外（4444・565・crop・scale・可逆の YUV・animation）、状態の写像（header の NOT_ENOUGH_DATA → WebPDecode では BITSTREAM_ERROR）、WebPMalloc・WebPFree、WebPFreeDecBuffer（外部の memory を解放しない、private_memory を消す）。
- `sh plan/ws344/tests/compare.sh` が全 fixture（364）で PASS（libwebp の報告と行ごとに一致）。
- `sh plan/ws344/tests/mutate.sh` が PASS、`sh plan/ws344/tests/fuzz.sh 600` が PASS（fuzz-target は 2^22 画素を越える入力を decode しない）。mutate の 1 file の時間（最大の fixture）を測って結果に書く。
- 大きな 0 bit の可逆の画像（5 つの code が全部 1 symbol、16384x16384、数十 byte）を test-decode.c で 1 回 decode し、終わること・時間を記録する（run-unit の 600 s の中で）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- 簡単な decode は WebPGetInfo の後に大きさを書いてから decode する（失敗でも大きさが書かれうる、libwebp と同じ）。
- flip は行を下から書き、出力の stride は正。YUVA の alpha は alpha の無い画像で 255。
- ALPH の decode の失敗は画像全体の失敗。features の has_alpha は riff の規則。
- 1 つの WebPDecode の中で確保した中間の物は全部解放する。
- 該当の block の comment に「Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c, src/dec/buffer_dec.c (BSD-3-Clause), the statuses and buffer checks of the interface」（ws.md の「Q1 の判断」1）。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。
- この Phase は他の module の .c の誤りを見つけたら直してよい（その module の単体試験を足し、結果の節に書く）。header は変えない（不備は Q1 に返す）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh decode
sh plan/ws344/tests/run-unit.sh all
sh plan/ws344/tests/make-fixtures.sh
sh plan/ws344/tests/compare.sh
sh plan/ws344/tests/mutate.sh
sh plan/ws344/tests/fuzz.sh 600
BASE=@BASE@ sh plan/ws344/tests/check-skeleton.sh
sh plan/ws344/tests/target-build.sh
git diff --check
```

## 7. 終わりの条件

- 上の command が全部 PASS（最後の行を自分で読んで確かめる）。`check-skeleton` は `BASE=@BASE@` で header と道具が変わっていないこと。
- 試験が通った後、実装をわざと数通りに壊し（git の外の複写で。例: 境界の比較を 1 つずらす、符号を 1 つ逆にする、表の値を 1 つ変える）、単体試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block ごとに出典の comment がある。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）が無い。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <作った path>...`。push しない。rm しない。
- 結果の節（下）: 日付（host の `date`）、実行した command と最後の行、件数、未実施、自分で決めた点、仕様と phase.md・header の食い違い。ws.md のこの行を「実装済み・試験 PASS（担当、日付）」に（cleared は Q1 が決める）。

## 結果

（未着手）
