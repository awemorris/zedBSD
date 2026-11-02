<!-- awesome-plan project=zedbsd record=ws321-p001 -->

# ws321-p001 調べと設計: BCM2712 の HEVC の block を V3D の executor の Vulkan Video として

- 親: [WS321](../ws.md)。Queue: q1923-i01（P3、worktree p3-ws321、branch agent/p3-ws321、基点 main d4056dfe3）。
- Status: cleared（2026-11-02 09:40 P3。設計と review 37 件の反映。p002 の実装が実機で ffmpeg と一致した事で前提を確かめた。Q1 の受け入れ待ち）
- 依存: なし。WS322（VF2）・WS324（RTX）の共有の部品（libvulkan の H.265 decode の転送 capset の bit 2、libmedia の H.265 の parse、vkvideo-probe `--h265`、plan/ws324/tests/conformance/）は main にある。

## 出典と license

| 出典 | license | 使い方 |
| --- | --- | --- |
| Raspberry Pi の Linux の tree `raspberrypi/linux` の branch rpi-6.12.y、commit 7821b6a1baefd97757142d9c3227b5219e9f4568（2025-03-25）、`drivers/media/platform/raspberrypi/hevc_dec/`（hevc_d.c・hevc_d.h・hevc_d_h265.c・hevc_d_hw.c・hevc_d_hw.h・hevc_d_video.c）。SHA-256 は `plan/ws321/temp/linux-hevc/`（git に入れない）の取り寄せの記録 | GPL-2.0（hevc_d_h265.c は GPL-2.0-or-later） | 値（register の offset・bit・式の定数・CABAC の初期値の表・scaling の位置の表）と手順（2 段の順、命令の FIFO の中身の順、終わりの判定）だけを block ごとの出典つきで使う。code・comment・構造・名前は写さない（guardrail「MIT・GPL の出典のデータの引用」） |
| 同じ tree の `arch/arm64/boot/dts/broadcom/bcm2712-ds.dtsi`（hevc の node）と `bcm2712.dtsi`（IOMMU2、AXI の dma-ranges） | GPL-2.0、GPL-2.0 OR MIT | node の compatible・reg・割り込み・clock の番号（binding の事実） |
| 同じ tree の `drivers/iommu/bcm2712-iommu.c` | GPL-2.0-only | IOMMU2 の CTRL・BYPASS の register を読むだけ（hvs6.c と同じ扱い） |
| Mesa（MIT）`src/broadcom/common/v3d_tfu.h`、`src/gallium/drivers/v3d/v3dx_tfu.c` の `tfu_sand`（P4 の取り寄せ `/home/awe/zedBSD-worktrees/p4-ws291pi/build/tmp/mesa-dl3/`、読むだけ） | MIT | V3D 7.1 の TFU の SAND の入力の field（IFORMAT 1、SVTWID 128、SVSWIZ 15、ICFMT 8 bit、入力の 1 KiB の整列、column の高さの 8 の倍数）。値だけ |
| ITU-T H.265（2026-01、`build/sources/specs/itu-t-h265-202601.pdf`） | — | slice segment header（7.3.6）、参照の list（8.3.4）、CABAC の初期化（9.3.2.2）、tile（6.5.1） |

firmware: HEVC の block は firmware を load しない（Linux の driver は clock の rate を firmware の mailbox に頼むだけ）。配る BLOB は無い。

## hardware（BCM2712 の HEVC decoder、Linux では rpi-hevc-dec）

