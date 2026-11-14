<!-- awesome-plan project=zedbsd record=ws349-p001 -->

# ws349-p001 設計: BCM2712 の HDMI の音

- WS: [ws.md](../ws.md)。第一版: Queue q2042-i01（P1、2026-11-13）。改訂（第二版）: Queue q2118-i01（P8、worktree `/home/awe/zedBSD-worktrees/p8-ws349`、branch `agent/p8-ws349`、基点 main 1bf2cbe5e3、2026-11-15）。
- 状態: cleared（2026-11-13 P1 の第一版）。第二版（2026-11-15 P8）: 乖離の確かめ、出典の読み、design-reviewer の敵対的レビュー（[design-review.md](design-review.md)、指摘 15 件）の反映。cleared のままでよいかは Q1 の判断（P8 の推し: cleared。未確定は p004 の実機の読みで閉じる計画）。

## 目的

Pi 5（と Pi 500）の HDMI から音を出す経路・所有の境界・名前を決め、実装の Phase に分ける。この Phase では code を書かない。

## 乖離の確かめ（2026-11-15、main 1bf2cbe5e3）

| 第一版の記述 | 今の事実 | 扱い |
| --- | --- | --- |
| 画面は `display-start.c`・`display-program.c` が動かし、HDMI の register の地図がそこにある | それは Pi 4（BCM2711、`CONFIG_DRIVER_RPI4_GPU`）の物。Pi 5 の display は `display/hvs6.c`・`hvs6-flip.c`・`display-device.c` で、HDMI の encoder・pixel valve の timing・mode を**一切書かない**（firmware が持つ、hvs6.c の冒頭の comment と実装）。mode は boot の時の 1 つで、hotplug・mode の変更を出さない（`display-device.c` の `output_events`） | display driver に「HDMI の port が動いているか・TMDS の clock」を問う口は作れない（持っていない）。音の driver が HDMI の register を自分で読む（D1） |
| DREQ は tree の下位の 0x0c・0x0d | D0 の dtb（`bcm2712d0-rpi-5-b.dtb`・`bcm2712-rpi-500.dtb`）は 12・13。**C1 の dtb（`bcm2712-rpi-5-b.dtb`）は 10・17**。試験の Pi 5 は D0 の chip だが firmware は C1 の dtb を渡すと [tools.md](../../tools.md) の「実機」に記録がある。一方 `mk/platform-rpi5/config.txt` の comment は「bootloader が stepping で選ぶ」と書く（記録どうしが食い違う） | tree の値だけを信じない。stepping を hardware から読み、stepping の表と照らす（D4）。p004 の実機の読みで、どちらの dtb が渡るかと FAT の中身・overlay を確かめる（Q1） |
| 上位の 16 bit（0x41fa）の意味は未確定 | Linux の BCM2712 の DMA の driver が dma の cell の上位の bit を自分の取り決めの印に使う: QoS 0xa・panic QoS 0xf・128 bit の source の読み・burst（値の表） | 確定。DREQ は下位 5 bit |
| audio の clock の provider は未確定 | `clock@7c700000`（`brcm,brcm2711-dvp`）の clock 0・1 は、108 MHz の固定 clock を `MISC_CONFIG`（+0x08）の bit 3・4 で止める gate（bit が 1 で止まる）。同じ node の `SW_INIT`（+0x04）が HDMI の reset | 確定。gate の bit だけを触り、reset は読むだけ（D2） |
| CTS は「TMDS の clock から」 | Linux は mode の pixel clock から計算する。Pi 5 の zedBSD は mode を持たない | TMDS の文字の rate を PHY の register の読み戻しから求める（D8） |
| mk の枠 `CONFIG_DRIVER_RPI5_AUDIO`「RPi5 RP1 Audio driver」 | source は空、`mk/config.mk` の説明は RP1（PCI に依存）を指す。BUG-418 の時に「source の無い枠」として n にされた | HDMI の音の新しい名前を案にする（D12、mk の案、[user-questions.md](../user-questions.md) U1） |
| 未確定: BCM2712 の DMA の control block の型 | Linux rpi-6.12.y では BCM2712 の channel 6〜11 は 40 bit の形（8 word、32 byte 揃え、CB の番地は 5 bit 右へずらす） | 確定（値の表） |
| 実装の Phase は設計 → 部品 → driver → 実機 | 実機で読まないと決まらない事（Q1〜Q6）が D3・D8・D10 の作りを左右する（レビューの指摘 10） | 読みだけの probe を driver の再生の部分より前に置く（Phase の表） |

