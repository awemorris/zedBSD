<!-- awesome-plan project=zedbsd record=ws349-p001 -->

# ws349-p001 設計: BCM2712 の HDMI の音

- WS: [ws.md](../ws.md)。Queue: q2042-i01（P1、worktree `/home/awe/zedBSD-worktrees/p1-pi500`、branch `agent/p1-pi500`）。
- 状態: cleared（2026-11-13 P1、設計の第一版。実装の Phase はまだ立てていない。下の「未確定」を p002 の始めに出典で確かめる）

## 目的

Pi 5（と Pi 500）の HDMI から音を出すための経路を決め、実装の Phase に分ける。この Phase では code を書かない。

## 今の状態（2026-11-13 の main）

- Pi 5 の kernel に音の driver は無い。`mk/platform-rpi5/defaults.mk` は `CONFIG_DRIVER_RPI5_AUDIO := n`、`mk/kernel.mk` の `CONFIG_DRIVER_RPI5_AUDIO`（「RPi5 RP1 Audio driver」）は source が空。`/dev/dsp0` が無いので audiod は「no device」で動く。
- Pi 4 の音は headphone の jack（PWM1 + legacy DMA、`src/drivers/platform/rpi4/rpi4-audio.c`）だけで、zedBSD には Pi 4 の HDMI の音も無い。Pi 5 には headphone の jack が無いので、HDMI が唯一の出口になる。
- 画面は zedBSD の VideoCore の display driver（`src/drivers/gpu/videocore/display/`、HVS6 と HDMI の encoder、`display-start.c`・`display-program.c`）が firmware から引き継いで動かしている。HDMI の controller の register の地図（`hdmi`・`dvp`・`phy`・`rm`・`packet`・`csc`・`hd`・`cec`）はそこに既にある。

## device tree から読める事（`bcm2712d0-rpi-5-b.dtb`、raspberrypi/firmware 3d301dd、dtc で読んだ）

- `hdmi@7c701400`（`brcm,bcm2712-hdmi0`）と `hdmi@7c706400`（`brcm,bcm2712-hdmi1`）。`reg-names` は `hdmi dvp phy rm packet metadata csc cec hd`（Pi 4 の BCM2711 に無い `metadata` がある）。
- 音の DMA: `dmas = <&dma 0x41fa000c>`（hdmi1 は `0x41fa000d`）、`dma-names = "audio-rx"`（Linux の名前は "rx" だが、HDMI へ送る向き）。相手は `dma@10600`（`brcm,bcm2712-dma`、channel 6〜10、`brcm,dma-channel-mask = <0x7c0>`）。Pi 4 の HDMI の音が使う legacy の `bcm2835-dma` ではなく、40 bit の番地を扱う BCM2712 の DMA。DREQ は下位の 0x0c・0x0d と読める。上位の `0x41fa` の意味は未確定。
- clock: `clock-names = "hdmi", "bvb", "audio", "cec"`。`audio` は reset と同じ provider（`resets = <… 0x01>` の相手）の 0 番。
- 両方の node は `status = "disabled"`（firmware が HDMI を持ち、OS の KMS の overlay で有効にする作り）。zedBSD は tree の status を見ずに display を動かしているので、音も同じく node の `reg` だけを使う。

## 経路（案）

1. 音の sample: audiod → `/dev/dsp0`（`drivers/audio/audio.c` の framework、16 bit・2 channel・48 kHz）→ framework の ring（coherent な buffer、fragment ごとに `drv_audio_interrupt()`）。
2. 変換: HDMI の MAI（Multichannel Audio Interface）は IEC 60958 の subframe（channel status・validity・parity の bit を含む 32 bit の word）を受ける。Pi 4 の jack の driver と同じく、driver が framework の ring の隣に変換済みの ring を持ち、interrupt ごとに次の fragment を変換する（`rpi4-audio-ring.c` の型を流用）。Linux は MAI の format を IEC958 にして ALSA に subframe を作らせているので、ここも driver が作る。
3. DMA: BCM2712 の DMA の cyclic な control block の鎖で、変換済みの ring を HDMI の `hd` の region の MAI の data の register へ。DREQ は tree の 12（hdmi0）・13（hdmi1）。
4. HDMI の側: MAI の FIFO の reset と threshold、MAI の format（2 channel、48 kHz）、N/CTS（`HDMI_CRP_CFG`、48 kHz は N = 6144、CTS は TMDS の clock から）、audio の InfoFrame を packet の RAM に、channel の map、GCP。音の clock（tree の `audio`）を入れる。
5. 画面との順: 音は display driver が HDMI を動かした後でしか始められない。mode の変更・hotplug・lease の終わりで HDMI が止まる時は音の DMA を先に止める。display driver に「HDMI の port が動いている・TMDS の clock」を問う口と、止める前の通知が要る（driver の間の口で、HAL は変えない）。

