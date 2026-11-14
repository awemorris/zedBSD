<!-- awesome-plan project=zedbsd record=ws349-p002 -->

# ws349-p002 値の照合と出典の表

- WS: [ws.md](../ws.md)。設計: [p001](../phase001/phase.md)。
- 状態: planned
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