## 決定

- **D1 独立の driver**: HDMI の音は display・GPU の driver から独立の driver にする（別の CONFIG、別の file）。理由: Pi 5 の display driver は HDMI に触れず mode も変えない。GPU は `hw.gpu.start=1` まで握られず `rpi4gpu.off=1` で無しにもなるが、音は画面の driver の有無に依らず出したい。音の driver は firmware が点けた HDMI をそのまま使う。
- **D2 書く register の境界**（これ以外は読むだけ、firmware の設定を壊さない）:
  - 書く: `hd` の region の port ごとの MAI（CTL・THR・FMT・SMP）、HDMI の core の MAI_CHANNEL_MAP・MAI_CONFIG・AUDIO_PACKET_CONFIG・CRP_CFG・CTS_0・CTS_1、packet RAM の audio InfoFrame の slot（4 番）とその slot の enable の bit（RAM_PACKET_CONFIG の bit 4 だけの read-modify-write）、DVP の MISC_CONFIG の自分の port の gate の bit（read-modify-write）、DMA の 1 channel。
  - 読むだけ: SCHEDULER_CONTROL（HDMI か DVI か）、HOTPLUG、RAM_PACKET_CONFIG の全体の enable（bit 16）、RAM_PACKET_STATUS、RM_OFFSET・PHY の VCOCLK_DIV と REFCLK、DVP の SW_INIT、HVS の VERSION。
  - 触らない: DVP の SW_INIT への書き込み（reset）、PHY の設定、timing、CSC、AVI など他の packet、CEC、DDC。
  - firmware（VPU）がこれらを実行中に書くか、read-modify-write が firmware の書き込みと競合しうるかは未観測（Q5）。driver の側の書き込みは 1 つの spin lock の下でだけ行う。
