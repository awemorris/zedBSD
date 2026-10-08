<!-- awesome-plan project=zedbsd record=ws179 -->

# WS179: UI のアクセントカラーを選べるように

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG006
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

UI のアクセントカラーを選べるように: light の 4 色と dark の 5 色、Settings・Files・検索・画面の keyboard・bar と標準 app が accent に従う（p001〜p003、T1-335・337・339・396）。

## 制限・移管

BUG-262 は別の ticket。全文規約はベータ3。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws179-p001 | 設計（design.md、色の表、accent を使う所の一覧、伝え方、KL の API）と実装: libkeiland・compositor の UI・Settings（Appe | cleared（2026-10-07） |
| ws179-p002 | Calendar・PDF Viewer・Phone・Mailer・Notes・Image Viewer の独自の accent を theme に従わせる（2026-10-07 Q | cleared（2026-10-07） |
| ws179-p003 | 規約の全文の見直し（WS の変えた C の全部、ベータ3）、AAT のシナリオ（`tests/scenarios/desktop/appearance/`）に accent | cleared（2026-10-08） → 全文規約はベータ3 |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
