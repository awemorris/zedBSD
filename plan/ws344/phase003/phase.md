<!-- awesome-plan project=zedbsd record=ws344-p003 -->

# ws344-p003 VP8L の bit reader と prefix code（lbits.c・prefix.c）

- Status: 実装済み・試験 PASS（Sonnet 5.5、2026-11-02。cleared は Q1 が決める）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p001。
- 目的: LSB-first の bit の読みと、正準の prefix code の 2 段の lookup table（pool）と symbol の読み。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `lbits.h`・`prefix.h`（仕様）。
- RFC 9649 §3.2（ReadBits）、§3.7.1、§3.7.2.1（完全な木・1 つの葉）。RFC 1951 §3.2.2（正準の符号の割り当て、step 1〜3）。

## 2. 作る物

- `lbits.c`、`prefix.c`、`unit/test-lbits.c`、`unit/test-prefix.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- lbits: lbitwriter で書いた乱数の幅（0〜24）・値の列を read で全部読み戻す、peek が消費しない、skip、0 bit、data の終わりちょうど・1 bit 越え（consumed_end が立ち、越えた bit は 0）、peek は終わりを越えても consumed_end を立てない、size 0。
- prefix: RFC 1951 §3.2.2 の例（長さ (3,3,3,3,3,2,4,4) → A=010 … H=1111）を lbitwriter_put_code で書いて読む。1 symbol の code（長さ 1〜15 のどれでも）は 0 bit で読む。完全でない・over-subscribed・symbol 0 個・長さ 16 は EINVAL、pool が変わらない（count が戻る）。root の 8 bit より長い符号（長さ 9〜15、2 段目の table）。alphabet 2328 の乱数の完全な code（test の中で頻度から Huffman の長さを作る、長さ 15 で切った物は Kraft の和を保つ手順で作る）を 100 個、全 symbol を往復。pool の limit を小さくして ENOMEM、その後に同じ pool で小さい code が作れる。data の終わりを越えた read は code の symbol を返す。
- Kraft の和: 作った code の全 symbol の 2^-長さの和が 1（test の側で確かめる性質）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- 符号は最上位の bit から stream に入る（RFC 1951 と同じ）。stream は LSB-first なので、peek した値の bit 0 が符号の最初の bit。table は「bit を逆にした符号」で引く。
- 2 段目の table の大きさは、その root の entry の下に来る最長の符号で決める。entry の意味は prefix.h の struct の comment の通り（他の module は entry を読まない）。
- pool は realloc で動くので、code は offset だけを持つ（pointer を持たない）。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh lbits
sh plan/ws344/tests/run-unit.sh prefix
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

日付: 2026-11-02（host の date）。担当: Sonnet 5.5（q1947-i01）。

実行と最後の行（worktree `/home/awe/zedBSD-worktrees/s-ws344-p003`）:

- `sh plan/ws344/tests/run-unit.sh all` → test-common PASS (32 checks)、test-helpers PASS (26 checks)、test-lbits PASS (205384 checks)、test-prefix PASS (1430293 checks)、`run-unit: PASS`（ASan・UBSan）。
- `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`。
- `git diff --check` → 出力なし。

作った物: `lbits.c`、`prefix.c`、`unit/test-lbits.c`、`unit/test-prefix.c`。header・道具・他 module は変えていない。

試験の中身: lbits は手計算の列、0 bit・size 0・NULL、data の終わりちょうどと 1 bit 越え、peek が消費も consumed_end も立てないこと、skip、lbitwriter の乱数の列（幅 0〜24、read だけの列と read・peek・skip の混在、各 300 列）。
prefix は RFC 1951 3.2.2 の例（A=010 … H=1111）、1 symbol の code（長さ 1〜15、alphabet 2328 の先頭・中・末、0 bit で読む、pool 不変）、EINVAL の各種（不完全・over-subscribed・symbol 0 個・長さ 16・255・count 0・2329、pool の count 不変）、
階段の code（最長 2〜15、table の大きさ 256 + 2^(最長-8) を確認）、等長の code（2^1〜2^11 の symbol）、pool の limit（ちょうど・1 足りない・0・ENOMEM の後に小さい code）、realloc で動いた後も古い code が読めること、data の終わりを越えた read、乱数 byte の読み（常に present な symbol）、
alphabet 2328 を含む Huffman の完全な code 100 個（長さ 15 で切って Kraft の和を保つ手順の物を含む。test が切った数と 8 bit を越えた数を確認）の全 symbol と乱数列の往復（各 symbol の後に 5 bit の marker を置き、code が bit をちょうど使ったことを確認）。Kraft の和 = 1 は test の側で確認。

わざと壊す確かめ（git の外の複写、`cmp` で元に戻ったことを確認）: 最初の回は 25 通りで 18 が FAIL（検出）、7 が残った。残りの 5 は等価な変更:
(1) lbits の取り込みの `count < bits` を `<=` にする（余分に 1 byte 先に取り込むだけで結果が変わらない）、(2) 取り込みの shift に `& 31` を足す（その時点で count は 24 未満）、(3) 1 symbol の code の symbol を「最後に見た present」から「最初」に変える（present が 1 つの時だけ使うので同じ）、(4) symbol 0 個の検査を外す（Kraft の検査が和 0 で同じ EINVAL を返す冗長な検査）、
(5) pool の capacity を 1 小さく申告する（保守的になるだけで誤りにならない）。
残る 2 つ（2 段目の table を 1 bit 余分に取る、第 1 level の幅の上限を 7 にする）は実際の見逃しで、table の大きさをちょうどで確認する検査を足して塞いだ。足した後に、この 2 つと新しい 2 つ（優先順位を変えた +1、上限を外す）を再実行し、4 つとも FAIL。

自分で決めた点（header が黙っている所）:
- count が 0 または 2328 を超える時は EINVAL。
- 1 symbol の code は table を作らず pool を変えない（offset は 0）。
- 長さ 0 の symbol は code を持たない。第 1 level の幅は `min(最長の長さ, 8)`（最長が 8 以下なら 2^最長 の entry、2 段目なし）。2 段目の table は、その root の entry の下の最長の符号の長さで幅を決め、root の後ろに root の index の順に並べる。link entry の value は code の先頭からの offset（最大 256 + 256×128 で uint16 に収まる）。
- pool の容量は初回 1024 entry、以後 2 倍（limit で頭打ち、必要量は常に確保）。limit 超過は table の大きさを先に求めて pool に触る前に ENOMEM（count は動かない。realloc に失敗した時も pool は元のまま）。
- `webpc_prefix_read` は pool の中身を信用する（build が作った code だけを渡す前提）。peek した値は root の幅の中に収まるので table の外は読まない。
- `webpc_lbits_peek` は reader を変える（byte の取り込み）が、消費・consumed_end は変えない。value は count より上が常に 0 で、data の終わりを越えた bit は 0 と読まれる。

仕様と phase.md・header の食い違い: なし。header の comment の誤り・不足: なし。

未実施: fixtures（run-fixtures・compare）、mutate.sh、fuzz（decoder が揃っていないため範囲外）、QEMU・実機。
