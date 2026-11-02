<!-- awesome-plan project=zedbsd record=ws321-p002 -->

# ws321-p002 実装: HEVC の block の driver と V3D の executor の H.265 decode

- 親: [WS321](../ws.md)。設計: [phase001](../phase001/phase.md)（review の反映を含む）。Queue: q1923-i01（P3）。
- Status: cleared（2026-11-02 P3。build と host 試験、実機の照合は p003。Q1 の受け入れ待ち）
- 依存: p001。

## 作った物

| file | 中身 |
| --- | --- |
| `src/drivers/gpu/bcm2711/hevc-decoder.[ch]` | block の driver: device tree（`brcm,bcm2712-hevc-dec`、窓 `hevc`・`intc`）、IOMMU2 の CTRL を読んで log、firmware の clock 11（最大の rate、on、最後の利用者で off）、phase 1（list の base・stride、CFNUM、CFBASE で開始、ICTRL bit 0 の latch、CFSTATUS == CFNUM、STATUS の list 不足）、phase 2（list、出力の 2 plane、16 の参照、CONFIG2・FRAMESIZE・CURRPOC、動き vector、NUMROWS で開始、ICTRL bit 4）。何も待たない（呼び手が poll） |
| `src/drivers/gpu/bcm2711/vk/vulkan-video-hevc.[ch]`・`vulkan-video-hevc-slice.c` | 純粋な計算（kernel を呼ばない、host 試験が走らせる）: SPS・PPS の検べ（Main 8 bit 4:2:0、16〜4096、WPP+tile 不可、各値の H.265 の範囲）、slice segment header の parse（start code の有無、emulation prevention、全ての読みに上限、参照の list 8.3.4、重み、entry point、extension、data の始まりの byte）、命令の FIFO の組み立て（tile と WPP の entry point、CABAC の初期値、scaling の表、slice の message）、phase 2 の値、SAND・動き vector・list の大きさ |
| `src/drivers/gpu/bcm2711/vk/vulkan-video-hevc.c` | executor: vulkan-video.h の口の Pi 5 版。libvulkan の H.265 の wire の reader（capabilities・format・family・session・memory の要求と bind・parameters・decode・query）、session の絵 = app が bind する memory（slot ごと + 予備）、decode は native job の中で step ごとに（最初の step で header を読み FIFO を作り phase 1、window を開けて poll、phase 2、V3D の TFU で SAND → 出力の image の raster NV12 の 2 plane、buffer の generation を進める）、誤りは status の ERROR |
| 共有の executor（`vk/*.c`、`render-runtime.c`） | NV12 の image・plane の view・query の `#if` を `CONFIG_DRIVER_BCM2711_VIDEO \|\| CONFIG_DRIVER_BCM2712_VIDEO` に。Pi 4 の MMAL の補正は Pi 4 だけ。capset の decode の bit は Pi 5 で bit 2（H.265） |
| `src/drivers/gpu/bcm2711/attach71.c` | Pi 5 の attach で block を探す（`CONFIG_DRIVER_BCM2712_VIDEO`） |
| `src/drivers/gpu/bcm2711/v3d71-job.c` | TFU の job の検べを raster の出力（IOC の形 0）にも開く |
| `Makefile`・`platform/arm64/vmunix.mk` | `CONFIG_DRIVER_BCM2712_VIDEO`（既定 n、Pi 5 で BCM2712 GPU の時だけ）、source の追加 |
| `plan/ws321/tests/config-rpi5-hevc.mk` | 試験の image（CI の Pi 5 + video + screen capture + probe） |
| `plan/ws321/tests/hevc-slice/` | host 試験（P3 の helper が作った）: libmedia（適合で確かめ済み）の StdVideo と header の parse に照合、100000 件の壊れた入力 |
| `plan/ws321/tests/pi5-hevc.sh` | 実機の script（check・deploy・files・probe・player・restore、cksum で戻す） |

HAL・UAPI・toolchain の変更は無い。

## 確かめ

| 確かめ | 結果 |
| --- | --- |
| `make ZEDBSD_CONFIG=plan/ws321/tests/config-rpi5-hevc.mk BUILD=build/p321 build/p321/vmunix` | rc 0、warning 0、`arm64 vmunix check: PASS` |
| `make ZEDBSD_CONFIG=config/ci/config-rpi5.mk BUILD=build/p321-default build/p321-default/vmunix`（video は既定 n） | rc 0、warning 0、PASS |
| `make ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/p321-rpi4 build/p321-rpi4/vmunix`（Pi 4 の非退行の build） | rc 0、warning 0、PASS |
| userland（vkvideo-probe・media-probe・videoplayer・wayland・files・keiland-shot、libvulkan・libmedia ほか）の Pi 5 の build | rc 0、warning 0 |
| `sh plan/ws321/tests/hevc-slice/run.sh` | `hevc-slice run: PASS (147 streams, 15 refused)`: 9235 絵（8289 を試験）、49158 segment で libmedia と不一致 0（I 1044、P 30116、B 17998、dependent 7180、entry point 1462、list の修正 1277、long-term 645、重み 579、tile 1071 絵、WPP 362 絵、scaling 186 絵）。refused は Main10・深さ違い 11、4096 を越える PICSIZE 4。fuzz 100000 件（seed 1、ほかに 2・3）で crash・sanitizer の報告 0、契約の違反 0 |

## helper の指摘と直し

- 参照の集合の途中の 0xFF（欠けた絵）で NumPicTotalCurr が少なく数えられ、list の修正の bit 幅がずれる（libmedia が 0xFF を残す時、適合の stream には無い）→ 集合の長さを最後の slot の位置までとし、欠けた所は最初の参照で埋める（0a1259dd8）。直した後も host 試験 PASS。
- `bcm2712_hevc_slice_read` は `bcm2712_hevc_check` が通った set を前提にする → executor は decode ごとに check する（`start_decode`）。

## 未実施・残り

- Pi 4 の実機の非退行（build だけ）。
- `vulkan-video-hevc.c` の速さの工夫（phase 1 と 2 を別の絵で重ねる、GIC の割り込み）: p003 の数で決める。
- 規約の全文の見直し（p004）。
