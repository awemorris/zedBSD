<!-- awesome-plan project=zedbsd record=ws324-p007 -->

# ws324-p007 H.265 decode の実機の debug（RTX 2070、NVDEC の HEVC）

Status: in-progress（2026-11-02 11:15 P1、q1922-i01: 受け入れの項目を全部流した。HEVC_v1 の 147 本のうち能力の中の 131 本で 130 本が正しい、1 本（WPP_D）は原因を書いた DIFFER。cleared の判定は Q1）

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

- （無し）。範囲の追加（2026-11-02 Q1 経由のユーザーの指示）: RTX の Video Player の GPU の道（`VP_GPU_PRESENT`）で H.264・H.265 の再生、frame の数・late・画面の PNG を報告する。libmedia の H.265 の back end は P2（ws322-p006）の物を main から使う。

## 6. 結果

### 6.1 commit（branch `agent/p1-ws324`）

- `ba477ad13b`: vkvideo-probe `--raw=FILE`（H.265 の各 frame の表示の窓を生の NV12 で書く、新しい `frame-raw.c/.h`）。
- `aa9d81a6a8`: vkvideo-probe `--copy`（新しい `copy-out.c/.h`: 出す時に vkCmdCopyImageToBuffer で行の並ぶ buffer へ写して hash する。nvrtx の video の image は block linear で、subresource の layout が空なので、写像を直に読む今までの道では hash が意味を持たなかった）。`main.c`・`h265.c`・`probe.h` は option と呼び出しの数行だけ（P2 の code、Q1 に知らせた）。
- `0471507afe`: driver の直し 3 つ（下の 6.2）、状態の語の log、`nvrtx.hevc.trace=1`（session の最初の 3 枚の picture 構造・push・状態を log に出す診断）、copy-out の host memory への写し（uncached の BAR を 1 byte ずつ読むと 1080p で 1 枚約 5 s かかった）、試験の道具（下の 6.4）、host 試験の追従。
- `4b072f451d`: main（`efee1fb83`、P2 の libmedia の H.265 の back end を含む）の merge。kernel の cksum は merge の前後で同じ（3900996930）。
- この後の commit: この phase.md、ws.md、design.md §8.4・§8.6、`rtx-video324.sh` の Video Player の段（SHA は Q1 への報告に書く）。
- HAL・`include/uapi/`・toolchain・libvulkan は変えていない。

### 6.2 driver の直し（`src/drivers/gpu/nvrtx/nvdec-records.c`、実機で 1 つずつ確かめた）

| 直し | 観測（直す前） | 確かめ |
| --- | --- | --- |
| `nvdec_hevc_main10_444_ext_s.log2MaxTransformSkipSize`（byte 224 の bit 3:0）を 2 にする | transform_skip_enabled の stream（能力の中の 130 本のうち 108 本）で IDR の CTB 0 の後から全部ずれる（CAINIT の IDR は CTB (7,4) で error 0x5）。文書の注記は「minus2」だが decoder は log2 の大きさとして読む | run4 の variant で TSKIP_A の IDR が MATCH、CAINIT の IDR の誤りが消えた |
| scaling list を各 list とも列ごと（転置）に書く | SLIST_A の IDR で luma だけが全部違い chroma は一致（16x16・32x32 の list が対称でない） | run7 の variant で SLIST_A が全 65 枚 MATCH |
| filter の buffer の上の deblock・上の SAO の部分を「16x16 の block ごとに 320 byte」以上（上の SAO は 4×768 byte／列以上）にする | CTB の行の境の上 4 行（例: CAINIT の y 316〜319）が壊れる。上の deblock は 640 byte／列では足りず（CTB の行ごとに約 150 byte／16 画素の列ずつ使い進む見込み）、後ろの上の SAO の書き込みが上書きする。上の SAO も 768 byte／列では buffer の末尾を越える（TILES_A の y 828〜831）。CTB 16 の SAODBLK では面積に比例する大きさでも足りず | run5（×4 の variant で TILES・CAINIT の IDR が一致）、run9・run11（SAODBLK の誤りが消えた） |