- **D2a 読む前の安全**: 使っていない port・抜かれた port の HDMI の clock を firmware が止めていると、その register の読みが SError か bus の停止になりうる（Linux は HDMI の register に触る前に必ず runtime PM で clock を入れる。推測を含む）。port の register は、DVP の SW_INIT でその port の reset が解けている時だけ読む。attach の各段の後に `drv_rpi5_serror_check()` を呼ぶ（`rpi5.c` の慣例）。使っていない port の読みが安全かは p004 の probe で確かめ（Q8）、安全でなければ firmware の mailbox の clock の状態（`hdmi` の clock）を読んでから触る形に直す。
- **D3 port**: 起動の時に HDMI0、HDMI1 の順に「reset が解けている、SCHEDULER_CONTROL が HDMI の mode、HOTPLUG が接続」の最初の 1 port を選び、`/dev/dsp` を 1 つだけ出す。起動の時にどちらも当てはまらなければ登録しない（log を 1 行、audiod は今どおり「no device」で動く）。登録の後で DVI・未接続になった時は D10 の「software の時計」で動く（登録は取り消さない）。二つの画面の時の扱いと「起動の後に画面を挿した時」は製品の判断（U2・U3）。
- **D4 stepping**: HVS の VERSION（0x53 は C 系、0x54 は D0。`hvs6.c` と同じ読み）で決める。DREQ は stepping の表（C: HDMI0 10・HDMI1 17、D0: 12・13。firmware 3d301dd の 3 つの dtb の値）と tree の cell を照らし、食い違えば stepping の表を使い log に残す。MAI_THR の field の幅も stepping で違う。HVS の VERSION がどちらでもなければ登録しない。
- **D5 DMA**: tree の `brcm,bcm2712-dma` のうち `brcm,dma-channel-mask` に入る channel（0x7c0 なら 6〜10）の大きい番号から 1 つ。channel の番号は node の reg の**物理番地**から（0x10600 ÷ 0x100 の下位 4 bit が最初の channel）、channel ごとに 0x100。割り込みは `interrupt-names` の `dmaN`。40 bit の形の control block の環を fragment ごとに 1 つ、各々 INTEN と WAIT_RESP、最後が先頭へ戻る。source は増やし、destination は MAI の data の register（増やさない、D_DREQ と DREQ の番号）。CS は ACTIVE・PROT と cell の QoS・panic QoS（各 4 bit）。source の幅と burst は cell の印どおり（burst は SRCI と DESTI の両方、実機で不調なら 32 bit・burst 無しへ落とす）。割り込みで INT を消す時は INT・ACTIVE・PROT と QoS を一緒に書く（INT だけを書くと channel が止まる）。止め方: CB が 0 なら止まっている。そうでなければ ACTIVE を落とし TRANSACTIONS が消えるのを上限つきで待ち、CS を PROT に戻し DEBUG の RESET、CB が 0 になったのを確かめてから戻る（時間切れでも reset の後に確かめる）。
- **D6 番地**: MAI の data の CPU の物理番地は `hd` の reg（soc の ranges で 0x10_7c720000）＋ port の DATA の offset。BCM2712 の DMA から見た番地は axi の `dma-ranges` が恒等なので同じ値（40 bit、上位 8 bit は DESTI に）。MAI の番地と RAM が dma-ranges の恒等の範囲に入る事を attach で確かめ、入らなければ登録しない。DMA の device は恒等なので `drv_dma_device_create`（40 bit、`segment_boundary` 4 GiB、非 coherent）。ring と control block が 4 GiB の境を跨がない（SRC と SRCI を別々に読むため）。
- **D7 形式と変換**: S16_LE・2 channel・48000 Hz の 1 つだけ（他の rate は audiod の ASRC が変える）。framework の ring（4096 byte × 8）の隣に、IEC 60958 の subframe（32 bit）の ring（8192 byte × 8）と control block を driver が持つ。
  - 変換の時: audio の core は `drv_audio_interrupt()` の中で、mmap の stream なら shadow から次の fragment を写す（`audio.c` の `audio_mmap_advance_locked`、lead 2 fragment）。audiod は mmap を使う。だから割り込みでは、自分の spin lock を外して `drv_audio_interrupt()` を先に呼び、その後で lock を取り直し `running` を確かめてから、DMA が今いる fragment k と k+1 を変換し直す（core が写し終えた所まで）。prepare では framework の ring の今の中身を全部変換し（core は書き込みの後・mmap の lead の写しの後に prepare を呼ぶ）、start の直前に 0 と 1 を変換し直す（`rpi4-audio.c` の prepare・start と同じ考え）。
  - subframe: bit 0〜3 preamble（block の始め 0x8、左 0x2、右 0x4）、bit 4〜27 に 24 bit の sample（16 bit は bit 12〜27、下位は 0）、V=0・U=0・C は channel status、bit 31 は bit 4〜30 の偶数 parity。
  - channel status の 192 frame の block の位置は ring の中の位置では決まらない（8192 frame は 192 の倍数でない）ので、fragment の通し番号（64 bit）× 1024 から求める。通し番号は割り込みの回数ではなく SRC の位置と前回の位置の差から進める（取りこぼした割り込みでもずれない）。同じ fragment を変換し直しても同じ値になる。channel status: 民生・LPCM・48 kHz・語長 16 bit（最大 20）。
  - 位置（`position`）は subframe の ring の中の offset ÷ 2。ring の終わりの番地を指していれば 0。
- **D8 N と CTS**: CTS = f_TMDS × N ÷ (128 × 48000)。f_TMDS（TMDS の文字の rate。deep color も含む規格どおりの値）は PHY の読み戻し: RM_OFFSET の bit 31（OFFSET_ONLY）を除いた値で VCO = RM × f_ref ÷ 2^21（f_ref は PLL_REFCLK の REFFRQ、MHz）、f_TMDS = VCO ÷ (VCODIV × 10)。VCODIV_EN が 0、OFFSET_ONLY が立っていない、25〜600 MHz の外のどれかなら「分からない」として登録しない（Linux は pixel clock を使うが、規格の CTS は TMDS の文字の rate による。8 bit の色では同じ）。N は規格の表（HDMI 1.4 の 48 kHz の推奨の N。1.001 の rate、例えば 74.25/1.001 MHz の N 11648 と CTS 210937.5）から選び、CTS が整数にならない時は CTS_0 と CTS_1 に前後の整数を書く（交互に使う period の field は p002 で出典を探す。無ければ近い整数 1 つ、数 ppm のずれは Q3 の長い再生で観る）。CRP_CFG は Linux と同じく EXTERNAL_CTS_EN を立てて N を書く（Linux の header の comment はこの bit を「N・audio の clock・video の clock から CTS を作る」と書き、名前と意味が食い違う。p005 で立てた時と落とした時の両方を試す、Q3）。
- **D9 MAI の clock**: tree の `audio` の clock（108 MHz の固定 clock）÷ 48000 を最良の有理近似で MAI_SMP の N・M に（108 MHz なら N=2250、M=1）。
- **D10 監視の thread と software の時計**: 割り込みに頼らない監視の kernel thread（`kthread_create`、`rpi5-fan.c` と同じ形）を 1 つ持ち、再生の間だけ 10 ms ごとに起きる。
  - 確かめる物（読むだけ）: SCHEDULER_CONTROL が HDMI、HOTPLUG、CRP_CFG と CTS が自分の値、RAM_PACKET_CONFIG の bit 4、MAI_CTL の ENABLE と ERRORE・ERRORF、MAI_SMP・FMT・THR、DVP の gate、PHY の読み戻しの f_TMDS、DMA の SRC が進んでいるか。
  - 書き換えられた（firmware が hotplug で張り直した、f_TMDS が変わった）と見たら、thread の中で DMA を止め、HDMI の側と MAI を全部書き直し（InfoFrame は slot を止め → RAM_PACKET_STATUS の消えるのを 100 ms の上限で待ち → 書き → enable）、DMA を同じ位置から始め直す。
  - DMA が 2 fragment の時間（約 43 ms）より長く進まず、HDMI が使えない（DVI・未接続・reset）時は「software の時計」に切り替える: DMA を止め、thread が 1 fragment の時間ごとに位置を進めて `drv_audio_interrupt()` を呼ぶ（音は捨てる）。書き手と drain は止まらない。HDMI が使える状態に戻れば書き直して DMA に戻る。
  - 割り込みの手続きは INT を消す・変換・`drv_audio_interrupt()` だけで、HDMI の register を書かない。MAI の FIFO の誤りは数えて log を間引く。
