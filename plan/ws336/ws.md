<!-- awesome-plan project=zedbsd record=ws336 -->

# WS336 zedBSD の自前の sshd と ssh（OpenSSH と互換の protocol、ed25519 の鍵と最新の鍵の交換 1 つ）

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG005。
- Status: planning（2026-11-01 01:56 新設、P5 が p001 の設計を q1791 で始めた）
- 目標: OpenSSH（`userland/packages/network/openssh`）を置き換える、zedBSD の自前の sshd と ssh を作る。OpenSSH の sshd と自前の sshd の両方に接続でき、
  OpenSSH の ssh から自前の sshd に接続できる。暗号の部品は [WS335](../ws335/ws.md) の libcrypto-compat（OpenSSL 3.5 互換の API）を使う（下の決定 4）。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。設計を終えてから実装に入る。WS335 と並べて進め、
  WS335 の暗号の module の口に依る所は待ち合わせる（WS335 の設計の凍結を待ってから、その header を使う実装の Phase を始める）。

## ユーザーの決定

- 2026-11-01 ユーザーから、次の趣旨の指示を受けた。
  1. 互換の sshd と ssh を作る WS を立てる。
  2. 鍵は ed25519 だけ使えればよい。鍵の交換は最新の定番を 1 つ持ち、OpenSSH の sshd と自前の sshd の両方に接続できればよい。
  3. WS335 での決定（暗号の部品の口を WS335 が持つ、OpenSSH の libcrypto の互換は作らない）をこの WS に反映する。
  4. （2026-11-01 追記）暗号は WS335 に足した libcrypto-compat（libbrowser と自前の ssh・sshd が使う部分だけの OpenSSL 互換の API）を使う。
  5. WS335 と並行して進め、依存の所では待ち合わせる。標準廉価実装手順で実装する。P5 で始める。
- 2026-11-01 ユーザーから、Q1〜Q9 はまず推しの案でよい、後で実装できるように、今は作らない物の TODO の一覧を WS に入れる、という趣旨の答えを受けた。

## Resume point

- p001（設計）を P5 が q1791 で進めている。成果: [design.md](design.md)、header と骨格 `userland/base/ssh/`、試験の道具 `plan/ws336/tests/`、[design-review.md](design-review.md)、[user-questions.md](user-questions.md)。
- header の基点: `d4ab182d2`（p001 の design-review を反映した header。実装の Phase の `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh`。header を変える時は Q1 が新しい基点をここに書く）
- 「WS335 header」= WS335 の libcrypto-compat の header（`include/libc/compat/crypto/*.h`）の凍結が main に入った後に始める。「WS335 実装」= libcrypto-compat の該当の実装が main に入り zedBSD の build にある後。他の Phase は host の OpenSSL 3.5 で試すので WS335 を待たない（design.md §7・§12）。

## Phase

設計は [design.md](design.md)。並べて起動できる組は design.md §12。

| ID | 目的 | 状態 | 依存 | WS335 の待ち |
| --- | --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（標準廉価実装手順 §4）: SSH の protocol の範囲（RFC 4251〜4254・8709 ほか）、鍵の交換の 1 つ、暗号の組、sshd と ssh の機能の範囲、module・header・骨格・試験の道具、実装の Phase の表、WS335 との待ち合わせ、design-reviewer、問い | in-progress | — | — |
| [p002](phase002/phase.md) | ssh-common・ssh-buf | planned | p001 | — |
| [p003](phase003/phase.md) | ssh-crypto（libcrypto-compat の呼び出し） | planned | p002 | **WS335 header** |
| [p004](phase004/phase.md) | ssh-cipher・ssh-packet | planned | p003 | p003 経由 |
| [p005](phase005/phase.md) | ssh-key | planned | p003 | p003 経由 |
| [p006](phase006/phase.md) | ssh-hostfile | planned | p005 | p003 経由 |
| [p007](phase007/phase.md) | ssh-kex | planned | p004 | p003 経由 |
| [p008](phase008/phase.md) | ssh-transport（Opus） | planned | p005・p007 | p003 経由 |
| [p009](phase009/phase.md) | ssh-auth | planned | p005 | p003 経由 |
| [p010](phase010/phase.md) | ssh-ttymodes・ssh-channel | planned | p002 | — |
| [p011](phase011/phase.md) | ssh-config | planned | p006 | p003 経由 |
| [p012](phase012/phase.md) | ssh-privsep（frame） | planned | p002 | — |
| [p013](phase013/phase.md) | ssh-sftp（Q3 次第） | planned | p002 | — |
| [p014](phase014/phase.md) | ssh-keygen の program | planned | p006 | p003 経由 |
| [p015](phase015/phase.md) | ssh の program・host の相互運用 A（Opus） | planned | p006・p008・p009・p010・p011 | p003 経由 |
| [p016](phase016/phase.md) | sshd の monitor と login の確かめ（Opus） | planned | p009・p011・p012・p014 | p003 経由 |
| [p017](phase017/phase.md) | sshd の pre-auth と session の process（Opus） | planned | p008・p010・p016 | p003 経由 |
| [p018](phase018/phase.md) | sshd の listener・相互運用 B（Opus） | planned | p015・p017 | p003 経由 |
| [p019](phase019/phase.md) | sftp-server の program（Q3 次第） | planned | p013・p018 | — |
| [p020](phase020/phase.md) | 敵対的な入力と privsep の見直し（Opus） | planned | p018・p019 | — |
| [p021](phase021/phase.md) | package と amd64 の build・libcrypto-compat での確かめ（Opus） | planned | p020 | **WS335 実装** |
| [p022](phase022/phase.md) | QEMU の相互運用（Opus） | planned | p021 | **WS335 実装** |
| [p023](phase023/phase.md) | 他の arch の build | planned | p021 | **WS335 実装** |
| [p024](phase024/phase.md) | OpenSSH からの入れ替え（Opus、Q6） | planned | p022・p023 | **WS335 実装** |
| [p025](phase025/phase.md) | 規約の全文の見直し（conformance-reviewer） | planned | p024 | — |
