<!-- awesome-plan project=zedbsd record=ws160 -->

# WS160: su・sudo・passwd

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG002
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

su・sudo・passwd: 既存の login・crypt・shadow と setuid の扱い、規則の file の設計と実装（p001、T1-127）、Settings の Users の頁の password の変更（p002、T1-121）。

## 制限・移管

全文規約はベータ3。実機の UAT は残りの UAT。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws160-p001 | 設計（既存の login・crypt・shadow の扱い、setuid の kernel の対応、規則の file の形、Settings からの変更の経路）と passwd・s | cleared（2026-10-05） |
| ws160-p002 | Settings の Users の頁の password の変更（GUI）。Settings → libkeiland の kl_system_account → composi | cleared（2026-10-05） |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