- **D11 mode の変更**: Pi 5 の zedBSD は mode を変えない（D1 の事実）。firmware が張り直した時は D10 が追う。将来 Pi 5 で mode を変える driver ができたら、その WS が音の driver に止める・再開の口を足す（この WS の範囲の外、docs に制約として書く）。
- **D12 名前**: CONFIG は `CONFIG_DRIVER_RPI5_HDMI_AUDIO`（mk の案、U1）。file は `src/drivers/platform/rpi5/rpi5-hdmi-audio.c`（hardware）と `rpi5-hdmi-audio-wire.c`・`.h`（hardware に触れない計算、host 試験が build する）、`rpi5-hdmi-audio.h`（`drv_rpi5_hdmi_audio_init(uint64_t fdt_physical)`）。呼ぶのは `src/kern/platform/rpi5.c`（`rpi4.c` の音と同じ場所の考え）。log の接頭辞 `rpi5-hdmi-audio:`。DMA の部品を共通の DMA の engine にはしない（他に使う driver が無い）。HAL は変えない。register・bit の名前は Linux の名前を写さず独自に付ける（[guardrail](../../guardrail.md)、p007 で監査）。
- **D13 音量**: hardware に無い（audiod が縮める、Pi 4 の jack と同じ）。`get_volume`・`set_volume` は無し。
- **D14 lock**: driver の spin lock（`LOCK_RANK_DEVICE`）が `running`・DMA の register・位置・HDMI の側の書き込みを守る。`position` は自分の lock を取らず、割り込みの手続きが lock の下で更新した値を読む（core の IRQ-safe の lock の下で呼ばれるため。`rpi4-audio.c` の形）。`drv_audio_interrupt()` は自分の lock の外で呼ぶ。stop は `running` を落としてから DMA を止め、遅れて来た割り込みは `running` を見て ring に触れずに INT だけを消す。
- **D15 reboot・poweroff**: 再生の途中の reboot・poweroff で DMA・MAI を止める手続きは作らず、SoC の reset に任せる（推測、p006 で再生の途中の reboot を 1 回試す）。
- **D16 範囲の外**: Pi 4 の HDMI の音、RP1 の I2S、ELD（EDID の音の能力）の読み、多 channel・圧縮音・48 kHz 以外の hardware の rate、二つの port の同時の出力、登録の後の port の切り替え。

## 起動から再生までの順（p005 の実装の骨組み）

