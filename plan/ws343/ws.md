<!-- awesome-plan project=zedbsd record=ws343 -->

# WS343 base の TLS と ssh を自前に: CI の config から OpenSSH・OpenSSL・curl・libcurl を外し、base に自前の ssh・sshd・libssl.so・libcrypto.so、fetch の https

- Master: [plan/master.md](../master.md)（2026-11-02 10:52 作成）。Primary Milestone: MG005。
- Status: planning（WS335 の completed と WS336 の後に始める）
- 目標:
  - 各 CI の config（config/ci/*.mk。ユーザーの物の config-amd64-server.mk は除き、ユーザーに確かめる）から OpenSSH・OpenSSL・curl・libcurl の package を外す。
  - 代わりに base に、自前の ssh・sshd（[WS336](../ws336/ws.md)）と、libssl.so・libcrypto.so（[WS335](../ws335/ws.md) の libssl-compat・libcrypto-compat）を入れる。
  - base の fetch が、互換の libssl・libcrypto で https を使えるようにする。
  - ca-certificates は packages に残し、自前の実装（libssl・ssh・fetch・ブラウザ）もそれを参照する。

## ユーザーの決定

- （2026-11-02 10:52）ユーザーから、上の目標の趣旨の指示を受けた（WS335 の完了の後、互換の ssh・sshd の完成と合わせて）。

## 関係する WS

- [WS335](../ws335/ws.md)（libssl-compat・libcrypto-compat）、[WS336](../ws336/ws.md)（自前の ssh・sshd）、[WS334](../ws334/ws.md)（Windows 版のブラウザの切り替え）、WS074（ブラウザの net/tls.c、WS335 p036）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws343-p001 | 調べと設計: CI の config ごとの package の依存（curl・libcurl・OpenSSL を使う program の洗い出し）、base に入れる名前（libssl.so・libcrypto.so の soname と互換の範囲）、fetch の https、ca-certificates の場所、menuconfig と MAC-T001 の試験 | planning | WS335 completed、WS336 |

## Resume point

- 2026-11-02 10:52 Q1: 作成。WS335 の completed と WS336 の ssh・sshd の完成の後に p001 を割り当てる。
