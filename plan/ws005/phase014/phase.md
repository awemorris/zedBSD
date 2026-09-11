# ws005-p014: net lanとnetworkdの有線LAN管理

Phase ID: `ws005-p014`
Parent: [ws005](../ws.md)
Status: planning
Phase disposition: normal
Date: 2026-09-11
Primary Milestone: MG005
Focused Goal: fg005
Queue: none / 実装未承認

## 範囲・手順

net CLI/protocolとnetworkdに有線の管理意図を接続し、既存linkイベント・ifconfig・dhcpc・net.confを使ってケーブル挿抜後のIP設定を常駐制御する。共通の状態snapshot/queryを整える。

共通要件と確定回答は `plan/ws005/network-improvements-2026-09-11.md`（ローカル計画、公開待ち）に記録する。

## 受け入れ・成果物

enable→後挿し→IP、接続済みenable、static/DHCP、抜線/再接続、disable後の再挿し、重複enable、デバイス再列挙で一貫した状態と子プロセス所有権を確認する。対象外/手動のIP・route・DNSを勝手に削除しない。既存Wi-Fiとconfirmed commitの意味を維持する。

## 前提・判断・制約

[ws005-p013](../phase013/phase.md)の対象範囲・状態/設定/所有権契約。
未決判断を確定してから有限Queueに選ぶ。[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則と既存のservice/net/WLAN契約を守る。Cコード生成前に全文該当規則をロードする。通常の技術判断は委任範囲で行い、HAL責務や未指定の機能へ範囲を拡大しない。
実行時には変更に対応するfocused checkと選択構成の `make -j16`、必要な通常系の確認を行う。aggregate `make check`、commit、pushはしない。既存の完了したp001〜p012の受け入れは保持する。
source/config・artifact hash・コマンド・結果・未実施・残条件を結果へ記録する。今回の作業は計画のみで、コード変更・実行確認は未実施。
