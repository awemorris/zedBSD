<!-- awesome-plan project=zedbsd record=ws352 -->

# WS352: desktop の idle の短い poll を無くす（compositor・input・client は事象を block して待つ）

Status: planned
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

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | 解析: idle の時に起きる thread と timeout の一覧（compositor・input・client・daemon）、各々が何を待つべきか（事象・期限）、本来の形（事象が来るまで block、期限は本当に要る時だけ）。WS351 の wake-reason.patch で数える | in-progress（一覧の確かめ済み、ユーザーの判断待ち） | — |
| p002 | 直しと試験（host・QEMU・5330 で idle の package・PC2 が WS333 p029 と同等、入力・描画・animation の応答が変わらない） | in-progress（直しの一部が branch に、試験は未完） | p001 |
| p003 | 規約の全文の見直し | planned | p002 |
