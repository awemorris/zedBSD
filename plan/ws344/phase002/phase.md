<!-- awesome-plan project=zedbsd record=ws344-p002 -->

# ws344-p002 container（riff.c）

- Status: 実装済み・試験 PASS（Sonnet q1946-i01、2026-11-02、Q1 の受け入れ待ち）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p001。
- 目的: RIFF・VP8X・optional chunk・bitstream の chunk を読み、features と bitstream・ALPH の位置を返す。状態の規則は libwebp と同じ。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `userland/desktop/libwebp-compat/riff.h`（仕様。step 1〜8 と complete 0 の規則）。
- RFC 9649 §2.3〜§2.7（Figure 1〜10、§2.7.1.6 の未知の chunk）、§3.4（VP8L の header の 5 byte）。RFC 6386 §9.1・§19.1（frame tag と start code、大きさの 14 bit）。

## 2. 作る物

- `userland/desktop/libwebp-compat/riff.c`、`plan/ws344/tests/unit/test-riff.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- 手で組んだ byte 列（test の中の配列か小さな組み立ての関数）で: simple の VP8（tag と start code と大きさだけの 10 byte の frame）・VP8L（5 byte の header）、VP8X + ALPH + VP8、VP8X + ICCP + 奇数の大きさの未知の chunk（padding）+ VP8L + 後ろの chunk、裸の VP8・VP8L、RIFF の無い chunk、File Size の後ろの余りの byte。
- step 1〜8 の各々の誤り（EINVAL・ENOBUFS）を 1 つ以上: 12 byte 未満（裸の VP8L でも ENOBUFS）、"WEBP" の無い RIFF、File Size 11 と 0xfffffff7、complete 1 で File Size が data より大、VP8X の size 9 と 11、VP8X の 18 byte の途中で切れる、canvas 65536x65536（2^32 以上）、RIFF の無い VP8X、chunk の合計が File Size を越える、chunk の size が 0xfffffff7、bitstream の chunk の size が File Size - 12 を越える、VP8 の interframe（bit 0 = 1）・version 4・show_frame 0・partition の size ≥ bitstream_size・幅 0・高さ 0・start code 違い、VP8L の signature 違い・version 1、VP8X の canvas と bitstream の大きさの違い。
- has_alpha の規則: VP8X の Alpha flag、VP8L の alpha_is_used が VP8X の flag を置き換える（flag 1・bit 0 → 0、flag 0・bit 1 → 1）、ALPH があれば 1。ALPH が 2 つなら後の物。animation（flag 0x02）は format UNDEFINED・has_animation 1・bitstream NULL で 0。
- complete 0: VP8X の後で切れた data は 0（canvas、format UNDEFINED）、VP8 の header の途中で切れた拡張形式は 0（format LOSSY）、VP8X の無い file の切れは ENOBUFS。
- 有効な file を 0〜長さ - 1 の全ての長さで切り、ちょうどの大きさの malloc の複写で complete 0 と 1 の両方を呼ぶ（ASan が範囲の外の読みを捕まえる）。乱数の byte 列 10000 個（先頭を "RIFF....WEBP" にした物を半分）で落ちない。
- 期待の値は header の step と RFC の図から手で決める。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- 状態の規則は libwebp に合わせた物（design.md §3）。RFC 9649 の「Readers SHOULD fail when chunks ... are out of order」は当てない（bitstream の chunk の後は読まない）。この block の comment に「Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal (BSD-3-Clause), the order of the checks and the statuses」と書く（ws.md の「Q1 の判断」1: interface の振る舞いだけ）。libwebp の source は読まない（header が手順を全部書いている）。
- size の算術は size_t で、引き算の前に大小を比べる（`size - 8` を size < 8 で作らない）。File Size + 8 は 32 bit を越えうる（uint64_t か比較の形で）。
- VP8 の first partition size は 19 bit（tag の bit 5〜23）。大きさは byte 6・8 の 16 bit の下位 14 bit、上位 2 bit は scale（読み捨て）。
- 拡張形式で VP8L の時、has_alpha は alpha_is_used で置き換わる（VP8X の flag ではない）。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh riff
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

- 日付: 2026-11-02（host の `date`）。担当: Sonnet 5.5（q1946-i01）。worktree `/home/awe/zedBSD-worktrees/s-ws344-p002`、branch `agent/s-ws344-p002`。
- 作った物: `userland/desktop/libwebp-compat/riff.c`（`webpc_riff_parse` と static の補助 7 つ）、`plan/ws344/tests/unit/test-riff.c`。header・道具・Makefile は変えていない。
- 実行した command と最後の行:
  - `sh plan/ws344/tests/run-unit.sh riff` → `test-riff: PASS (42324 checks)` / `run-unit: PASS`
  - `sh plan/ws344/tests/run-unit.sh all` → `test-common: PASS (32 checks)`、`test-helpers: PASS (26 checks)`、`test-riff: PASS (...)`、`run-unit: PASS`
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`
  - `git diff --check` → 出力なし。
