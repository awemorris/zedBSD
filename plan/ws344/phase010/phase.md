<!-- awesome-plan project=zedbsd record=ws344-p010 -->

# ws344-p010 VP8 の表（vp8-tables.c）

- Status: 実装済み・試験 PASS（Sonnet、2026-11-02。cleared は Q1 が決める）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p001。
- 目的: tree・確率・係数の既定と更新・band・zigzag・category・量子化の表を RFC から入れる。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8-tables.h`（仕様、配列の形）。
- RFC 6386 §8.1・§11.2（kf_ymode_tree、uv_mode_tree、bmode_tree、kf_ymode_prob）、§9.3（mb_segment_tree）、§11.4（kf_uv_mode_prob）、§11.5（kf_bmode_prob）、§13.2（coeff_tree、Pcat1〜6、dct_cat の base）、§13.3（coeff_bands）、§13.4（coeff_update_probs）、§13.5（default_coeff_probs）、§14.1（dc_qlookup、ac_qlookup）、§20.16（zigzag）。

## 2. 作る物

- `vp8-tables.c`（全部の値）、`unit/test-vp8_tables.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- 全部の表を `rfc-vp8-tables.h`（run-unit.sh が RFC の text から vp8-tables-extract.py で作る `rfc_*` の配列）と全要素で比べる。cat_probs の行は rfc_pcat1〜6（0 で終わる）、残りは 0。coeff_bands の 17 番目は 0。
- 形の検査: 各 tree の葉が 0..値の数 - 1 をちょうど 1 回ずつ持ち、内部の entry は偶数で前に戻らない（全ての道が葉で終わる）。zigzag が 0〜15 の並べ替え。dc_q・ac_q が単調増加（dc_q 4〜157、ac_q 4〜284）。cat_base が 5・7・11・19・35・67、cat の extra の bit の数が 1・2・3・4・5・11。確率の表の値が 1〜255。
- 表の値は自分で RFC から入れる（vp8-tables-extract.py の出力を写して .c にしない。照合が同じ道になる）。text で崩れた所は RFC の該当の行を目で見て確かめ、結果の節に書く。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- kf_bmode_prob は [above][left]（§11.5 の文）。
- tree の葉は -値（B_DC_PRED・DCT_0 の葉は 0、§8.1 の `t[i] > 0` で葉と分かる）。coeff_tree の葉の値は vp8-tables.h の WEBPC_VP8_DCT_*（dct_eob 11）。
- 各表の block の直前に複数行の comment「Values referenced: RFC 6386 (November 2011) section <節> <名前>」。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8_tables
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

日付: 2026-11-02（host の `date`）。担当: Sonnet（q1949-i01）。

作った物: `userland/desktop/libwebp-compat/vp8-tables.c`（全部の表）、`plan/ws344/tests/unit/test-vp8_tables.c`。

実行した command と最後の行:

- `sh plan/ws344/tests/run-unit.sh vp8_tables` → `test-vp8_tables: PASS (37674 checks)` / `run-unit: PASS`。
- `sh plan/ws344/tests/run-unit.sh all` → test-bool 8376、test-common 32、test-helpers 26、test-vp8_tables 37674 checks、`run-unit: PASS`。
- `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`。
- `git diff --check` → 出力なし。

表の入れ方: 大きい表（kf_bmode_prob 900、coeff_default・coeff_update 各 1056、dc_q・ac_q 各 128）は RFC の text の該当の行を sed・awk で切り出して字下げだけ整え（python の extract とは別の経路）、小さい表（tree・確率・bands・zigzag・cat）は RFC の text を目で見て手で書いた。表の数（900・1056・1056・128・128）は件数で確かめた。

試験の中身: 全表を `rfc_*`（extractor の出力）と全要素で比較、cat_probs と rfc_pcat1〜6、coeff_bands の 17 番目が 0、tree の形（葉が値を 1 回ずつ、内部の entry が偶数で前向き、各 pair の親が 1 つ）、各 tree の全ての道の符号長を手の計算（pair から読んだ長さ）と比べ、Kraft の和が 1、手で作った bit 列の読み、乱数の bit 列 2000 本、確率が 1〜255、zigzag が置換、cat_base が 5・7・11・19・35・67 かつ extra bit の数 1・2・3・4・5・11 から導いた値と一致、dc_q・ac_q の範囲と順序、RFC の text から目で読んだ数値（kf_bmode_prob・default・update・量子化）。

mutation（git の外の複写 `build/tmp/p010-mut/m00`〜`m22`、23 通り: tree の葉・符号、各確率表の 1 値（先頭・末尾・中間）、bands の 17 番目と中間、zigzag の入れ替え、cat_probs の値・terminator、cat_base、dc_q・ac_q の端・中間）: 23 通りとも FAIL（survivor 0）。元の file は複写で変えていない。

自分で決めた点・仕様との食い違い:

- RFC 6386 §11.2 の bmode_tree の comment は B_LD_PRED を "111110"、B_VL_PRED を "1111110"、B_HD_PRED を "11111110"、B_HU_PRED を "11111111" と書くが、tree の pair からは "11110"・"111110"・"1111110"・"1111111"（1 bit 短い）。tree の entry が正しい（§20 の参照 decoder の b_mode_tree は同じ entry）。試験は pair から読んだ長さで確かめた。
- dc_q は RFC の表のまま同値が続く所がある（10, 10 など）ので「単調増加」でなく非減少（ac_q は厳密に増加）で確かめた。最大の刻みは dc 4、ac 6（239→245）。
- tree の葉は macro でなく数の直書き（macro は unsigned で int8_t への代入が符号の警告になるため）。各行の comment に名前を書いた。
- tree の entry 0 の葉（B_DC_PRED・DC_PRED・DCT_0・segment 0）は 0（`t[i] > 0` なら pair、そうでなければ葉）。coeff_tree は RFC の通り entry 0 が -11（dct_eob）、entry 2 が 0（DCT_0）。
- phase.md の「各 tree の葉の値」の記述と header の comment は矛盾なし。header の不足は無かった。

未実施: QEMU・実機（この Phase の範囲外）、fixture の全体の試験（後続の Phase）。

