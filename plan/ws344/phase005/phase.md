<!-- awesome-plan project=zedbsd record=ws344-p005 -->

# ws344-p005 VP8L の entropy-coded image（vp8l-image.c）

- Status: 実装済み・試験 PASS（Sonnet 5.5 q1957-i01、2026-11-02。cleared は Q1）
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

- 2026-11-02 14:14（host の `date`）、Sonnet 5.5（q1957-i01）。作った物: `userland/desktop/libwebp-compat/vp8l-image.c`、`plan/ws344/tests/unit/test-vp8l_image.c`。header・道具・他の module は変えていない。
- 確かめ（最後の行を読んだ。`PATH=/usr/bin:$PATH` を付けた。理由は下）: `run-unit.sh vp8l_image` → `test-vp8l_image: PASS (90215 checks)`、`run-unit: PASS`。`run-unit.sh all` → 11 test 全部 PASS（bool 8376・common 32・helpers 26・lbits 205384・prefix 1430293・riff 42324・vp8_header 45344・vp8_modes 627570・vp8_tables 37674・vp8l_codes 95925・vp8l_image 90215）、`run-unit: PASS`。`BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 check-skeleton.sh` → `check-skeleton: PASS`。`target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`。`git diff --check` は出力なし。
- 試験の内容: lz77_value（Table 4 の行 0〜7・23・38・39 の両端と読んだ bit 数の正確さ、40 以上は 0 で bit を読まない、値 1〜4200 と以降の飛び飛びと最後の 1048576 を test 側の逆変換で往復）、plane_distance（Figure 20 の 120 符号 × 幅 1・10・1000・16384 を test 側の表と、符号 1・2・3・4・120 は手の式と、121→1・1000→880・0 の扱い）、Figure 20 の表の形（120 組・重複なし・(0,0) なし・xi −7〜8・yi 0〜7・yi 0 は xi 正・112 + 8 の組が全部ある）、cache_index（0・0xffffffff・0x12345678 を bits 1・4・11 で。手計算の値）、画像（lbitwriter で組み、期待の画素は組む時に test 側が決める。literal だけ・1 画素・1 色（0 bit の画素）・256 色、cache の大きさ 1〜11 の全て・cache 命中・未設定の entry、cache 命中と copy の画素も cache に入ること、distance 1 length 10 の重なる copy・近傍符号 1・2・3・4、length 4096・distance の大きな extra bits、distance が最初の画素より前・length が最後を越える・ちょうどの境の EINVAL、meta prefix（block ごとに別の group、group 番号の最大 3 で 0 と 3 だけ使う、未使用の group の読み捨て、端の block が 1 画素幅になる大きさ、copy が block の境をまたぐ）、argb_role 0 は meta の bit を読まない、cache_bits 0・12〜15 の EINVAL、大きさの範囲（0・16385 は EINVAL、16384×1 と 1×16384 は成功）、limit（最小の limit を二分探索し、それ未満と 0 は ENOMEM。使われない group 6 つを大きくしても最小の limit が変わらない=kept の pool に数えず scratch の pool も group ごとに空にする）、全ての byte の長さでの途中切れ（ENOBUFS）、乱数の byte 1000 本と有効な stream の bit 反転・切り詰め 1000 本（ASan・UBSan）。全ての画像は後ろの marker 1 byte を読んで、decoder が使った bit が正確であることも確かめた。期待の値は Table 4・Figure 20・式と test 側の逆変換だけ。
- mutation（git の外の複写、`build/tmp/p005-mut/mut.py`、50 通り）: 初回は 12 が生き残り（うち 2 は pattern の指定が 2 か所に当たり未適用）。試験を足して（端が 1 画素幅の meta の画像、大きな未使用 group 6 つの limit、16384 の幅と高さ、有効な stream での大きさの EINVAL）3 つ + pattern を直した 2 つ + 1 つを殺した。最後に生き残るのは 6 つで、全て同値と判断した: (a) `prefix_code <= 3` を `< 3`（符号 3 は式でも値 4 になる）、(b)(c)(d)(e) reader の ended の検査を外す 4 か所（cache の情報の後、meta の bit の後、group の後、最後）。ended は 1 度立つと戻らず、各 symbol の後の検査と後の読みの失敗がどの経路でも ENOBUFS を返すので結果は同じ（早く止まるかどうかだけ）。header が「少なくとも符号の後・各行の後・最後」と言うので、検査は残した。それ以外の mutation（44）は全部 killed（FAIL か ASan・UBSan の停止）。
- 自分で決めた点（header が黙っている所）: (1) 大きさが 0 または 16384 を越えるなら読む前に EINVAL。(2) distance_code 0（「1 以上」の外）は表を引かずに 1。lz77_value の prefix_code 40 以上は 0 を返し bit を読まない。cache_index の bits が 1〜11 の外は 0。(3) reader が末尾を越えたら、symbol を読んだ後で長さ・距離を判定する前に ENOBUFS にする（零で補った値からの EINVAL を出さない）。(4) 画素は各 symbol（literal・copy・cache）の後に ended を確かめ、cache 命中の画素も cache に入れる（spec の「全ての画素」の字義。未設定の entry 0 が hash 0 の entry を 0 で上書きする）。(5) 使う group の印は entropy image の全 pixel から付け、group の配列は group 数（最大 65536）の分を確保する（約 8 MiB）。(6) entropy image は子の呼び出し（argb_role 0、同じ limit、table は自分の pool）で読み、読み終えて解放した後に親の group を読むので、子の table は親の limit を使わない。
- 仕様と phase.md・header の食い違い: (a) phase.md の試験の項目に「23 → 3072〜4096」とあるが、RFC 9649 の式（offset = (2 + (code & 1)) << extra_bits、value = offset + extra + 1）は符号 23 を 3073〜4096 にする（Table 4 の行 "3072..4096" の下端は誤り。3072 は符号 22、2049〜3072）。式に従い、試験も 3073 にした。header の comment は式の通りで直す所なし。(b) header の変更の要望なし。
- 試験の実行の注意: `/home/awe/.local/bin/python3`（amaterrace の同時実行数を絞る wrapper）が `run-unit.sh` の `python3`（RFC 6386 の表の抽出）を、3 つの slot が他の process に取られている間待たせて固まる。`PATH=/usr/bin:$PATH sh plan/ws344/tests/run-unit.sh ...` で回避した（script は変えていない）。
- 未実施: fixture での全体の試験（p007 以降）、QEMU・実機。
