<!-- awesome-plan project=zedbsd record=ws319-p004 -->

# ws319-p004 5330 の実機: HEVC_v1 の stream ごとの表と Video Player の H.265 の GPU の道

- 親: [WS319](../ws.md)。設計と実装は [ws319-p003](../phase003/phase.md)。
- Status: cleared（2026-11-02 P4 q1926-i01、Q1 の受け入れ待ち）
- 依存: p003、p002（GPU の道）。

## 方法

- 1 本の script `plan/ws319/tests/run-5330-video.sh OUT BUILD CLIPS HEVC`（`PARTS` で h264・h265・probe・raw を選ぶ）: Debian（solaris10-man）から NVMe の zedBSD の kernel を BUILD の物に差し替え（元は ~/ws319/vmunix.orig）、その起動だけ login=graphical を外し、BootNext で 1 回起動、programs と clips を /tmp（tmpfs）へ、root で実行、回収、Debian へ戻して kernel と zedbsd.cfg を元に戻し cksum を出す。/tmp/i915-hw.lock を持つ。
- clip: `plan/ws319/tests/make-clips.sh`（host の ffmpeg、testsrc2 と粒、1920x1080、30 fps 20 s・60 fps 10 s、H.264 は libx264 High、H.265 は libx265 Main、B あり）と参照（`plan/tools/rtx-video/nv12-hashes.py`）。HEVC_v1: `plan/ws322/tests/p006/make-hevc.sh`（146 本、ffmpeg の NV12 の SHA-256）。表: `plan/ws319/tests/probe-table.py`。
- build: `make ZEDBSD_CONFIG=plan/tools/hw5330/config-5330.mk BUILD=build/p4 build/p4/vmunix build/p4/bin/{videoplayer,media-probe,vkvideo-probe,wayland,files,keiland-shot}`（warning 0）。

## 結果（2026-11-02、kernel cksum 2053531102、commit 350551baad・01c0463444）

### HEVC_v1（run11、`PARTS=probe`、vkvideo-probe --copy --expect、ffmpeg の参照）

- 146 本: MATCH 128（参照の全 frame が出て一致。うち 6 本 BUMPING_A・NoOutPrior_A・NoOutPrior_B・OPFLAG_B・OPFLAG_C・RAP_B は decode した絵の方が多いが、出す絵は参照と同じ数で全部一致）、REFUSED 15（Main10 の 12、IPRED_B（Main Still Picture、libvulkan が Main だけを出す）、PICSIZE_A〜D（4096 を超える））、DIFFER 3（下）。
- DIFFER の 3 本は参照の誤りで、i915 の出力は正しい:
  - SAODBLK_A・SAODBLK_B（1016x760、500 枚）: run13 の `PARTS=raw`（vkvideo-probe --raw を host で I420 に、MD5）が zip の MD5 と一致（A b9c98937bcb6be445348fdc89c208f1e、B 031043495e9b9f4ce120afa316aa5705）。host の ffmpeg の decode は zip の MD5 と違う（A 23f03fff…、B de53c4f4…）。WS322 p006 の未決（VF2 でも同じ 2 本が ffmpeg と不一致）はこれで ffmpeg 側と分かる。
  - VPSSPSPPS_A（6 枚の I、大きさがみな違う）: ffmpeg は流れ全体では後の大きさの絵を最初の大きさに拡大縮小して hash する。各絵を単独に decode した参照（`plan/ws322/tests/p006/split-pictures.py`、reorder-ps.py の写し）と run12 で 6/6 一致。
- 失敗・hang: 0（kernel の video の誤りの行も 0）。

stream ごとの表（run11、`probe-table.py`。DIFFER の 3 本の正否は上）:

