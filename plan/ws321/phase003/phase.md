<!-- awesome-plan project=zedbsd record=ws321-p003 -->

# ws321-p003 実機の受け入れ: vkvideo-probe の適合の stream と Video Player の GPU の道

- 親: [WS321](../ws.md)。Queue: q1923-i01（P3）。
- Status: cleared（2026-11-02 P3。Q1 の受け入れ待ち。未決 2 本は下の表）
- 依存: p002。Video Player の H.265 は libmedia の Vulkan Video H.265 の backend（ws322-p006、main 5c8c1e7dc、worktree に merge 715b94dec）。

## 板と戻し

- Raspberry Pi 5（10.0.30.7、SwitchBot 2）。CI の image（1.0.0-beta2+g8f91a0d、`login=graphical rpi5gpu.start=boot`）。
- script: `plan/ws321/tests/pi5-hevc.sh`（deploy は vmunix を vmunix.p321old に一度だけ残し、kernel を cksum で確かめて置く。textlogin は cmdline.txt を cmdline.txt.p321old に残して desktop の login 無しで起動。restore は両方を戻し起動し cksum を deploy の前と比べる）。programs と library は `/tmp/p321`（tmpfs）に置き LD_LIBRARY_PATH で使う。
- 元: vmunix 2112791070、cmdline.txt 3178356281、config.txt 2738850246。戻した後: 同じ（`RESTORED OK`、2 回）。
- 板は始めに網から消えていた（電源は on）ので SwitchBot で一度入れ直した。自分の compositor を desktop の session と同時に動かした時に session が終わり system が止まった（boot-000031 の log「the system is stopping」、kernel の hang ではない）ので、SwitchBot で入れ直し、以後は textlogin で compositor を一つにした。
- HDMI の capture（/dev/video0）は 420 秒の全部が壊れた MJPEG（緑の雑音）だった（板を capture を開いたまま再起動しても）。画面は compositor の capture（keiland-shot）で撮った。25.8 GB の capture は Q1 が消した。

## vkvideo-probe（HEVC_v1、ffmpeg の framehash と照合）

`PI5=10.0.30.7 OUT=... sh plan/ws321/tests/pi5-hevc.sh probe`（`vkvideo-probe --h265 --expect=NAME.sha256 --time`）。stream と参照は `sh plan/ws322/tests/p006/make-hevc.sh`（146 本、in 136・Main10 10）。

全 146 本: 全 frame 一致 128 本、断る 15 本（Main10 10、4096 を越える PICSIZE 4、Main Still Picture 1）、未決 3 本（SAODBLK_A・B、VPSSPSPPS_A。VF2 の ws322-p006 と同じ 3 本）。最後の kernel（ec865a923 の source、build/p321/vmunix）で 2026-11-02 10:10 頃に全部を一度に流した結果。kernel の log に fault・lost・command failed は無い。