- 試験の中身: 手で組んだ file（simple VP8・VP8L、VP8X+ALPH+VP8、VP8X+ICCP+奇数の未知 chunk+VP8L+後ろの chunk、裸の VP8・VP8L、RIFF の無い chunk、余りの byte）、step 1〜8 の各誤り、VP8 header の各 field（version 0..7、partition 19 bit、14 bit の size と scale、start code の 3 byte）、VP8L header（version 1..7、14 bit の size の端）、has_alpha の規則（flag × alpha bit の 4 通り、ALPH）、animation、complete 0 の部分の file、4 種の有効な file を 0〜長さ - 1 の全長で切る（complete 0 と 1、ちょうどの大きさの malloc の複写で ASan）、乱数 10000 個（RIFF・VP8X・既知の tag を混ぜた構造入り、各 complete 0・1）。期待の値は RFC の図と手の計算（VP8L の `2f 02 40 00 10` など）だけから作った。
- わざと壊す確かめ（git の外、`build/tmp/ws344-mut-p002/mut.py`、99 通りの変異: 境界の比較を 1 つずらす・符号を逆にする・bit の位置と幅・定数・分岐の削除）: 最初の版の見逃し 12 件のうち 5 件は試験を足して塞いだ（canvas の面積ちょうど 2^32 - 1、RIFF の無い VP8X のちょうど 18 byte、ちょうど ALPH で終わる部分の file、RIFF の無い chunk の size が data を 8 越える、partition size の bit 16）。残った見逃し 7 件は全て等価の変異（観測できる違いが無い）: 
  - 「tag の 4 byte が残っていない」の判定（step 4、`< 4` を `< 3`・`<= 4` に）: 直後の step 5・6 が 8 byte 未満を同じ ENOBUFS（拡張形式は soften で 0）にするため、結果が同じ。header の手順どおりに残した。
  - step 5 の chunk size の `> 0xfffffff6` を `>=` に: size ちょうど 0xfffffff6 の chunk は padded size が必ず File Size を越えて直後の和の検査で EINVAL になる。
  - 裸の bitstream の判定の `left >= 5` と `chunk[4] >> 5`: ここへ来る時は left が常に 8 以上。signature 0x2f は frame tag の bit 0 が 1（interframe）なので、lossless と見なされなくても lossy の検査で必ず EINVAL になり、失敗の時の format は未規定。
  - soften の中の bitstream の項目を 0 にする代入: 成功の前は元々 0。
  - step 6 の `left < 8`: step 5 と step 3 を通った時は left が常に 8 以上。
  - 元に戻したことは `git status`（riff.c は無変更、複写だけで変異した）で確かめた。
- 自分で決めた点（header が黙っている所）:
  1. File Size + 8 より長い data は complete 0 でも切る（header の「Bytes after File Size + 8 are never used」）。File Size が小さく VP8X の 18 byte が切れた範囲に入る時は、VP8X が「そこに無い」ので ENOBUFS（失敗）。VP8X の後で 8 byte 未満しか残らない時は complete 0 で部分の成功、complete 1 で ENOBUFS。
  2. RIFF の無い bitstream の chunk の size が 0xfffffff6 を越える時は、complete 1 の「data より大」の ENOBUFS の後で EINVAL にした（libwebp の ParseHeadersInternal の compressed_size の検査に合わせた。header の step 6 は RIFF の File Size - 12 だけを書く）。
  3. complete 0 の部分の成功でも、見つけた ALPH の alpha・alpha_size は out に残す（header は has_alpha に含めると書くだけ）。bitstream・bitstream_size・available_size は 0 / NULL。
  4. has_alpha の計算: VP8X の flag → VP8L の alpha_is_used で置き換え（lossless のみ）→ ALPH があれば 1。VP8L の header に届かず部分の成功になった時は flag と ALPH のみ。
  5. 部分の成功の時の format は step 6 を通っていれば LOSSY / LOSSLESS、通っていなければ UNDEFINED。animation は UNDEFINED のまま（step 3 の直後に返る）。
  6. 内部の補助の名前は riff_state・tag_is・parse_riff_header・parse_vp8x・parse_optional_chunks・parse_bitstream_chunk・read_lossy_header・read_lossless_header・soften（全て static）。
- 仕様と phase.md・header の食い違い: 無し。header の step 4 は step 5・6 の検査と重なる（等価の変異の 1 つ目）が、誤りではない。
- 未実施: fixtures の全体の試験（phase.md の command に無い。p019 の compare で確かめる）、QEMU・実機（この Phase の範囲外）。
- Q1 が消す物: `/home/awe/zedBSD-worktrees/s-ws344-p002/build/tmp/`（ws344-unit・ws344-skeleton・ws344-mut-p002 など、使い捨て）。
