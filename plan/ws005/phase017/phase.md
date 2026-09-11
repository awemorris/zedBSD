# ws005-p017: ネットワーク改善の統合受け入れと最終規約確認

Phase ID: `ws005-p017`
Parent: [ws005](../ws.md)
Status: planning
Phase disposition: normal
Date: 2026-09-11
Primary Milestone: MG005
Focused Goal: fg005
Queue: none / 実装未承認

## 範囲・手順

有線管理・Wi-Fi enable・起動oneshot・DE通知を最終sourceで通し、今回変更したsource/config/service/docsの全文規約適合と検証結果を確認する。

共通要件と確定回答は `plan/ws005/network-improvements-2026-09-11.md`（ローカル計画、公開待ち）に記録する。

## 受け入れ・成果物

起動→両方式enable→どちらかIP→DE表示、起動timeout→後挿し/遅延接続→IP→DE更新、disable→通知の一貫性を確認する。適用した全文規則、formatter、focused check/build、source/config/hash、結果と未実施を記録する。後続変更で無効になった範囲を再確認し、旧完了Phaseを理由なく再試験しない。

## 前提・判断・制約

[ws005-p014](../phase014/phase.md)/[ws005-p015](../phase015/phase.md)/[ws005-p016](../phase016/phase.md)の成果。
未決判断を確定してから有限Queueに選ぶ。[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則と既存のservice/net/WLAN契約を守る。Cコード生成前に全文該当規則をロードする。通常の技術判断は委任範囲で行い、HAL責務や未指定の機能へ範囲を拡大しない。
実行時には変更に対応するfocused checkと選択構成の `make -j16`、必要な通常系の確認を行う。aggregate `make check`、commit、pushはしない。既存の完了したp001〜p012の受け入れは保持する。
source/config・artifact hash・コマンド・結果・未実施・残条件を結果へ記録する。今回の作業は計画のみで、コード変更・実行確認は未実施。