- **node**: compatible `brcm,bcm2712-hevc-dec`（`raspberrypi,hevc-dec`）。reg `hevc` 0x10_0080_0000 + 0x10000（decoder の register）、`intc` 0x10_0084_0000 + 0x1000（割り込みの制御）。割り込み GIC SPI 98（level high）。clock は firmware の clock 11（HEVC）。`iommus = <&iommu2>`（0x10_0000_5100）。AXI の dma-ranges は恒等（物理 = bus の番地）。
- **IOMMU2**: Linux の driver は 40 GiB からの aperture だけを訳し、その下は素通しにする。zedBSD は IOMMU2 を設定しない。probe で CTRL・BYPASS を読んで log に出し、有効（ENABLE）で aperture が DRAM に掛かっていれば decode を出さない（実機で確かめる、R2）。
- **clock・電源**: firmware の mailbox（`bcm2712_firmware_property`、v3d71-firmware.c）で clock 11 の最大の rate を問い（GET_MAX_CLOCK_RATE）、rate を設定し（SET_CLOCK_RATE）、on にする（SET_CLOCK_STATE）。電源の domain は Linux の DT に無い（firmware の clock が持つ）。session が一つも無くなったら rate を下げ off（Linux の stop_clock と同じ）。
- **番地の符号**: 番地の register は `物理 >> 6`（64 byte 単位、32 bit で 256 GiB まで）、長さ・stride は `(x + 63) >> 6`。
- **割り込み**（intc の offset 0、ICTRL）: bit 0 ACTIVE1_INT（phase 1 の終わり、1 を書いて消す）、bit 2 ACTIVE1_EN、bit 4 ACTIVE2_INT（phase 2）、bit 6 ACTIVE2_EN。bit 11 と 19:12 は 0 を書く。初期化で EN を立て、溜まった INT を消す。最初の版は GIC の割り込みを使わず、native job の step の中で ICTRL の INT を bounded に poll する（R4。割り込みにするのは速さが要る時）。
- **2 段の pipeline**:
  - **phase 1（entropy）**: memory の命令の FIFO（(register の offset 16 bit, 値 32 bit) の組の列、8 byte ずつ）を block が順に実行し、CABAC を解いて PU の列と係数の列を書く。開始: PUWBASE・PUWSTRIDE・COEFFWBASE・COEFFWSTRIDE（stride は buffer を CTB の行の数で割り 64 に切り下げた物）、CFNUM（組の数）、最後に CFBASE（FIFO の番地、これが開始）。終わり: ACTIVE1。成功は CFSTATUS == CFNUM。違えば STATUS の bit 3（係数の列が足りない）・bit 4（PU が足りない）を見て、その buffer を大きくして phase 1 を最初からやり直す。どちらでもなければ誤り。
  - **phase 2（再構成）**: PURBASE・PURSTRIDE・COEFFRBASE・COEFFRSTRIDE（phase 1 と同じ buffer）、OUTYBASE・OUTYSTRIDE・OUTCBASE・OUTCSTRIDE（出力の SAND の 2 plane、stride は column の間隔）、16 の参照の組（0x9000 + 16 i: 輝度の番地・stride・色差の番地・stride）、CONFIG2、FRAMESIZE（高さ << 16 | 幅）、CURRPOC、COLSTRIDE・MVSTRIDE・MVBASE（この絵の動き vector の書き出し）・COLBASE（collocated の絵の読み）。最後に NUMROWS（CTB の行の数、これが開始）。終わり: ACTIVE2。
  - Linux は phase 1 と phase 2 を別の絵で重ねる（PU・係数の buffer を 3 組）。最初の版は 1 絵ずつ phase 1 → phase 2 を順に（R5。1080p60 に足りなければ重ねる）。
