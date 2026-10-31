<!-- awesome-plan project=zedbsd record=ws336-p021 -->

# ws336-p021: package と amd64 の build、libcrypto-compat での確かめ

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p020
WS335 の待ち: **WS335 の実装**（libcrypto-compat の SHA-256・SHA-3・ML-KEM-768・X25519・Ed25519・ChaCha20・Poly1305・RAND が実装され main に入り、zedBSD の build に `libcrypto-compat.so` がある事）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/ws336/design.md` §10・§11.2
- `platform/common/userland-dynamic.mk:569〜588`（外部の library を link する規則の例）
- `Makefile:470〜503`・`:538〜551`（package の登録と installer の image）
- `userland/packages/network/openssh/`（今の package の file）
- WS335 の記録（libcrypto-compat の host の build の作り方）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/Makefile`（ssh・sshd・ssh-keygen・sftp-server の package。menu は `tests`、既定 n。sshd は login/verify.c の source を共有）
- `platform/common/userland-dynamic.mk` の link の規則（libcrypto-compat.so）
- `userland/base/ssh/files/`（sshd_config・ssh_config・sshd-start・sshd.service）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- `SSH_CRYPTO_INCLUDE`・`SSH_CRYPTO_LIB` で libcrypto-compat の host の build を指し、`crypto-contract.sh`・`run-unit.sh all`・`interop-host.sh`・`sshd-pair-host.sh` が PASS
- amd64 の `$(BUILD)/bin/ssh`・`sshd`・`ssh-keygen`・`sftp-server` の build（警告 0）と `check-dynamic-elf.py` の needed（libcrypto-compat.so・libc.so）

## 4. 注意

- libcrypto-compat の host の build が無ければ Q1 に返す。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての library の header、HAL・toolchain・共有の `build/`、OpenSSH の package（`userland/packages/network/openssh/`）、既定の config（`config/ci/`、p024 まで変えない）、他の WS の file。header の不備は Q1 に返す。

## 6. 確かめの command

- `sh plan/ws336/tests/crypto-contract.sh`（libcrypto-compat で）
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