## Pi 4 の vc4 の HDMI の音との違い（Linux の vc4_hdmi で見る点）

| 点 | Pi 4（BCM2711） | Pi 5（BCM2712） |
| --- | --- | --- |
| DMA | legacy の bcm2835-dma（DREQ の値は Linux の bcm2711 の tree で確かめる、未確定） | `brcm,bcm2712-dma`（40 bit、channel 6〜10、DREQ 12・13） |
| register の region | `hdmi dvp phy rm packet csc cec hd` | 同じに `metadata` が加わる。field の位置は Linux の `vc4_hdmi_regs.h` の BCM2712 の表（vc6 の表）で別 |
| 音の clock | Linux の BCM2711 の tree で確かめる（未確定） | tree の `audio` は reset の provider の clock 0 |
| 外付けの jack | PWM の jack がある（zedBSD は対応済み） | 無い。HDMI だけ |

## 出典の方針（guardrail の値の規則）

- 値（register の offset と bit、MAI の format の code、N/CTS の表、DMA の control block の型と DREQ の encode）は、Raspberry Pi Linux rpi-6.12.y の `drivers/gpu/drm/vc4/vc4_hdmi.c`・`vc4_hdmi_regs.h`・`vc4_regs.h`、`drivers/dma/bcm2835-dma.c`（BCM2712 の 40 bit の扱い）、`arch/arm64/boot/dts/broadcom/bcm2712.dtsi` から、使う block ごとに「Values referenced」で書く。code は写さず、値と手順だけを読む（GPL の code を取り込まない）。
- HDMI の N/CTS・InfoFrame・IEC 60958 の bit は HDMI 1.4 と CEA-861、IEC 60958-3 の値で、Linux の値と照らす。
- device tree から取れる物（番地、DREQ、clock）は tree から読み、定数にしない。

## audiod の device の口

- 新しい driver は `drv_audio_register()` で 1 つの再生の device（`playback = 1`、`capture = 0`）を出す。framework が `/dev/dsp0` を作るので、audiod は今のまま開く（Pi 5 の audiod の変更は要らない見込み）。
- 音量は hardware に無い（audiod が sample を縮める。`get_volume`・`set_volume` は無し、Pi 4 の jack と同じ）。
- 2 つの HDMI の port: 最初は display driver が動かしている port（firmware が点けた方）の 1 つだけ。両方を出すか、port の切り替えをどうするかは後の Phase（製品の判断が要れば Q1 へ）。

## 実装の Phase（案、立てるのは Q1）

| Phase | 中身 |
| --- | --- |
| p002 | 出典の読み: 上の未確定（DREQ の上位の bit、BCM2712 の DMA の control block、vc6 の MAI と CRP の field、audio clock の入れ方）を Linux の source で確かめ、表にする。host の模型の試験の設計 |
| p003 | BCM2712 の DMA の cyclic な転送の部品（他の RP1 の外の DMA にも使える形）と host 試験 |
| p004 | HDMI の音の driver（MAI・N/CTS・InfoFrame・IEC 60958 の変換の ring）、display driver との口、`CONFIG_DRIVER_RPI5_AUDIO` の source（mk の変更はユーザーの確認） |
| p005 | Pi 5 の実機: 48 kHz の試験音、mode の変更と hotplug の時の止め方、audiod からの再生 |
| p006 | 規約の全文の見直し |

## 未確定（p002 で出典で確かめる）

- DMA の指定 `0x41fa000c` の上位 16 bit の意味（Linux の BCM2712 の DMA の xlate）。
- BCM2712 の DMA の control block の型（40 bit の番地の置き方）と、HDMI の MAI の data の register の bus の番地。
- vc6 の HDMI の MAI・CRP・RAM_PACKET の register の位置と、`metadata` の region の役目。
- audio clock の provider（reset と同じ node）の種類と、clock の入れ方。
- Pi 4 の HDMI の DREQ の値（表の比較のため。実装には要らない）。

## 確かめ

- 読んだ物: `mk/platform-rpi5/defaults.mk`、`mk/kernel.mk`（音の driver の節）、`include/drivers/audio/audio.h`、`src/drivers/platform/rpi4/rpi4-audio.c`、`src/drivers/gpu/videocore/display/`、`userland/base/audiod/device.c`（`/dev/dsp0`）、`bcm2712d0-rpi-5-b.dtb`（zedBSD-firmware 2670dc1 の `rpi5-boot/`、dtc）。
- 設計の敵対的レビュー（design-reviewer）は未実施（この Queue は設計の第一版まで）。実装の Phase を立てる前に流す。
