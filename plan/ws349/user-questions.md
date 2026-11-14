# WS349 ユーザーへの問い（Raspberry Pi 5 の HDMI の音）

WS349 は、Pi 5（と Pi 500）の HDMI から音を出す driver を作る WS（状態 incomplete、設計の第二版まで。[p001](phase001/phase.md)）。下の問いは製品の判断と、Makefile・`mk/` の変更（ユーザーの物）。それぞれ推しを付けた。

## U1 mk の枠の名前と当てる時期

- 背景: 今の `mk/kernel.mk` の `CONFIG_DRIVER_RPI5_AUDIO`（「RPi5 RP1 Audio driver」）は source が空で、`mk/config.mk` の説明は RP1（Pi 5 の南の chip。GPIO の header の I2S などを持つ）を指す。HDMI の音は RP1 ではなく BCM2712 の側にある。
- 案: 空の RP1 の枠を消し、`CONFIG_DRIVER_RPI5_HDMI_AUDIO`（source 2 つ）に置き換える。差分は [p001 の「mk の案」](phase001/phase.md)。
- 時期: (a) kernel.mk と config.mk の枠は p004（実機の読みだけの probe）の build の前に要る。(b) `mk/platform-rpi5/defaults.mk` で audio の core と HDMI の音を y にするのは p006 の実機の再生が通ってから。
- 選択肢:
  1. **推し**: 名前は `CONFIG_DRIVER_RPI5_HDMI_AUDIO`、空の RP1 の枠は消す、時期は (a)・(b) の 2 回。将来 RP1 の I2S を作る時は別の名前の枠を立てる。
  2. 今の `CONFIG_DRIVER_RPI5_AUDIO` の名前のまま、中身を HDMI に替える（説明の RP1 と PCI の依存を消す）。
  3. 両方を残す（RP1 の枠は空のまま）。
- 補い: audio の core が n で HDMI の音が y の時の link の誤りは、今の Pi 4・VF2 の枠と同じく config.mk の「Depends:」の注記だけにするのが推し（kernel.mk に条件を足すかはお任せ）。

## U2 二つの画面をつないだ時の音の出口

- 背景: Pi 5 には micro HDMI が 2 つある。driver は `/dev/dsp` を 1 つだけ出す。
- 選択肢:
  1. **推し**: 起動の時に HDMI0、HDMI1 の順で、HDMI の mode で画面がつながっている最初の port。単純で、多くの人は HDMI0 だけを使う。
  2. 両方の port に同じ音を出す（driver が 2 つの DMA を回す。作りが倍になる）。
  3. 起動の引数（cmdline.txt）で選べるようにする。
- この WS では 1 にし、2・3 は要望があれば別の WS。

## U3 起動の後に画面を挿した時

- 背景: driver は起動の時に HDMI の port を選ぶ。起動の時に画面が無い（または DVI の画面）と `/dev/dsp` を出さず、後で HDMI の画面を挿しても再起動まで音が出ない。
- 選択肢:
  1. **推し**: この WS ではこのまま（起動の時に決める）。Pi 5 の画面そのものも起動の時の mode のままなので（firmware が mode を持ち、zedBSD は mode を変えない）、画面と同じ扱い。
  2. 起動の時に画面が無くても `/dev/dsp` を出し、画面が来るまで音を捨てる（software の時計）。audiod や app からは「音が出ている」ように見えて何も聞こえない。
