<!-- awesome-plan project=zedbsd record=ws344-p008 -->

# ws344-p008 ALPH（alpha.c）

- Status: 実装済み・試験 PASS（Sonnet q1964-i01、2026-11-02。cleared は Q1）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p002, p007（fixture-check は riff.c を使う）。
- 目的: ALPH の header の byte、raw と可逆の値、逆 filter。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `alpha.h`（仕様）、`vp8l.h`。
- RFC 9649 §2.7.1.2（Figure 10・11、filter の式と端の規則、圧縮の方法）。

## 2. 作る物

- `alpha.c`、`unit/test-alpha.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- unfilter: 4 つの方法を 3x3・1x4・4x1 で手計算（端の規則: (0,0) は 0、方法 1・3 の左の列は上から、方法 2・3 の上の行は左から、gradient の clip）、mod 256。
- decode: raw（C 0）の大きさちょうど・1 byte 短い（EINVAL）・長い（残りは無視）、C 1 の可逆の stream（lbitwriter で組む。green が alpha、他の channel は無視）、C 2・3 は EINVAL、Rsv と P の bit が何でも同じ結果、F の bit の位置（header の byte の bit 2〜3）。
- fixture: `run-fixtures.sh alpha` が全部 PASS（dwebp の -pam の alpha と一致）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- header の byte は上位から Rsv(2)・P(2)・F(2)・C(2)。
- 予測は復元済みの値から（in place、scan 順）。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh alpha
sh plan/ws344/tests/run-unit.sh all
sh plan/ws344/tests/make-fixtures.sh
sh plan/ws344/tests/run-fixtures.sh alpha
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

- 日付: 2026-11-02（Sonnet 5.5、q1964-i01）。作った物: `userland/desktop/libwebp-compat/alpha.c`、`plan/ws344/tests/unit/test-alpha.c`。header・道具・他の module は変えていない。
- 実行した command と最後の行（PATH=/usr/bin:$PATH）:
  - `sh plan/ws344/tests/run-unit.sh alpha` → `test-alpha: PASS (2293 checks)` / `run-unit: PASS`
  - `sh plan/ws344/tests/run-unit.sh all` → 全 19 test PASS（test-alpha 2288 checks の時点）、`run-unit: PASS`
  - `sh plan/ws344/tests/make-fixtures.sh` → `make-fixtures: PASS (364 files, 350 decoded by dwebp, hashes same; ...)`、`sh plan/ws344/tests/run-fixtures.sh alpha` → `run-fixtures: PASS (alpha, 54 checks)`
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`
  - `git diff --check` 問題なし。
- 試験の内容: 逆 filter を手計算（3x3 で方法 0〜3、gradient の下と上の clip と mod 256、1x4・4x1・1x1 の端の規則、不明な方法・幅か高さ 0・NULL は何もしない）、decode の raw（ちょうど・1 byte 短い・長い・1x1・size 0 と 1）、header byte（Rsv と P の 16 通りと F 4 通りが同じ結果、F が bit 2〜3、C 2・3 は他の欄が何でも EINVAL）、引数の誤り（NULL、辺 0、16384、巨大値）、C 1 の可逆の stream（lbitwriter と平坦な符号。大きさ 6 通り × F 4 通り、green が alpha で他の channel は乱数）、色 index の transform（2 色、幅 11 で 8 個を 1 pixel に束ねる）、途中で切れた stream が ENOBUFS、test 側の順 filter（RFC の式から別に書いた）との往復 200 回（raw と可逆）、乱数の payload 600 回（正確な大きさの複写で ASan が範囲外を見る）。
- わざと壊す確かめ（23 通り: raw の大きさの比較 2 通り、方法 1・2 の取り違え、gradient の符号と clip、左の列・上の行の端の規則、F の shift と mask、C の mask と値、size の下限、green の shift、(0,0) の予測、辺の上限、stream の始点と大きさ 2 通り、加算の符号、方法の上限、raw の始点、高さの検査、座標の取り違え）: 最初に 2 つの見逃し（size の下限 `size < 1`＝C 1 で header byte だけの時、辺の上限 +1）があり、試験を足して全て FAIL になった。元に戻したことは `cmp` で確かめた。script は `build/tmp/mut-p008/run.py`（git の外）。
- 自分で決めた点（header が黙っている所）:
  - payload か alpha が NULL、width か height が 0 または 16383 超（`WEBPC_VP8_SIDE_MAX`）は EINVAL（確保の前）。
  - 検査の順は 引数 → size 2 未満（EINVAL）→ C の読み。size 1 は C が何でも EINVAL（C 1 で空の stream の ENOBUFS にならない）。
  - C 1 は width * height * 4 byte を malloc し（失敗は ENOMEM）、`webpc_vp8l_decode_stream` の誤りはそのまま通し、成功後に green（bit 8〜15）を取って free。
  - `webpc_alpha_unfilter` は alpha が NULL・方法が 0 または 3 超・width か height が 0 の時は何もしない。
  - 逆 filter は 1 pixel ごとに predict_alpha（静的）を呼ぶ構造にした（端の規則を 1 か所に集める）。
- 仕様と phase.md・header の食い違い: 見つからなかった。alpha.h の 1 箇所（decode の comment 内の行の折り返しが長い）は書式だけ。
- 未実施: 実機・QEMU の試験は無い（host の単体試験と fixture のみ）。mutate.sh・fuzz は割り当て外のため未実施。
