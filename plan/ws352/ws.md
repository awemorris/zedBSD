<!-- awesome-plan project=zedbsd record=ws352 -->

# WS352: desktop の idle の短い poll を無くす（compositor・input・client は事象を block して待つ）

Status: completed（2026-11-15 Q1。実装は WS353 と一緒に main に入り、受け入れは WS353 p016 の 5330 の idle で取った）
Primary Milestone: MG006
Related Milestones: MG004
Parent: [Master](../master.md)
Owner: Q1

## 目標

desktop が何もしていない時に、compositor（`/bin/wayland`）・input の thread・client（files・settings・ime ほか）が数 ms ごとに起きないようにする。[WS351](../ws351/ws.md) で tick を可変（idle で 100 Hz）にし時刻を 1 ms の精度にした所、5330 の idle が 4.61 W・PC2 46 %（WS333 p029 の 100 Hz 固定では 4.37 W・87 %）になった。WS351 の診断で、compositor の 2 つの thread が 1〜11 ms の timeout の poll を 10 秒に各 8000〜10000 回、touch pad がある間の input thread が 10 ms の poll（`userland/desktop/wayland/input-thread.c` の `INPUT_PAD_TICK_MS`）、client が 2〜22 ms の poll を繰り返していた。

## ユーザーの決定（要点）

- 2026-11-13 ユーザーから、idle の電力の未達は compositor（と client）の poll を直して解く（kernel の timer slack は足さない）、という趣旨の決定を受けた。
- 前からの決定（Keiland）: blocking の仕事は名前の付いた thread で、数を決めて持つ（fd の事象の loop ではなく）。compositor の描画は Vulkan だけ。

- 2026-11-13: ユーザーの決定で、待ち方の根本の作り直しを [WS353](../ws353/ws.md) で設計する。p002 は WS353 の設計を待つ。

- 2026-11-14: branch `agent/p1-ws352`（b05187933a を除く）は WS353 p005 で main に取り込んだ（merge 6b1c2de16a の中の 3e9c426f74）。p002 の残りは WS353 の各 Phase が引き継ぐ。

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | 解析: idle の時に起きる thread と timeout の一覧（compositor・input・client・daemon）、各々が何を待つべきか（事象・期限）、本来の形（事象が来るまで block、期限は本当に要る時だけ）。WS351 の wake-reason.patch で数える | cleared（2026-11-15 Q1。ユーザーの判断は 2026-11-13 の WS353 の設計の承認で済んだ） | — |
| p002 | 直しと試験（host・QEMU・5330 で idle の package・PC2 が WS333 p029 と同等、入力・描画・animation の応答が変わらない） | cleared（2026-11-15 Q1。直しは main（6b1c2de16a ほか）。受け入れの 5330 の idle は WS353 p016 が測った: 4.54 W・PC2 66〜74 %（WS352 fix4 4.52 W・73 % と同等、WS333 p029 4.37 W・87 % より下。thread ごとの起床は WS353 p016 で）） | p001 |
| p003 | 規約の全文の見直し | canceled（2026-11-15 Q1。規約の見直しは WS353 の規約の見直しの Phase に含める） | p002 |

## Resume point

- 2026-11-15 Q1: completed。p001・p002 cleared、p003 は WS353 の規約の見直しに含めて canceled。5330 の idle の PC2 が WS333 p029 に届かない残りは WS353 p016 の thread ごとの起床の数で追う。
