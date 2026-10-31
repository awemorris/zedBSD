<!-- awesome-plan project=zedbsd record=ws336-p002 -->

# ws336-p002: ssh-common・ssh-buf（約束・byte 順・name-list・base64、RFC 4251 の型）

Status: cleared（2026-11-01 05:10 Q1、main 87c8caf2d）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p001
WS335 の待ち: 無し

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-common.h`・`ssh-buf.h`（仕様）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4251.txt`（SHA-256 `23a5052796d29edc…`、全桁は同じ directory の SHA256SUMS）: §5（data の型: byte・boolean・uint32・uint64・string・name-list）、§6（名前の規則、64 文字以下）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4648.txt`（SHA-256 `84e14418f795d503…`、全桁は同じ directory の SHA256SUMS）: §4（base64 の表 1）、§3.5（使わない bit は 0）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-common.c`・`ssh-buf.c`
- `plan/ws336/tests/unit/test-common.c`・`test-buf.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- byte 順の読み書き（0、最大値、各 byte が別の値）
- name-list: 空・1 つ・複数、空の名前（`a,,b`・`,a`・`a,`）、65 文字の名前、0x20・0x7f・0x80 以上の byte、SSH_NAMELIST_MAX ちょうどと +1、contains の前方一致の誤り（`aes` と `aes128`）
- base64: RFC 4648 §10 の vector（"" "f" "fo" "foo" "foob" "fooba" "foobar"）の encode と decode、pad 0 の時の長さ、4 の倍数でない長さ・alphabet の外・`=` の位置の誤り・使わない bit が 0 でない（例 "Zh=="）を EINVAL、capacity の不足で ENOBUFS
- ssh-buf: 各型の書きを `wire.h` で組んだ bytes と比べ、読みで往復。limit ちょうど・+1（EMSGSIZE）、sticky な誤り（誤りの後の書き・読みが何もしない）、string の長さが残りを超える・0xffffffff、cstring の中の NUL、namelist の誤り、`ssh_reader_finish` の残り、`ssh_buf_consume` の境界、secret の buffer が伸びた後に古い memory を消している事（allocator を差し替えずに確かめられる範囲で: 伸びる前の内容が新しい場所に正しく写る事）
- 乱数の入力: 10 万回の random な bytes を reader の全関数の random な順の呼び出しで読み、ASan・UBSan で落ちない

## 4. 注意

- `ssh_buf` の secret は realloc を使わず、新しい memory に写して古い方を `ssh_wipe` で消す。`ssh_wipe` は ssh-crypto.c の関数で p003 まで骨格（何もしない）なので、この Phase の試験は消去の結果を確かめない（p003 の後に p021 で確かめる、と結果に書く）。
- 負の値の右 shift・32 bit の shift の幅に注意（uint64 の組み立ては 2 つの 32 bit の半分から、ssh-common.h の約束）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh common buf`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

実施: 2026-11-01（host の `date`）。worktree `s-ws336-p002`、基点 main 71b604808。

### 作った物

- `userland/base/ssh/ssh-common.c`（byte 順、name-list の確認と検索、base64 の符号化・復号）
- `userland/base/ssh/ssh-buf.c`（RFC 4251 の型を書く buffer と読む reader）
- `plan/ws336/tests/unit/test-common.c`、`plan/ws336/tests/unit/test-buf.c`

### 実行した command と最後の行

- `sh plan/ws336/tests/run-unit.sh common buf`: `test-buf: PASS (323 checks)`、`test-common: PASS (2316 checks)`、`run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all`: `test-buf: PASS (323 checks)`、`test-common: PASS (2316 checks)`、`test-tools: PASS (13 checks)`、`run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh`: `check-skeleton: PASS`
- `git diff --check`: 出力なし（成功）
- 単体試験は run-unit.sh の設定（`-std=c89 -O1 -Wall -Wextra -Werror`、ASan・UBSan、leak 検出あり）で実行。cc と clang の警告 0 は check-skeleton の中で確認。

### 試験の期待値の出所

RFC 4648 §10 の vector（"" から "foobar"）、Table 1 の alphabet（0 から 63 の 6 bit 値を試験の中の別の packer で 48 byte に詰め、alphabet の文字列と比べる）、RFC 4251 §5 の例（uint32 0x29b7f4aa、"testing"、"zlib,none"）、手の計算（unused bit の mask、境界の値）。他の実装の出力は使っていない。`ssh_buf` の書きは `wire.h` で組んだ bytes と比べた。

### 壊して確かめた物（git の外、元に戻して `cmp` で一致を確認）

