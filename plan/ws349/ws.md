<!-- awesome-plan project=zedbsd record=ws349 -->

# WS349: Raspberry Pi 5 の HDMI の音

Status: incomplete
Primary Milestone: MG006
Parent: [Master](../master.md)
Owner: Q1

## 目標

Pi 5（と Pi 500）は `/dev` に音の device が無く HDMI の音が出ない。

## ユーザーの決定（要点）

- 2026-11-13 に外部の利用者から Raspberry Pi 500（BCM2712 D0、8 GB）で zedBSD（c4cede8、2026-11-03 の source）が起動したという報告を受けた（Pi 500 の dtb を FAT に足して起動、desktop・日本語入力・Wi-Fi・有線・ブラウザ・SSH・shutdown は動作、HDMI の音は無し）。
- 2026-11-13 ユーザーから、Pi 500 の報告の #1・#6・#7 をそれぞれ WS に、#2・#3・#4 を Bug に（修正済みの疑いが強い）、#5（起動の直後の時計）は仕様、#8（build の手順）は見直して誤りがあれば直す、という趣旨の指示を受けた。

## Resume point

- 2026-11-13 Q1 が立てた。P1 が処理する。
- 2026-11-13 P1: p001 の設計の第一版（[phase.md](phase001/phase.md)）。次: design-reviewer、未確定の出典の確かめ（p002 の案）、実装の Phase を Q1 が立てる。

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: BCM2712 の HDMI audio の経路（HDMI の controller・DMA・audiod の device の口）と、Pi 4 の vc4 の HDMI audio との違い、出典（guardrail の値の規則）。設計の後に実装の Phase を立てる | cleared（2026-11-13 P1。設計の第一版、design-reviewer は未実施） | — |
