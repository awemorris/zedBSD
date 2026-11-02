<!-- awesome-plan project=zedbsd record=ws344-p009 -->

# ws344-p009 bool decoder（bool.c）

- Status: 実装済み・試験 PASS（担当 Sonnet q1948-i01、2026-11-02。cleared は Q1 が決める）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p001。
- 目的: VP8 の boolean entropy decoder、literal、header の符号付きの値、tree。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `bool.h`（仕様）。
- RFC 6386 §7（§7.3 の decoder の code と説明）、§8・§8.1（treed_read、tree の表の書き方）、§9.3・§19.2（符号付きの値: 大きさの後に符号）。

## 2. 作る物

- `bool.c`、`unit/test-bool.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- boolwriter で書いた乱数の bool（確率 0〜255、両端を含む）を 20000 個読み戻す。literal（1〜24 bit）、signed（正・負・0）。
- tree: test の中の小さな tree（葉 0 を含む）と、start 2 から読む（EOB の無い木の形）。
- ended: ちょうど書いた分を読んだ後は 0。読み続けて窓が data の終わりを越えた bool で 1 になり、その後も 1。size 0 は init で 1。size 1 の data（2 byte 目は 0 とみなす）は最初の bool で 0。境界: `shifts + 8 > 8 * size` になる最初の bool で立つことを、shift の数を test で数えて確かめる。
- data の外を読まない（ちょうどの大きさの malloc の複写、ASan）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- `read_signed_literal`（§7.3、2 の補数）は使わない。header の符号付きは大きさ L(n) の後に符号 L(1)。
- value に 2 byte より多く先読みしてもよいが、結果と ended は §7.3 の decoder と同じに。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh bool
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

2026-11-02（host の `date`）。担当 Sonnet（q1948-i01）。

- 作った物: `userland/desktop/libwebp-compat/bool.c`（RFC 6386 §7.3 の read_bool と同じ算術。value は 16 bit の窓、byte は data の外を 0 と読む）、`plan/ws344/tests/unit/test-bool.c`。
- 実行した command と最後の行:
  - `sh plan/ws344/tests/run-unit.sh bool` → `test-bool: PASS (8376 checks)`、`run-unit: PASS`
  - `sh plan/ws344/tests/run-unit.sh all` → `test-bool: PASS (8376 checks)`、`test-common: PASS (32 checks)`、`test-helpers: PASS (26 checks)`、`run-unit: PASS`
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`
  - `git diff --check` → 出力なし。
- 試験の内容: 手計算の decode（窓 0x8000・prob 128 で 1,0,0,...）、ランダムの bool 20000 個 x 3 組（任意の確率、確率 0・255 だけ、確率に沿った値）の往復、literal 1〜24 bit（両端の値）と幅 25 以上の切り詰め、signed（正・負・0、1〜24 bit）、tree（葉 0 を含む木、root に葉の無い木、start 2・4）、葉に届かない木の打ち切り、ended の手計算（size 0・1・2）、ended の model（size 0〜40 の 3 種の data で、返った bool から range と shift 数を試験の側で数え、全 bool の後で ended が一致）。全ての partition は ちょうどの大きさの malloc の複写から読む（ASan）。
- 変異（git の外の複写で壊し、FAIL を確かめ、`cmp` で元に戻った事を確かめた、25 通り）: ended の境界（`>`→`>=`、+8→+7・+9）、符号の逆、split の +1、比較の `>=`→`>`、正規化の閾値、bit_count の 8→7、init の 2 byte 目・next、size 0 の ended、literal の bit の順・幅の切り詰め、tree の確率の添字・葉の符号・枝の取り違え・start、tree の打ち切りの上限、split の shift、負の符号、確率の mask、data の終わりの判定の境界。全て FAIL。
- 見逃し（等価）: ended を「sticky にしない」変異（`ended = (window_end > data_bits)`）は PASS のまま。shifts は単調に増えるので、size が 1 以上なら条件が成り立った後も成り立ち続け、結果が同じ（等価な変異）。size 0 の ended は init が立て、read の度に条件 shifts + 8 > 0 が再び立てる。
- 自分で決めた点（header が黙っている所）:
  - `webpc_bool_read` の prob は `prob & 255` に切る（header は 0..255 とする。範囲外でも split が範囲内に収まる）。
  - `webpc_bool_literal` と `webpc_bool_signed` の幅が 24 を超えたら 24 に切る（header は 0..24 と 1..24）。signed の幅 0 は magnitude 0 の後に符号を読む。
  - `webpc_bool_tree` は start の下位 1 bit を落として偶数にし（header は偶数）、上限 64 step で打ち切り、葉に届かなければ 0 を返す（header は「well formed」を前提。定数の木は 64 step 以内に葉に着く）。
  - ended は read の始めに判定する（header の「shifts + 8 > 8 * size で始まる bool」）。literal の各 bit は read を通るので同じ。init は size 0 でだけ立てる。
  - value は 16 bit の窓のまま unsigned の算術。壊れた先頭 2 byte（range 255 に対し value が大きい入力）では窓の外の bit が上位に溜まるが、unsigned の wrap で未定義の動作は無い。
- 仕様と phase.md・header の食い違い: 無し。
- 未実施: 他 module との結合（vp8-header 以降の Phase）、fixture の全体の試験（この Phase の範囲外）。
- 削除してほしい path: `/home/awe/zedBSD-worktrees/s-ws344-p009/build/tmp/p009-mut`（変異の script と原本の複写）、`build/tmp/ws344-unit`（run-unit の出力）。
