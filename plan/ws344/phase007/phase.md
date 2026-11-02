<!-- awesome-plan project=zedbsd record=ws344-p007 -->

# ws344-p007 VP8L の decoder（vp8l.c）

- Status: 実装済み・試験 PASS（Sonnet q1962-i01、2026-11-02。cleared は Q1）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p002, p005, p006（fixture-check は riff.c を使う）。
- 目的: header、transform の読み、ARGB 画像、逆変換を逆順で。ALPH の header の無い stream。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8l.h`（仕様）、`vp8l-image.h`、`vp8l-transform.h`。
- RFC 9649 §3.4、§3.5（transform の読み、各 1 回）、§3.5.4（table の差分の和）、§3.8.1、§3.8.2。

## 2. 作る物

- `vp8l.c`、`unit/test-vp8l.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- read_header: signature、14 bit の大きさ + 1、alpha bit、version 1〜7 は EINVAL、5 byte 未満は ENOBUFS。
- lbitwriter で組んだ stream: transform 無し、subtract green だけ、predictor だけ、color だけ、indexing だけ（色 2・3・4・5・16・17・256 で width_bits 3・2・2・1・1・0・0）、indexing の後の predictor（縮んだ幅で）、4 つ全部、同じ transform 2 回は EINVAL。table の差分の和（mod 256）。decode_stream（header 無し）。途中で切れた stream は ENOBUFS、失敗で何も残らない（ASan の leak の検査）。
- fixture: `sh plan/ws344/tests/make-fixtures.sh` の後、`sh plan/ws344/tests/run-fixtures.sh lossless` が全部 PASS（174 件、dwebp の -pam と全画素一致）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- 逆変換は読んだ順の逆。各変換は読んだ時の幅で。主の画像は最後の幅で decode し、出力は元の幅（indexing の展開で戻る）。
- subresolution の画像は argb_role 0（meta prefix 無し、transform 無し）。
- table の limit は WEBPC_VP8L_TABLE_ENTRIES_MAX。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8l
sh plan/ws344/tests/run-unit.sh all
sh plan/ws344/tests/make-fixtures.sh
sh plan/ws344/tests/run-fixtures.sh lossless
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

- 日付: 2026-11-02（Sonnet 5.5、q1962-i01）。作った物: `userland/desktop/libwebp-compat/vp8l.c`、`plan/ws344/tests/unit/test-vp8l.c`。header・道具は変えていない。
- 実行した command と最後の行（PATH=/usr/bin:$PATH）:
  - `sh plan/ws344/tests/run-unit.sh vp8l` → `test-vp8l: PASS (9490 checks)` / `run-unit: PASS`
  - `sh plan/ws344/tests/run-unit.sh all` → 全 15 test PASS、`run-unit: PASS`
  - `sh plan/ws344/tests/make-fixtures.sh`（133 images、fixtures.sha256 と一致）→ `sh plan/ws344/tests/run-fixtures.sh lossless` → `run-fixtures: PASS (lossless, 174 checks)`
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`
  - `git diff --check` 問題なし。
- 試験の内容: header（手で組んだ byte、version 1〜7、signature、5 byte 未満）、lbitwriter と平坦な prefix code（全 channel 値を 8 bit の符号）で組んだ stream、test 側の順変換（predictor mode 0〜13、color、subtract green、indexing）から元の画像が戻ること、indexing の色数 1・2・3・4・5・16・17・256 と幅（端数）、表の差分の和（手の計算）と表の外の index が 0x00000000、transform の組合せ（indexing の後の predictor は縮んだ幅、4 つ全部を 3 通りの順）、同じ type 2 回は EINVAL、全ての途中切れが成功しない（ASan の leak 検査）、乱数と変異の stream、decode()（header + stream）。
- わざと壊す確かめ（27 通り: 版の判定、height・alpha の shift、signature、5 byte の判定、width_bits の境 3 か所、差分の和、表の wrap、逆変換の順序、重複の判定、size_bits の基点、幅の縮小、表の padding、14 bit の上限 2 か所、色数 +1、subresolution の高さ、subtract green の数、transform の幅、ENOBUFS の判定 2 か所、stream の始点と大きさ 2 か所、role）: 最初は 3 つの見逃し（width・height の上限の `>=`、present bit の ENOBUFS）があり、試験を足して全て FAIL になった。元に戻したことは `cmp` で確かめた。
- 自分で決めた点（header が黙っている所）:
  - decode_stream の width・height が 1〜16384 の外、または pixels が NULL は EINVAL。read_header は 5 byte 未満が ENOBUFS で、それ以上で data か header が NULL なら EINVAL。
  - decode() は pixels・width・height が NULL なら EINVAL。header の後で width * height * 4 byte を先に確保（見出しだけで大きな画像を言う stream は、確保は遅延で commit されず、続く decode が ENOBUFS で返し解放する）。失敗時 width・height は書かない。
  - transform の record は読む前に数に入れ、失敗の時に data を呼び元が必ず解放する。
  - 色 table の buffer は 256 entry を常に確保し、色数の後を 0 で埋める。decode_image の limit は全部 WEBPC_VP8L_TABLE_ENTRIES_MAX。
  - transform の読みの途中で lbits が尽きたら（present bit、size_bits、色数の後）その場で ENOBUFS。
- 仕様と phase.md・header の食い違い: 見つからなかった。
- 未実施: 実機・QEMU の試験は無い（host の単体試験と fixture のみ）。mutate.sh・fuzz は割り当て外のため未実施。
