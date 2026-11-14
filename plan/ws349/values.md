<!-- awesome-plan project=zedbsd record=ws349-values -->

# WS349 値と手順の出典の表（Raspberry Pi 5 の HDMI の音）

- 作った Phase: [p002](phase002/phase.md)（2026-11-15 P8、Queue q2124-i01）。設計: [p001](phase001/phase.md) の「値の表」。
- code の block には、この表の「出典」の列を「Values referenced:」（値）・「Procedure referenced:」（手順）として書く（[guardrail](../guardrail.md) の「MIT・GPL の出典のデータの引用」）。名前・comment・構造は写さない。
- 出典の file は tree に入れていない（P8 の worktree の `build/tmp/ws349-sources/` に取得、下の SHA-256）。行番号はその版の file の行。

## 出典の file

| 略号 | repo と版 | path | SHA-256 | license |
| --- | --- | --- | --- | --- |
| L-hdmi | raspberrypi/linux `rpi-6.12.y` ab4372076bb840600e522ae776887ae2bc488fa6（2026-11-09） | `drivers/gpu/drm/vc4/vc4_hdmi.c` | be27895e5748a0dd3fca317e11c2d53c74475f4a742053244ac7a4e3e4cbc940 | GPL-2.0 |
| L-hregs | 同上 | `drivers/gpu/drm/vc4/vc4_hdmi_regs.h` | e97390a7ef71d57683cfb03d3c294b75eae372e332f25aa182a19efbd8894ab4 | GPL-2.0 |
| L-regs | 同上 | `drivers/gpu/drm/vc4/vc4_regs.h` | 992e746d155028da8024b448a283a2f6efa9465bc185759f0801fe77b36de0ab | GPL-2.0 |
| L-phy | 同上 | `drivers/gpu/drm/vc4/vc4_hdmi_phy.c` | 9c865f2a3234a8757577d394915bb41b96fafa5e62a84d849797d925aabcf842 | GPL-2.0 |
| L-drv | 同上 | `drivers/gpu/drm/vc4/vc4_drv.c` | ff6d303c3e4f669d151d8ee8da5c69c4067e4169e28fa4b0b690868e48baf34f | GPL-2.0 |
| L-hvs | 同上 | `drivers/gpu/drm/vc4/vc4_hvs.c` | 3e2914eeee9ae9a41f54b3b95b5b43a5f1421234ae59758fe4b7736299d0e6f5 | GPL-2.0 |
| L-dma | 同上 | `drivers/dma/bcm2835-dma.c` | 936f55cca6cb9989f24d72bce8f6788c94fa101110ef815aae219b7f6dbec6eb | GPL-2.0+ |
| L-dvp | 同上 | `drivers/clk/bcm/clk-bcm2711-dvp.c` | acd1d1cb847b945f24d6fbbad607d4f8789c51e5b2a781666402251ca4bb7962 | GPL-2.0 |
| L-rst | 同上 | `drivers/reset/reset-simple.c` | 87108ee6cb4a491c6d180fe7260696d840723d0ca6987f3cfac21b0fbc403ed5 | GPL-2.0+ |
| L-aes | 同上 | `include/sound/asoundef.h` | ed697c70fa22435af713f5e743203317429f562723e7d11898bf6dafb15a5ebc | GPL-2.0+ |
| L-cs | 同上 | `sound/core/pcm_iec958.c` | 0299f94a39db852372adc5d3ef543ea75ded26bc1601aa0c3db2776dba9cda37 | GPL-2.0 |
| L-codec | 同上 | `sound/soc/codecs/hdmi-codec.c` | 96180b76d3dd9316149db895d83f35c298c215361df7f710e5110b0215ad292f | GPL-2.0 |
| L-ifh | 同上 | `include/linux/hdmi.h` | d0786532230d46c8fd4bd633d74ecb19f9a80dfa3d283cfa7945255048f64f6f | MIT |
| L-ifc | 同上 | `drivers/video/hdmi.c` | 43a6223cb0fc85708aee89b6a69ffbd43d6b9f7e9f2b56d7c113dc4b9bfb3d78 | MIT |
| L-dw | 同上 | `drivers/gpu/drm/bridge/synopsys/dw-hdmi.c` | 3aa45a83f8d69cbd1b7457b9ab88091a298991b370791fcf6503adb62c33681f | GPL-2.0+ |
| L-acr | 同上（読んだが値は無かった） | `drivers/gpu/drm/display/drm_hdmi_helper.c` | bac4c2b4369f2001edafce341dd4097176aae53a6a50b5c57a1dd2bb265d6c40 | MIT |
| A-iec | alsa-project/alsa-lib master f44e0abd5382f519cc6fa69850c34e6e9c09d5f5（2026-10-19） | `src/pcm/pcm_iec958.c` | d1fc423298d121a344124582886e7b4f23f0bf71a1c3fcd19e9adf6d1913bea5 | LGPL-2.1+ |
| D-C1 | raspberrypi/firmware 3d301dd924bcd758a4c8cb19fe8531031f033f43（`mk/platform-rpi5/firmware.mk` と同じ版、dtb の SHA-256 も一致） | `boot/bcm2712-rpi-5-b.dtb` | 40a2fbe9c29e8b9a4912cf726a943068defb779fc052ec38e457a79c58abca00（dtc の text 6b7a9543b5e123fe5f0cbf7ffc353bdee4812c579985e99d93e5774db5a8e7f4） | GPL-2.0（COPYING.linux） |
| D-D0 | 同上 | `boot/bcm2712d0-rpi-5-b.dtb` | 03d80fc8196bfd113f86534c60ee2cb35ba95ae182e34a42f66caf7fa15f48ba（text 6432dd7001ea4018d758c213430ae67fb3fb543822df58290a86e53611b9e55e） | 同上 |
| D-500 | 同上 | `boot/bcm2712-rpi-500.dtb` | dd98c7c3780e4c88148401c95d2222ccb4e585ed4bfbe2b5c67531b0a06958b5（text 99b129b79fcfcdc0b2c73bc4076f3c417877c2186ba28bb5832ce4acc40ede3f） | 同上 |