出典: 値の位置は NVIDIA open-gpu-doc `nvdec_drv.h`（MIT、block ごとに「Value referenced:」）。大きさと読みの向きは実機の観測で、source の comment には hardware の振る舞いとして書いた（開発の管理情報は書いていない）。

### 6.3 実機（RTX 2070、10.0.10.3。QEMU は使っていない。2026-11-02 08:24〜11:14、`/tmp/zedbsd-rdna.lock` の中で 13 回）

- 各回とも始めに NVMe の kernel（1463007375）と zedbsd.cfg（38198411）を読み戻し、終わりに戻して cksum 一致（restore MATCH、13 回とも）、Debian（vfio-pci）に戻した。run6 は 1080p の stream の読み戻しが遅く（1 本 120 s の上限）止め、`rtx-restore324.sh` で戻した（MATCH）。最後の機械の状態: Debian、NVMe の kernel 1463007375。
- 最後の kernel は `build/ws324-p007/k/vmunix` cksum 3900996930（`0471507afe` と merge `4b072f451d` で同じ）、programs は `build/ws324-p007/v`（merge 後の tree、warning 0）。

**H.265（run13、11:05〜11:14、最後の kernel、HEVC_v1 の 147 本の全部の frame）**: ffmpeg の framehash（nv12、sha256）と 127 本が全 frame 一致。SAODBLK_A・B は適合の zip の md5 と、VPSSPSPPS_A は絵ごとの参照と一致（下の表）。能力の外で probe が断った物 16 本（Main 10 が 11、Main Still Picture が 1、4096 を越える大きさが 4）。DIFFER は WPP_D_ericsson_MAIN_2 の 1 本（原因は表）。どの回も decoder の timeout・hang・channel の誤り・fault（boot の vram-window の既知の行の他）は無かった。

