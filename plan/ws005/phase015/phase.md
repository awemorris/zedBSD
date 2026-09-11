# ws005-p015: oneshot network-enableと設定可能な起動待機

Phase ID: `ws005-p015`
Parent: [ws005](../ws.md)
Status: planning
Phase disposition: normal
Date: 2026-09-11
Primary Milestone: MG005
Focused Goal: fg005
Queue: none / 実装未承認

## 範囲・手順

networkd readyと既存net bootの設定適用に整合するoneshotを追加し、net lan enableとnet wifi enableを発行して共通状態を待つ。

共通要件と確定回答は `plan/ws005/network-improvements-2026-09-11.md`（ローカル計画、公開待ち）に記録する。

## 受け入れ・成果物

片方のIPで終了、既にIPあり、片方未搭載、ケーブルなし/プロファイルなし、遅い応答、既定30秒・設定変更・不正設定を確認する。enable要求と待機に一つのdeadlineを使い、timeout後も起動とdaemon接続処理が継続する。IP成功とtimeoutを観測上区別する。既存fd3 notify-timeoutをoneshot待機として誤用しない。

## 前提・判断・制約

[ws005-p013](../phase013/phase.md)/[ws005-p014](../phase014/phase.md)の要求受理・共通ready状態。DE購読は不要。
未決判断を確定してから有限Queueに選ぶ。[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則と既存のservice/net/WLAN契約を守る。Cコード生成前に全文該当規則をロードする。通常の技術判断は委任範囲で行い、HAL責務や未指定の機能へ範囲を拡大しない。
実行時には変更に対応するfocused checkと選択構成の `make -j16`、必要な通常系の確認を行う。aggregate `make check`、commit、pushはしない。既存の完了したp001〜p012の受け入れは保持する。
source/config・artifact hash・コマンド・結果・未実施・残条件を結果へ記録する。今回の作業は計画のみで、コード変更・実行確認は未実施。
