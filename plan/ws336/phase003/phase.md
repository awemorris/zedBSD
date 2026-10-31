<!-- awesome-plan project=zedbsd record=ws336-p003 -->

# ws336-p003: ssh-crypto（libcrypto-compat の呼び出し）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p002
WS335 の待ち: **WS335 の libcrypto-compat の header の凍結（main に merge）を待つ**。始める前に Q1 が `include/libc/compat/crypto/evp.h` の存在と、ssh-crypto.h の冒頭の関数の一覧が全部宣言されている事を確かめる

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-crypto.h`（仕様。冒頭に使う library の関数の一覧）
- `plan/ws336/design.md` §7（WS335 の口）・§2.4（host の OpenSSL での確かめ）
- `plan/ws336/tests/crypto-contract.c`（各呼び出しの引数の形の手本。code は写さず、呼び方の約束だけを使う）
- WS335 の `include/libc/compat/crypto/evp.h`・`rand.h`・`crypto.h`（名前と約束は OpenSSL 3.5 と同じ）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-mlkem-hybrid-kex-10.txt`（SHA-256 `d7e1ff8723466a80…`、全桁は同じ directory の SHA256SUMS）: §2.1（C_INIT・S_REPLY の長さの確かめ）、§2.3.3、§2.4（K = HASH(K_PQ || K_CL)、固定長）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-chacha20-poly1305-04.txt`（SHA-256 `a8aa91696db885d2…`、全桁は同じ directory の SHA256SUMS）: §3（64 bit の counter と 8 byte の nonce）、§4
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc8439.txt`（SHA-256 `25bef70fbf7a07ff…`、全桁は同じ directory の SHA256SUMS）: §2.4.2・§2.5.2（試験の vector）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc8032.txt`（SHA-256 `ed63657ff3893012…`、全桁は同じ directory の SHA256SUMS）: §7.1 TEST 1〜3
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc7748.txt`（SHA-256 `279ca0ecc5e92e29…`、全桁は同じ directory の SHA256SUMS）: §6.1
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-crypto.c`（`#include <compat/crypto/evp.h>`・`<compat/crypto/rand.h>`・`<compat/crypto/crypto.h>` を足す）
- `plan/ws336/tests/unit/test-crypto.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- SHA-256: FIPS 180-2 B.1 の "abc"、空、1 回と分けた update の一致
- Ed25519: RFC 8032 §7.1 TEST 1〜3 の公開鍵と署名、確かめの成功、署名・message・公開鍵の 1 bit の変更で EBADMSG
- hybrid: client_start → server → client_finish で K が一致、C_INIT の長さ ±1 で EINVAL、ek の最初の係数を 4095 にして EINVAL（FIPS 203 §7.2）、S_REPLY の長さ ±1 で EINVAL、S_REPLY の ciphertext の 1 bit の変更で K が違う（エラーではない）、X25519 の公開鍵が 0 の点で EINVAL（server 側と client 側の両方）、K が SHA-256(K_PQ||K_CL) である事を、hybrid の値から別に SHA-256 を計算して確かめる（K_PQ と K_CL は試験の中で library を直接呼んで求める）
- chacha20_xor: RFC 8439 §2.4.2 の vector を counter 1・nonce `00 00 00 4a 00 00 00 00` で（design §2.4 の写像）、in と out が同じ buffer
- poly1305: RFC 8439 §2.5.2
- ct_equal・wipe・random（2 回が違う）
- 失敗の道: 各関数の後で library の object が全部 free されている事（ASan の leak 検出）

## 4. 注意

- library の失敗（0 以下・NULL）は EIO（ssh-crypto.h の冒頭）。ただし ML-KEM の ek の取り込みの拒否と X25519 の全 0 の結果は EINVAL（相手の値の拒否）。derive の失敗が全 0 によるのか他の理由かは library から区別できないので、derive の失敗は EINVAL とする（header の約束どおり）。
- K_CL は X25519 の出力の 32 bytes そのまま（draft §2.4。mpint にしない）。
- `RAND_bytes` の引数は int なので、大きい size は INT_MAX 以下の塊に分ける。
- host の OpenSSL で試験する（`run-unit.sh` の既定）。WS335 の実装が入った後の libcrypto-compat での試験は p019。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh crypto`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
