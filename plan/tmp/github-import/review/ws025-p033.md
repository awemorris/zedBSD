# ws025-p033: AX211 の自動接続・DHCP・復旧回帰修正

日付: 2026-09-09
Phase ID: `ws025-p033`
Status: completed; q124。修正・host gate・実 AX211 VFIO 受け入れ完了。詳細は [results.md](results.md)。
Parent: [WS025](../ws.md)
依存: [p032](../phase032/phase.md) の PC-98 起動回復。

## 目的

10.0.10.25 の AX211 を amd64 QEMU へ PCI パススルーし、
`net wifi enable` の後に `net wifi set-key SSID KEY auto` を実行しても
自動接続・DHCP アドレス取得に至らず、復旧できなくなるという報告を再現・修正する。
遅れて表示される wlan0 activation が、L2 完了・DHCP 起動・lease 適用の
どの段階に対応するかを時刻付きで確認する。原因をドライバと決めつけない。

## 実行環境

既存 `plan/ws004/tests/run-intel-ax211-vfio-qemu.sh` を基準にする。
実行前に SSH 経路が AX211 と独立、IOMMU group が隔離済み、reset 可能であることを
確認する。ゲストは disposable image、終了時は iwlwifi と経路を復元・検証する。
初期読取りで 8086:51f0 / 8086:4090 / revision 01、iwlwifi、独立した USB Ethernet
経路と QEMU、sudo が確認できた。IOMMU group 11 の singleton と reset、KVM、OVMF も確認済み。
SSID・キーは計画書や公開ログへ書かず、既に提供された実験用設定を使う。

## 手順

1. 現行ソースの net → networkd → WLAN 管理 → wifi child → dhcpc の状態遷移、
   設定世代、子プロセスの再利用・取消し・timeout を静的に追う。
2. 現行通常ビルドのゲストで報告順序を再現し、コマンド受付、scan/association、
   WPA 完了、DHCP DISCOVER/OFFER/REQUEST/ACK、アドレス適用を区別して記録する。
3. 原因箇所を修正する。enable の反復で状態を壊さず、キー更新を稼働中の管理へ
   反映し、失敗した試行の子・タイマー・世代が次の試行を妨げないようにする。
4. dhcpc が選択可能な初回 OFFER を受信した時点のコンソール通知を追加する。
   interface・提示アドレス・server を表示し、取得完了とは区別する。
   不正 packet、別 transaction の OFFER、重複再送を成功通知しない。
   長期運用で通知が増殖しない単位（取得試行）を実装時に明確化する。
5. 下記の実デバイス受け入れと既存 Wi-Fi30 を実行し、通常成果物を復元する。
   修正原因と結果は results.md、再利用 fixture は WS tests、使い捨て証拠は temp/p033-*。

## 受け入れ

- enable → set-key auto: 追加の enable や手動 dhcpc なしで L2 と DHCP が成立する。
- set-key auto → enable: 同様に成立する。
- enable の反復、接続中の enable、disable → enable で状態が破損しない。
- 誤ったキー → 正しいキーで再起動せず復旧し、古い試行が新しい状態を上書きしない。
- DHCP 応答なし → 応答再開で bounded retry/復旧でき、子プロセスが増殖しない。
- scan/association 中の disable と再 enable が復旧し、不要な dhcpc が残らない。
- OFFER 通知と ACK/lease 適用のログを区別でき、アドレス・route と LAN peer 疎通を確認する。
- 上記基本順序を独立したゲスト起動で再確認する。既存 Wi-Fi30 の全シナリオも合格する。
- 終了後にホストの driver_override、iwlwifi、SSH 経路、VFIO 所有解放を確認する。

実デバイスで成立しない項目を host test の合格で代替しない。
ビルド・テスト・runtime は直列、make -j16、commit・aggregate make check・.internal 参照なし。
現在の coding-style.md とリファクタリング済みソースを基準とする。
p027–p030 は本 Phase 完了後も既存の採用条件を維持する。