- dtb の行番号は `dtc -q -I dtb -O dts` の text の行（3 つの text で行の位置は同じ）。
- HDMI 1.4 の規格書・IEC 60958-3・CEA-861 の本文は読んでいない（手元に無い）。規格の値は上の source の値と、計算での確かめで扱った（下の各行に書いた）。

## 値の表

「p001」の列は p001 の値の表の行（上から 1〜29）。30 以降は p001 の決定（D1〜D16）の本文にある値か、p002 で足した値。照合の列: 一致・訂正（p001 を直した）・補い（p001 に足した）。

| # | p001 | 値 | 出典（略号:行） | 照合 |
| --- | --- | --- | --- | --- |
| V01 | 1 | hdmi0 の node `hdmi@7c701400`（compatible `brcm,bcm2712-hdmi0`）、hdmi1 `hdmi@7c706400`（`brcm,bcm2712-hdmi1`）。reg-names `hdmi dvp phy rm packet metadata csc cec hd`。reg（soc の番地）: hdmi0 `hdmi` 0x7c701400/0x300、`dvp` 0x7c701000/0x200、`phy` 0x7c701d00/0x300、`rm` 0x7c702000/0x80、`packet` 0x7c703800/0x200、`metadata` 0x7c704000/0x800、`csc` 0x7c700100/0x80、`cec` 0x7d510800/0x100、`hd` 0x7c720000/0x100。hdmi1 は 0x7c706400、0x7c706000、0x7c706d00、0x7c707000、0x7c708800、0x7c709000、0x7c700180、0x7d511000、`hd` 0x7c720000（両方の port で同じ `hd`）。clock-names `hdmi bvb audio cec`、`audio` は DVP の clock 0（hdmi0）・1（hdmi1）。resets は DVP の 1（hdmi0）・2（hdmi1）。dma-names `audio-rx`。**3 つの dtb で status = "disabled"** | D-C1・D-D0・D-500: 525〜558 | 一致。補い: status が disabled（zedBSD の config.txt は overlay を持たない。firmware が書き換えるかは未確認）→ driver は status で node を除外しない（p001 D3 に注記） |
| V02 | 2 | `hd` の MAI: hdmi0 CTL 0x10・THR 0x14・FMT 0x18・DATA 0x1c・SMP 0x20。hdmi1 CTL 0x30・THR 0x34・FMT 0x38・DATA 0x3c・SMP 0x40 | L-hregs: 433〜437（hdmi0 の表）、533〜537（hdmi1 の表） | 一致 |
| V03 | 3 | `hdmi` の region: MAI_CHANNEL_MAP 0x0a4、MAI_CONFIG 0x0a8、AUDIO_PACKET_CONFIG 0x0c0、RAM_PACKET_CONFIG 0x0c4、RAM_PACKET_STATUS 0x0cc、CRP_CFG 0x0d0、CTS_0 0x0d4、CTS_1 0x0d8、SCHEDULER_CONTROL 0x0e8、HOTPLUG 0x1c8。hdmi1 の表は MAI と VID_CTL・FRAME_COUNT（hd の region）の他は hdmi0 と同じ offset | L-hregs: 442〜448・456〜457・471（hdmi0）、542〜548・556〜557・571（hdmi1）。全行の比較は 431〜529 と 531〜629 | 一致（hdmi1 の field の表が MAI 以外で hdmi0 と同じ事を確かめた。違いは MAI の 5 つと VID_CTL 0x44/0x48・FRAME_COUNT 0x60/0x64 だけ） |
| V04 | 4 | `rm` の RM_OFFSET +0x018（offset 0〜30、OFFSET_ONLY bit 31）。`phy` の PLL_REFCLK +0x01c（REFFRQ 0〜9、REFCLK_SEL_CMOS 13。Linux は REFFRQ に 54 を書く）、PLL_VCOCLK_DIV +0x02c（VCODIV 0〜9、VCODIV_EN 10） | L-hregs: 484・486・502。L-phy: 121〜123・137〜138・144〜145・984〜998 | 一致。補い: REFFRQ の値の単位は MHz（Linux は 54 を書く、L-phy: 986） |
| V05 | 5 | packet RAM: `packet` の region の先頭（RAM_PACKET_START = +0x000）から slot ごとに 0x24 byte、slot = InfoFrame の型 − 0x80（audio 0x84 → 4、+0x90〜+0xb3）。7 byte ごとに 2 word: 1 語目 byte 0〜2（bit 0〜23）、2 語目 byte 3〜6（bit 0〜31）。書いた後、次の slot の始めまでを 0 で埋める | L-hregs: 8（stride）・505。L-hdmi: 696〜773（write_infoframe、packet_id・packet_reg の計算 702〜707、2 word の詰め方 738〜751、残りの 0 757〜758） | 一致 |
| V06 | 6 | MAI_CTL: RESET 0（読み値は不定）、ERRORF 1（overflow、1 を書いて消す）、ERRORE 2（underflow、同）、ENABLE 3、CHNUM 4〜7、PAREN 8（MAI が bit 31 の parity を作る）、FLUSH 9、EMPTY 10、FULL 11、WHOLSMP 12、CHALIGN 13、BUSY 14、DLATE 15（FIFO が空になった、1 を書いて消す） | L-regs: 957〜979 | 一致。補い: CHNUM には channel 数そのもの（2）を書く（L-hdmi: 2136） |
| V07 | 7 | MAI_THR: C 系と Pi 4 は PANICHIGH 24〜29、PANICLOW 16〜21、DREQHIGH 8〜13、DREQLOW 0〜5。D0 は 23〜29、15〜21、7〜13、0〜6。値は PANIC 0x10・0x10、DREQ 0x1c・0x1c（C・D0 で同じ値） | L-regs: 981〜997。L-hdmi: 2164〜2179 | 一致 |
| V08 | 8 | MAI_FMT: sample rate の code 8〜15（48000 Hz = 9）、audio format 16〜23（PCM = 2） | L-regs: 766〜791（enum の 48000 = 9 は 787、PCM = 2 は 770）。L-hdmi: 2141〜2151 | 一致 |
| V09 | 9 | MAI_SMP: N 8〜31、M 0〜7。sample の周期は N ÷ (M+1) clock。Linux は 108 MHz ÷ fs の最良の有理近似（N の上限 0xffffff、M の上限 256）で M−1 を書く | L-regs: 999〜1005。L-hdmi: 1884〜1911 | 一致（108000000 ÷ 48000 = 2250 ÷ 1、N=2250・M の field 0） |
| V10 | 10 | MAI_CONFIG: BIT_REVERSE 26、FORMAT_REVERSE 27、channel の mask 0〜15（2 channel で 0x3）。Linux は 3 つを立てる | L-regs: 747〜750。L-hdmi: 2192〜2195 | 一致 |
| V11 | 11 | MAI_CHANNEL_MAP: channel i に i を 4 bit ずつ（BCM2711・BCM2712 の形。2 channel で 0x10） | L-hdmi: 1852〜1862（4 bit の形）、3591・3563（BCM2712 の 2 つの variant が 4 bit の形を使う。variant は 3540〜3594） | 一致 |
| V12 | 12 | AUDIO_PACKET_CONFIG: ZERO_DATA_ON_SAMPLE_FLAT 29、ZERO_DATA_ON_INACTIVE_CHANNELS 24、B frame の印 10〜13（Linux は 8、alsa-lib の値に合わせる）、CEA の mask 0〜7（2 channel で 0x3）。Linux は 29・24・8・mask を書く | L-regs: 752〜764。L-hdmi: 2153〜2161・2199 | 一致。補い: 「flat の時 0、無い channel は 0」は Linux が両方の bit を立てる意味（p001 の起動の順 3 の書き方と同じ） |
| V13 | 13 | CRP_CFG: N 0〜19、EXTERNAL_CTS_EN 24、DISABLE 25、USE_MAI_BUS_SYNC_FOR_CTS 26。comment の要点: bit 24 は「N・audio の clock・video の clock から CTS を作る、N は 128 で割り切れること」、bit 25 は「CRP の packet を送らない」、bit 26 は「CTS_PERIOD の数え方を pixel clock から MAI の bus の sync の pulse に替える」。Linux は bit 24 と N を書き、CTS_0・CTS_1 に同じ値を書く | L-regs: 796〜809。L-hdmi: 1913〜1938 | 一致。補い: **N は 128 で割り切れる事**（6144・11648・5120 は割り切れ、5824・6864 は割り切れない）→ p001 D8 に反映（下の「p001 の訂正」） |
| V14 | 14 | SCHEDULER_CONTROL の MODE_HDMI bit 0、HOTPLUG の CONNECTED bit 0 | L-regs: 746・843。L-hdmi: 1864〜1881（BCM2711・BCM2712 の hotplug の読み） | 一致 |
| V15 | 15 | RAM_PACKET_CONFIG: 全体の ENABLE bit 16、slot ごとの enable は bit（slot の番号）、audio は bit 4。RAM_PACKET_STATUS の bit（slot の番号）が送信中 | L-regs: 796。L-hdmi: 669〜694（stop: bit を落とし STATUS が消えるのを 100 ms 待つ）、726〜728（全体の ENABLE が要る）、760〜770（start: bit を立て STATUS が立つのを 100 ms 待つ） | 一致 |
| V16 | 16 | RM_OFFSET = ((VCO × 2) << 22) ÷ f_ref の後 2 bit 右（= VCO × 2^21 ÷ f_ref、切り捨て）、f_ref 54 MHz。VCO は f_TMDS × VCODIV × 10、BCM2712 の VCO の範囲 8〜12 GHz（VCODIV はその中の中央の値） | L-phy: 171・250〜261・605〜628・968〜998 | 一致。補い: 逆算 f_TMDS = RM × f_ref ÷ 2^21 ÷ (VCODIV × 10) の誤差は切り捨ての分で 1 Hz の桁（54 MHz ÷ 2^21 ≈ 25.7 Hz を VCODIV × 10 で割る）。Linux の mode の clock は kHz の単位（74.25/1.001 は 74176000 Hz）なので、規格の名目の rate との差は数 ppm |
| V17 | 17 | DVP（`clock@7c700000`、reg 0x7c700000/0x10、compatible `brcm,brcm2711-dvp`、親 `clk-108M` 108000000 Hz）: MISC_CONFIG +0x08 の bit 3（hdmi0）・bit 4（hdmi1）が 1 で 108 MHz を止める（gate）。SW_INIT +0x04 が reset（6 本）、**bit が 1 で reset の中**（reset-simple の active-high）。hdmi0 は reset 1（bit 1）、hdmi1 は reset 2（bit 2） | L-dvp: 10〜13・48〜55・58〜75。L-rst: 35〜47・89〜95。D-C1: 474〜481・525〜558（resets・clocks）・2541〜2547 | 一致。補い: SW_INIT の bit の番号と極性（p001 D2a に足した） |
| V18 | 18 | DMA の node `dma@10600`（axi、reg 0x10_0001_0600/0x600、compatible `brcm,bcm2712-dma`）、`brcm,dma-channel-mask` 0x7c0（channel 6〜10）、interrupts SPI 0x56〜0x5b（86〜91）、interrupt-names `dma6`〜`dma11` | D-C1・D-D0・D-500: 2432〜2440 | 一致 |
| V19 | 19 | DMA の cell（Linux の取り決め）: 下位 5 bit が DREQ。bit 16〜19 QoS、20〜23 panic QoS（CS へそのまま）、24 source を 128 bit、25 destination を 128 bit、27 WAIT_RESP を付けない、28 WAIT_FOR_WRITES、29 DISDEBUG（CS へ）、30 burst（長さ 3、SRCI と DESTI の両方）。`0x41fa000c` は DREQ 12、QoS 0xa、panic 0xf、128 bit の source、burst、WAIT_RESP を付ける | L-dma: 164〜190（cell の印と CS_FLAGS）、257〜260（40 bit の CS_FLAGS）、1063〜1064（cyclic の info の組み立て）、1237〜1251（cell をそのまま dreq に） | 一致 |
| V20 | 20 | DREQ: C1 の dtb は hdmi0 10（0x41fa000a）・hdmi1 17（0x41fa0011）。D0 と Pi 500 は 12（0x41fa000c）・13（0x41fa000d） | D-C1: 535・552。D-D0・D-500: 535・552 | 一致 |
| V21 | 21 | 40 bit の channel の register: CS 0x00、CB 0x04（CB の番地 >> 5）、DEBUG 0x0c（RESET bit 23）、TI 0x10、SRC 0x14、SRCI 0x18、DEST 0x1c、DESTI 0x20、LEN 0x24、NEXT_CB 0x28、DEBUG2 0x2c。BCM2712 は channel 6〜11 が 40 bit の形。channel の register は base + channel × 0x100 | L-dma: 217〜219・226〜236・286・335〜339・394〜398・1198〜1207 | 一致 |
| V22 | 22 | CS: ACTIVE 0、END 1、INT 2、DREQ 3、RD_PAUSED 4、WR_PAUSED 5、DREQ_PAUSED 6、WAITING_FOR_WRITES 7、PROT 8〜9（両方を立てる）、ERR 10、QoS 16〜（macro は 5 bit の mask、cell からは 4 bit だけ通す）、panic QoS 20〜、TRANSACTIONS 25、WAIT_FOR_WRITES 28、DISDEBUG 29、ABORT 30、HALT 31 | L-dma: 238〜260 | 一致（QoS の幅は 4 bit で扱う） |
| V23 | 23 | TI: INTEN 0、TDMODE 1、WAIT_RESP 2、WAIT_RD_RESP 3（Linux は S_DREQ の時だけ）、DREQ の番号 9〜13、S_DREQ 14、D_DREQ 15、S_WAIT 16〜23、D_WAIT 24〜31。memory → 周辺の時の TI = INTEN（割り込みの CB）・WAIT_RESP・D_DREQ・DREQ の番号 | L-dma: 263〜271・370〜378・1095〜1110 | 一致 |
| V24 | 24 | SRCI・DESTI: 番地の上位 8 bit 0〜7、burst 長 8〜11、INC 12、幅 13〜14（0 = 32 bit、2 = 128 bit）、IGNORE 15、STRIDE 16〜。memory → 周辺: SRCI に INC と（cell しだいで）128 bit と burst、DESTI は INC 無し・32 bit・burst | L-dma: 297〜306・380〜392・541〜550 | 一致 |
| V25 | 25 | 40 bit の control block: 8 word（TI、SRC、SRCI、DEST、DESTI、LEN、NEXT_CB（番地 >> 5）、予約）、32 byte 揃え（番地の下位 5 bit が 0）。cyclic は period ごとに CB を作り、period の終わりの CB に INTEN、最後の CB の NEXT_CB を先頭へ | L-dma: 72〜81・394〜398・417〜465・504〜627・1135〜1143 | 一致 |
| V26 | 26 | IEC 60958 の subframe（32 bit）: preamble 0〜3（Z 0x08 = block の始めの左、X 0x02 = 左、Y 0x04 = 右。右は block の始めでも Y）、AUX 4〜7（0）、sample 8〜27（16 bit は 12〜27）、V 28（0）、U 29（0）、C 30（channel status の bit）、P 31（bit 4〜31 の 1 の数を偶数にする）。channel status の bit は 192 frame の block の frame の番号 n で byte n/8 の bit n%8（下位から）、同じ frame の左右で同じ bit | A-iec: 73〜90（parity）・92〜131（subframe）・190〜234（frame の counter、左右で同じ counter、192 で巻く）・697〜698（preamble の値） | 一致 |
| V27 | 27 | channel status の 24 byte（48 kHz、16 bit、民生、LPCM）: byte 0 = 0x04（民生・音声・著作権の主張なし・emphasis なし・mode 0）、byte 1 = 0x00（category 一般）、byte 2 = 0x00（source・channel の番号を示さない）、byte 3 = 0x02（Fs 48 kHz・clock の精度 level II の 1000 ppm）、byte 4 = 0x02（最大の語長 20 bit・語長 16 bit）、byte 5〜23 = 0 | L-aes: 17・18・30・32・47・110〜116・129〜136。L-cs: 29〜45（Linux の民生の既定: byte 0 = NOT_COPYRIGHT、byte 1 = GENERAL）・48〜117（48 kHz と 16 bit の埋め方）。A-iec: 573〜579（alsa-lib の既定は byte 0 = 0、byte 1 = 0x82。こちらは使わない） | 補い: p001 は「民生・LPCM・48 kHz・語長 16」だけ。byte 0〜4 の値を決めた（Linux の民生の既定と 48 kHz・16 bit の埋め方。IEC 60958-3 の本文は未確認） |
| V28 | 28 | Audio InfoFrame: HB0 型 0x84、HB1 版 1、HB2 長さ 10、PB0 checksum（header と payload の 14 byte の和が 0 になる値 = 256 − 和）、PB1 = CT（bit 4〜7、0 = stream を見る）・CC（bit 0〜2、channel 数 − 1、2 channel で 1）、PB2 = SF（bit 2〜4、0）・SS（bit 0〜1、0）、PB3 = 0、PB4 = CA（0）、PB5 = LSV（bit 3〜6、0）・DM_INH（bit 7、0）、PB6〜PB10 = 0。2 channel の値: `84 01 0a 70 01 00 00 00 00 00`（＋ 0 が 4 byte） | L-ifh: 49・55・58・270・289・296。L-ifc: 35〜52（checksum）・354〜364（init）・391〜410（payload）・427〜456（pack）。L-codec: 537〜547（Linux の HDMI の codec が stream・0 を使う） | 一致。補い: byte ごとの値（checksum 0x70 は 0x84+0x01+0x0a+0x01 = 0x90 から計算） |
| V29 | 29 | N・CTS（48 kHz）: CTS = f_TMDS × N ÷ (128 × 48000)。下の「N と CTS の表」。Linux vc4 は表を持たず、N = 128 × fs ÷ 1000 = 6144、CTS は mode の pixel clock（kHz × 1000）から切り捨て。CTS_0・CTS_1 は「period の field で交互に使う」と comment にあるが、**period の field の定義は出典に無い**（Linux は同じ値を両方に書く） | L-hdmi: 1913〜1938。L-regs: 796〜809（period の field の define が無い事を grep で確かめた: `PERIOD` は 798 の comment だけ）。L-dw: 561〜620（48 kHz の N の表 600〜614）・664〜666（CTS の式） | **訂正**: p001 D8 の「74.25/1.001 MHz の N 11648 と CTS 210937.5」は 32 kHz の値。48 kHz は N 11648・CTS 140625（整数）。下の「p001 の訂正」 |
| V30 | D4 | HVS の VERSION: HVS の reg（`hvs@107c580000`、0x10_7c580000/0x1a000）の +0x000 の bit 0〜7。0x53 = C0（C 系）、0x54 = D0。Linux は tree の `brcm,bcm2712-vc6` で C 系と仮に決め、HVS の VERSION が 0x54 なら D0 に直す（D0 の dtb の gpu node は `brcm,bcm2712d0-vc6` も持つが、Linux の照合の表は `brcm,bcm2712-vc6` だけ） | L-regs: 543〜546。L-hvs: 2083〜2091。L-drv: 335・500〜504。D-C1・D-D0: 2549〜2560（hvs）・991〜995（gpu の compatible） | 一致（zedBSD の `hvs6.c` も同じ読み） |
| V31 | D6 | 番地の変換: soc の ranges `<0x0 → 0x10_0000_0000、0x8000_0000>`（hd の 0x7c720000 は CPU の 0x10_7c720000）。axi の ranges と dma-ranges は恒等（0〜64 GiB、0x10_0000_0000〜+4 GiB、0x14・0x18・0x1c_0000_0000〜各 16 GiB）。Linux の HDMI は MAI の DMA の番地を `hd` の resource の始め ＋ MAI_DATA の offset にする（幅 4 byte） | D-C1: 313〜315・983〜988。L-hdmi: 2365〜2378。L-dma: 1108（phys_to_dma で destination） | 一致 |
| V32 | D5 | channel の番号の始め: Linux は map した reg の番地 ÷ 0x100 の下位 4 bit（0x10600 → 6）を最初の channel にし、base から 6 × 0x100 を引く。割り込みは名前 `dmaN` で引く | L-dma: 1296〜1302・1383〜1394 | 一致（Linux は仮想の番地で計算するが page の中の offset は物理の番地と同じ） |
| V33 | D5 | 止め方（40 bit）: CB が 0 なら止まっている（ACTIVE は当てにならない）。ACTIVE を落とし TRANSACTIONS が消えるのを上限つき（100 回）で待つ、CS に PROT だけを書く、DEBUG に RESET を足す。始め方: CB に先頭の CB の番地 >> 5、CS に ACTIVE・PROT・cell の CS の印。割り込み: CS に INT・ACTIVE・PROT・cell の CS の印を書く（INT を消し、cyclic を続ける）。位置: SRC と SRCI の下位 8 bit から 40 bit の番地 | L-dma: 676〜708（abort）、749〜769（start）、778〜825（callback）、925〜934（位置） | 一致 |
| V34 | D7 | MAI の始め・止めの順（Linux）: startup で MAI_CTL に RESET・FLUSH・DLATE・ERRORE・ERRORF。prepare で MAI_SMP → MAI_CTL（CHNUM・WHOLSMP・CHALIGN・ENABLE）→ MAI_FMT → MAI_THR → MAI_CONFIG → MAI_CHANNEL_MAP → AUDIO_PACKET_CONFIG → CRP_CFG・CTS → audio InfoFrame。shutdown で MAI_CTL に DLATE・ERRORE・ERRORF、その後 InfoFrame の slot を止め（待たない）、MAI_CTL に RESET、ERRORF、FLUSH を順に。BCM2712 の variant は PHY の RNG の手続きを持たない | L-hdmi: 1963〜2003・2005〜2025・2027〜2058・2099〜2216・3540〜3594 | 一致（p001 の起動から再生までの順 3・5 と同じ） |
| V35 | 補い | 各 HDMI の node の `dvp` の region（hdmi0 0x7c701000、hdmi1 0x7c706000）の +0x0bc に CLOCK_STOP がある（Linux は pixel の clock を止める時に使う）。音の driver は読みも書きもしない | L-hregs: 474・574。L-hdmi: 227〜228・1432 | 補い（p001 の D2 の「触らない」に足した） |