| stream | 大きさ | decode した絵 | 参照と一致した出力（出力の全部） | decode ms（平均/最長、step と TFU を含む） | 結果 |
| --- | --- | --- | --- | --- | --- |
| AMP_A_Samsung_7 | 2560x1600 | 17 | 17 | 7.5/12 | 全 frame 一致 |
| AMP_B_Samsung_7 | 2560x1600 | 17 | 17 | 8.4/12 | 全 frame 一致 |
| AMP_D_Hisilicon_3 | 1920x1080 | 100 | 100 | 3.7/6 | 全 frame 一致 |
| AMP_E_Hisilicon_3 | 1920x1080 | 100 | 100 | 3.6/6 | 全 frame 一致 |
| AMP_F_Hisilicon_3 | 1920x1080 | 100 | 100 | 3.7/8 | 全 frame 一致 |
| AMVP_A_MTK_4 | 416x240 | 30 | 30 | 2.0/3 | 全 frame 一致 |
| AMVP_B_MTK_4 | 416x240 | 41 | 41 | 2.2/4 | 全 frame 一致 |
| AMVP_C_Samsung_7 | 832x480 | 60 | 60 | 2.0/3 | 全 frame 一致 |
| BUMPING_A_ericsson_1 | 416x240 | 66 | 49 | 2.1/3 | 全 frame 一致 |
| CAINIT_A_SHARP_4 | 832x480 | 50 | 50 | 2.1/3 | 全 frame 一致 |
| CAINIT_B_SHARP_4 | 832x480 | 50 | 50 | 2.0/3 | 全 frame 一致 |
| CAINIT_C_SHARP_3 | 832x480 | 50 | 50 | 2.2/3 | 全 frame 一致 |
| CAINIT_D_SHARP_3 | 832x480 | 50 | 50 | 2.0/3 | 全 frame 一致 |
| CAINIT_E_SHARP_3 | 832x480 | 50 | 50 | 2.1/3 | 全 frame 一致 |
| CAINIT_F_SHARP_3 | 832x480 | 50 | 50 | 2.1/3 | 全 frame 一致 |
| CAINIT_G_SHARP_3 | 832x480 | 50 | 50 | 2.1/3 | 全 frame 一致 |
| CAINIT_H_SHARP_3 | 832x480 | 50 | 50 | 2.4/3 | 全 frame 一致 |
| CIP_A_Panasonic_3 | 416x240 | 2 | 2 | 2.5/3 | 全 frame 一致 |
| cip_B_NEC_3 | 416x240 | 5 | 5 | 2.2/3 | 全 frame 一致 |
| CIP_C_Panasonic_2 | 416x240 | 2 | 2 | 2.5/3 | 全 frame 一致 |
| CONFWIN_A_Sony_1 | 416x240 | 60 | 60 | 2.1/3 | 全 frame 一致 |
| DBLK_A_MAIN10_VIXS_4 | 176x144 | - | - | - | 断る（Main10） |
| DBLK_A_SONY_3 | 832x480 | 30 | 30 | 2.1/4 | 全 frame 一致 |
| DBLK_B_SONY_3 | 832x480 | 30 | 30 | 2.0/3 | 全 frame 一致 |
| DBLK_C_SONY_3 | 832x480 | 30 | 30 | 2.7/4 | 全 frame 一致 |
| DBLK_D_VIXS_2 | 1280x720 | 8 | 8 | 2.1/3 | 全 frame 一致 |
| DBLK_E_VIXS_2 | 1280x720 | 8 | 8 | 2.0/2 | 全 frame 一致 |
| DBLK_F_VIXS_2 | 1280x720 | 8 | 8 | 2.1/3 | 全 frame 一致 |
| DBLK_G_VIXS_2 | 1280x720 | 8 | 8 | 2.0/2 | 全 frame 一致 |
| DELTAQP_A_BRCM_4 | 1920x1088 | 96 | 96 | 5.0/7 | 全 frame 一致 |
| DELTAQP_B_SONY_3 | 832x480 | 30 | 30 | 4.8/8 | 全 frame 一致 |
| DELTAQP_C_SONY_3 | 832x480 | 30 | 30 | 2.9/5 | 全 frame 一致 |
| DSLICE_A_HHI_5 | 1920x1080 | 50 | 50 | 4.0/6 | 全 frame 一致 |
| DSLICE_B_HHI_5 | 1920x1080 | 50 | 50 | 4.3/5 | 全 frame 一致 |
| DSLICE_C_HHI_5 | 1920x1080 | 50 | 50 | 3.7/5 | 全 frame 一致 |
| ENTP_A_QUALCOMM_1 | 1920x1080 | 24 | 24 | 7.1/8 | 全 frame 一致 |
| ENTP_B_Qualcomm_1 | 1920x1080 | 24 | 24 | 4.1/8 | 全 frame 一致 |
| ENTP_C_Qualcomm_1 | 1920x1080 | 24 | 24 | 4.0/8 | 全 frame 一致 |
| EXT_A_ericsson_4 | 416x240 | 3 | 3 | 3.0/3 | 全 frame 一致 |
| FILLER_A_Sony_1 | 416x240 | 60 | 60 | 2.5/3 | 全 frame 一致 |
| HRD_A_Fujitsu_3 | 416x240 | 96 | 96 | 2.1/3 | 全 frame 一致 |
| INITQP_A_Sony_1 | 416x240 | 60 | 60 | 2.1/3 | 全 frame 一致 |
| INITQP_B_Main10_Sony_1 | 416x240 | - | - | - | 断る（Main10） |
| ipcm_A_NEC_3 | 416x240 | 1 | 1 | 3.0/3 | 全 frame 一致 |
| ipcm_B_NEC_3 | 416x240 | 1 | 1 | 3.0/3 | 全 frame 一致 |
| ipcm_C_NEC_3 | 416x240 | 1 | 1 | 2.0/2 | 全 frame 一致 |
| ipcm_D_NEC_3 | 416x240 | 1 | 1 | 3.0/3 | 全 frame 一致 |
| ipcm_E_NEC_2 | 416x240 | 1 | 1 | 2.0/2 | 全 frame 一致 |
| IPRED_A_docomo_2 | 832x480 | 20 | 20 | 2.0/2 | 全 frame 一致 |
| IPRED_B_Nokia_3 | 1920x1080 | - | - | - | 断る（Main Still Picture の profile） |
| IPRED_C_Mitsubishi_3 | 832x480 | 4 | 4 | 2.8/4 | 全 frame 一致 |
| LS_A_Orange_2 | 416x240 | 9 | 9 | 2.9/5 | 全 frame 一致 |
| LS_B_Orange_4 | 832x480 | 25 | 25 | 4.6/11 | 全 frame 一致 |
| LTRPSPS_A_Qualcomm_1 | 416x240 | 500 | 500 | 2.0/3 | 全 frame 一致 |
| MAXBINS_A_TI_5 | 416x240 | 2 | 2 | 6.5/7 | 全 frame 一致 |
| MAXBINS_B_TI_5 | 416x240 | 3 | 3 | 4.7/8 | 全 frame 一致 |
| MAXBINS_C_TI_5 | 416x240 | 3 | 3 | 4.0/8 | 全 frame 一致 |
| MERGE_A_TI_3 | 416x240 | 8 | 8 | 2.2/3 | 全 frame 一致 |
| MERGE_B_TI_3 | 416x240 | 8 | 8 | 2.1/3 | 全 frame 一致 |
| MERGE_C_TI_3 | 416x240 | 8 | 8 | 2.0/2 | 全 frame 一致 |
| MERGE_D_TI_3 | 416x240 | 8 | 8 | 2.2/3 | 全 frame 一致 |
| MERGE_E_TI_3 | 416x240 | 8 | 8 | 2.2/3 | 全 frame 一致 |
| MERGE_F_MTK_4 | 416x240 | 41 | 41 | 2.0/3 | 全 frame 一致 |
| MERGE_G_HHI_4 | 832x480 | 2 | 2 | 3.0/4 | 全 frame 一致 |
| MVCLIP_A_qualcomm_3 | 416x240 | 5 | 5 | 3.0/3 | 全 frame 一致 |
| MVDL1ZERO_A_docomo_4 | 832x480 | 500 | 500 | 2.1/4 | 全 frame 一致 |
| MVEDGE_A_qualcomm_3 | 416x240 | 17 | 17 | 2.1/3 | 全 frame 一致 |
| NoOutPrior_A_Qualcomm_1 | 832x480 | 43 | 40 | 2.1/3 | 全 frame 一致 |
| NoOutPrior_B_Qualcomm_1 | 832x480 | 50 | 47 | 2.0/3 | 全 frame 一致 |
| NUT_A_ericsson_5 | 416x240 | 34 | 34 | 2.3/3 | 全 frame 一致 |
| OPFLAG_A_Qualcomm_1 | 416x240 | 500 | 500 | 2.0/3 | 全 frame 一致 |
| OPFLAG_B_Qualcomm_1 | 832x480 | 100 | 98 | 2.9/4 | 全 frame 一致 |
| OPFLAG_C_Qualcomm_1 | 832x480 | 100 | 96 | 2.2/3 | 全 frame 一致 |
| PICSIZE_A_Bossen_1 | 1056x8440 | - | - | - | 断る（4096 を越える） |
| PICSIZE_B_Bossen_1 | 8440x1056 | - | - | - | 断る（4096 を越える） |
| PICSIZE_C_Bossen_1 | 528x4216 | - | - | - | 断る（4096 を越える） |
| PICSIZE_D_Bossen_1 | 4216x528 | - | - | - | 断る（4096 を越える） |
| PMERGE_A_TI_3 | 416x240 | 8 | 8 | 2.1/3 | 全 frame 一致 |
| PMERGE_B_TI_3 | 416x240 | 8 | 8 | 2.2/3 | 全 frame 一致 |
| PMERGE_C_TI_3 | 416x240 | 8 | 8 | 2.6/3 | 全 frame 一致 |
| PMERGE_D_TI_3 | 416x240 | 8 | 8 | 2.6/3 | 全 frame 一致 |
| PMERGE_E_TI_3 | 416x240 | 8 | 8 | 2.4/3 | 全 frame 一致 |
| POC_A_Bossen_3 | 416x240 | 5 | 5 | 2.0/2 | 全 frame 一致 |
| PPS_A_qualcomm_7 | 832x480 | 81 | 81 | 2.2/4 | 全 frame 一致 |
| PS_B_VIDYO_3 | 416x240 | 25 | 25 | 2.2/3 | 全 frame 一致 |
| RAP_A_docomo_6 | 416x240 | 86 | 86 | 2.0/3 | 全 frame 一致 |
| RAP_B_Bossen_2 | 448x256 | 83 | 80 | 2.0/3 | 全 frame 一致 |
| RPLM_A_qualcomm_4 | 416x240 | 300 | 300 | 2.0/2 | 全 frame 一致 |
| RPLM_B_qualcomm_4 | 416x240 | 300 | 300 | 2.0/3 | 全 frame 一致 |
| RPS_A_docomo_5 | 416x240 | 44 | 44 | 2.0/3 | 全 frame 一致 |
| RPS_B_qualcomm_5 | 416x240 | 300 | 300 | 2.0/3 | 全 frame 一致 |
| RPS_C_ericsson_5 | 416x240 | 40 | 40 | 2.0/2 | 全 frame 一致 |
| RPS_D_ericsson_6 | 416x240 | 68 | 68 | 2.8/3 | 全 frame 一致 |
| RPS_E_qualcomm_5 | 416x240 | 300 | 300 | 2.1/3 | 全 frame 一致 |
| RPS_F_docomo_2 | 416x240 | 50 | 50 | 2.0/3 | 全 frame 一致 |
| RQT_A_HHI_4 | 832x480 | 2 | 2 | 3.0/4 | 全 frame 一致 |
| RQT_B_HHI_4 | 832x480 | 2 | 2 | 3.0/4 | 全 frame 一致 |
| RQT_C_HHI_4 | 832x480 | 2 | 2 | 3.0/4 | 全 frame 一致 |
| RQT_D_HHI_4 | 832x480 | 2 | 2 | 3.0/4 | 全 frame 一致 |
| RQT_E_HHI_4 | 832x480 | 2 | 2 | 3.5/4 | 全 frame 一致 |
| RQT_F_HHI_4 | 832x480 | 2 | 2 | 3.0/4 | 全 frame 一致 |
| RQT_G_HHI_4 | 832x480 | 2 | 2 | 3.0/4 | 全 frame 一致 |
| SAO_A_MediaTek_4 | 416x240 | 60 | 60 | 2.1/3 | 全 frame 一致 |
| SAO_B_MediaTek_5 | 832x480 | 60 | 60 | 2.0/3 | 全 frame 一致 |
| SAO_C_Samsung_5 | 1920x1080 | 5 | 5 | 6.8/13 | 全 frame 一致 |
| SAO_D_Samsung_5 | 1920x1080 | 5 | 5 | 7.0/12 | 全 frame 一致 |
| SAO_E_Canon_4 | 416x240 | 16 | 16 | 2.2/3 | 全 frame 一致 |
| SAO_F_Canon_3 | 416x240 | 16 | 16 | 2.2/3 | 全 frame 一致 |
| SAO_G_Canon_3 | 416x240 | 16 | 16 | 2.2/3 | 全 frame 一致 |
| SAO_H_Parabola_1 | 416x240 | 4 | 4 | 3.0/3 | 全 frame 一致 |
| SAODBLK_A_MainConcept_4 | 1016x760 | 500 | 0 | 2.4/4 | 未決（ffmpeg の参照が zip の md5 と違う、VF2 と同じ） |
| SAODBLK_B_MainConcept_4 | 1016x760 | 500 | 1 | 2.5/15 | 未決（同上） |
| SDH_A_Orange_4 | 1920x1080 | 2 | 2 | 25.5/37 | 全 frame 一致 |
| SLICES_A_Rovi_3 | 640x480 | 9 | 9 | 2.9/4 | 全 frame 一致 |
| SLIST_A_Sony_5 | 832x480 | 65 | 65 | 2.1/3 | 全 frame 一致 |
| SLIST_B_Sony_9 | 832x480 | 65 | 65 | 2.1/3 | 全 frame 一致 |
| SLIST_C_Sony_4 | 832x480 | 65 | 65 | 2.2/3 | 全 frame 一致 |
| SLIST_D_Sony_9 | 832x480 | 65 | 65 | 2.2/3 | 全 frame 一致 |
| SLPPLP_A_VIDYO_2 | 416x240 | 33 | 33 | 2.2/3 | 全 frame 一致 |
| STRUCT_A_Samsung_7 | 832x480 | 60 | 60 | 2.1/3 | 全 frame 一致 |
| STRUCT_B_Samsung_7 | 832x480 | 60 | 60 | 2.0/3 | 全 frame 一致 |
| TILES_A_Cisco_2 | 1920x1080 | 100 | 100 | 4.0/7 | 全 frame 一致 |
| TILES_B_Cisco_1 | 1920x1080 | 100 | 100 | 4.0/7 | 全 frame 一致 |
| TMVP_A_MS_3 | 416x240 | 17 | 17 | 2.1/3 | 全 frame 一致 |
| TSCL_A_VIDYO_5 | 416x240 | 73 | 73 | 2.1/3 | 全 frame 一致 |
| TSCL_B_VIDYO_4 | 416x240 | 73 | 73 | 2.0/3 | 全 frame 一致 |
| TSKIP_A_MS_3 | 1280x720 | 17 | 17 | 2.2/4 | 全 frame 一致 |
| TUSIZE_A_Samsung_1 | 2560x1600 | 5 | 5 | 12.8/18 | 全 frame 一致 |
| VPSID_A_VIDYO_2 | 416x240 | 33 | 33 | 2.2/3 | 全 frame 一致 |
| VPSSPSPPS_A_MainConcept_1 | 1280x720 | 6 | 0 | 2.8/4 | 未決（参照は並べ替えた写しの ffmpeg、VF2 と同じ） |
| WP_A_MAIN10_Toshiba_3 | 416x240 | - | - | - | 断る（Main10） |
| WP_A_Toshiba_3 | 416x240 | 256 | 256 | 2.0/3 | 全 frame 一致 |
| WP_B_Toshiba_3 | 416x240 | 256 | 256 | 2.0/3 | 全 frame 一致 |
| WP_MAIN10_B_Toshiba_3 | 416x240 | - | - | - | 断る（Main10） |
| WPP_A_ericsson_MAIN10_2 | 416x240 | - | - | - | 断る（Main10） |
| WPP_A_ericsson_MAIN_2 | 416x240 | 48 | 48 | 2.1/3 | 全 frame 一致 |
| WPP_B_ericsson_MAIN10_2 | 416x240 | - | - | - | 断る（Main10） |
| WPP_B_ericsson_MAIN_2 | 416x240 | 48 | 48 | 2.0/3 | 全 frame 一致 |
| WPP_C_ericsson_MAIN10_2 | 416x240 | - | - | - | 断る（Main10） |
| WPP_C_ericsson_MAIN_2 | 416x240 | 48 | 48 | 2.0/3 | 全 frame 一致 |
| WPP_D_ericsson_MAIN10_2 | 64x240 | - | - | - | 断る（Main10） |
| WPP_D_ericsson_MAIN_2 | 64x240 | 48 | 48 | 1.9/2 | 全 frame 一致 |
| WPP_E_ericsson_MAIN10_2 | 128x240 | - | - | - | 断る（Main10） |
| WPP_E_ericsson_MAIN_2 | 128x240 | 48 | 48 | 2.0/2 | 全 frame 一致 |
| WPP_F_ericsson_MAIN10_2 | 192x240 | - | - | - | 断る（Main10） |
| WPP_F_ericsson_MAIN_2 | 192x240 | 48 | 48 | 2.1/3 | 全 frame 一致 |