| stream | decoded | output | match（ffmpeg） | result |
| --- | --- | --- | --- | --- |
| AMP_A_Samsung_7 | 17 | 17 | 17 | MATCH |
| AMP_B_Samsung_7 | 17 | 17 | 17 | MATCH |
| AMP_D_Hisilicon_3 | 100 | 100 | 100 | MATCH |
| AMP_E_Hisilicon_3 | 100 | 100 | 100 | MATCH |
| AMP_F_Hisilicon_3 | 100 | 100 | 100 | MATCH |
| AMVP_A_MTK_4 | 30 | 30 | 30 | MATCH |
| AMVP_B_MTK_4 | 41 | 41 | 41 | MATCH |
| AMVP_C_Samsung_7 | 60 | 60 | 60 | MATCH |
| BUMPING_A_ericsson_1 | 66 | 49 | 49 | MATCH |
| CAINIT_A_SHARP_4 | 50 | 50 | 50 | MATCH |
| CAINIT_B_SHARP_4 | 50 | 50 | 50 | MATCH |
| CAINIT_C_SHARP_3 | 50 | 50 | 50 | MATCH |
| CAINIT_D_SHARP_3 | 50 | 50 | 50 | MATCH |
| CAINIT_E_SHARP_3 | 50 | 50 | 50 | MATCH |
| CAINIT_F_SHARP_3 | 50 | 50 | 50 | MATCH |
| CAINIT_G_SHARP_3 | 50 | 50 | 50 | MATCH |
| CAINIT_H_SHARP_3 | 50 | 50 | 50 | MATCH |
| CIP_A_Panasonic_3 | 2 | 2 | 2 | MATCH |
| CIP_C_Panasonic_2 | 2 | 2 | 2 | MATCH |
| CONFWIN_A_Sony_1 | 60 | 60 | 60 | MATCH |
| DBLK_A_MAIN10_VIXS_4 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| DBLK_A_SONY_3 | 30 | 30 | 30 | MATCH |
| DBLK_B_SONY_3 | 30 | 30 | 30 | MATCH |
| DBLK_C_SONY_3 | 30 | 30 | 30 | MATCH |
| DBLK_D_VIXS_2 | 8 | 8 | 8 | MATCH |
| DBLK_E_VIXS_2 | 8 | 8 | 8 | MATCH |
| DBLK_F_VIXS_2 | 8 | 8 | 8 | MATCH |
| DBLK_G_VIXS_2 | 8 | 8 | 8 | MATCH |
| DELTAQP_A_BRCM_4 | 96 | 96 | 96 | MATCH |
| DELTAQP_B_SONY_3 | 30 | 30 | 30 | MATCH |
| DELTAQP_C_SONY_3 | 30 | 30 | 30 | MATCH |
| DSLICE_A_HHI_5 | 50 | 50 | 50 | MATCH |
| DSLICE_B_HHI_5 | 50 | 50 | 50 | MATCH |
| DSLICE_C_HHI_5 | 50 | 50 | 50 | MATCH |
| ENTP_A_QUALCOMM_1 | 24 | 24 | 24 | MATCH |
| ENTP_B_Qualcomm_1 | 24 | 24 | 24 | MATCH |
| ENTP_C_Qualcomm_1 | 24 | 24 | 24 | MATCH |
| EXT_A_ericsson_4 | 3 | 3 | 3 | MATCH |
| FILLER_A_Sony_1 | 60 | 60 | 60 | MATCH |
| HRD_A_Fujitsu_3 | 96 | 96 | 96 | MATCH |
| INITQP_A_Sony_1 | 60 | 60 | 60 | MATCH |
| INITQP_B_Main10_Sony_1 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| IPRED_A_docomo_2 | 20 | 20 | 20 | MATCH |
| IPRED_B_Nokia_3 | - | - | 0 | 能力の外: Main Still Picture（profile_idc 3）、probe が断る |
| IPRED_C_Mitsubishi_3 | 4 | 4 | 4 | MATCH |
| LS_A_Orange_2 | 9 | 9 | 9 | MATCH |
| LS_B_Orange_4 | 25 | 25 | 25 | MATCH |
| LTRPSPS_A_Qualcomm_1 | 500 | 500 | 500 | MATCH |
| MAXBINS_A_TI_5 | 2 | 2 | 2 | MATCH |
| MAXBINS_B_TI_5 | 3 | 3 | 3 | MATCH |
| MAXBINS_C_TI_5 | 3 | 3 | 3 | MATCH |
| MERGE_A_TI_3 | 8 | 8 | 8 | MATCH |
| MERGE_B_TI_3 | 8 | 8 | 8 | MATCH |
| MERGE_C_TI_3 | 8 | 8 | 8 | MATCH |
| MERGE_D_TI_3 | 8 | 8 | 8 | MATCH |
| MERGE_E_TI_3 | 8 | 8 | 8 | MATCH |
| MERGE_F_MTK_4 | 41 | 41 | 41 | MATCH |
| MERGE_G_HHI_4 | 2 | 2 | 2 | MATCH |
| MVCLIP_A_qualcomm_3 | 5 | 5 | 5 | MATCH |
| MVDL1ZERO_A_docomo_4 | 500 | 500 | 500 | MATCH |
| MVEDGE_A_qualcomm_3 | 17 | 17 | 17 | MATCH |
| NUT_A_ericsson_5 | 34 | 34 | 34 | MATCH |
| NoOutPrior_A_Qualcomm_1 | 43 | 40 | 40 | MATCH |
| NoOutPrior_B_Qualcomm_1 | 50 | 47 | 47 | MATCH |
| OPFLAG_A_Qualcomm_1 | 500 | 500 | 500 | MATCH |
| OPFLAG_B_Qualcomm_1 | 100 | 98 | 98 | MATCH |
| OPFLAG_C_Qualcomm_1 | 100 | 96 | 96 | MATCH |
| PICSIZE_A_Bossen_1 | - | - | 0 | 能力の外: 4096 を越える大きさ（1056x8440・8440x1056・528x4216・4216x528）、probe が断る |
| PICSIZE_B_Bossen_1 | - | - | 0 | 能力の外: 4096 を越える大きさ（1056x8440・8440x1056・528x4216・4216x528）、probe が断る |
| PICSIZE_C_Bossen_1 | - | - | 0 | 能力の外: 4096 を越える大きさ（1056x8440・8440x1056・528x4216・4216x528）、probe が断る |
| PICSIZE_D_Bossen_1 | - | - | 0 | 能力の外: 4096 を越える大きさ（1056x8440・8440x1056・528x4216・4216x528）、probe が断る |
| PMERGE_A_TI_3 | 8 | 8 | 8 | MATCH |
| PMERGE_B_TI_3 | 8 | 8 | 8 | MATCH |
| PMERGE_C_TI_3 | 8 | 8 | 8 | MATCH |
| PMERGE_D_TI_3 | 8 | 8 | 8 | MATCH |
| PMERGE_E_TI_3 | 8 | 8 | 8 | MATCH |
| POC_A_Bossen_3 | 5 | 5 | 5 | MATCH |
| PPS_A_qualcomm_7 | 81 | 81 | 81 | MATCH |
| PS_B_VIDYO_3 | 25 | 25 | 25 | MATCH |
| RAP_A_docomo_6 | 86 | 86 | 86 | MATCH |
| RAP_B_Bossen_2 | 83 | 80 | 80 | MATCH |
| RPLM_A_qualcomm_4 | 300 | 300 | 300 | MATCH |
| RPLM_B_qualcomm_4 | 300 | 300 | 300 | MATCH |
| RPS_A_docomo_5 | 44 | 44 | 44 | MATCH |
| RPS_B_qualcomm_5 | 300 | 300 | 300 | MATCH |
| RPS_C_ericsson_5 | 40 | 40 | 40 | MATCH |
| RPS_D_ericsson_6 | 68 | 68 | 68 | MATCH |
| RPS_E_qualcomm_5 | 300 | 300 | 300 | MATCH |
| RPS_F_docomo_2 | 50 | 50 | 50 | MATCH |
| RQT_A_HHI_4 | 2 | 2 | 2 | MATCH |
| RQT_B_HHI_4 | 2 | 2 | 2 | MATCH |
| RQT_C_HHI_4 | 2 | 2 | 2 | MATCH |
| RQT_D_HHI_4 | 2 | 2 | 2 | MATCH |
| RQT_E_HHI_4 | 2 | 2 | 2 | MATCH |
| RQT_F_HHI_4 | 2 | 2 | 2 | MATCH |
| RQT_G_HHI_4 | 2 | 2 | 2 | MATCH |
| SAODBLK_A_MainConcept_4 | 500 | 500 | 0 | MATCH（適合の md5）: 500 枚の I420 の md5 が zip の md5 b9c98937… と一致。ffmpeg の decode は zip の md5 と違う（ffmpeg の参照が誤り） |
| SAODBLK_B_MainConcept_4 | 500 | 500 | 1 | MATCH（適合の md5）: 500 枚の I420 の md5 が zip の md5 03104349… と一致。ffmpeg の参照が誤り |
| SAO_A_MediaTek_4 | 60 | 60 | 60 | MATCH |
| SAO_B_MediaTek_5 | 60 | 60 | 60 | MATCH |
| SAO_C_Samsung_5 | 5 | 5 | 5 | MATCH |
| SAO_D_Samsung_5 | 5 | 5 | 5 | MATCH |
| SAO_E_Canon_4 | 16 | 16 | 16 | MATCH |
| SAO_F_Canon_3 | 16 | 16 | 16 | MATCH |
| SAO_G_Canon_3 | 16 | 16 | 16 | MATCH |
| SAO_H_Parabola_1 | 4 | 4 | 4 | MATCH |
| SDH_A_Orange_4 | 2 | 2 | 2 | MATCH |
| SLICES_A_Rovi_3 | 9 | 9 | 9 | MATCH |
| SLIST_A_Sony_5 | 65 | 65 | 65 | MATCH |
| SLIST_B_Sony_9 | 65 | 65 | 65 | MATCH |
| SLIST_C_Sony_4 | 65 | 65 | 65 | MATCH |
| SLIST_D_Sony_9 | 65 | 65 | 65 | MATCH |
| SLPPLP_A_VIDYO_2 | 33 | 33 | 33 | MATCH |
| STRUCT_A_Samsung_7 | 60 | 60 | 60 | MATCH |
| STRUCT_B_Samsung_7 | 60 | 60 | 60 | MATCH |
| TILES_A_Cisco_2 | 100 | 100 | 100 | MATCH |
| TILES_B_Cisco_1 | 100 | 100 | 100 | MATCH |
| TMVP_A_MS_3 | 17 | 17 | 17 | MATCH |
| TSCL_A_VIDYO_5 | 73 | 73 | 73 | MATCH |
| TSCL_B_VIDYO_4 | 73 | 73 | 73 | MATCH |
| TSKIP_A_MS_3 | 17 | 17 | 17 | MATCH |
| TSUNEQBD_A_MAIN10_Technicolor_2 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| TUSIZE_A_Samsung_1 | 5 | 5 | 5 | MATCH |
| VPSID_A_VIDYO_2 | 33 | 33 | 33 | MATCH |
| VPSSPSPPS_A_MainConcept_1 | 6 | 6 | 0 | MATCH（絵ごとの参照）: 大きさの違う 6 枚。ffmpeg は全体を decode すると後の絵を最初の大きさに縮めて hash するので、絵ごとの参照（split-pictures.py、reorder-ps.py の写し）と 6/6 一致 |
| WPP_A_ericsson_MAIN10_2 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| WPP_A_ericsson_MAIN_2 | 48 | 48 | 48 | MATCH |
| WPP_B_ericsson_MAIN10_2 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| WPP_B_ericsson_MAIN_2 | 48 | 48 | 48 | MATCH |
| WPP_C_ericsson_MAIN10_2 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| WPP_C_ericsson_MAIN_2 | 48 | 48 | 48 | MATCH |
| WPP_D_ericsson_MAIN10_2 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| WPP_D_ericsson_MAIN_2 | 48 | 48 | 0 | DIFFER: 64x240（CTB 64 の 1 列）の WPP。48 枚中 10 枚で decoder が CTB の行 1 から誤り（error 0xd、32/64 block）を返す。参照は正しい（ffmpeg の md5 が zip の md5 と一致）。1 CTB 幅の WPP の context の扱いを hardware がしていない見込み（未確認） |
| WPP_E_ericsson_MAIN10_2 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| WPP_E_ericsson_MAIN_2 | 48 | 48 | 48 | MATCH |
| WPP_F_ericsson_MAIN10_2 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| WPP_F_ericsson_MAIN_2 | 48 | 48 | 48 | MATCH |
| WP_A_MAIN10_Toshiba_3 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| WP_A_Toshiba_3 | 256 | 256 | 256 | MATCH |
| WP_B_Toshiba_3 | 256 | 256 | 256 | MATCH |
| WP_MAIN10_B_Toshiba_3 | - | - | 0 | 能力の外: Main 10（10 bit）、probe が断る |
| cip_B_NEC_3 | 5 | 5 | 5 | MATCH |
| ipcm_A_NEC_3 | 1 | 1 | 1 | MATCH |
| ipcm_B_NEC_3 | 1 | 1 | 1 | MATCH |
| ipcm_C_NEC_3 | 1 | 1 | 1 | MATCH |
| ipcm_D_NEC_3 | 1 | 1 | 1 | MATCH |
| ipcm_E_NEC_2 | 1 | 1 | 1 | MATCH |


