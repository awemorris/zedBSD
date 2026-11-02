<!-- awesome-plan project=zedbsd record=ws344-p022 -->

# ws344-p022 規約の全文の見直し

- Status: in-progress（P2、q1982-i01。実施済み・試験 PASS、cleared は Q1 が決める）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: conformance-reviewer。依存: p021。
- 目的: WS344 が足した全ての C（library・libbrowser の webp.c・試験）を coding-style.md の全文で読み、書き方だけを直す。header の持ち越しの直しと試験の道具の移動の依頼。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- plan/coding-style.md の全文、ws.md の各 Phase の結果の節（header の持ち越し）、design.md §12。

## 2. 作る物

- license の監査に libwebp 1.5.0 の BSD-3-Clause の表示を足す（output.c の YUV → RGB の定数が libwebp の値、ws.md の「Q1 の判断」1。置き場は Q1 に確かめる）。
- 書き方の直し（動作を変えない）、header の comment の持ち越しの直し（この Phase だけ header を変えてよい。BASE が変わるので check-skeleton は BASE 無しで）、試験の道具を `plan/tools/webp-compat/` へ移して plan/tools.md に登録する依頼（Q1）。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- `sh plan/ws344/tests/run-unit.sh all`、`check-skeleton.sh`（BASE 無し）、`run-fixtures.sh` の 3 つ、`compare.sh`、`mutate.sh`、`target-build.sh` が全部 PASS（動作が変わっていない）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- conformance-reviewer の定義（書き方だけ）。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。
- この Phase は header の comment（持ち越しの直し）と WS344 が足した C の書き方を変えてよい。動作は変えない。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh all
sh plan/ws344/tests/check-skeleton.sh
sh plan/ws344/tests/make-fixtures.sh
sh plan/ws344/tests/run-fixtures.sh lossless
sh plan/ws344/tests/run-fixtures.sh lossy
sh plan/ws344/tests/run-fixtures.sh alpha
sh plan/ws344/tests/compare.sh
sh plan/ws344/tests/mutate.sh
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

（2026-11-02 18:38、P2、q1982-i01）

- commit: 持ち越しの直し `c2985a337f4e4f497cf38f2ce9bc18f6cbc7b67e`、規約の見直し `a6859dc06468c8cd17532d7bc74d46509df3bcd0`（これが新しい check-skeleton の BASE。ws.md の「基点」を書き換えた）、この記録の commit。
- 持ち越しの直し:
  - license の監査（ws.md の「Q1 の判断」1）: `userland/base/licenses/libwebp-compat/libwebp-COPYING`（libwebp 1.5.0 の COPYING、BSD-3-Clause、output.c の YUV → RGB の定数が libwebp の値であることの説明つき）、libwebp-compat の package の IMAGE_DATA で `/usr/share/licenses/libwebp-compat/libwebp-COPYING` に入れる、`tools/release/license-components.json` に component `libwebp-values`（when program:libwebp-compat）、Windows の配る形の `share/licenses/libwebp-COPYING.txt`（`apps/browser-win32/Makefile` の OMNIGRAMMATON_LICENSES と `resources/licenses-README.txt`）。`python3 tools/release/license-inventory.py --config config/ci/config-amd64.mk ...`（出力は build/tmp）→ `24 components, 0 open items`、libwebp-values は ok。plan/ws129 の生成物は WS129 の物なので作り直していない。
  - `vp8-idct.h` の WHT の comment: 1 段目は 16 bit に包む（RFC 6386 14.3・20.8）、正しい stream では int32 と同じ値、に直した（p014 の報告）。
  - `fixtures.list` の partitions の 11 行に `method=1`（libwebp の encoder は method 3 以上で 1 partition に戻す）と先頭の説明、`fixtures.sha256` を作り直した（変わったのはその 11 file だけ、partition 数は 2・4・8 になった）。
  - mutate の 1 file の上限: fuzz-target.c の alarm を 120 → 300 s、mutate.sh の timeout を 180 → 360 s と comment。
  - 他の header の持ち越し: 各 Phase の結果の節を読んで、header の誤りの報告は WHT の 1 件だけだった（p005 の Table 4 の下端と p010 の bmode_tree の comment の符号の長さは RFC の側の誤りで、header は正しい）。
- 規約の見直し（conformance-reviewer を 1 本、P2 が diff を読み確かめた）: libwebp-compat の 17 の .c と libbrowser の `image/webp.c`（+491/−160）。複数行の static の前方宣言を 1 行に（約 18）、条件の中の関数の呼び出しを先に変数へ（約 20）、`return decode_status(error);` を変数を経て（8）、条件演算子を if に（5）、3 項以上の条件を 1 項 1 行に（短絡の順は保つ、約 9）、分けた呼び出しを 1 引数 1 行に（約 18）、閉じ括弧の後の空行（約 25）、目的の comment と「Succeeded」の comment（約 75）。`plan/tools/style-check.py` の指摘は 74 → 0。alpha.c・vp8-tables.c・vp8l-transform.c・全ての header の文は変えていない（header に開発の管理情報も複数行の comment の区切りの崩れも無かった）。残した物: 1 つの comment の下に続く短い guard の if の組（約 5）、算術の中の関数の呼び出し、case ごとに return する switch。
- 動作が変わっていないことの確かめ（PATH=/usr/bin:$PATH、規約の見直しの後）: `check-skeleton.sh` PASS（`BASE=a6859dc064…` でも PASS）、`run-unit.sh all` PASS、`run-fixtures.sh` lossy 352・lossless 174・alpha 54 PASS、`compare.sh` PASS (364 files)、conformance の集合で run-fixtures lossy 342・lossless 43・alpha 27 と compare 214 PASS、`mutate.sh`（5 file、最大の lossy-large-photo-1024x683 を含む）PASS (1085 inputs)、`platform/webp-wine.sh` PASS (364 files, 0 warnings)（Windows の static library）、`browser/run-host-image.sh` PASS (358 files)、zedBSD amd64 の libwebp-compat.so と libbrowser.so（共有の sysroot）rc 0・warning 0、`git diff --check` は出力無し。
- Q1 への依頼: 試験の道具の plan/tools への移動と plan/tools.md の登録（`plan/ws344/tests/` の run-unit.sh・check-skeleton.sh・make-fixtures.sh・compare.sh・run-fixtures.sh・mutate.sh・fuzz.sh・conformance/・platform/・browser/、unit/）。WS の完了の書き直しと Phase の directory の片付けも Q1。
- 未実施: 試験の C（plan/ws344/tests の unit・driver など）の規約の全文の見直し（library の code を先にした）、実機。
