# ws005-p013: ネットワーク改善の共通契約・Wi-Fi enable仕様確認

Phase ID: `ws005-p013`
Parent: [ws005](../ws.md)
Status: planning
Phase disposition: normal
Date: 2026-09-11
Primary Milestone: MG005
Focused Goal: fg005
Queue: none / 実装未承認

## 範囲・手順

net/main.cのwifi_command、networkdのwifi_request_enableとmanaged-wlan、net.conf、service定義を照合し、要求受理・管理意図・carrier/L2・IP準備・disabledを区別した状態表を作る。

共通要件と確定回答は `plan/ws005/network-improvements-2026-09-11.md`（ローカル計画、公開待ち）に記録する。

## 受け入れ・成果物

有線対象と手動/boot設定の優先関係、root Wi-Fiポリシー、複数NICのroute/DNS、通知方式とready query、timeout設定・エラー処理・DE受信境界を設計する。ユーザー確定要件と提案を区別し、p014〜p016が同じ状態定義を使える文書を完成させる。

## 前提・判断・制約

現行sourceと今回のユーザー指示。実装はこのPhaseの目的ではない。
未決判断を確定してから有限Queueに選ぶ。[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則と既存のservice/net/WLAN契約を守る。Cコード生成前に全文該当規則をロードする。通常の技術判断は委任範囲で行い、HAL責務や未指定の機能へ範囲を拡大しない。
実行時には変更に対応するfocused checkと選択構成の `make -j16`、必要な通常系の確認を行う。aggregate `make check`、commit、pushはしない。既存の完了したp001〜p012の受け入れは保持する。
source/config・artifact hash・コマンド・結果・未実施・残条件を結果へ記録する。今回の作業は計画のみで、コード変更・実行確認は未実施。