## N と CTS の表（48 kHz、8 bit の色）

CTS = f × N ÷ 6144000。「整数」は CTS が整数になるか（f は名目の値、1.001 の rate は f ÷ 1.001 の有理数で計算した）。

| f_TMDS（名目） | N | CTS | 整数 | N ÷ 128 | 出典 |
| --- | --- | --- | --- | --- | --- |
| 25.2/1.001 MHz（Linux の mode では 25175000 Hz） | 6864 | 28125 | はい | 53.625（割り切れない） | L-dw: 603〜604 |
| 25.2 MHz | 6144 | 25200 | はい | 48 | L-dw: 613〜614（既定）、計算 |
| 27 MHz | 6144 | 27000 | はい | 48 | 同上 |
| 27 × 1.001 MHz（27027000 Hz） | 6144 | 27027 | はい | 48 | L-dw: 605〜606 |
| 54 MHz | 6144 | 54000 | はい | 48 | 計算 |
| 74.25/1.001 MHz（74176000 Hz） | 11648 | 140625 | はい | 91 | L-dw: 607〜608 |
| 74.25 MHz | 6144 | 74250 | はい | 48 | L-dw: 613〜614、計算 |
| 148.5/1.001 MHz（148352000 Hz） | 5824 | 140625 | はい | 45.5（割り切れない） | L-dw: 609〜610 |
| 148.5 MHz | 6144 | 148500 | はい | 48 | 計算 |
| 297/1.001 MHz | 5824 | 281250 | はい | 45.5（割り切れない） | 出典の file 無し（HDMI 1.4b の表の値、規格書は未確認。整数になる事は計算で確かめた） |
| 297 MHz | 5120 | 247500 | はい | 40 | L-dw: 611〜612 |
| 594/1.001 MHz | 5824 | 562500 | はい | 45.5（割り切れない） | 出典の file 無し（同上） |
| 594 MHz | 6144 | 594000 | はい | 48 | 計算 |

