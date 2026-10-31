<!-- awesome-plan project=zedbsd record=ws336-p002 -->

# ws336-p002: ssh-common・ssh-buf（約束・byte 順・name-list・base64、RFC 4251 の型）

Status: in-progress（2026-11-01 04:54 q1809、Sonnet）
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

（未着手）