- **命令の FIFO の中身**（絵ごと、slice segment ごと）: SPS0・SPS1・PPS（SPS・PPS の field の詰め）、scaling list がある時は 0x2000 からの 4064 byte の係数の表（最初の segment だけ）、SLICESTART（segment の最初の CTB の x・y）、slice の message（SLICECMDS と 0x4000 からの 16 bit の列: slice の種類・参照の数・merge の候補の数・collocated の向き、参照ごとの DPB の番号・long-term・POC と重み、deblocking と QP の offset）、bitstream（BFBASE・BFNUM・BFCONTROL を 2 回: stop、続いて emulation prevention を外す指定と 64 byte の中の offset）、CABAC の初期の確率（0x1000 から 156 byte、slice の QP と init_type で 3 つの表から計算）と TRANSFER の backup、entry point（TILESTART・TILEEND・BEGINTILEEND・SLICE・QP・MODE・CONTROL）、tile と WPP の行の終わりの STATUS と TRANSFER（保存・読み戻し）。tile・WPP・dependent slice の手順は Linux の decode_slice・wpp_decode_slice の順に従う（手順の引用）。
- **出力の形**: 8 bit は SAND128（Linux の NV12MT_COL128）の 2 plane: 輝度は幅 128 byte の column を並べた物で、column の間隔は `高さ × 128`、色差（CbCr の組）は別の plane で column の間隔はその半分。column の数は ceil(幅 / 128)。参照も同じ形で読む。10 bit（SAND の 3 画素 4 byte）は扱わない（Main だけ）。
- **動き vector の buffer**（temporal MVP の SPS の時、絵ごと）: stride = align(幅, 64)、大きさ = stride × (align(高さ, 64) >> 4)。
- **PU・係数の buffer**: 初め PU = round(幅 × 高さ / 4)、係数 = round(幅 × 高さ)（round は Linux の `hevc_d_round_up_size`: 3·2^n か 4·2^n に切り上げ）。足りなければ一段大きく。
- **制限**（Linux の try_ctrl と format）: 幅・高さ 16〜4096、4:2:0 だけ、bit depth は輝度と色差が同じで 8 か 10、WPP と tile（2 つ以上）の同時は不可、DPB 16。

## Vulkan Video への写し

- **見せ方**: Pi 4 の H.264（WS236、`vk/vulkan-video.c`）と同じく、V3D の render node の queue family 0 に `VK_QUEUE_VIDEO_DECODE_BIT_KHR`、capset の bit 2（H.265 decode）だけ（Pi 5 に H.264 の hardware は無い）。新しい cdev・UAPI・HAL の口は作らない。
- **build**: 新しい `CONFIG_DRIVER_BCM2712_VIDEO`（Pi 5 の CI の config で y）。今 `CONFIG_DRIVER_BCM2711_VIDEO` の `#if` の中にある NV12 の image・plane の view・import の対応は、Pi 4 の MMAL と Pi 5 の HEVC の両方で使う（`BCM2711_VULKAN_VIDEO` の派生の macro に）。vulkan-video.h の口（dispatch・run・settle・mark・nv12_layout・readback）を Pi 5 では新しい `vk/vulkan-video-hevc.c` が実装し、Pi 4 の `vulkan-video.c` は変えない（一つの image が両方を持たない: Pi 4 と Pi 5 は別の config）。
- **profile と能力**: H.265 Main、4:2:0、8 bit、progressive。maxDpbSlots 16、maxActiveReferencePictures 15、最小 16×16、最大 4096×4096（実機の確かめは 1080p と、1 本の 2160p まで）、pictureAccessGranularity 16×16、bitstream の offset・大きさの整列 64、DPB_AND_OUTPUT_COINCIDE、level 5.1、std `VK_STD_vulkan_video_codec_h265_decode` 1.0.0。WS324（nvrtx）の報告と揃える。
- **wire の reader**: nvrtx の `vk/vk-video.c` の H.265 の reader（同じ libvulkan の wire、WS324 design §10）を写して bcm2711 の構造に合わせる（同じ project の Zlib の code）。
- **slice segment header**: block は解いた header の値を命令で受ける（V4L2 の stateless と同じ）。Vulkan は header を解かないので、executor が bitstream の各 slice segment の header を CPU で読む（新しい `vk/vulkan-video-hevc-slice.c`、hardware に触れない純粋な計算で host 試験する）。読む: first_slice・dependent・address・slice_type・pic_output・RPS（short-term は飛ばす、long-term の数）・temporal MVP・SAO・参照の数・list の修正・mvd_l1_zero・cabac_init・collocated・重み・merge の候補・QP・cb/cr の offset・deblocking・loop filter・entry point の数（offset は読んで捨てる）・header の終わりの byte（emulation prevention を含む元の bitstream の位置）。SPS・PPS は StdVideo（session parameters）から。参照の list（8.3.4）は picture info の RefPicSetStCurrBefore・After・LtCurr（DPB の slot の番号）と list_entry から作る。
- **DPB の slot と block の参照**: block の 16 の参照の番号 = Vulkan の DPB の slot の番号（0〜15）。各 slot の絵は driver の pool の SAND の frame（session の pool、slot ごと、動き vector の buffer つき）。setup の slot の decode は pool[slot] に書き、参照は pool[参照の slot] を読む。setup の slot が無い decode は pool の予備の 1 枚。
- **出力の image へ**（GPU で、guardrail「画像の処理は GPU で」）: phase 2 の後、V3D 7.1 の TFU が pool の SAND を出力の image（今の NV12 の raster、pitch は幅を 32 に、行を 16 に揃えた物）の 2 plane へ写す: 輝度は R8、色差は RG8、入力の形 SAND（IFORMAT 1、SVTWID 128）、出力の形 raster（IOC の形 0、stride は pitch）。今の TFU の job の検べ（出力は tile の形だけ）を raster の出力にも開く。SAND→raster が実機で出なければ SAND→UIF の一時の buffer → UIF→raster の 2 段（R3）。写した後に buffer の generation を進める（sampling の snapshot を作り直させる）。CPU は画素に触れない。Video Player の GPU の道（VP_GPU_PRESENT）は image を fd で import して plane を sample するので、今の Pi の sampling（raster → UIF の TFU）がそのまま使える。
- **bitstream**: Vulkan の buffer の memory は物理に連続（bcm2711_buffer）なので、block は app の bitstream の buffer を直に読む（BFBASE = 物理 >> 6、BFCONTROL に 64 の中の offset）。CPU が書いた範囲を decode の前に dcache clean。header の parse は kernel の map から読む。
- **命令の FIFO・PU・係数・動き vector**: driver の連続の buffer（uncached か clean で）。
- **同期**: Pi 4 と同じく decode は native job の中の CPU の段。block を起こし、ICTRL を短く poll し、終わらなければ EINPROGRESS で step を返す（他の session が進む）。上限（500 ms）を越えたら誤り（status query は ERROR、image は前のまま）。block の reset の手順は Linux に無い（R6）: timeout の後は clock を off/on して session を失わせる。
- **status query**: RESULT_STATUS（成功・誤り）。