- 表に無い rate（deep color の 1.25・1.5 倍など）は、N を 6144 から近い順に 4096〜20480（128 fs ÷ 1500〜128 fs ÷ 300、規格の推奨の範囲として広く知られる値。規格書は未確認）で探し、CTS が整数になる最初の N（例 92.8125 MHz は N 4096・CTS 61875）。名目の値が分からない時（表の rate から ±200 ppm の外）は f を 1 kHz に丸め N 6144（CTS = f ÷ 1000、Linux と同じ形）。
- 上の 13 の rate を同じ探し方で計算すると、297 MHz（探すと N 6144・CTS 297000）の他は表と同じ値になる。297 MHz は表の 5120 を使う。
- EXTERNAL_CTS_EN を立てる時（Linux の形）は CRP_CFG の comment の「N は 128 で割り切れる」に従い N 6144 を使う（Linux もそう）。表の N は bit を落として software の CTS を送る時に使う（p006 の 2 通りの試し）。

## 手順の出典（Procedure referenced の対象）

| 手順 | 出典 |
| --- | --- |
| MAI の始め・止め・reset | L-hdmi: 1963〜2058・2099〜2216（V34） |
| InfoFrame の書き込み・止め | L-hdmi: 669〜694・696〜773（slot を止め STATUS が消えるのを 100 ms 待つ → 書く → 残りを 0 → slot の bit を立てる → STATUS を 100 ms 待つ） |
| 40 bit の DMA の環の作り・始め・割り込み・止め・位置 | L-dma: 504〜627・1054〜1143・749〜769・778〜825・676〜708・925〜934（V25・V33） |
| PHY の RM_OFFSET と VCODIV の作り（逆算の元） | L-phy: 250〜261・605〜628・954〜998 |
| subframe の作り | A-iec: 73〜131・190〜234 |
| channel status の埋め方 | L-cs: 29〜117 |
| InfoFrame の詰め方と checksum | L-ifc: 35〜52・391〜456 |
| HVS の VERSION による C・D0 の判定 | L-hvs: 2083〜2091 |

