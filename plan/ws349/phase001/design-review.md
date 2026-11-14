# ws349-p001 設計の敵対的レビュー（design-reviewer、2026-11-15）

- 対象: [phase.md](phase.md) の第二版の下書き（P8、2026-11-15）。reviewer は読むだけで、build・実機は使っていない。
- 照合で合っていた物: MAI の offset（HDMI0 0x10〜0x20、HDMI1 0x30〜0x40）と core・RM・PHY・packet RAM の offset（`vc4_hdmi_regs.h` の vc6 の表）、MAI_THR の C と D0 の field、MAI_SMP・CRP_CFG・AUDIO_PACKET_CONFIG の bit、40 bit の DMA の register と CB、CB の番地の 5 bit のずらし、cell `0x41fa000c` の読み、RM_OFFSET の逆算の式、DVP の gate、C1 と D0 の DREQ の差。
- P8 が確かめた物: 指摘 1（`audio.c` の `audio_mmap_advance_locked` と `rpi4-audio.c` の割り込みの変換の順）、指摘 6（`vc4_regs.h` の EXTERNAL_CTS_EN の comment）、指摘 12（`dma.h` の `segment_boundary` と `drv_dma_device_create`）は source で確かめた。

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| 1 | 重大 | 割り込みで変換してから `drv_audio_interrupt()` を呼ぶと、core が mmap の shadow から写す前の古い中身を変換し 1 ring 遅れる。「rpi4 と同じ」の記述も違う（rpi4 は今の k と k+1） | D7: `drv_audio_interrupt()` を先に lock の外で呼び、その後 `running` を確かめて k と k+1 を変換。p003 に順の模擬 |
| 2 | 重大 | prepare で無音を変換すると、core が prepare の前に書いた先頭が消える | D7: prepare で ring の今の中身を変換、start の直前に 0・1 を変換し直す |
| 3 | 重大 | 監視が DMA の割り込みに頼ると、firmware が MAI を止めた時に永久に無音。hd の側と gate を見ていない | D10: 監視の kernel thread（10 ms）と software の時計。確かめる物に MAI・gate・PHY・SRC の進みを足した |
| 4 | 重大（推測） | clock の止まった port の register の読みで SError の恐れ | D2a: reset の解けた port だけを読む、`drv_rpi5_serror_check()`、Q8 を p004 の probe で |
| 5 | 中 | InfoFrame の slot の残りの 0、byte の並び、割り込みの中の短い待ちが外れて送信の途中の packet を壊す | 値の表に並びと 0 を書いた。InfoFrame の書き込みは thread の中だけ（D10、起動から再生までの順 3） |
| 6 | 中 | EXTERNAL_CTS_EN の意味が comment と逆の恐れ、deep color、1.001 の mode の ppm のずれ、RM の OFFSET_ONLY と VCODIV_EN | D8: Linux と同じ設定を既定に p006 で両方を試す。f_TMDS は文字の rate（deep color を含む）。N の表と CTS_0・CTS_1、OFFSET_ONLY・VCODIV_EN の確かめ。Q3 |
| 7 | 中 | lock と `running` の規則、INT の消し方、stop の CB=0 | D14 を足した。D5 に INT の消し方と止め方 |
| 8 | 中 | 抜去・DVI で DMA が止まると書き手が無期限に待つ。D3 と D10 の食い違い | D10 の software の時計。D3 を「起動の時だけ選ぶ、後の DVI は software の時計」に直した |
| 9 | 中 | config.txt の comment と tools.md が食い違う、overlay の可能性 | 乖離の表と Q1 に FAT・overlay の確かめ、p006 で 2 つの dtb |
| 10 | 中 | 実機の読みが実装の後にある | Phase の順を直した（p004 に読みだけの probe） |
| 11 | 軽 | 試験が左右・bit の並び・置き場所・遅れ・経路の差・短い音を見ない | 試験の方法に左右で別の周波数、振幅、遅れ、mmap と直の write、20 ms の音を足した。monitor の付け替えは台に別の monitor が無いので p006 で「未実施になりうる」と書く |
| 12 | 軽 | QoS・panic の幅、TI の WAIT_RESP、burst は DESTI にも、位置の折り返し、4 GiB の境、通し番号を SRC から、channel の番号は物理番地から、`drv_dma_device_create`、MAI の番地の dma-ranges | 値の表と D5・D6・D7 を直した |
| 13 | 軽 | 手順の出典（Procedure referenced）、名前を独自に | 値の表の後に手順の出典、D12 に名前、p007 で監査 |
| 14 | 軽 | link 先の無い file、「反映した」の記述、ws.md の表 | この file と phase002〜007・user-questions.md を作り、ws.md の表を直した |
| 15 | 軽 | mk の案の依存、reboot・poweroff | mk の案の注記（U1 で聞く）、D15 |

reviewer が確かめられなかった事（推測）: EXTERNAL_CTS_EN の意味、firmware が clock を止めるか、DVI の時に MAI が FIFO を読むか、firmware が D0 の overlay を自動で当てるか。いずれも Q3・Q4・Q8・Q1 として p004・p006 の実機で確かめる。