1. attach（起動の時）: tree から HDMI の node 2 つ・DMA・DVP・108 MHz の clock・hd の region を探し map。HVS の VERSION で stepping。D2a の安全の確かめ。D3 の port を選ぶ。DREQ を照らす。PHY の読み戻し。DMA の channel を休止の状態にして割り込みを登録。監視の thread を作る（寝たまま）。`drv_audio_register()`。
2. prepare: 変換の ring と control block の環を作り、framework の ring の今の中身を変換する。
3. start: 0 と 1 を変換し直す → gate を開ける → MAI_CTL を reset・flush（DLATE・ERRORE・ERRORF も） → MAI_SMP → MAI_CTL（channel 数 2・WHOLSMP・CHALIGN・ENABLE） → MAI_FMT（48 kHz の code 9、PCM の code 2） → MAI_THR → MAI_CONFIG（BIT_REVERSE・FORMAT_REVERSE・channel の mask 0x3） → MAI_CHANNEL_MAP（0x10） → AUDIO_PACKET_CONFIG（flat の時 0、無い channel は 0、B frame の印 8、mask 0x3） → CRP_CFG と CTS_0・CTS_1 → InfoFrame（slot を止め、待ち、書き、残りを 0 で埋め、enable、待ち。thread の文脈なので 100 ms の上限） → `running` → DMA を始める → 監視の thread を起こす。HDMI が使えなければ software の時計で始める。
4. 割り込み: lock → INT を消す（ACTIVE・PROT と一緒）→ DMA の誤り（CS の ERR、DEBUG の bit）を見る → 位置と通し番号を進める → unlock → `drv_audio_interrupt()` → lock → `running` なら k と k+1 を変換 → unlock。
5. stop: `running` を落とす → 監視の thread を寝かせる → DMA を止める（D5）→ MAI_CTL（DLATE・ERRORE・ERRORF）→ InfoFrame の slot を止める → MAI を reset・ERRORF・flush。gate は開けたまま（firmware が開けていた時と区別が付かないので閉じない）。

## 値の表（出典。code にする時は block ごとに「Values referenced:」、手順は「Procedure referenced:」で書く）

出典の版: Raspberry Pi Linux `rpi-6.12.y`（ab4372076bb8、2026-11-09）、alsa-lib master（f44e0abd5382、2026-10-19）、raspberrypi/firmware 3d301dd の dtb（zedBSD-firmware 2670dc1 の `rpi5-boot/`、dtc で読んだ）。code・comment・構造・名前は写さない（[guardrail](../../guardrail.md) の「MIT・GPL の出典のデータの引用」）。行番号つきの表は p002 で `plan/ws349/values.md` に作る。