## C1 と D0 の dtb の差（音に関わる物）

`diff` の全体は 44 行（`bcm2712-rpi-5-b` と `bcm2712d0-rpi-5-b` の dtc の text）。Pi 500 の dtb は音に関わる node で D0 と同じ（V01・V18・V20 の行を比べた）。

| node | C1（`bcm2712-rpi-5-b`） | D0（`bcm2712d0-rpi-5-b`） | 音への関わり |
| --- | --- | --- | --- |
| hdmi0 の dmas（535 行） | `0x41fa000a`（DREQ 10） | `0x41fa000c`（DREQ 12） | DREQ（V20） |
| hdmi1 の dmas（552 行） | `0x41fa0011`（DREQ 17） | `0x41fa000d`（DREQ 13） | DREQ（V20） |
| gpu の compatible（992 行） | `brcm,bcm2712-vc6` | `brcm,bcm2712d0-vc6`、`brcm,bcm2712-vc6` | stepping の手がかり（Linux は使わず HVS の VERSION で決める、V30） |
| spi@7d004000 の dmas（576 行） | DREQ 6・7 | DREQ 3・4 | 音には関わらない（DREQ の番号が stepping で変わる別の例） |
| 他（375・397・399・635・636・740・770・771 行） | 割り込みの番号・GPIO の pinctrl の名前と reg | | 音には関わらない |