## 試験の道

- **host**（p002）: `vulkan-video-hevc-slice.c`（slice header の parse、参照の list、命令の FIFO の組み立て、CABAC の初期値、scaling の表、tile の境）を host の cc で build し、HEVC_v1 の適合の stream で libmedia（h265-stream・h265-std、適合で確かめ済み）の StdVideo と slice header の値と照合する。命令の FIFO は固定の trace と照合（hardware の照合は p003）。
- **実機**（p003、Pi 5 10.0.30.7）: 1 本の script（配置 → 起動 → vkvideo-probe `--h265` の各 stream → 回収 → ffmpeg の framehash と照合 → 要約）。stream は HEVC_v1 の Main 8 bit で block の制限の中の物（WPP+tile・10 bit・4:2:2 を除く）。続いて ffmpeg で作った 1080p30・1080p60 の H.265 の clip を Video Player の GPU の道で再生し、frame の数・遅れ・画面（image の screen capture か HDMI の capture）。板の元の file は cksum で戻す（BUG-393 の形）。

## 危険・未確認

- R1: slice header の parse を kernel に置く（信頼しない bitstream の解析）。全ての読みを上限で守り、範囲の外は ENOTSUP・EINVAL で絵を ERROR にする。host 試験に壊れた header も入れる。
- R2: IOMMU2 が firmware の後で有効なら、block は物理の番地で読めない。probe で読む。
- R3: TFU の SAND→raster は Mesa に例が無い（Mesa は SAND→UIF だけ）。実機で照合し、駄目なら 2 段。
- R4: 割り込みを使わない poll の形の速さ（1080p60 で 1 絵 16 ms の枠）。足りなければ GIC の割り込み（`kern_irq_register`）。
- R5: phase 1 と 2 を重ねない形の速さ。
- R6: timeout の後の block の立て直しの手順が分からない。
- R7: pool の slot ごとの絵は「slot の絵 = その slot を setup にした最後の decode」の約束に頼る（app が slot の image を自分で書き換えると食い違う）。libmedia・vkvideo-probe はしない。
- R8: Video Player の H.265 の backend（libmedia の Vulkan Video H.265）は P2 の ws322-p006 が作る。p003 の Video Player の段はそれが main に入ってから。

