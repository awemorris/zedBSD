<!-- awesome-plan project=zedbsd record=ws336-p004 -->

# ws336-p004: ssh-cipher・ssh-packet（chacha20-poly1305@openssh.com と binary packet）

Status: cleared（2026-11-01 08:40 Q1 の判定、main a7cb17936。run-unit cipher・packet を Q1 も流して PASS）
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
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4253.txt`（SHA-256 `b471173305154f6c…`、全桁は同じ directory の SHA256SUMS）: §4.2（識別の行）、§6（packet の形、padding 4〜255。§6 の「最小 16 bytes」は送り手の規則で、受けは packet_length 5 以上を受ける。鍵ありの NEWKEYS は packet_length 8 になる）、§6.4（seq）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-chacha20-poly1305-04.txt`（SHA-256 `a8aa91696db885d2…`、全桁は同じ directory の SHA256SUMS）: Appendix A（完全な例: 64 bytes の鍵、seq 7、packet と tag。既知の答えの試験にする）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-strict-kex-02.txt`（SHA-256 `c08126dcaaf63958…`、全桁は同じ directory の SHA256SUMS）: §3.3（seq の 0 への戻し）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-cipher.c`・`ssh-packet.c`
- `plan/ws336/tests/unit/test-cipher.c`・`test-packet.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- cipher の既知の答え: draft-ietf-sshm-chacha20-poly1305-04 の Appendix A の鍵・seq・平文の packet（padding を含む）で `ssh_chachapoly_seal` の暗号と tag が Appendix A の値と byte ごとに一致し、`ssh_chachapoly_length` と `ssh_chachapoly_open` が元に戻す（値は spec の text から取り、取った行を試験の comment に書く）
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

2026-11-01 08:34 時点（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。

### 作った物

- `userland/base/ssh/ssh-cipher.c`（init・clear・length・seal・open）、`userland/base/ssh/ssh-packet.c`（init・free・feed・next・send・set_send_key・set_receive_key・reset_seq・send_raw・take_line・pending・written）。header は変えていない。
- `plan/ws336/tests/unit/test-cipher.c`（1064 checks）、`plan/ws336/tests/unit/test-packet.c`（369168 checks。うち 10 万回は乱数の feed）。

### 実行した command と最後の行

- `sh plan/ws336/tests/run-unit.sh cipher packet` → `test-cipher: PASS (1064 checks)`・`test-packet: PASS (369168 checks)`・`run-unit: PASS`（ASan・UBSan・leak 検出つき、`-std=c89 -Wall -Wextra -Werror`）。
- `sh plan/ws336/tests/run-unit.sh all` → buf 323・channel 1603651・cipher 1064・common 2316・crypto 1656・packet 369168・privsep 947・sftp 1836・tools 13・ttymodes 20682 の全部 PASS、最後の行 `run-unit: PASS`。
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- `git diff --check` → 出力なし（rc 0）。

### 試験の中身

- cipher: draft-ietf-sshm-chacha20-poly1305-04 Appendix A の完全な例（Figure 5 の鍵・seq 7・Figure 4 の平文・Figure 12 の暗号・Figure 17 の tag）で seal の暗号と tag が byte ごとに一致し、length と open が元に戻す。別の seq では EBADMSG。`ssh_chacha20_xor`・`ssh_poly1305` を直に呼んで組んだ期待（K_1 = 前半、K_2 = 後半、nonce は seq の 64 bit big-endian、length は K_2 の counter 0、本体は K_1 の counter 1 から、Poly1305 の鍵は K_1 の block 0 の先頭 32 bytes）と seq 0・1・0xffffffff、size 5・12・64・300 で一致。tag の 128 bit・暗号の全 bit の反転、1 byte 短い長さの全部で EBADMSG かつ packet 不変。size < 5・size - 4 > SSH_PACKET_LIMIT で EINVAL。init の鍵の分割と clear の wipe。
- packet: 上の 3「試験の項目」の全部（鍵なしの送り 1〜40 bytes と最大 261888 bytes、padding 4 以上・整列、1 byte ずつ・2 packet 一度の feed、鍵ありの往復と seq・整列・1 bit の変更、packet_length 4・LIMIT+1 の EMSGSIZE、padding_length 3・packet_length 以上の EINVAL、tag の途中の EAGAIN、seq の wrap と reset_seq、take_line の各場合、send_raw と pending・written、入力と出力の上限の境界、乱数 10 万回）。鍵なしの受けの枠は `wire.h` で組んだ（モジュールとは別）。

### わざと壊す確かめ（git の外ではなく worktree の file を書き換え、`cmp` で戻した事を確認）

`build/tmp/p004m-1/mut.py`・`build/tmp/p004m-2/mut.py` で 23 通り（cipher: counter の取り違え、K_1 と K_2 の取り違え、tag の確かめを外す、tag の範囲を 1 byte 減らす、size の下限を 1 ずらす、nonce の位置をずらす。packet: packet_length の上限・下限、padding_length の下限・room の比較、wrap の閾値、take_line の上限・探索範囲・NUL 検査の範囲、整列の keyed の反転、reset_seq で wrapped を戻さない、feed・出力の上限の `>` を `>=`、tag 分の byte を数えない、bytes の勘定、set_send_key の packets 未リセット、set_receive_key の length 未リセット・keyed 未設定）を入れ、全部 FAIL を確認。最初に見逃した 1 つ（set_receive_key が `packet->length` を戻さない）は test-packet に確認を足して FAIL するようにした。全 mutation の後に両 .c が元と一致（`cmp`、`git status` は test-packet.c の追加分のみ）。

### 自分で決めた点（header が黙っている所）

- cipher の seal・open は `size < 5` または `size - 4 > SSH_PACKET_LIMIT` で EINVAL（header は 0・EBADMSG・EIO のみ記載）。
- seal・open が EIO の時は packet 全体（seal は tag も）を wipe する。open は tag 不一致の時 packet を変えない。
- `ssh_packet` の input・output の buffer は両方 secret（wipe あり）。
- feed の EMSGSIZE・ENOMEM と send・send_raw の ENOBUFS・ENOMEM は buffer の sticky error を残さない（`error` を 0 に戻す）。nothing appended。
- feed は data が NULL で size > 0 のとき EINVAL。send_raw も同じ。send は payload が NULL・size 0・size > SSH_PACKET_LIMIT - 255 - 1 のとき EINVAL。
- `bytes` は wire 上の全 bytes（長さ・padding・tag を含む）を数える。`wrapped` は seq が 0xffffffff から 0 へ進む時点（その 1 回目の送受）で立つ。
- `set_receive_key` は `packet->length` を 0 に戻す（旧 cipher で読んだ長さを持ち越さない）。ただし input に残る bytes は触らない（呼び出し側は NEWKEYS の直後に鍵ありの bytes を feed する）。
- take_line: capacity 0 は EINVAL。LF の探索は先頭 capacity+1 bytes まで。LF が無い時は、末尾が CR なら CR を除いた長さで EMSGSIZE を判断（CR LF の途中を誤って拒まない）。NUL は LF が見つかってから EINVAL。CR は LF の直前の 1 つだけ除く（`a\rb\r\r\n` は内容 `a\rb\r`）。
- 受けでは MAC の確認後に padding_length を検査（EINVAL）。payload の追加が失敗した後は layer は使えない（header の約束どおり）。
- 鍵ありの受けの packet_length の 1 bit 変更は、長さが大きくなれば EAGAIN（残りを待つ）、小さくなれば EBADMSG、範囲外なら EMSGSIZE のどれか（試験はそれらだけを許し、payload が出ない事と seq が進まない事を確かめる）。

### 未実施・限界

- coding-style §14 の checklist の全文の見直しは、ここでは `-std=c89 -Wall -Wextra -Werror` の compile と自分の読みまで。WS の最後の「規約の全文の見直し」の Phase に任せる。
- ssh-crypto.c は host の OpenSSL 3.5 の compat shim で流した（libcrypto-compat との試験は p021）。zedBSD の build（Makefile）は WS の方針でまだ無い。QEMU・実機・host の OpenSSH との相互運用は未実施（p015 以降）。
- clang での unit test の build は未実施（`check-skeleton.sh` は header と骨格の compile）。