**H.264 の非退行**（run12、最後の kernel と merge 後の libmedia）: h264-1080p30 600/600 MATCH、h264-slices 3/3 MATCH。h264-1080・h264-main・h264-pyramid は p005 と同じ DIFFER（media-probe が先頭に負の時刻の余分な絵を出す、p005 §6.4）。

**Video Player の GPU の道**（run12、`VP_GPU_PRESENT`、1280x720 の window、16 s、desktop の session）: 

| clip | media-probe（libmedia、backend=vulkan-video） | Video Player | 画面 |
| --- | --- | --- | --- |
| h264-1080p30（600 枚） | 600/600 MATCH | shown 470、late 13、DRAW gpu=30/30、copy 0、誤り 0 | `build/review/ws324-p007/vp-h264-1080p30.png`（cksum 1608554191、window に絵） |
| h265-1080p30（x265、B 3 枚、20 s、600 枚） | 600/600 MATCH | shown 469、late 14、DRAW gpu=30/30、copy 0、誤り 0 | `build/review/ws324-p007/vp-h265-1080p30.png`（cksum 3278910996、window に絵） |

**timeout の後の隔離**: 13 回のどれでも decoder の timeout は起きず、起こす手段（壊した stream で止まる事）は試していない（未実施）。

### 6.4 host 試験・build（2026-11-02）

