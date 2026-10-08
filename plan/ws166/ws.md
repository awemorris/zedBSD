<!-- awesome-plan project=zedbsd record=ws166 -->
# WS166: IME の予測変換

Status: incomplete（2026-10-08 q910 P2 の照合: p002・p003 は cleared（表を直した）、p004（規約）はベータ3。実装の残りは無い。WS の完了を Q1 が判定）（2026-10-05 追加、**ベータ2**（2026-10-05 ユーザー）、見積もり 3 LW）。q768（P2）: 画面キーボードの予測を実装済み、T1 の試験待ち
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-05）「予測変換の実装」

## 範囲

ユーザー（2026-10-05、Q1 経由）「予測変換はインラインの通常IMEではなく、オンスクリーンキーボードのことでした。…もし作ってくれてしまったなら、通常IMEでは、オプションで有効にできるようにしましょう。」

- 画面キーボード（WS102 の flick panel）の「候補」タブに、打った仮名の読みから予測した語を出し、tap で読みを置き換える（smartphone の予測と同じ）。辞書と学習は IME の process の日本語の engine。
- 通常の IME のインラインの予測は Future Work の F-078（既定 off の option、後の Phase、Q1 2026-10-05）。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws166-p001](phase001/phase.md) | 要件と設計（画面キーボードへ改訂） | cleared（2026-10-05、Q1 が改訂の設計を承認） | — |
| [ws166-p002](phase002/phase.md) | IME の側: 予測の生成、keiland_ime_status_v1 version 2、engine の predict・learn | cleared（2026-10-05 Q1: T1-196c PASS（QEMU、画面キーボードの予測の候補・確定・学習）、osk-guest の回帰も T1-196 で PASS）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（実装済み、host 試験済み。QEMU は p004）） | p001 |
| [ws166-p003](phase003/phase.md) | 画面キーボードの「候補」タブ（読みの追跡、予測の表示、置き換えと学習） | cleared（2026-10-05 Q1: T1-196c PASS（QEMU、画面キーボードの予測の候補・確定・学習）、osk-guest の回帰も T1-196 で PASS）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（実装済み、QEMU は p004）） | p002 |
| [ws166-p004](phase004/phase.md) | T1（QEMU）と全文の規約 | planned | p002, p003 |