| 値 | 中身 | 出典 |
| --- | --- | --- |
| HDMI の node | hdmi0 `0x7c701400`、hdmi1 `0x7c706400`。reg-names `hdmi dvp phy rm packet metadata csc cec hd`、`hd` は両方 `0x7c720000 0x100` | 3 つの dtb |
| hd の MAI（port の offset） | HDMI0: CTL 0x10・THR 0x14・FMT 0x18・DATA 0x1c・SMP 0x20。HDMI1: 0x30・0x34・0x38・0x3c・0x40 | `vc4_hdmi_regs.h`（BCM2712 の hdmi0・hdmi1 の field の表） |
| HDMI の core（`hdmi` の region） | MAI_CHANNEL_MAP 0x0a4、MAI_CONFIG 0x0a8、AUDIO_PACKET_CONFIG 0x0c0、RAM_PACKET_CONFIG 0x0c4、RAM_PACKET_STATUS 0x0cc、CRP_CFG 0x0d0、CTS_0 0x0d4、CTS_1 0x0d8、SCHEDULER_CONTROL 0x0e8、HOTPLUG 0x1c8 | 同上 |
| RM・PHY | RM_OFFSET（`rm` +0x018、bit 31 OFFSET_ONLY）、PHY の PLL_REFCLK +0x01c（REFFRQ 0〜9）、PLL_VCOCLK_DIV +0x02c（VCODIV 0〜9、EN 10） | 同上、`vc4_hdmi_phy.c` |
| packet RAM | `packet` の region の先頭から slot ごとに 0x24 byte、InfoFrame の型 − 0x80 が slot（audio 0x84 → 4）。byte の並びは HB0 型・HB1 版・HB2 長さ・PB0 checksum・PB1 以降。7 byte ごとに 2 word（3 byte・4 byte）、slot の残りは 0 | `vc4_hdmi_regs.h`、`vc4_hdmi.c`（infoframe の書き込み） |
| MAI_CTL の bit | RESET 0、ERRORF 1、ERRORE 2、ENABLE 3、CHNUM 4〜7、PAREN 8、FLUSH 9、EMPTY 10、FULL 11、WHOLSMP 12、CHALIGN 13、BUSY 14、DLATE 15 | `vc4_regs.h` |
| MAI_THR | C 系: PANICHIGH 24〜29、PANICLOW 16〜21、DREQHIGH 8〜13、DREQLOW 0〜5。D0: 23〜29、15〜21、7〜13、0〜6。値は 0x10・0x10・0x1c・0x1c | `vc4_regs.h`、`vc4_hdmi.c`（prepare） |
| MAI_FMT | sample rate 8〜15（48 kHz = 9）、audio format 16〜23（PCM = 2） | `vc4_regs.h` |
| MAI_SMP | N 8〜31、M 0〜7（M−1 を書く） | `vc4_regs.h`、`vc4_hdmi.c` |
| MAI_CONFIG | BIT_REVERSE 26、FORMAT_REVERSE 27、channel の mask 0〜15 | `vc4_regs.h` |
| MAI_CHANNEL_MAP | channel i に i を 4 bit ずつ（2 channel で 0x10） | `vc4_hdmi.c`（vc5 の channel map） |
| AUDIO_PACKET_CONFIG | ZERO_DATA_ON_SAMPLE_FLAT 29、ZERO_DATA_ON_INACTIVE_CHANNELS 24、B frame の印 10〜13（8）、CEA の mask 0〜7 | `vc4_regs.h`、`vc4_hdmi.c` |
| CRP_CFG | N 0〜19、EXTERNAL_CTS_EN 24、DISABLE 25、USE_MAI_BUS_SYNC_FOR_CTS 26 | `vc4_regs.h` |
| SCHEDULER_CONTROL・HOTPLUG | MODE_HDMI bit 0、CONNECTED bit 0 | `vc4_regs.h` |
| RAM_PACKET_CONFIG | 全体の ENABLE bit 16、slot ごとの bit | `vc4_regs.h` |
| RM_OFFSET の形 | VCO × 2^21 ÷ f_ref（9.22 の固定小数点を 2 bit 落とした値）、f_ref 54 MHz | `vc4_hdmi_phy.c` |
| DVP | MISC_CONFIG +0x08 の bit 3（hdmi0）・4（hdmi1）が 1 で clock を止める。SW_INIT +0x04 が reset | `clk-bcm2711-dvp.c`、dtb（`clock@7c700000`、親 `clk-108M` 108000000 Hz） |
| DMA の node | `dma@10600`（axi、0x10_0001_0600、0x600）、mask 0x7c0、interrupts `dma6`〜`dma11` が SPI 86〜91 | dtb |
| DMA の cell | 下位 5 bit が DREQ。Linux の取り決めの印: bit 16〜19 QoS、20〜23 panic QoS、24 source を 128 bit、25 destination を 128 bit、27 WAIT_RESP を付けない、28 WAIT_FOR_WRITES、29 DISDEBUG、30 burst（長さ 3）。`0x41fa000c` は DREQ 12、QoS 0xa、panic 0xf、128 bit の source、burst、WAIT_RESP を付ける | `bcm2835-dma.c` |
| DREQ | C1: hdmi0 10、hdmi1 17。D0・Pi 500: 12、13 | 3 つの dtb |
| 40 bit の channel の register | CS 0x00、CB 0x04（番地 >> 5）、DEBUG 0x0c（RESET 23）、TI 0x10、SRC 0x14、SRCI 0x18、DEST 0x1c、DESTI 0x20、LEN 0x24、NEXT_CB 0x28 | `bcm2835-dma.c` |
| CS の bit | ACTIVE 0、END 1、INT 2、PROT 8〜9、ERR 10、QoS 16〜19、panic 20〜23、TRANSACTIONS 25、WAIT_FOR_WRITES 28、DISDEBUG 29、ABORT 30、HALT 31 | `bcm2835-dma.c` |
| TI の bit | INTEN 0、WAIT_RESP 2、WAIT_RD_RESP 3、DREQ の番号 9〜13、S_DREQ 14、D_DREQ 15 | `bcm2835-dma.c` |
| SRCI・DESTI | 上位 8 bit の番地 0〜7、burst 長 8〜11、INC 12、幅 13〜14（0 = 32 bit、2 = 128 bit） | `bcm2835-dma.c` |
| control block | 8 word: TI、SRC、SRCI、DEST、DESTI、LEN、NEXT_CB（>> 5）、予約。32 byte 揃え | `bcm2835-dma.c` |
| IEC 60958 の subframe | preamble 0〜3（Z 0x08、X 0x02、Y 0x04）、sample 4〜27、V 28、U 29、C 30、P 31（4〜30 の偶数 parity） | alsa-lib `src/pcm/pcm_iec958.c`、IEC 60958-1 |
| channel status | 民生、LPCM、Fs 48 kHz の code（byte 3 の下位 4 bit = 0b0010）、語長（byte 4 = 0b0010、16 bit） | IEC 60958-3（Linux の `include/sound/asoundef.h` の値と照らす、p002） |
| Audio InfoFrame | 型 0x84、版 1、長さ 10、checksum は全 byte の和が 0、CC = channel 数 − 1、CA = 0 | CEA-861 |
| N・CTS | 48 kHz の N の推奨の表（6144、1.001 の rate の値）、CTS = f_TMDS × N ÷ (128 × fs) | HDMI 1.4 の audio clock regeneration（p002 で表を写す） |

