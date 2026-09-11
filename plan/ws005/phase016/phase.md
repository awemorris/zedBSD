# ws005-p016: networkd状態通知とデスクトップ受信

Phase ID: `ws005-p016`
Parent: [ws005](../ws.md)
Status: planning
Phase disposition: normal
Date: 2026-09-11
Primary Milestone: MG005
Focused Goal: fg005
Queue: none / 実装未承認

## 範囲・手順

read-only通知APIの方式・パス・レコード・権限を確定し、networkdの共通状態をsnapshotと変更通知で提供する。現行DEの受信・状態表示箇所を特定して接続する。

共通要件と確定回答は `plan/ws005/network-improvements-2026-09-11.md`（ローカル計画、公開待ち）に記録する。

## 受け入れ・成果物

接続/切断・admin状態・IP取得/喪失を正しく通知し、DEの初回起動・再接続・daemon再起動・欠落後に一致した状態へ戻る。遅い/停止したDEがdaemonやDHCPを止めない。秘密情報を含まず、状態閲覧から制御権限を得られない。制限付きのキュー/再同期を確認する。

## 前提・判断・制約

[ws005-p013](../phase013/phase.md)/[ws005-p014](../phase014/phase.md)の共通状態モデル。socketは第一候補であり固定済みのユーザー指定ではない。
未決判断を確定してから有限Queueに選ぶ。[guardrail](../../guardrail.md)、`plan/coding-style.md`全文の該当規則と既存のservice/net/WLAN契約を守る。Cコード生成前に全文該当規則をロードする。通常の技術判断は委任範囲で行い、HAL責務や未指定の機能へ範囲を拡大しない。
実行時には変更に対応するfocused checkと選択構成の `make -j16`、必要な通常系の確認を行う。aggregate `make check`、commit、pushはしない。既存の完了したp001〜p012の受け入れは保持する。
source/config・artifact hash・コマンド・結果・未実施・残条件を結果へ記録する。今回の作業は計画のみで、コード変更・実行確認は未実施。
