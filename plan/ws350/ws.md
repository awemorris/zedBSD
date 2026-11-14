<!-- awesome-plan project=zedbsd record=ws350 -->

# WS350: Raspberry Pi 500 を動作の確かめた機種として記録する

Status: completed（2026-11-15 ユーザーの判断。Pi 500 は README と plan/tools.md の機種の表に「外部の報告で動作」として載った）
Primary Milestone: MG003
Parent: [Master](../master.md)
Owner: Q1

## 目標

外部の利用者の報告（下）で Pi 500 の動作が分かった。

## ユーザーの決定（要点）

- 2026-11-13 に外部の利用者から Raspberry Pi 500（BCM2712 D0、8 GB）で zedBSD（c4cede8、2026-11-03 の source）が起動したという報告を受けた（Pi 500 の dtb を FAT に足して起動、desktop・日本語入力・Wi-Fi・有線・ブラウザ・SSH・shutdown は動作、HDMI の音は無し）。
- 2026-11-13 ユーザーから、Pi 500 の報告の #1・#6・#7 をそれぞれ WS に、#2・#3・#4 を Bug に（修正済みの疑いが強い）、#5（起動の直後の時計）は仕様、#8（build の手順）は見直して誤りがあれば直す、という趣旨の指示を受けた。

## Resume point

- 2026-11-15 Q1: ユーザーの判断で completed。外部の利用者への返信の一覧は [reply-links.md](reply-links.md)（残す）。HDMI の音は [WS349](../ws349/ws.md)、dtb は [WS348](../ws348/ws.md)（completed）。

- 2026-11-13 Q1 が立てた。P1 が処理する。
- 2026-11-13 P1: p001 cleared（phase.md は完了の時に消した）。返信用の一覧は [reply-links.md](reply-links.md)。

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | docs の対応機種と plan/tools.md の機種の表に Pi 500（BCM2712 D0、8 GB）を「外部の報告で動作、2026-11-03 の source、dtb を足して起動」として載せる。分かっている事（HDMI の音が無い、WS349）と WS348 の後は dtb を足さずに起動する事を書く | cleared（2026-11-13 P1。README と plan/tools.md） | — |
