<!-- awesome-plan project=zedbsd record=ws280 -->

# WS280 H.264 の software decoder（libh264dec）

- Master: [plan/master.md](../master.md)（2026-10-21 作成、実装はまだしない）。Primary Milestone: MG006。Related: MG008（Pi 5）。
- Status: canceled（2026-10-30 ユーザー「WS280は取り下げます。」。H.264 の software decoder は [WS326](../ws326/ws.md) が libmedia の中に作る）
- 出典: 2026-10-21 ユーザー「HEVCはライセンス料が高いので、とりあえず実装しないでいいです。H.264のソフトウェアデコーダを、あとで実装するWSを作っておきましょう。既存のlibmediaにオプションでdlopenで繋ぎ込むlibh264decとして入れます。まだ実装しなくていいです。」

## 目的と完了の条件（案）

- hardware の H.264 decoder の無い機種（Raspberry Pi 5 など）で、H.264 の動画を CPU で decode して再生できる。
- 独立した library `libh264dec` として作り、既存の `userland/desktop/libmedia` が実行時に dlopen で使う（任意。無ければ今の道のまま。libmedia の vkvideo-runtime.c が libvulkan を dlopen するのと同じ形）。
- 範囲・対象の profile・速さの目標・license（独立実装、特許の扱い）は、着手の時にユーザーと決める。

## 範囲の外

- HEVC（H.265）: 実装しない（ユーザー「HEVCはライセンス料が高いので、とりあえず実装しないでいいです。」）。

## Phase

| Phase | 内容 | 依存 | 状態 |
| --- | --- | --- | --- |
| p001 | 設計（profile・level の範囲、libmedia との dlopen の口、Pi 5 の A76 での速さの見込み、license） | — | planning（着手はユーザーの指示まで） |

## 現在の状態・再開点

- 2026-10-21: 作成だけ。実装はユーザーの指示まで始めない。
