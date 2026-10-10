<!-- awesome-plan project=zedbsd record=ws203-p003 -->
# ws203-p003: 実機受入

Parent: [WS203](../ws.md)
Status: planned / normal
Queue: none (user hardware handoff)
Prerequisites: p001,p002 source integration and new SD image.
Acceptance: RPi4 onboard RJ45 cableでPHY/link/接続先に応じた10/100/1000 full-duplex negotiation、DHCP IPv4、ssh kei@assigned-IP、双方向traffic、ケーブル抜き差し。USB未動作でも実施可能。ログのgenet: init/PHY/en0/linkを確認。結果はuserが提供。現時点で実機成功を主張しない。

## 試験の入口

mainへの統合後、`CONFIG_DRIVER_BCM2711_GENET=y`（RPi4既定）でSDイメージを作り直す。RJ45を接続して起動し、`genet: at ...` → `genet: PHY ...` → `genet: en0 ready ...` → `genet: en0 RX/TX enabled` → `genet: en0 link up ...` を確認する。LAN側のDHCP leaseからIPを調べてSSH接続し、`ifconfig en0`・`dmesg`を取得する。ケーブル抜き差しでlink down/upを確認する。どのgenetログまで到達したかを残す。USB障害の詳しいログ取得はSSH疎通後の別件。

p001/p002の成果は[results](../tests/results.md)。このPhaseは未実行・plannedであり、main統合、新イメージ、ユーザー実機結果を待つ。

## 2026-10-11 prerequisite確認

mainへの実装統合は `d1def8aef` で確認済み。現在のRPi4 configはGENET既定ON。新しいSDイメージの作成とユーザー実機結果は未実施。このPhaseの実行状態・実機受入はplannedのまま。
