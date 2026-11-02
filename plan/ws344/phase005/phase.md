<!-- awesome-plan project=zedbsd record=ws344-p005 -->

# ws344-p005 VP8L の entropy-coded image（vp8l-image.c）

- Status: planned
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p004。
- 目的: color cache、meta prefix code と entropy image、literal・LZ77・cache の画素の decode。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8l-image.h`（仕様）、`vp8l-codes.h`。
- RFC 9649 §3.6（Table 4、Figure 20）、§3.6.2.3、§3.7.2.2、§3.7.2.2.1、§3.7.2.2.2、§3.7.2.3、§3.8.3。

## 2. 作る物

- `vp8l-image.c`、`unit/test-vp8l_image.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- lz77_value: prefix 0〜3 → 1〜4、4 → 5〜6（extra 1 bit）、7 → 13〜16、23 → 3072〜4096、38・39 の両端（Table 4 の行）。
- plane_distance: Figure 20 の code 1（(0,1) → width）、2（(1,0) → 1）、3、4、120（(8,7)）を幅 1・10・1000 で。(xi + yi * width) < 1 の時 1。code 121 → 1、1000 → 880。Figure 20 の 120 組の表は data として block の出典の comment を付け、test は表の形（同じ組が 2 度無い、(0,0) が無い、xi の範囲 −7〜8・yi の範囲 0〜7）を確かめる。
- cache_index: 手で計算した 3 つの値（0、0xffffffff、0x12345678 を bits 1・4・11 で）。
- 画像（lbitwriter で組む）: literal だけの 1 group、cache を使う画素、LZ77 の重なる copy（distance 1・length 10）、distance が最初の画素より前は EINVAL、length が最後を越えるのは EINVAL、meta prefix（argb_role 1、prefix_bits 2、block ごとに違う group、group の最大の番号 3 で 0 と 3 だけ使う）、argb_role 0 で meta の bit を読まない、cache_bits 0（1 の後に 4 bit の 0）と 12 は EINVAL、limit を小さくして ENOMEM、途中で切れた stream は ENOBUFS、乱数の stream 1000 個（ASan・UBSan）。期待の画素は test が stream を組む時に決める。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- copy の後の symbol は copy の後の画素の block の group で読む。copy した画素も 1 つずつ cache に入れる（literal と同じ順）。
- 距離の Figure 20 は今の画像の幅（subresolution の画像はその幅）で計算する。
- 使わない group を読み捨てる時の scratch の pool は別の pool（同じ limit）で、group ごとに空にする。残す group の pool の limit には数えない。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8l_image
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

（未着手）
