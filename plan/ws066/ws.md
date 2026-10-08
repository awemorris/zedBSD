<!-- awesome-plan project=zedbsd record=ws066 -->

# WS066: 動的 link の program の起動を速くする（`ld.so` の最適化）

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG002
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

動的 link の program の起動を速くした: amd64 の libc.so などを -Bsymbolic-functions と GNU hash に（p002）、libc の symbol の再配置 515 → 19。測定（T1-165）: true 約 680〜710 µs、sh -c 約 936 µs で受け入れ（true ≤ 700 µs、sh -c ≤ 950 µs）。

## 制限・移管

全文規約の見直しはベータ3。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws066-p001 | 起動の費用の内訳（`ld.so` の各段階、再配置の数、探索の回数）と、候補の選択・設計 | cleared（2026-10-08） |
| ws066-p002 | libc.so などの -Bsymbolic-functions と、base の program の GNU hash（amd64） | cleared（2026-10-05） |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