30 通りの変異を試験が捕まえるか確かめた（name の上限 +1、name-list の上限 +1、許す byte の範囲の両端、byte 順の入れ替え、末尾の comma の確認を外す、unused bit の mask を 3 通り、長さが 4 の倍数の確認を外す、`=` を 1 つ減らす、prefix 一致、符号化の capacity の比較、復号の capacity の比較、encoded_size の上限、reserve の limit の比較、string の長さの比較、cstring の capacity の比較、NUL の確認を外す、name-list の確認を外す、finish の残りの確認を外す、bool の正規化、成長の copy の長さ、consume の move、string の +4、u32 の残りの確認、get_bytes の残りの確認、成長の倍増と limit の切り詰め）。
最初の実行で 2 件の見逃し: (1) 復号の「`=` が 1 つの時の unused bit」の mask を 0x03 から 0x01 にしても通った。bit 1 だけが立つ入力（"AAC=" など）が無かったので試験を足し、3 通りの mask の変異が全て FAIL することを確かめた。(2) `ssh_buf_consume` の `size >= buf->size` を `>` にする変異は、size が等しい時に memmove 0 byte と wipe 0 byte になるだけで、動作が同じ（等価な変異）。
最終の実装は変異の script が `cmp` で元の file と一致することを確認済み。

### header が黙っていて自分で決めた点

1. `ssh_base64_encode` の capacity は、pad が 0 でも padded の長さ + 1 で確かめる（header の文言どおり）。pad 0 の出力は短くなる。size が大きすぎて `ssh_base64_encoded_size` が 0 を返す時は ENOBUFS。`in` が NULL で size > 0 は EINVAL、`out` が NULL は ENOBUFS（header に無い値は引数の誤りの EINVAL）。
2. `ssh_base64_decode` は入力全体を確かめてから capacity を比べるので、不正な入力は capacity に関わらず EINVAL（ENOBUFS より優先）。`=` は最後の 4 文字の末尾にだけ許し、途中の `=` や `====`、"Z===" は EINVAL。size 0 は成功で 0 byte（`out` が NULL でもよい）。`in` が NULL で size > 0 は EINVAL。`*out_size` は最初に 0 にする。
3. `ssh_namelist_check`: size > SSH_NAMELIST_MAX を先に判定して EMSGSIZE（中身が不正でも）。size > 0 で list が NULL は EINVAL。
4. `ssh_namelist_contains`: name か list が NULL、name が空は 0。
5. `ssh_buf_init`: limit が 0 の扱いは決めていない（header は 1 以上）。最初の capacity は 64 で、limit を超えない。以後は倍にし、limit で切る。成長は secret の有無に関わらず malloc・copy・（secret なら capacity 全体を wipe）・free の 1 本の経路。
6. `ssh_buf_free` は capacity 全体を wipe、`ssh_buf_reset` は使った分だけ wipe（header どおり）。`ssh_buf_consume` は移した後の末尾の古い写しも wipe し、エラーが記録されていても動く（書きではないため）。
7. 書きの引数の誤り: `data` が NULL で size > 0 の put_bytes・put_string と、`text` が NULL の put_cstring は EINVAL を記録して何も書かない（記録済みの誤りは上書きしない）。put_string は長さと中身の分を先にまとめて reserve するので、入らない時に長さだけが残ることはない。
8. `ssh_buf_put_space`: size 0 でも成功時は NULL でない pointer を返す（memory が無ければ 1 byte 分を確保する）。NULL は失敗だけ。
9. `ssh_reader_init`: data が NULL で size > 0 は EINVAL を記録し、size を 0 にする。
10. 失敗した読み（u8・u32・u64・get_bytes・string・cstring・namelist）は offset を動かさない（string・cstring・namelist は始まりに戻す）。`ssh_reader_get_bytes` は size 0 で成功し、data が NULL の reader では NULL を返す（誤りではない）。
11. `ssh_reader_get_cstring`: capacity 0 は NUL も置けないので EINVAL を記録し、何も書かない。NUL を含む string と capacity 不足が両方の時は EINVAL（NUL を先に判定）。
12. `ssh_buf_put_bytes` の `data` が自分の buffer の中を指す場合（成長で古い memory が解放される）は保証しない。呼び出し側が避ける。

### 契約への注意（header の変更案ではない）

- `ssh_reader_get_bool` の戻りは `int` で、成功は 0 か 1、失敗時は 0 を返す（sticky error は `ssh_reader_error` で見る）。骨格の `return ENOSYS` は実装で置き換えた。

### 未実施・後に確かめる物

- secret の buffer の古い memory が実際に消えることは確かめていない。`ssh_wipe` は p003 まで骨格（何もしない）で、試験は消去を調べない。p003 の後に p021 で確かめる。試験が確かめたのは、成長・reset・consume・free の後で内容が正しく写ること。
- ENOMEM（malloc の失敗）の経路は host の試験で起こしていない（allocator を差し替えていない）。code は new memory の確保に失敗したら ENOMEM を記録して古い buffer を保つ。
- zedBSD 向けの交差 build は check-skeleton の項目 7（sysroot がある時だけ実行）の結果に従う。出力が PASS のみで、skip の注記は出なかった。
- QEMU・実機の試験は無し（この Phase は host の単体試験だけ）。
