<!-- awesome-plan project=zedbsd record=ws339 -->

# WS339: x86 の PC の UEFI の起動の画面の解像度を monitor に合わせる

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG004。
- Status: planning（2026-11-01 09:16 新設）
- 目標: amd64 の UEFI の loader（bootloader/uefi/video.c）が、GPU の driver が動く前の画面（起動の splash と文字の console）の mode を、monitor の最適な解像度（GOP の EDID、EFI_EDID_ACTIVE_PROTOCOL で読める時）を第一に選ぶ。読めなければ今と同じ（1920x1080 を好み、無ければ firmware のまま）。あわせて amd64 の標準の起動の設定（platform/amd64/zedbsd.cfg・zedbsd-native-uefi.cfg）の `video=640x480` を外す。
- 範囲の外: desktop の解像度（GPU の driver が決める）、amd64 以外の platform。
- 受け入れ: QEMU（OVMF）と 5330 で、EDID の最適な解像度で splash と console が出る事（boot-test.sh の login の確かめ）。EDID が読めない firmware で今と同じ動きになる事。
- Resume point: p001（実装と試験、担当の割当はユーザーの指示で）。

## ユーザーの決定（要点）

- 2026-11-01 09:16 ユーザーから、起動の画面の解像度は monitor に合わせる（EDID を第一にし、`video=640x480` を外す）、という趣旨の答えを受けた（Q1 の推しの案）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | loader の EDID による mode の選び方と、標準の設定の video= の除去、試験 | in-progress（2026-11-15 P5 q2119） | — |