| stream | class | reference | exit | decoded | output | matched | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| AMP_A_Samsung_7 | in | 17 | 0 | 17 | 17 | 17 | MATCH |
| AMP_B_Samsung_7 | in | 17 | 0 | 17 | 17 | 17 | MATCH |
| AMP_D_Hisilicon_3 | in | 100 | 0 | 100 | 100 | 100 | MATCH |
| AMP_E_Hisilicon_3 | in | 100 | 0 | 100 | 100 | 100 | MATCH |
| AMP_F_Hisilicon_3 | in | 100 | 0 | 100 | 100 | 100 | MATCH |
| AMVP_A_MTK_4 | in | 30 | 0 | 30 | 30 | 30 | MATCH |
| AMVP_B_MTK_4 | in | 41 | 0 | 41 | 41 | 41 | MATCH |
| AMVP_C_Samsung_7 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| BUMPING_A_ericsson_1 | in | 49 | 0 | 66 | 49 | 49 | MATCH |
| CAINIT_A_SHARP_4 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CAINIT_B_SHARP_4 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CAINIT_C_SHARP_3 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CAINIT_D_SHARP_3 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CAINIT_E_SHARP_3 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CAINIT_F_SHARP_3 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CAINIT_G_SHARP_3 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CAINIT_H_SHARP_3 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| CIP_A_Panasonic_3 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| cip_B_NEC_3 | in | 5 | 0 | 5 | 5 | 5 | MATCH |
| CIP_C_Panasonic_2 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| CONFWIN_A_Sony_1 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| DBLK_A_MAIN10_VIXS_4 | out:main10 | 8 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| DBLK_A_SONY_3 | in | 30 | 0 | 30 | 30 | 30 | MATCH |
| DBLK_B_SONY_3 | in | 30 | 0 | 30 | 30 | 30 | MATCH |
| DBLK_C_SONY_3 | in | 30 | 0 | 30 | 30 | 30 | MATCH |
| DBLK_D_VIXS_2 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| DBLK_E_VIXS_2 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| DBLK_F_VIXS_2 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| DBLK_G_VIXS_2 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| DELTAQP_A_BRCM_4 | in | 96 | 0 | 96 | 96 | 96 | MATCH |
| DELTAQP_B_SONY_3 | in | 30 | 0 | 30 | 30 | 30 | MATCH |
| DELTAQP_C_SONY_3 | in | 30 | 0 | 30 | 30 | 30 | MATCH |
| DSLICE_A_HHI_5 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| DSLICE_B_HHI_5 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| DSLICE_C_HHI_5 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| ENTP_A_QUALCOMM_1 | in | 24 | 0 | 24 | 24 | 24 | MATCH |
| ENTP_B_Qualcomm_1 | in | 24 | 0 | 24 | 24 | 24 | MATCH |
| ENTP_C_Qualcomm_1 | in | 24 | 0 | 24 | 24 | 24 | MATCH |
| EXT_A_ericsson_4 | in | 3 | 0 | 3 | 3 | 3 | MATCH |
| FILLER_A_Sony_1 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| HRD_A_Fujitsu_3 | in | 96 | 0 | 96 | 96 | 96 | MATCH |
| INITQP_A_Sony_1 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| INITQP_B_Main10_Sony_1 | out:main10 | 90 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| ipcm_A_NEC_3 | in | 1 | 0 | 1 | 1 | 1 | MATCH |
| ipcm_B_NEC_3 | in | 1 | 0 | 1 | 1 | 1 | MATCH |
| ipcm_C_NEC_3 | in | 1 | 0 | 1 | 1 | 1 | MATCH |
| ipcm_D_NEC_3 | in | 1 | 0 | 1 | 1 | 1 | MATCH |
| ipcm_E_NEC_2 | in | 1 | 0 | 1 | 1 | 1 | MATCH |
| IPRED_A_docomo_2 | in | 20 | 0 | 20 | 20 | 20 | MATCH |
| IPRED_B_Nokia_3 | in | 1 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| IPRED_C_Mitsubishi_3 | in | 4 | 0 | 4 | 4 | 4 | MATCH |
| LS_A_Orange_2 | in | 9 | 0 | 9 | 9 | 9 | MATCH |
| LS_B_Orange_4 | in | 25 | 0 | 25 | 25 | 25 | MATCH |
| LTRPSPS_A_Qualcomm_1 | in | 500 | 0 | 500 | 500 | 500 | MATCH |
| MAXBINS_A_TI_5 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| MAXBINS_B_TI_5 | in | 3 | 0 | 3 | 3 | 3 | MATCH |
| MAXBINS_C_TI_5 | in | 3 | 0 | 3 | 3 | 3 | MATCH |
| MERGE_A_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| MERGE_B_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| MERGE_C_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| MERGE_D_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| MERGE_E_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| MERGE_F_MTK_4 | in | 41 | 0 | 41 | 41 | 41 | MATCH |
| MERGE_G_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| MVCLIP_A_qualcomm_3 | in | 5 | 0 | 5 | 5 | 5 | MATCH |
| MVDL1ZERO_A_docomo_4 | in | 500 | 0 | 500 | 500 | 500 | MATCH |
| MVEDGE_A_qualcomm_3 | in | 17 | 0 | 17 | 17 | 17 | MATCH |
| NoOutPrior_A_Qualcomm_1 | in | 40 | 0 | 43 | 40 | 40 | MATCH |
| NoOutPrior_B_Qualcomm_1 | in | 47 | 0 | 50 | 47 | 47 | MATCH |
| NUT_A_ericsson_5 | in | 34 | 0 | 34 | 34 | 34 | MATCH |
| OPFLAG_A_Qualcomm_1 | in | 500 | 0 | 500 | 500 | 500 | MATCH |
| OPFLAG_B_Qualcomm_1 | in | 98 | 0 | 100 | 98 | 98 | MATCH |
| OPFLAG_C_Qualcomm_1 | in | 96 | 0 | 100 | 96 | 96 | MATCH |
| PICSIZE_A_Bossen_1 | in | 10 | 6 | 0 | 0 | 0 | REFUSED (refused: the stream's 1056x8440 is larger than the decoder's) |
| PICSIZE_B_Bossen_1 | in | 10 | 6 | 0 | 0 | 0 | REFUSED (refused: the stream's 8440x1056 is larger than the decoder's) |
| PICSIZE_C_Bossen_1 | in | 10 | 6 | 0 | 0 | 0 | REFUSED (refused: the stream's 528x4216 is larger than the decoder's ) |
| PICSIZE_D_Bossen_1 | in | 10 | 6 | 0 | 0 | 0 | REFUSED (refused: the stream's 4216x528 is larger than the decoder's ) |
| PMERGE_A_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| PMERGE_B_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| PMERGE_C_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| PMERGE_D_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| PMERGE_E_TI_3 | in | 8 | 0 | 8 | 8 | 8 | MATCH |
| POC_A_Bossen_3 | in | 5 | 0 | 5 | 5 | 5 | MATCH |
| PPS_A_qualcomm_7 | in | 81 | 0 | 81 | 81 | 81 | MATCH |
| PS_B_VIDYO_3 | in | 25 | 0 | 25 | 25 | 25 | MATCH |
| RAP_A_docomo_6 | in | 86 | 0 | 86 | 86 | 86 | MATCH |
| RAP_B_Bossen_2 | in | 80 | 0 | 83 | 80 | 80 | MATCH |
| RPLM_A_qualcomm_4 | in | 300 | 0 | 300 | 300 | 300 | MATCH |
| RPLM_B_qualcomm_4 | in | 300 | 0 | 300 | 300 | 300 | MATCH |
| RPS_A_docomo_5 | in | 44 | 0 | 44 | 44 | 44 | MATCH |
| RPS_B_qualcomm_5 | in | 300 | 0 | 300 | 300 | 300 | MATCH |
| RPS_C_ericsson_5 | in | 40 | 0 | 40 | 40 | 40 | MATCH |
| RPS_D_ericsson_6 | in | 68 | 0 | 68 | 68 | 68 | MATCH |
| RPS_E_qualcomm_5 | in | 300 | 0 | 300 | 300 | 300 | MATCH |
| RPS_F_docomo_2 | in | 50 | 0 | 50 | 50 | 50 | MATCH |
| RQT_A_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| RQT_B_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| RQT_C_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| RQT_D_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| RQT_E_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| RQT_F_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| RQT_G_HHI_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| SAO_A_MediaTek_4 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| SAO_B_MediaTek_5 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| SAO_C_Samsung_5 | in | 5 | 0 | 5 | 5 | 5 | MATCH |
| SAO_D_Samsung_5 | in | 5 | 0 | 5 | 5 | 5 | MATCH |
| SAO_E_Canon_4 | in | 16 | 0 | 16 | 16 | 16 | MATCH |
| SAO_F_Canon_3 | in | 16 | 0 | 16 | 16 | 16 | MATCH |
| SAO_G_Canon_3 | in | 16 | 0 | 16 | 16 | 16 | MATCH |
| SAO_H_Parabola_1 | in | 4 | 0 | 4 | 4 | 4 | MATCH |
| SAODBLK_A_MainConcept_4 | in | 500 | 4 | 500 | 500 | 0 | DIFFER |
| SAODBLK_B_MainConcept_4 | in | 500 | 4 | 500 | 500 | 1 | DIFFER |
| SDH_A_Orange_4 | in | 2 | 0 | 2 | 2 | 2 | MATCH |
| SLICES_A_Rovi_3 | in | 9 | 0 | 9 | 9 | 9 | MATCH |
| SLIST_A_Sony_5 | in | 65 | 0 | 65 | 65 | 65 | MATCH |
| SLIST_B_Sony_9 | in | 65 | 0 | 65 | 65 | 65 | MATCH |
| SLIST_C_Sony_4 | in | 65 | 0 | 65 | 65 | 65 | MATCH |
| SLIST_D_Sony_9 | in | 65 | 0 | 65 | 65 | 65 | MATCH |
| SLPPLP_A_VIDYO_2 | in | 33 | 0 | 33 | 33 | 33 | MATCH |
| STRUCT_A_Samsung_7 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| STRUCT_B_Samsung_7 | in | 60 | 0 | 60 | 60 | 60 | MATCH |
| TILES_A_Cisco_2 | in | 100 | 0 | 100 | 100 | 100 | MATCH |
| TILES_B_Cisco_1 | in | 100 | 0 | 100 | 100 | 100 | MATCH |
| TMVP_A_MS_3 | in | 17 | 0 | 17 | 17 | 17 | MATCH |
| TSCL_A_VIDYO_5 | in | 73 | 0 | 73 | 73 | 73 | MATCH |
| TSCL_B_VIDYO_4 | in | 73 | 0 | 73 | 73 | 73 | MATCH |
| TSKIP_A_MS_3 | in | 17 | 0 | 17 | 17 | 17 | MATCH |
| TUSIZE_A_Samsung_1 | in | 5 | 0 | 5 | 5 | 5 | MATCH |
| VPSID_A_VIDYO_2 | in | 33 | 0 | 33 | 33 | 33 | MATCH |
| VPSSPSPPS_A_MainConcept_1 | in | 2 | 4 | 6 | 6 | 0 | DIFFER |
| WP_A_MAIN10_Toshiba_3 | out:main10 | 256 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WP_A_Toshiba_3 | in | 256 | 0 | 256 | 256 | 256 | MATCH |
| WP_B_Toshiba_3 | in | 256 | 0 | 256 | 256 | 256 | MATCH |
| WP_MAIN10_B_Toshiba_3 | out:main10 | 256 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WPP_A_ericsson_MAIN10_2 | out:main10 | 48 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WPP_A_ericsson_MAIN_2 | in | 48 | 0 | 48 | 48 | 48 | MATCH |
| WPP_B_ericsson_MAIN10_2 | out:main10 | 48 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WPP_B_ericsson_MAIN_2 | in | 48 | 0 | 48 | 48 | 48 | MATCH |
| WPP_C_ericsson_MAIN10_2 | out:main10 | 48 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WPP_C_ericsson_MAIN_2 | in | 48 | 0 | 48 | 48 | 48 | MATCH |
| WPP_D_ericsson_MAIN10_2 | out:main10 | 48 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WPP_D_ericsson_MAIN_2 | in | 48 | 0 | 48 | 48 | 48 | MATCH |
| WPP_E_ericsson_MAIN10_2 | out:main10 | 48 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WPP_E_ericsson_MAIN_2 | in | 48 | 0 | 48 | 48 | 48 | MATCH |
| WPP_F_ericsson_MAIN10_2 | out:main10 | 48 | 6 | 0 | 0 | 0 | REFUSED (refused: the decoder does not take the stream's H.265 profil) |
| WPP_F_ericsson_MAIN_2 | in | 48 | 0 | 48 | 48 | 48 | MATCH |

counts: DIFFER 3, MATCH 128, REFUSED 15

### Video Player の H.265（run12、`PARTS="h265 probe raw"`、VP_GPU_PRESENT、素の compositor）

| 項目 | 結果 |
| --- | --- |
| media-probe h265-1080p30・60 | 600/600・600/600 一致、backend=vulkan-video |
| GPU の道 | 「pictures the GPU holds, 14 slots」、DRAW gpu=60〜64 /s、picture_us 16〜66 |
| 1080p30 16 s | shown 481、late 0、compositor の誤り 0 |
| 1080p60 10 s | shown 548 / 600、late 52（始めの約 6 s に 50）、compositor の誤り 0 |
| 画面 | `build/tmp/p319-run12/h265-1080p30.png`（正しい絵） |

- 5330 は各 run の後に Debian に戻し、kernel cksum 1363937226・zedbsd.cfg 2629068843 に戻した（restore の行で確かめた）。

## 未実施・残り

- 1080p60 の late（H.264 も 19〜80）: swapchain の acquire の待ち（8〜13 ms/frame）が主。調べていない。
- session の desktop（graphical login の compositor）での再生は流していない（run は素の compositor）。
- 4K（2160p）、Main10、4096 を超える絵、長時間の再生。
