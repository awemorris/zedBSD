<!-- awesome-plan project=zedbsd record=ws336-p001 -->

# ws336-p001: 設計（自前の sshd・ssh、標準廉価実装手順 §4）

Status: in-progress（2026-11-01 01:56 P5 q1791）
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
