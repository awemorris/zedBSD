<!-- awesome-plan project=zedbsd record=ws336-p004 -->

# ws336-p004: ssh-cipher・ssh-packet（chacha20-poly1305@openssh.com と binary packet）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p003
WS335 の待ち: p003 経由（header の凍結の後）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-cipher.h`・`ssh-packet.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-chacha20-poly1305-04.txt`（SHA-256 `a8aa91696db885d2…`、全桁は同じ directory の SHA256SUMS）: §6（K_1 と K_2、nonce、counter）、§7（受けでは tag を先に確かめる）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4253.txt`（SHA-256 `b471173305154f6c…`、全桁は同じ directory の SHA256SUMS）: §4.2（識別の行）、§6（packet の形、padding 4〜255、最小 16 bytes）、§6.4（seq）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-strict-kex-02.txt`（SHA-256 `c08126dcaaf63958…`、全桁は同じ directory の SHA256SUMS）: §3.3（seq の 0 への戻し）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-cipher.c`・`ssh-packet.c`
- `plan/ws336/tests/unit/test-cipher.c`・`test-packet.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- cipher: 64 bytes の鍵の前半が K_1・後半が K_2 である事を、試験の中で `ssh_chacha20_xor` と `ssh_poly1305` を直に呼んで組み立てた期待の bytes と比べる（seq 0・1・0xffffffff）。length の復号、seal → open の往復、tag・暗号の長さ・暗号の本体の各 1 bit の変更で EBADMSG かつ packet が変わらない
- packet（鍵なし）: payload 1・最大の長さで送り、`wire.h` で組んだ期待の枠（長さ・padding_length・payload）と、padding の長さが 4 以上で整列する事を確かめる。分けて feed（1 byte ずつ）しても同じ payload、2 つの packet を一度に feed
- packet（鍵あり）: 2 つの `struct ssh_packet` をつなぎ、送りと受けの鍵を入れて往復、seq の増加、1 bit の変更で EBADMSG、packet_length が 4・SSH_PACKET_LIMIT+1 で EMSGSIZE、padding_length が 3・packet_length 以上で EINVAL、tag の途中で EAGAIN
- seq の wrap（seq を 0xffffffff に置いて 2 回送ると wrapped が立つ）、reset_seq
- take_line: CR LF・LF だけ、253 文字ちょうどと 254 文字（EMSGSIZE）、NUL を含む行、LF の前の不完全で EAGAIN、行の後の packet の bytes が残る事
- send_raw と pending・written の境界
- 乱数の入力: 鍵なし・鍵ありの両方で random な bytes を 10 万回 feed して落ちない

## 4. 注意

- 鍵ありの padding の整列は「padding_length の byte + payload + padding」が 8 の倍数（ssh-cipher.h の SSH_CHACHAPOLY_BLOCK の comment）。鍵なしは 4 byte の長さも含めて 8 の倍数。
- 受けは整列を確かめない（ssh-packet.h の冒頭の判断）。
- 鍵ありの受けは、長さを復号した後で、tag の確かめが済むまで本体を復号しない。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh cipher packet`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
