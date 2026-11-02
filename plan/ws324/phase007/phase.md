<!-- awesome-plan project=zedbsd record=ws324-p007 -->

# ws324-p007 H.265 decode の実機の debug（RTX 2070、NVDEC の HEVC）

Status: in-progress（2026-11-02 08:23 P1、q1922-i01）

- WS: [WS324](../ws.md)。設計: [design.md](../design.md) §3.2・§8.4・§8.6・§12 R4・§13・§14.3。前の Phase: [p005](../phase005/phase.md)（§6.4・§6.5 の観測と残り）。
- 基点: main `71dbb8425`（p005 と P2 の vkvideo-probe の H.265 を含む）。worktree `/home/awe/zedBSD-worktrees/p1-ws324`、branch `agent/p1-ws324`。
- 実機: 10.0.10.3 の RTX 2070（SwitchBot plug 1 `AC276E42B252`）。`/tmp/zedbsd-rdna.lock` の中で、各回の始めに NVMe の kernel と zedbsd.cfg を読み戻し、終わりに戻して cksum を照合し Debian に戻す（`plan/ws324/tests/rtx-video324.sh`）。

## 1. 目的

vkvideo-probe の H.265（`--h265 --expect`）の各 frame の SHA-256 を、host の ffmpeg の decode（framehash、nv12、sha256）と一致させる。対象は HEVC_v1 の適合の stream のうち decoder の能力（Main、8 bit、4:2:0、progressive、64×64〜4096×4096、level 5.1 まで）に入る物。

## 2. 範囲

1. p005 の残りの確かめ: CPU の窓の直しの後の skip の形（既定）と DXVA の形（`nvrtx.hevc.dxva=1`）、状態の語（CTB の誤りの数・error_status・slice header の code）の読み。
2. 誤りの原因を順に絞る: slice header の参照の集合の扱い（skip の bit 数・DXVA の欄）、filter の buffer の配置（縦の filter・SAO・BSD の行ごとの区画、上の deblock・上の SAO の offset）、co-located の MV の大きさ、scaling list の区画、tile の区画、参照の list・POC の差・picidx、行の揃え・面の offset。
3. 直しは nvrtx の側（`src/drivers/gpu/nvrtx/vk/vk-video-h265.c`・`vk-video.c`・`nvdec-records.c`・`nvrtx-nvdec.h`）と、要る時は host 試験（`plan/ws324/tests/hevc-host-test.*`）・実機の script（`plan/ws324/tests/rtx-video324.sh`）。vkvideo-probe（`userland/tests/vkvideo-probe/`）の変更は debug の出力（frame の dump）だけで、要る時は Q1 に知らせてから。
4. 範囲の外: libvulkan、Main10・4:4:4・SCC・field、encode（p008〜）、HAL、toolchain。

## 3. 出典

- 値と手順は GPL・MIT の公開の source から、block ごとに「Value(s) referenced:」「Procedure referenced:」で引用する（guardrail「MIT・GPL の出典のデータの引用」）。code・comment・構造・名前は写さない。取得物は scratchpad（repo に入れない）。
- 主な出典: NVIDIA open-gpu-doc `nvdec_drv.h`・`clc5b0.h`（MIT）、averne の FFmpeg の nvtegra（GPL-2.0+）、ITU-T H.265。他に要る時は GPL の Linux・FFmpeg・Mesa など、版と path を記録する。

## 4. 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| H.265 の stream ごとの表 | 実機で HEVC_v1 の能力内の stream（まず p005 の 6 本、次に広げる）ごとに MATCH か、MATCH でない物は原因（観測した事実と、残る見込み）を書いた表 |
| H.264 の非退行 | 実機で `rtx-video324.sh` の V1（h264-1080p30 600/600、h264-slices 3/3 ほか p005 と同じ結果） |
| timeout の後の隔離 | 起こせる時は実機で decoder の timeout を起こし、隔離の log（数と大きさ）と次の decode を確かめる。起こせない時はその旨 |
| build・host 試験 | `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws324-p007/k build/ws324-p007/k/vmunix` が warning 0、`sh plan/ws324/tests/hevc-host-test.sh`・`h265-wire-host-test.sh`・`plan/tools/rtx-video/nvdec-host-test.sh` が PASS |
| 機械の戻し | 各回の終わりに NVMe の kernel と zedbsd.cfg の cksum が始めと一致、Debian（vfio-pci） |

## 5. 判断を待つ点

- （無し）

## 6. 結果

（作業の中で書く）
