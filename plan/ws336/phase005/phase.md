<!-- awesome-plan project=zedbsd record=ws336-p005 -->

# ws336-p005: ssh-key（ed25519 の blob・署名・公開鍵の行・秘密鍵の file・指紋）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p003
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-key.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc8709.txt`（SHA-256 `ea4aa3a8feec6bd5…`、全桁は同じ directory の SHA256SUMS）: §4（公開鍵の形）、§6（署名の形）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-PROTOCOL.key`（SHA-256 `8479c767aeb256e4…`、全桁は同じ directory の SHA256SUMS）: §1・§3・§5（openssh-key-v1、checkint、padding、none）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-ssh-agent-16.txt`（SHA-256 `2b83bc2029cce5d6…`、全桁は同じ directory の SHA256SUMS）: §3.2.3（Ed25519 の秘密鍵: ENC(A) と k || ENC(A)）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-sshd.8`（SHA-256 `74dde0667a4c99e6…`、全桁は同じ directory の SHA256SUMS）: AUTHORIZED_KEYS FILE FORMAT（公開鍵の 1 行の形）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-ssh-keygen.1`（SHA-256 `d40597a5a1d841a0…`、全桁は同じ directory の SHA256SUMS）: -l（SHA256 の指紋の形）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc8032.txt`（SHA-256 `ed63657ff3893012…`、全桁は同じ directory の SHA256SUMS）: §7.1（試験の鍵）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-key.c`
- `plan/ws336/tests/unit/test-key.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- blob: RFC 8032 TEST 1 の公開鍵で、`wire.h` で組んだ期待の 51 bytes と一致、parse の往復、名前の違い（`ssh-rsa`）で ENOTSUP、長さ 31・33・末尾の余り・壊れた string で EINVAL
- 署名: TEST 1 の seed で署名して blob が 83 bytes で名前と 64 bytes の署名、verify の成功、名前・長さ・1 bit の変更
- 公開鍵の行: comment あり・なし・tab・複数の空白、型の欄と blob の中の型が違う（EINVAL）、base64 の誤り
- 秘密鍵の file: 手で組んだ file（`wire.h` で中身を組み、`ssh_base64_encode` を使わずに試験の中の独立の base64 で包む、70 桁で折る）を読む。checkint の不一致、padding の誤り（1,2,4）、cipher `aes256-ctr`（ENOTSUP）、鍵の数 2（ENOTSUP）、blob と秘密の中の公開鍵の不一致、seed と公開鍵の不一致（EINVAL）、BEGIN・END の欠け、CR LF の行、前後の余分な行。put → parse の往復
- 指紋: TEST 1 の公開鍵の blob の SHA-256 を試験の中で計算して base64（pad なし）にした物と一致、capacity 不足で ENOBUFS
- 乱数の入力: 秘密鍵の file の parser に random な変異（1 byte の変更・切り詰め）を 10 万回

## 4. 注意

- OpenSSH が書く実物の鍵の file を試験の期待の値にしない（他の実装の出力）。相互の確かめ（OpenSSH の ssh-keygen が書いた鍵を読む、逆も）は p015 の `interop-host.sh` で行う。
- 秘密鍵の decode の途中の bytes は秘密（secret の buffer、失敗の道でも消す）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh key`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
