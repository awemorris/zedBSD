<!-- awesome-plan project=zedbsd record=ws349-p002 -->

# ws349-p002 値の照合と出典の表

- WS: [ws.md](../ws.md)。設計: [p001](../phase001/phase.md)。
- 状態: cleared（Q1 の受け入れ待ち。2026-11-15 P8、Queue q2124-i01、worktree `/home/awe/zedBSD-worktrees/p8-ws349p002`、branch `agent/p8-ws349p002`、基点 main dc991a7f65）
- 担当の定義の推し: phase-runner（読みと表、code は書かない）。

## 目的

p001 の値の表の全部を出典の行で確かめ、code の block に「Values referenced:」「Procedure referenced:」を書ける形の表 `plan/ws349/values.md` を作る。

## 手順

1. 出典を取得し SHA-256 を記録する（tree に入れない。置き場所は自分の `build/tmp/ws349-sources/`）:
   - raspberrypi/linux `rpi-6.12.y` の固定の commit（p001 は ab4372076bb8）の `drivers/gpu/drm/vc4/vc4_hdmi.c`・`vc4_hdmi_regs.h`・`vc4_regs.h`・`vc4_hdmi_phy.c`・`vc4_drv.c`・`vc4_hvs.c`、`drivers/dma/bcm2835-dma.c`、`drivers/clk/bcm/clk-bcm2711-dvp.c`、`include/sound/asoundef.h`、`include/linux/hdmi.h`。
   - alsa-lib の固定の commit（p001 は f44e0abd5382）の `src/pcm/pcm_iec958.c`。
   - 3 つの dtb（zedBSD-firmware 2670dc1 の `rpi5-boot/`）を dtc で読んだ text。
2. p001 の値の表の各行を、file・版・表か関数の名前・行で確かめる。違えば p001 を直す（同じ commit）。特に:
   - hdmi1 の field の表が hdmi0 と同じ offset か（MAI 以外）。
   - MAI_THR の C と D0 の field。Linux が C と D0 を分ける根拠（`vc4_hvs.c` の HVS の版の読み、`vc4_drv.c` の gen）と HVS の VERSION の値 0x53・0x54。
   - HDMI の 48 kHz の N の推奨の表と、1.001 の rate の N・CTS（規格の表、Linux の `vc4_hdmi.c` は表を持たない事を確かめる）。CTS_0・CTS_1 を交互に使う period の field が出典にあるか（無ければ「無い」と書き、p001 の D8 の整数 1 つの扱いにする）。
   - EXTERNAL_CTS_EN・USE_MAI_BUS_SYNC_FOR_CTS の comment の全文の要点（p006 の 2 通りの試しの前提）。
   - IEC 60958-3 の channel status の byte 0〜4 の値（`asoundef.h` の define の値）。
   - CEA-861 の Audio InfoFrame の byte の並びと checksum（`include/linux/hdmi.h` の値と、`hdmi_audio_infoframe_pack` の手順の要点）。
   - DMA の abort・start・callback の手順の要点（Procedure referenced の対象）。
3. C1 と D0 の dtb の差のうち音に関わる物の一覧（HDMI・DMA・DVP・clock・HVS の node）。

## 受け入れ条件

- `plan/ws349/values.md` が全ての値を行番号つきで持ち、p001 の値の表と食い違いが無い（食い違いは p001 を直した）。
- 出典の file の SHA-256 の一覧が values.md にある。
- code・comment の写しを tree に入れていない（値と手順の要点だけ）。

## 確かめ

- 自分で数える: values.md の行の数と p001 の値の表の行の数の対応。
- 未実施の確かめは「未確認」と書く。

## 結果（2026-11-15 P8）

- 作った物: [values.md](../values.md)（出典の file 20 個の版・SHA-256・license、値の表 V01〜V35、N と CTS の表、手順の出典、C1 と D0 の dtb の差、p001 の訂正と補い）。
- 出典の取得: raspberrypi/linux ab4372076bb840600e522ae776887ae2bc488fa6 の 16 file（手順の 10 file と、照合に要った `reset-simple.c`・`sound/core/pcm_iec958.c`・`hdmi-codec.c`・`drivers/video/hdmi.c`・`dw-hdmi.c`・`drm_hdmi_helper.c`）、alsa-lib f44e0abd5382f519cc6fa69850c34e6e9c09d5f5 の `pcm_iec958.c`、raspberrypi/firmware 3d301dd の dtb 3 つ（SHA-256 は `mk/platform-rpi5/firmware.mk` の値と一致）。置き場所は worktree の `build/tmp/ws349-sources/`（tree に入れていない）。
- 照合: p001 の値の表 29 行を V01〜V29 で 1 行ずつ確かめた（一致 21、補いを含む一致 7、訂正 1）。hdmi1 の field の表は MAI と VID_CTL・FRAME_COUNT の他は hdmi0 と同じ offset。MAI_THR の C と D0 の field、HVS の VERSION 0x53・0x54 と Linux の C・D0 の決め方（`vc4_drv.c` の `brcm,bcm2712-vc6` を C とし `vc4_hvs.c` で VERSION 0x54 なら D0）を確かめた。Linux `vc4_hdmi.c` は N・CTS の表を持たない（N = 128 × fs ÷ 1000）。CTS_0・CTS_1 の period の field は出典に無い。
- p001 の訂正（同じ commit）: 74.25/1.001 MHz の 48 kHz の CTS（210937.5 → 140625、210937.5 は 32 kHz の値）。補い: DVP の SW_INIT の bit（hdmi0 1、hdmi1 2、1 が reset）、HDMI の node が 3 つの dtb で status disabled、EXTERNAL_CTS_EN の「N は 128 で割り切れる」（立てる時は N 6144、表の N は落とした時）、channel status の byte、各 port の CLOCK_STOP は触らない。
- 未確認: 規格書（HDMI 1.4b・IEC 60958-3・CEA-861）の本文（手元に無い。297/1.001・594/1.001 MHz の N・CTS は出典の file が無く、整数になる事だけを計算で確かめた）、firmware が dtb の status を書き換えるか（p004）、EXTERNAL_CTS_EN の本当の意味（p006）。
- code は書いていない。build・試験は無し。