| 試験 | 結果 |
| --- | --- |
| `sh plan/ws324/tests/hevc-host-test.sh` | 160 checks, 0 failed（transform skip の大きさ、list を列ごとに書く事、filter の部分の大きさを足した・直した） |
| `sh plan/ws324/tests/h265-wire-host-test.sh` | 54 checks, 0 failed |
| `plan/tools/rtx-video/nvdec-host-test.sh`・`wire-host-test.sh`・`plane-header-host-test.sh`、`plan/ws324/tests/nvenc-host-test.sh` | 88・32・40・61 checks, 0 failed |
| `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws324-p007/k build/ws324-p007/k/vmunix` | rc 0、warning 0（-Werror）、cksum 3900996930 |
| userland（vkvideo-probe・media-probe・videoplayer・libmedia ほか、`BUILD=build/ws324-p007/v`） | rc 0、warning 0 |

道具（`plan/ws324/tests/`）: `h265-conformance-prep.sh`（147 本の展開・ffmpeg の参照・特徴の表）、`h265-features.py`、`h265-raw-compare.py`（生の frame と ffmpeg の比べ、64x64 の block の地図）、`hevc-trace-decode.py`（`nvrtx.hevc.trace=1` の log を field の名前で読む）、`h265-table.py`（回の表）、`rtx-restore324.sh`（止めた回の後の機械の戻し）、`rtx-video324.sh` の `RAW`・`H265_FRAMES`・`H265_NAMES`・`H265_OPTIONS`・`VARIANTS`・`PLAY_DIR`。