- HDMI の node の reg・clock・reset・割り込み、DVP、DMA の node（reg・mask・割り込み）、`clk-108M`、HVS の node、axi・soc の ranges は 3 つの dtb で同じ。

## p001 の訂正と補い（同じ commit で p001 を直した）

1. **訂正** D8 と値の表の「N・CTS」: 74.25/1.001 MHz の 48 kHz は N 11648・CTS **140625**（p001 の 210937.5 は 32 kHz の値）。48 kHz の 8 bit の色の表の rate では CTS は全て整数。
2. **補い** D8: CTS_0・CTS_1 を交互に使う period の field は出典に無い → 両方に同じ整数を書く（Linux と同じ）。整数にならない rate は近い整数。
3. **補い** D8: EXTERNAL_CTS_EN の comment は「N は 128 で割り切れる」と書く。Linux の既定（bit を立てる）は N 6144。表の N（5824・6864 は 128 で割り切れない）は bit を落とした時に使う。Q3 の 2 通りの試しの前提に足した。
4. **補い** D2a: DVP の SW_INIT の bit は hdmi0 が 1、hdmi1 が 2、1 が reset の中。
5. **補い** D3: 3 つの dtb で HDMI の node（と HVS・gpu の node）は status disabled。driver は status で除外しない。
6. **補い** D2: 各 port の `dvp` の region の CLOCK_STOP（+0x0bc）は触らない。
7. **補い** 値の表: channel status の byte 0〜4 の値（V27）と InfoFrame の byte（V28）を書いた。

## 照合の数

- p001 の値の表は 29 行（`plan/ws349/phase001/phase.md` の「値の表」の本文の行を数えた）。この表の V01〜V29 が 1 行ずつ対応する（一致 21、補いを含む一致 7、訂正 1）。V30〜V35 は p001 の決定の本文の値と足した値。
- 未確認: 規格書（HDMI 1.4b・IEC 60958-3・CEA-861）の本文との照合（手元に無い）、firmware が dtb の status を書き換えるか（p004 の実機）、EXTERNAL_CTS_EN の本当の意味（p006）。
