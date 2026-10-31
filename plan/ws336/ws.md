<!-- awesome-plan project=zedbsd record=ws336 -->

# WS336 zedBSD の自前の sshd と ssh（OpenSSH と互換の protocol、ed25519 の鍵と最新の鍵の交換 1 つ）

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG005。
- Status: planning（2026-11-01 01:56 新設、P5 が p001 の設計を q1791 で始めた）
- 目標: OpenSSH（`userland/packages/network/openssh`）を置き換える、zedBSD の自前の sshd と ssh を作る。OpenSSH の sshd と自前の sshd の両方に接続でき、
  OpenSSH の ssh から自前の sshd に接続できる。暗号の部品は [WS335](../ws335/ws.md)（libssl-compat）の内部の口を使う。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。設計を終えてから実装に入る。WS335 と並べて進め、
  WS335 の暗号の module の口に依る所は待ち合わせる（WS335 の設計の凍結を待ってから、その header を使う実装の Phase を始める）。

## ユーザーの決定

- 2026-11-01 ユーザーから、次の趣旨の指示を受けた。
  1. 互換の sshd と ssh を作る WS を立てる。
  2. 鍵は ed25519 だけ使えればよい。鍵の交換は最新の定番を 1 つ持ち、OpenSSH の sshd と自前の sshd の両方に接続できればよい。
  3. WS335 での決定（暗号の部品の口を WS335 が持つ、OpenSSH の libcrypto の互換は作らない）をこの WS に反映する。
  4. （2026-11-01 追記）暗号は WS335 に足した libcrypto-compat（libbrowser と自前の ssh・sshd が使う部分だけの OpenSSL 互換の API）を使う。
  5. WS335 と並行して進め、依存の所では待ち合わせる。標準廉価実装手順で実装する。P5 で始める。

## Resume point

- p001（設計）を P5 が q1791 で進めている。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（標準廉価実装手順 §4）: SSH の protocol の範囲（RFC 4251〜4254・8709 ほか）、鍵の交換の 1 つ、暗号の組、sshd と ssh の機能の範囲、module・header・骨格・試験の道具、実装の Phase の表、WS335 との待ち合わせ、design-reviewer、問い | in-progress | — |
