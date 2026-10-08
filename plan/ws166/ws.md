<!-- awesome-plan project=zedbsd record=ws166 -->

# WS166: IME の予測変換

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG006
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

IME の予測変換: 画面の keyboard の予測の候補・確定・学習（p002・p003、T1-196c、osk-guest の回帰も PASS）。

## 制限・移管

全文規約（p004）はベータ3。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws166-p001 | 要件と設計（画面キーボードへ改訂） | cleared（2026-10-05） |
| ws166-p002 | IME の側: 予測の生成、keiland_ime_status_v1 version 2、engine の predict・learn | cleared（2026-10-05） |
| ws166-p003 | 画面キーボードの「候補」タブ（読みの追跡、予測の表示、置き換えと学習） | cleared（2026-10-05） |
| ws166-p004 | T1（QEMU）と全文の規約 | planned（2026-10-06） → 全文規約はベータ3 |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