手順の出典（「Procedure referenced:」）: MAI の始め・止め・reset の順（`vc4_hdmi.c` の audio の startup・prepare・shutdown・reset）、InfoFrame の書き込み（同 write_infoframe・stop_packet）、40 bit の DMA の始め・割り込みの消し方・止め方（`bcm2835-dma.c` の start・callback・abort）。

## 未確定（p002 の読みと p004・p006 の実機で確かめる）

- Q1 試験の Pi 5 の stepping（HVS の VERSION）と firmware が渡す dtb、FAT の dtb と overlay。tools.md の「D0 の chip に C1 の dtb」が正しければ tree の DREQ（10）は誤り。p006 では `device_tree=` で C1・D0 の両方の dtb で起動し照合の経路を試す（config.txt は試験の image の中で変える）。
- Q2 firmware が PHY を Linux と同じ形（RM_OFFSET・VCODIV）で設定するか。読み戻しの f_TMDS が mode と合うか。
- Q3 CTS: EXTERNAL_CTS_EN の意味、software の CTS と hardware の CTS のどちらで音切れが無いか、1.001 の mode の数 ppm のずれ（長い再生）。
- Q4 DVI・未接続の時に MAI が FIFO を読むか（D10 の software の時計はどちらでも動く作り）。
- Q5 firmware が hotplug の時に HDMI の register（packet・CRP・MAI）を書き換えるか、RAM_PACKET_CONFIG の全体の enable が立っているか、DVI の mode を選ぶ事があるか。
- Q6 audio の gate（MISC_CONFIG）が起動の時に開いているか。
- Q7 試験の台の HDMI の capture の板が音（USB audio）を出すか（host で録れるか）。
- Q8 使っていない port・抜かれた port の register を読んで SError・停止にならないか。

## 試験の方法

- host（p003）: `rpi5-hdmi-audio-wire.c` を ASan・UBSan と通常の 2 通りで（Pi 4 の `plan/tools/rpi4-audio/` と同じ形の script）。subframe・parity・channel status の block の位置（ring の巻き戻し、変換し直し、取りこぼした割り込みをまたいで）、core の mmap の写しと変換の順の模擬、CTS と N（74.25・74.25/1.001・148.5・297・594 MHz と deep color の RM の読み戻し、OFFSET_ONLY・VCODIV_EN の扱い）、MAI_SMP、InfoFrame と packet RAM の word（残りの 0）、DMA の環（40 bit の番地の分け方、NEXT_CB、INTEN・WAIT_RESP、4 GiB の境）、SRC からの位置と通し番号、cell の読み、stepping の表。
- 実機（p004 の読みだけの probe と p006 の再生、Q1 が Pi 5 を割り当てる）: 1 本の script（配置 → 起動 → 読みだけの register の記録 → 再生 → host の capture の音の録音 → 解析 → 要約）。判定は capture の録音の解析（kernel の log は register の値の記録にだけ使う）。解析は、左右で別の周波数（左 1 kHz・右 1.5 kHz）、振幅（−12 dBFS からの差）、左右の入れ替わり、開始の遅れ、途切れ（5 分）、20 ms の短い音、audiod（mmap）経由と `/dev/dsp0` への直の write の両方。capture が音を出さなければ未実施として、人の耳の確かめを「ユーザーが拾える試験の一覧」に載せる（依頼しない）。QEMU に Pi 5 の機種は無いので QEMU の試験は無い。

## 実装の Phase

