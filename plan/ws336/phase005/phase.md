<!-- awesome-plan project=zedbsd record=ws336-p005 -->

# ws336-p005: ssh-key（ed25519 の blob・署名・公開鍵の行・秘密鍵の file・指紋）

Status: in-progress（2026-11-01 06:59 q1824、Sonnet）
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

2026-11-01 08:54（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。Status は in-progress のまま Q1 に渡す。

### 作った物

- `userland/base/ssh/ssh-key.c`（骨格の置き換え。header は不変）。前の attempt の実装を引き継ぎ、coding-style §14 の見直しで「条件の中の関数呼び出し」を直した（`key_skip_blank`・`key_skip_field`・`key_trim_end` を足し、`key_text_is`・`ssh_ct_equal`・`ssh_buf_error` の結果は先に変数へ入れてから判定）。動作は変えていない。
- `plan/ws336/tests/unit/test-key.c`（2771 checks）。期待の値は RFC 8032 7.1 の TEST 1・2（seed・公開鍵・署名）、`wire.h` で組んだ blob・鍵の file、試験の中の独立の base64（`test_base64`）、手で出した公開鍵の行の base64 と指紋だけ。OpenSSH の出力は使っていない。

### 試験の内容

blob（51 bytes の一致・往復・`ssh-rsa` など別名は ENOTSUP・空と 65 bytes の名前・31/33/0 bytes の鍵・末尾の余り・壊れた string・全ての切り詰めは EINVAL）、署名（TEST 1・2 の署名 83 bytes が RFC の署名と一致・verify・bit 反転 83 bytes x 8 bit が全て失敗で署名部分は EBADMSG・名前 ENOTSUP・63/65/0 bytes・末尾の余り・切り詰め・seed の無い鍵は EINVAL・入らない out は EMSGSIZE で何も足さない）、公開鍵の行（comment あり・なし・tab・複数の空白・先頭と末尾の空白・CR・型の欄が別は ENOTSUP・blob の型が違う EINVAL・base64 の誤り・NUL と LF・comment 254〜257 bytes の境界・全ての切り詰め）、秘密鍵の file（良い形 CR LF・前後の余分な行・LF 無しの END・折り返し 1/64/70/1 行・comment 0〜20 bytes で padding 0〜7・checkint 不一致・padding の誤り 4 通り・aes256-ctr+bcrypt は ENOTSUP・鍵 2 本と 0 本は ENOTSUP・blob/秘密/pair の不一致・seed の不一致・magic・BEGIN/END の欠け・空行・空の armour・不正な base64・16385 bytes は EMSGSIZE・file の全ての切り詰め）、`ssh_key_put_private` が手で組んだ file と byte 単位で一致（comment 0〜20）、往復、行の幅、指紋（TEST 1 の期待を 2 通りで確認、capacity 0〜50 は ENOBUFS で何も書かない）、乱数の変異 10 万回（1 byte の変更・切り詰め・両方。成功は鍵が整合、失敗は key が全部 0 で EINVAL か ENOTSUP のみ）。

### 実行したコマンドと最後の行（2026-11-01 08:53）

- `sh plan/ws336/tests/run-unit.sh key` → `test-key: PASS (2771 checks)` / `run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all` → 全 11 試験 PASS、最後の行 `run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし（終了 0）

### わざと壊す確かめ

`ssh-key.c` の複製（`build/tmp/mut5/ssh-key.c.orig`）に対し 38 通りの変更を 1 つずつ入れて `run-unit.sh key` を流し、最後に `cmp` で元に戻ったことを確かめた（2 回目の全部流しは、§14 の直しの後の版で行った）。例: 31 bytes の鍵を許す・末尾の余りの確かめを外す・名前の長さの境界をずらす・空の名前を許す・署名の長さ `!=` を `>` に・verify の名前の確かめを外す・sign の private の確かめを外す・行の CR を外す・comment の改行の確かめを外す・comment の切り詰め 255 を 256 に・checkint の確かめを外す・padding `>=` を `>`・padding の値・section の 8 の倍数・seed から出す公開鍵の照合を外す・pair の公開鍵の照合を外す・型の照合を外す・鍵の数・cipher と kdf と options の確かめを外す・magic の長さ・comment の NUL・空行の許可・file の大きさの上限・put の折り返し 70 を 64・put の padding の順・put の seed と公開鍵の順・put の private の確かめ・指紋の capacity・指紋の `=`・指紋の blob の位置・sign の領域の先取り・blob の名前の長さ。

- 最初の版では次の 4 つが生き残った: comment の切り詰めの境界（256 bytes 丁度）、padding が丁度 8 bytes の境界、blank 行の許可（照合の文字列の不一致で SKIP だった）、および下の 2 つ。試験を足して前の 3 つは FAIL するようになった（comment 254〜257 bytes、5 bytes の comment で section が揃っている所に 1 block 余分の padding）。
- **生き残る 2 つは同値の変更で、試験では区別できない**: (1) 公開鍵の行の blob の欄の 72 文字の上限を外しても、decode 先の buffer が 54 bytes なので `ssh_base64_decode` が ENOBUFS を返し、結果は同じ EINVAL。(2) 秘密鍵の中で「blob の公開鍵と private の公開鍵の一致」の確かめを外しても、「private の公開鍵と pair の後半の一致」と「seed から出した公開鍵と blob の一致」が残るので、推移的に同じ事を確かめている。

### 自分で決めた点（header が黙っている所）

- blob の名前が空または 64 bytes 超は EINVAL（RFC 4251 6 の名前の長さ）、それ以外の別名は ENOTSUP。名前の後ろが壊れていても別名なら ENOTSUP。
- `ssh_key_sign` で out に入らない時は、out に記録された error（EMSGSIZE・ENOMEM）をそのまま返し、何も足さない（先に 83 bytes を reserve）。header の列挙（0・EINVAL・EIO）に無い値を返す。
- `ssh_key_put_public_line` の comment に CR・LF があれば out に EINVAL を記録して何も足さない（行の注入の防止）。
- `ssh_key_parse_public_line`: 行頭の空白は読み飛ばす。末尾の空白・tab・CR は comment に含めない。NUL と LF が行の中にあれば EINVAL。blob の欄が 72 文字超は EINVAL。blob の中の型が別なら EINVAL（ENOTSUP を EINVAL に直す）。成功した時だけ `*key` を書く。
- `ssh_key_parse_private`: BEGIN と END の間の空行は EINVAL、行の幅は 70 に限らない。先に `*key` を消し、失敗の道でも消す。cipher と kdf の確かめ（ENOTSUP）を kdfoptions の空の確かめ（EINVAL）より先に行うので、暗号化された正しい形の file は ENOTSUP になる。鍵の数が 1 でないのは ENOTSUP（0 も）。private section は長さが 8 の倍数で 0 でなく、padding は 8 bytes 未満で 1,2,3...。private section の鍵の型が blob と違えば EINVAL。comment に NUL があれば EINVAL、255 bytes に切る。
- `ssh_key_put_private`: comment が配列の中で NUL で終わらなければ EINVAL。file 全体の長さを先に out に reserve する。
- `ssh_key_fingerprint`: capacity が 51 未満は ENOBUFS（out を書かない）。

### 未実施

- OpenSSH の ssh-keygen が書いた鍵を読む・逆の相互運用の確かめ（p015 の `interop-host.sh`）。
- 規約の全文の見直しの Phase（WS の最後）。ここでは §14 の「条件の中の関数呼び出し」「標準の comment」を自分の diff で見直しただけ。
- QEMU・実機は対象外（host の試験のみ）。

### header の不備

なし（header は変えていない）。
