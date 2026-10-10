<!-- awesome-plan project=zedbsd record=ws203-p003 -->
# ws203-p003: 実機受入

Parent: [WS203](../ws.md)
Status: uncleared / normal
Queue: none (user hardware handoff)
Prerequisites: p001,p002 source integration and new SD image.
Acceptance: RPi4 onboard RJ45 cableでPHY/link/接続先に応じた10/100/1000 full-duplex negotiation、DHCP IPv4、ssh kei@assigned-IP、双方向traffic、ケーブル抜き差し。USB未動作でも実施可能。ログのgenet: init/PHY/en0/linkを確認。結果はuserが提供。現時点で実機成功を主張しない。

## 試験の入口

mainへの統合後、`CONFIG_DRIVER_BCM2711_GENET=y`（RPi4既定）でSDイメージを作り直す。RJ45を接続して起動し、`genet: at ...` → `genet: PHY ...` → `genet: en0 ready ...` → `genet: en0 RX/TX enabled` → `genet: en0 link up ...` を確認する。LAN側のDHCP leaseからIPを調べてSSH接続し、`ifconfig en0`・`dmesg`を取得する。ケーブル抜き差しでlink down/upを確認する。どのgenetログまで到達したかを残す。USB障害の詳しいログ取得はSSH疎通後の別件。

p001/p002の成果は[results](../tests/results.md)。このPhaseは未実行・plannedであり、main統合、新イメージ、ユーザー実機結果を待つ。

## 2026-10-11 prerequisite確認

mainへの実装統合は `d1def8aef` で確認済み。現在のRPi4 configはGENET既定ON。新しいSDイメージの作成とユーザー実機結果は未実施。このPhaseの実行状態・実機受入はplannedのまま。

## User UAT / startup investigation, 2026-10-11

User reports sshd now starts after the independent WS193 libutil fix, but DHCP address appears absent; later says link appears established and suspects LAN enable was not invoked. This is not yet an SSH-connectivity or DHCP acceptance result. [Read-only source/image comparison](../tests/network-startup-20261011.md) confirms equal amd64/RPi4 configs, enabled boot services and the existing automatic LAN_ENABLE → UP → DHCP path. Physical service state/output requested; no speculative config or driver changes. Phase remains planned for full hardware acceptance; no runtime success/clearance inferred from source inspection.

## Physical result / 2026-10-11

User写真でPHY認識後GENET initialization failed21、MMIO release failed3を確認。en0登録前なのでDHCP/SSH受入は未達、p003はuncleared。既存自動network起動定義は共通、net lan enable未実行との推測は写真で更新。[p004](../phase004/phase.md)が既存HAL IRQとMMIO契約を修正しsource/buildまでclear。新imageで有線LAN/DHCP/SSH再試験がresume条件、既存成果を実機成功とはしない。