## review（design-reviewer、37 件、[review.md](review.md)）の反映

設計は次のとおり直した（実装は p002 でこの形）。

- column の高さは 16 に揃える（色差の column が 8 の倍数、TFU の SAND の入力の条件）。pool の絵は V3D の MMU に map された memory（TFU は V3D の番地で読む）。
- **pool の memory は app の物にする**: session の memory の要求を絵ごとの binding（DPB の slot の数 + 予備 1、各々 SAND の 2 plane と動き vector、page 揃え）で報告し、app が bind した VkDeviceMemory を使う（kernel が隠れて大きな連続の memory を取らない、DoS を避ける）。phase 1 の list と命令の FIFO だけが executor の物（小さく、足りなければ大きくする）。
- SPS・PPS は decode ごとに `bcm2712_hevc_check` で検べ、絵の大きさは session の最大（pool の大きさ）以下に限る。block の書く大きさ（FRAMESIZE、stride、PU・係数の list の長さ）は全て検べた値から作る。IOMMU2 は使わない（firmware の後で off を実機で確かめた）。残る危険: 壊れた CABAC の data で block がどう振る舞うかは未確認（Linux も同じ形で使う）。
- ICTRL の INT は、読んだ値から zero mask を除き EN を保って書き戻して消す。
- decode の誤り（解けない header、block の失敗、memory の未 bind など）は status query の ERROR で返し、job を失敗させない。block が時間内に終わらない時は block をその boot の間使わない。session・decode の記録の解放は、block がその memory を使う decode を終えるまで待つ（終わらなければ解放しない）。block は一度に一つの decode、他の session の decode は EINPROGRESS で待つ。
- header は app の uncached の memory から kernel の buffer（64 KiB）に写して読む。segment は表（256）を越えれば絵ごと ERROR（黙って捨てない）。参照の集合の 0xFF（欠けた絵）は数えに入れ、list では最初の参照で埋める。
- Pi 4 の部品: buffer-copy の Pi 4 の footroom の補正は Pi 4 だけ、capset の decode の bit は Pi 5 で bit 2（H.265）。vulkan-video.h の口は全て vulkan-video-hevc.c が持つ（settle・mark は decode が event の中で終わるので空）。
- 最小の大きさは 32（Vulkan の報告）。命令の FIFO の 1 組は 32 bit の番地と 32 bit の値。
- 出力の形は NV12MT_COL128（2 plane）のまま: 出典の commit が列挙から外したのは userspace の都合で、block の register（OUTCBASE・OUTCSTRIDE）は plane ごとに取る。実機で一致を確かめた（p002）。
- 試験: libmedia との照合に加え、host 試験で 3 つの形（start code あり・なし・4 byte）、ENOSPC の後の作り直し、100000 件の壊れた入力（ASan・UBSan）。hardware の照合は実機の ffmpeg の framehash。
- Phase: p004「規約の全文の見直し」を足す。p003 の Video Player の段は ws322-p006（libmedia の H.265 の backend、main 5c8c1e7dc）に依る。

## Phase の分け（p002・p003）

- p002 実装: ① slice header・命令の FIFO の純粋な部分と host 試験、② HEVC の block の driver（probe・clock・poll・2 段・誤り）、③ executor の H.265 の reader と run（session・parameters・pool・TFU の写し・query）、④ build（Pi 5 の config、warning 0）、Pi 4 の build の非退行。
- p003 実機: vkvideo-probe の表、Video Player の GPU の道。
- p004 規約の全文の見直し（AGENTS §2）。