### 6.5 未実施・残り

- WPP_D_ericsson_MAIN_2 の DIFFER の原因の確定（1 CTB 幅の WPP の扱いが hardware に無いのか、driver で渡せる物があるのか）。能力の最小の幅（64）を上げて断る道もある（NVIDIA の最小の幅は文書に無い）。
- timeout の後の隔離の実機の確かめ（timeout を起こせていない）。
- filter の buffer の大きさは観測からの余裕を持たせた値で、hardware の正しい式は文書に無い（4K の 1 session で filter が約 30 MB）。
- `nvrtx.hevc.dxva=1` の形は全部の絵が誤りのまま（skip の形が既定で正しいので直していない）。
- `plan/tools/` への道具の登録（`rtx-video324.sh` ほか）は WS の完了の時に Q1。

### 6.6 encode（NVENC）を再開する時に要る物（ユーザーの指示: 今日は NVDEC で終え、encode は次の指示ですぐ再開する）

- 次の Phase は p008a（libvulkan の encode の転送）と、並行して p009b の続き（NVENC の関門）。p009b の観測（H.264・H.265 の 1 枚目の encode が rc_process と入力を読んだ後、出力の前に HW の誤り error_status 1・GSP の RC 47、約 90 通りで同じ）は [phase009b](../phase009b/phase.md) §4〜§5。
- p007 で分かった事のうち encode にも効きそうな物: open-gpu-doc の注記と hardware の読みが違う所がある（transform skip の大きさ、scaling list の向き、buffer の大きさ）。NVENC の設定（`nvenc_h264_drv_pic_setup_s`）でも、文書の注記どおりの値を疑い、1 field ずつ変える variant の起動（`VARIANTS`）と `nvrtx.hevc.trace=1` のような設定の dump で比べる手が使える。
- 実機の道具: `plan/ws324/tests/rtx-nvenc-test.sh`（p009b）、戻しは `rtx-restore324.sh` の形。