## Video Player（GPU の道、VP_GPU_PRESENT、libmedia の backend=vulkan-video codec=h265）

clip は host の ffmpeg（libx265 -preset fast、testsrc2、20 秒、mp4、hvc1）: `h265-1080p30.mp4`（600 frame）、`h265-1080p60.mp4`（1200 frame）。

| 実行 | 結果 |
| --- | --- |
| 板の desktop の session（user、wayland-0）、1080p30、20 秒 | `VIDEOPLAYER OPEN ... video=h265/vulkan-video`、shown 600、late 0、Vulkan Video「60 pictures in 2000 ms, 3 in flight」、top の CPU 94.8% idle |
| 同じ、1080p60、20 秒 | shown 1197、late 3（始めの 2 秒）、「120 pictures in 2000 ms」、CPU 97.9% idle |
| 自分の compositor（textlogin）、1080p30、16 秒、keiland-shot | shown 481、late 0、compositor の誤り 0、PNG `build/tmp/p321-video/player-own-1080p30.png`（testsrc2 の絵が正しい色と位置で出ている、time code 00:00:07.900） |
| 同じ、1080p60、16 秒 | shown 959、late 3、PNG `build/tmp/p321-video/player-own-1080p60.png` |

## 未実施・残り

- SAODBLK_A・B の正否（VF2 と同じく ffmpeg の参照が zip の md5 と合わない。板の出力の I420 の md5 を zip の md5 と照らす道具が要る、ws322-p006 の残りと同じ）。
- VPSSPSPPS_A の正しい参照での照合（VF2 は正しい参照で一致、その参照の作り方は ws322-p006）。
- Pi 4 の実機の非退行（build だけ）。
- `CONFIG_DRIVER_BCM2712_VIDEO` の既定 y（Q1 の判断、p003 の受け入れの後）。→ 2026-11-02 に Q1 が既定 y にした。今の build system では option は無く、`CONFIG_DRIVER_RPI5_GPU` に含まれる。
- 速さ: 1080p60 の始めの late 3 は phase 1 と phase 2 を重ねていない事と、poll の形（p002 の R4・R5）。後の Phase。