| Phase | 中身 |
| --- | --- |
| [p002](../phase002/phase.md) | 値の照合: 値の表を出典の行で確かめ `plan/ws349/values.md` に（hdmi1 の表、asoundef.h、CEA-861 の InfoFrame、HDMI の N・CTS の表、CTS の period の field）。C1 と D0 の dtb の差の一覧 |
| [p003](../phase003/phase.md) | `rpi5-hdmi-audio-wire.c`・`.h` と host 試験 |
| [p004](../phase004/phase.md) | driver の attach と読みだけの probe（何も書かず、登録もしない）、`rpi5.c` の呼び出し。Pi 5 で Q1・Q2・Q5（静的な部分）・Q6・Q8 を記録。結果で D3・D8・D10 を直す |
| [p005](../phase005/phase.md) | driver の再生（MAI・HDMI の側・DMA・監視の thread・software の時計）、audio の core への登録、`docs/architecture/audio.md`。build（warning 0） |
| [p006](../phase006/phase.md) | Pi 5 の実機: 再生の判定、hotplug（capture の閉じ開け）、5 分の再生、CTS の 2 通り、2 つの dtb、再生の途中の reboot |
| [p007](../phase007/phase.md) | 規約の全文の見直し（名前・出典の監査を含む） |

## mk の案（当てない。ユーザーの確認、[user-questions.md](../user-questions.md) U1）

```diff
--- a/mk/kernel.mk
+++ b/mk/kernel.mk
-# [RPi5 RP1 Audio driver]
-#  - XXX: not implemented yet??
-ifneq ($(filter y,$(CONFIG_DRIVER_RPI5_AUDIO)),)
-KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI5_AUDIO=1
-KERN_SOURCES +=
-endif
+# [RPi5 HDMI Audio driver]
+ifneq ($(filter y,$(CONFIG_DRIVER_RPI5_HDMI_AUDIO)),)
+KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI5_HDMI_AUDIO=1
+KERN_SOURCES += \
+	src/drivers/platform/rpi5/rpi5-hdmi-audio.c \
+	src/drivers/platform/rpi5/rpi5-hdmi-audio-wire.c
+endif
--- a/mk/config.mk
+++ b/mk/config.mk
-# [RPi5 RP1 Audio driver]
+# [RPi5 HDMI Audio driver]
 #  - Depends: CONFIG_DRIVER_AUDIO
-#  - Depends: CONFIG_DRIVER_PCI
-CONFIG_DRIVER_RPI5_AUDIO          ?= n
+CONFIG_DRIVER_RPI5_HDMI_AUDIO     ?= n
--- a/mk/platform-rpi5/defaults.mk
+++ b/mk/platform-rpi5/defaults.mk
-CONFIG_DRIVER_AUDIO               := n
-CONFIG_DRIVER_RPI5_AUDIO          := n
+CONFIG_DRIVER_AUDIO               := y
+CONFIG_DRIVER_RPI5_HDMI_AUDIO     := y
```

- kernel.mk と config.mk の枠は p004 の build の前に要る（無いと source が build されない）。defaults.mk の y は p006 で再生を確かめてから（それまでは試験の config `plan/ws349/tests/config-rpi5-hdmi-audio.mk` で y）。当てる時期も U1 で聞く。
- 依存（CONFIG_DRIVER_AUDIO が n で HDMI の音が y の時の link の誤り）は、今の Pi 4・VF2 の枠と同じく config.mk の「Depends:」の注記だけにする（kernel.mk に条件を足さない）。足すかは U1 の中で聞く。
- firmware が DVI の mode を選ぶ機械では `mk/platform-rpi5/config.txt` に `hdmi_drive=2` が要るかもしれない（Q5 の結果しだい、要れば別の案）。

## 確かめ

- 読んだ物（2026-11-15）: `AGENTS.md`、`plan/agents/protocol.md`、`plan/ws349/ws.md`、第一版、`src/drivers/audio/audio.c`（先頭・mmap の写し）と `include/drivers/audio/audio.h`、`include/drivers/generic/dma.h`、`src/drivers/platform/rpi4/rpi4-audio.c`・`rpi4-audio-ring.h`、`src/drivers/gpu/videocore/display/hvs6.c`・`hvs6.h`・`display-device.c`（output の部分）、`mk/kernel.mk`（音と Pi 5 の GPU の節）、`mk/config.mk`、`mk/platform-rpi5/defaults.mk`・`config.txt`、`src/kern/platform/rpi5.c`・`rpi4.c`、`docs/architecture/audio.md`、`plan/guardrail.md`（出典の節）、`plan/tools.md`（Pi 5 の行、capture）、3 つの dtb（dtc）、上の出典の file（scratchpad に取得、tree に入れていない）。
- design-reviewer: [design-review.md](design-review.md)（指摘と反映の対応）。
- code は書いていない。build・試験は無し（設計の Phase）。
