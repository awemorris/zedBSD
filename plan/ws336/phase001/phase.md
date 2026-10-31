<!-- awesome-plan project=zedbsd record=ws336-p001 -->

# ws336-p001: 設計（自前の sshd・ssh、標準廉価実装手順 §4）

Status: cleared（2026-11-01 04:54 Q1、main 451dd0f9e）
Parent: [WS336](../ws.md)

## 範囲

1. 今の事実: OpenSSH の package（`userland/packages/network/openssh`）の image での使われ方（sshd の起動・設定・host の鍵の作成・rc・試験の道具の guest の SSH）、
   OpenSSH を置き換える時に壊れてはならない物（plan/tools の guest の SSH、実機の試験の script、Q1・担当が使う ssh の使い方）。
2. protocol の範囲: transport（RFC 4253）、認証（RFC 4252、publickey の ed25519 だけ。password は要るか）、connection（RFC 4254: session・pty・shell・exec・env・
   window の変更・port の転送は要るか）、scp・sftp は要るか（範囲の外にするか）、known_hosts・authorized_keys の形（OpenSSH と同じ）、鍵の file の形（OpenSSH の
   ed25519 の秘密鍵の形）、ssh-keygen に当たる道具、agent は要るか。
3. 暗号: 鍵の交換の 1 つ（mlkem768x25519-sha256 か curve25519-sha256、WS335 と揃える）、host の鍵と利用者の鍵は ssh-ed25519（RFC 8709）、暗号は
   chacha20-poly1305@openssh.com か aes256-gcm@openssh.com（1 つ）、hash は SHA-256・SHA-512。全部 WS335 の module の口を使う（重ねて作らない）。
4. 安全: 定数時間、乱数、権限の分離（sshd の privilege separation をどこまで）、login の処理（pty・utmp・session）、失敗の記録、総当りへの備え。
5. 置き場所・名前（`userland/base/ssh` など）、OpenSSH の package との入れ替えの手順、zedBSD と Linux の host で試す道具（相手を OpenSSH にした相互の試験）。
6. 標準廉価実装手順の §4 と、WS335 との待ち合わせ（WS335 の暗号の header が凍結するまで、それを使う実装の Phase を始めない）。design-reviewer、ユーザーへの問い（AGENTS.md §3 の形）。
7. code は header・骨格・試験の道具の他は書かない。

## 結果（2026-11-01 03:0x P5、q1791-i01）

- 成果: [design.md](../design.md)、header 16 本と骨格 23 本（`userland/base/ssh/`）、試験の道具（`plan/ws336/tests/`: run-unit.sh・check-skeleton.sh・crypto-contract.c/.sh・unit/check.h・unit/wire.h・unit/test-tools.c・host-crypto/）、実装の Phase p002〜p025 の phase.md、[design-review.md](../design-review.md)（32 件を全部反映）、[user-questions.md](../user-questions.md)（Q1〜Q9）。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/ssh/`（31 file と SHA256SUMS）。
- 確かめ（2026-11-01 03:0x、host）: `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（cc・clang の C89 pedantic、zedBSD の clang と sysroot。libcrypto-compat の header の照合は header が無いので skip）、`sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`（test-tools 13 checks）、`sh plan/ws336/tests/crypto-contract.sh` → `crypto-contract: PASS`（108 checks、host の OpenSSL 3.5.6）、`git diff --check` → 差なし。
- WS335 との合意: design.md §7（P6 が compat の header と骨格で crypto-contract.c の compile と link を確かめ済み、WS335 の p023・p024 の受け入れに入れる）。
- header の基点: d4ab182d2。
- 未実施: QEMU・実機（設計の Phase なので無し）。Status の cleared は Q1 が決める。
