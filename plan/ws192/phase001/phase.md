<!-- awesome-plan project=zedbsd record=ws192-p001 -->

# ws192-p001: 状態の島の操作パネル（glass）の設計と実装

Status: planned（2026-10-09 Q1、q917 P1）
Parent: [WS192](../ws.md)

## 手順

1. 今の右上の状態の島（compositor の bar の右端の icon 群）、各 icon の click の動作（WiFi・音量・IME・Bluetooth・電池・通知）、既存の glass の描画（例 notify-popup・App Home 等で使っている物）を読む。
2. パネルの設計を phase.md に書く: 項目と並び、寸法（指で押せる大きさ、最小 44 px 相当を目安）、開閉（島の tap・外の tap・Esc）、mouse と touch、多画面の時に開く画面、既存の個別 popup との関係。境界（app → libkeiland → compositor → backend）を守る。
3. 実装と host 試験、build warning 0。AAT の scenario の案を T1 の依頼として足す（p002）。

## 受け入れ

- 設計の記録、build warning 0、host 試験 PASS、T1 の依頼の行。
