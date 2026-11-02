<!-- awesome-plan project=zedbsd record=ws321-p001-review -->

# ws321-p001 設計の敵対的レビュー（design-reviewer、2026-11-02）

対象: [phase.md](phase.md)（commit bd4ade2a0 の版）。照らした物: `plan/ws321/temp/linux-hevc/`（Linux rpi-6.12.y 7821b6a1 の hevc_dec・dtsi・iommu）、
Mesa `v3dx_tfu.c`・`v3d_tfu.h`・`v3d_resource.c`・`v3d_blit.c`（P4 の mesa-dl3）、`src/drivers/gpu/bcm2711/`（vk/vulkan-video.[ch]・vulkan-native-draw.c・
vulkan-native-job.c・vulkan-buffer-copy.c・vulkan-resource.c・vulkan-memory.c・v3d71-job.c・v3d-job.h・buffer.[ch]・render-runtime.c・native-storage.c）、
nvrtx の `vk/vk-video.c`・`vk/vk-video-h265.c`・`vk/nvrtx-video.h`、`userland/desktop/videoplayer/present.c`、`userland/tests/vkvideo-probe/`、
`userland/desktop/libmedia/vkvideo-runtime.c`、`Makefile`、`config/`、AGENTS.md、guardrail.md。

重大度: 重大（直さないと安全か受け入れが壊れる）・高（受け入れの失敗か大きな手戻り）・中（特定の stream・状況で壊れる、または規則の違反）・低（明確化・小さな誤り）。
「推測」と書いた物は実機・文書で確かめていない。HAL（`include/hal/hal.h`）の変更は要らない（kern_device_map・kern_irq_register・kern_pmem_*・
kern_mmio_* で足りる）ことを確かめた。UAPI の追加も無い（capset の bit 2 は既存）。

---

## A. hardware の事実・TFU との接続

### 1.（高）pool の SAND の column の高さを 8 に揃えるだけでは、色差の TFU が 1080p で通らない

- 根拠: 設計 phase.md:33（「column の間隔は `高さ × 128`、色差は… その半分」、高さの揃えの記述なし）。Linux は NV12MT で
  `height = ALIGN(height, 8)`（hevc_d_video.c:132-140）、`luma_stride = dst_fmt.height * 128`、`chroma_stride = luma_stride / 2`
  （hevc_d_h265.c:1656-1662）。Mesa の SAND 入力は「stride（column の高さ）が 8 の倍数、0x7fff 以下」「入力の番地が 1 KiB 揃え」
  （v3dx_tfu.c:263-270）。
- 起こる状況: 1080 行の絵で輝度の column の高さ 1080（8 の倍数）、色差は 540 行（540 mod 8 = 4）。色差の TFU job の IIS が 8 の倍数でなく、
  Mesa なら拒む条件。hardware が何を出すかは不明（推測: 色差の column がずれて縞になる）。p003 の 1080p の clip と、高さが 16 の倍数でない
  適合の stream（416×240 は通るが 1080・1088 以外の多く）で framehash が合わない。
- 直し方: pool の frame の column の高さ（輝度）を `ALIGN(pic_height, 16)` 以上（色差が 8 の倍数）に決め、OUTYSTRIDE・OUTCSTRIDE・参照の
  stride・TFU の IIS をその値から出す。設計に「輝度の column の高さ Hc = align(H,16)、色差 Hc/2、plane の先頭は 1 KiB 揃え」を明記し、
  host 試験で 1080・540 の例を入れる。

### 2.（中）選んだ NV12MT（2 plane）は、出典の commit で Linux が列挙から外した形

- 根拠: commits.json の 7821b6a1 の message（新しい形式を Mesa 24 まで enum_fmt から外す）。hevc_d_video.c:287-288・300-306 で
  NV12MT_COL128 が comment out、実際に FFmpeg が使うのは NV12_COL128（1 plane、column の中に輝度の後に色差、chroma_stride = luma_stride、
  chroma = luma + height×128、hevc_d_h265.c:1664-1671）。Mesa の SAND の取り込みも 2 つの plane に同じ column の高さを使う
  （v3d_resource.c:949-958、v3d_blit.c:871）。
- 起こる状況: register の上では NV12MT も書けるが、実機で広く通っている道は NV12_COL128 + Mesa の tfu_sand。MT で色差の向きや
  stride の意味が違っていた時、出典に照らせる実績の道が無い（推測）。
- 直し方: 第一案を NV12_COL128（column の高さ Hc = align(H×3/2 相当, 8) かつ TFU の 8 の倍数を満たす値、例 align(H,16)×3/2）にし、
  MT は第二案として p003 で比べる。どちらにしても TFU の IIS = column の高さ（行）であることを表で示す。

### 3.（高）pool の SAND・動き vector の buffer は V3D の MMU に map しないと TFU が読めない

- 根拠: TFU の番地は V3D の仮想番地で、`validate_span` が V3D の page table の entry を確かめる（v3d71-job.c:310-340、248-262）。
  HEVC の block は物理の番地（>>6）を使う（phase.md:26）。設計 phase.md:45-48 は pool を「driver の pool の SAND の frame」とだけ書き、
  V3D の VA を持つかを書いていない。
- 起こる状況: pool を `kern_pmem` だけで取ると、TFU の job が EFAULT で拒まれる。逆に V3D の VA だけを見て物理を block に渡すと、
  page の境で連続していない時に壊れる（bcm2711_buffer は連続なので後者は起きない）。
- 直し方: pool の各 frame（と予備の 1 枚）を `bcm2711_buffer_create_uncached` で取り、`bcm2711_v3d_memory_map` で V3D の VA を持たせる。
  block には `memory.paddr`、TFU には view の address を渡す。VA と物理の両方が 1 KiB（色差の plane の先頭も）に揃うことを確かめる。

### 4.（高）ICTRL の INT を消す書き込みが EN を落とすと、以後の全ての decode が timeout になる

- 根拠: hevc_d_hw.h:160-168（ACTIVE1_EN が 0 なら bit 0 が立たない、という注記）、Linux の割り込みは読んだ値から zero mask を除いて書き戻す
  （hevc_d_hw.c:214-221）。設計 phase.md:27 は「1 を書いて消す」「bit 11 と 19:12 は 0」「初期化で EN を立て」とあるだけ。
- 起こる状況: poll の後に `ICTRL = ACTIVE1_INT` のように INT だけを書くと EN（と EDGE）が 0 になり、次の phase の INT が latch されない。
  最初の絵だけ出て、2 枚目から 500 ms の timeout が続く。
- 直し方: 消す時は「読んだ値 & ~zero mask」（EN・EDGE を保ち、消したい INT だけ 1）を書く、と設計に書く。EDGE は 0（負の edge）のまま
  にする。clock を入れ直した後にも EN を書き直す（9 番）。

### 5.（中）最小の大きさは 16 でなく 32

- 根拠: hevc_d_video.c:240-245（「Local limitations」で幅・高さ 32 未満を拒む）。設計 phase.md:36・42 は 16〜4096、minCodedExtent 16×16。
- 起こる状況: 16〜31 の絵を受けて block の挙動が不明（推測: 壊れた絵か hang）。
- 直し方: minCodedExtent を 32×32 にし、SPS の幅・高さが 32 未満なら decode を ERROR にする。validate_sps の他の制限
  （CTB ≤ 64、最大 TB ≤ 32、TB の深さ、log2_max_poc_lsb、num_short_term_ref_pic_sets ≤ 64、num_long_term_ref_pics_sps ≤ 32、
  hevc_d_video.c:230-280）も 7 番の検べに入れる。

### 6.（中）命令の FIFO の 1 組は 32 bit の番地 + 32 bit の値。大きさの上限と 64 byte 揃えが無い

- 根拠: `struct rpi_cmd { u32 addr; u32 data; } __packed`（hevc_d_h265.c:109-112）。設計 phase.md:29 は「register の offset 16 bit, 値 32 bit」。
  Linux は組の数を 2 の冪で伸ばし、0x100000 を越える要求を拒む（hevc_d_h265.c:240-263）。CFBASE・PU・係数・MV の番地は `>> 6` で書く
  （hevc_d_hw.h:111-123）ので 64 byte に揃っていないと下位が落ちる。
- 起こる状況: 16 bit で詰めると全ての命令がずれる。slice が多い（数百）か WPP の行が多い絵で固定の大きさを越え、命令が切れる
  （Linux の p1_apb_write は黙って捨てる）。
- 直し方: 1 組 8 byte（offset を 32 bit の word に、値を次の word に、little endian）と明記。FIFO は絵ごとに必要数を先に数えて確保するか、
  上限（例 2^20 組 = 8 MiB）を決め越えたら ERROR。FIFO・PU・係数・MV・SAND の先頭は 64 byte（SAND は 1 KiB）揃えと明記。

### 7.（重大）StdVideo の SPS・PPS（app が送る値）を検べずに block の geometry に使うと、IOMMU 無しの DMA が範囲を越える

- 根拠: 設計 phase.md:44（SPS・PPS は StdVideo から）、R1（phase.md:59）は slice header の parse だけを対象にする。Linux は SPS・PPS を
  try_ctrl と validate_sps で検べる（hevc_d_h265.c:2414-2470、2487-2505、hevc_d_video.c:230-280）。tile の境は PPS の column_width_minus1・
  row_height_minus1 から作る（hevc_d_h265.c:1375-1400）。block は IOMMU を通さない（phase.md:24）。
- 起こる状況: 悪意の process が、tile の幅の和が PicWidthInCtbs を越える PPS、pic_width が session の maxCodedExtent や pool の frame より
  大きい SPS、CTB 128 などを送る。TILESTART・TILEEND・SLICESTART・FRAMESIZE・NUMROWS が pool・PU・係数・MV の buffer の外を指し、
  block が物理の memory を書き壊す（kernel の memory を含む）。
- 直し方: decode の前に kernel で全ての geometry を検べる: SPS の幅・高さ ≤ session の maxCodedExtent ≤ pool の frame、CTB・TB・PCM の
  大きさが規格と block の範囲、tile の数・幅・高さの和が絵の CTB 数と一致（uniform_spacing の時は 6.5.1 で計算）、slice_segment_address <
  PicSizeInCtbsY、dst の image と codedExtent が SPS の大きさ以上。範囲の外は ERROR。host 試験に壊れた SPS・PPS を入れる。

### 8.（高）IOMMU を使わない決定は、信頼しない data を読む DMA の block を kernel の memory 全体に開く（危険の受け入れはユーザーの判断）

- 根拠: phase.md:24（zedBSD は IOMMU2 を設定しない）、bcm2712-ds.dtsi:283-290（IOMMU2 は HEVC と PISP-BE のため）、hevc_d_h265.c:1945-1950
  （collocated が無い時「P2 が壊れた data で暴れるかもしれない」ので中止する、という Linux の注記）。
- 起こる状況: 7 番を全て直しても、block の内部の誤り（未知）で外を書けば kernel が壊れる。AGENTS §2 で危険の受け入れはユーザーが決める。
- 直し方: (a) IOMMU2 を使い block の DMA を pool・PU・係数・MV・FIFO・bitstream の page に閉じる（bcm2712-iommu.c の CTRL・PT・
  BYPASS・ADDR_CAP の値で）、(b) 使わずに危険を受け入れる、の二つを Q1 経由でユーザーに示す。決まるまでは (b) の時に何が露出するかを
  R の項に明記する。

### 9.（中）register に触れてよいのは clock が入っている間だけ。clock を落とした後の状態も分からない

- 根拠: phase.md:25（session が無くなったら off）、phase.md:27（初期化で EN）。Linux は probe で ICTRL を書くが、clock は start_streaming で
  入れる（hevc_d_hw.c:349-352、hevc_d_video.c:601-625）。clock・電源の domain の関係は文書に無い。
- 起こる状況: zedBSD の probe が clock を入れる前に `CTRL`・`ICTRL` を読み書きすると、bus の誤り（SError）か無視（推測）。
  clock を落とした後に EN が初期値に戻っていると 4 番と同じ timeout。
- 直し方: clock を入れた後にだけ ICTRL を初期化する、clock を入れ直すたびに EN を書き直し溜まった INT を消す、と設計に書く。
  mailbox の呼び出しは controller の mutex の外（session の作成・破棄の ioctl の中）で行い、mailbox の lock と controller の mutex の順を決める。

### 10.（低）IOMMU2 の検べの条件が足りない

- 根拠: phase.md:24・60（ENABLE と aperture）。bcm2712-iommu.c:184-232 は BYPASS_START・BYPASS_END・ADDR_CAP・ILLEGAL_ADR も設定する。
- 直し方: CTRL の ENABLE と BYPASS、BYPASS_END（素通しの上限）、ADDR_CAP を読み、使う物理の番地（Vulkan の memory は 1 GiB 未満、
  vulkan-memory.c:417）が素通しの範囲に入ることを確かめてから decode を出す。

---

## B. 資源の寿命・状態・同期・誤りの道

### 11.（高）block が動いている間の buffer の寿命と、timeout の後の立て直しが決まっていない

- 根拠: phase.md:49（timeout の後は clock を off/on して session を失わせる）、R6（phase.md:64）。v3d-job.h:96-100（launch した job が失敗したら
  global の reset が確かめられるまで持ち主を隔離する、という既存の規則）。Pi 4 は flight で寿命を持つ（vulkan-video.c:4005-4190）。
- 起こる状況: (a) decode の途中で process が死に session・command が release され、pool・FIFO・PU・係数・app の bitstream の buffer が
  解放されて別の用途に回った後、block が書く・読む。(b) timeout で clock を落としても block の AXI の書き込みが残っていれば、解放した
  memory を後で壊す。どちらも DMA の use-after-free。
- 直し方: block の「持ち主の記録」（今動いている decode の FIFO・pool・PU・係数・MV・bitstream の buffer の参照）を driver の大域に置き、
  block が idle と確かめるまで（INT を見た、または reset の後に idle を確かめた）解放しない。timeout の後は、それらを隔離したまま block を
  使えない状態にし、idle を確かめる手順（推測: clock の off/on と STATUS・CFSTATUS の読み）が実機で確かめられるまで再利用しない。
  release_session は block の持ち主なら idle を待つか隔離に移す。

### 12.（高）EINPROGRESS で step を返した後の再入と、session の間の block の取り合いが決まっていない

- 根拠: phase.md:49。vulkan-native-job.c:295-317（EINPROGRESS の時 cursor をその event に残し、次の step で同じ record で
  `bcm2711_vulkan_video_run` を呼び直す）。block は一つ、V3D の render device は全 process で共有。
- 起こる状況: 二度目の呼び出しが decode を最初から出し直すと、動いている phase を壊す。session A の decode の最中に session B の decode の
  step が来て register を書くと、A の絵が壊れる。A の step が他の session の長い step で遅れ、block は終わっているのに 500 ms を越えて
  ERROR にする。
- 直し方: driver に block の状態機械（idle → phase1 → (retry) → phase2 → TFU → done）と持ち主（session と record）を持たせ、run は
  「持ち主の record なら状態を進める、他の record なら block が空くまで EINPROGRESS」にする。timeout を宣言する前に必ず ICTRL を読み直す。
  二つの process が同時に decode する試験を p003 に入れる（25 番）。

### 13.（中）decode の誤りで run が誤りを返すと job 全体が失敗する。slot の状態も直す

- 根拠: vulkan-native-job.c:318-322（video の event の誤りは job を prepare_failed にして返す）。Pi 4 は skip で status を ERROR にし 0 を返す
  （vulkan-video.c の skip・set_status）。設計 phase.md:49-50 は「誤り（status query は ERROR、image は前のまま）」とだけ書く。
- 起こる状況: 壊れた stream の 1 枚で submission 全体（後の copy・barrier を含む）が失敗し、vkvideo-probe と libmedia が device lost 扱いにする。
  誤った decode の setup の slot が前の絵のまま「有効」として後の参照に使われる。
- 直し方: bitstream・parameter・hardware の誤りは query を ERROR にして 0 を返す（kernel の資源の誤りだけ errno）。誤った decode の
  setup の slot と、reset（VIDEO_CODING_CONTROL_RESET）・begin の slot の外しで無効になった slot に「無効」の印を付け、無効の slot を
  参照する decode は ERROR にする（または 15 番の既定の frame で読む）。

### 14.（中）MMIO の開始の書き込みの前と、INT を見た後の barrier

- 根拠: Linux は開始の register だけ `writel`（barrier つき）、他は relaxed（hevc_d_hw.h:80-91）。`kern_mmio_write32` は normal memory との
  順を約束しない（include/kern/device-io.h:36-62）。
- 起こる状況: CPU が NC の FIFO に書いた最後の組が write buffer に残ったまま CFBASE を書き、block が古い組を読む（推測、Cortex-A76 で稀）。
- 直し方: FIFO を書き終えてから `kern_io_write_barrier()`、その後 CFBASE。phase 2 も NUMROWS の前に同じ。INT を見た後、TFU を出す前と
  STATUS を読む前に `kern_io_read_barrier()`。

### 15.（高）参照の無い位置（STD_VIDEO_H265_NO_REFERENCE_PICTURE）と NumPicTotalCurr の数え方

- 根拠: Linux は 16 の参照の番地を先に自分の frame で埋め、壊れた参照でも有効な番地にする（hevc_d_h265.c:1744-1750）。設計 phase.md:44-45 は
  RefPicSetStCurrBefore・After・LtCurr から list を作るとだけ書く。写す元の nvrtx は最初の 0xFF で数えを止める
  （vk-video-h265.c:708-742 `h265_set_fill`・`h265_set_count`）。
- 起こる状況: (a) CRA・BLA の後の RASL や、app が参照を失った stream（RAP_A・RAP_B・NoOutPrior の類）で 0xFF の位置がある時、
  ref_addrs が 0（物理 0 番地）か未初期化のまま block が読む。(b) 0xFF が途中にあると NumPicTotalCurr を少なく数え、list_entry の bit 数
  （Ceil(Log2(NumPicTotalCurr))）を誤って header の parse が以後ずれる。
- 直し方: block の 16 の参照の番地を全て、まず現在の絵の pool の frame（か予備）で埋める。0xFF の参照には 8.3.3 の「無い参照」の扱い
  （既定の frame）を当て status を ERROR にするかは決めて書く。NumPicTotalCurr は Vulkan の配列の 0xFF の数え止めでなく、RPS の構造
  （SPS の st_ref_pic_set の used フラグ、header の long-term の used_by_curr）から出す。header の中の st_ref_pic_set を「飛ばす」なら、
  used の数だけは StdVideo の SPS か picture info から確かに取れることを示す（取れなければ st_ref_pic_set も読む）。

### 16.（中）collocated の絵の動き vector が書かれていない slot

- 根拠: Linux は MV を作るのは mk_aux の時だけ（SPS の temporal MVP、かつ sub layer と NAL の種類、hevc_d_h265.c:1755-1767）、CONFIG2 の
  bit 15（hevc_d_h265.c:1560-1561）。collocated の aux が無ければ中止（hevc_d_h265.c:1930-1952）。設計 phase.md:34・45 は slot ごとに
  MV の buffer を持つとだけ書く。
- 起こる状況: collocated の slot の絵が MV を書かなかった（temporal MVP が無い SPS の時、別の stream の残り、予備の frame）時、block が古い
  MV を読み絵が壊れる。番地は有効なので kernel は壊れないが framehash が合わない（TSCL・NUT の類で推測）。
- 直し方: slot ごとに「MV 有効」の印を持ち、CONFIG2 の bit 15 を立てた decode の後だけ立てる。collocated の slot が無効なら ERROR。
  setup の slot が無い decode は MV を書かない（bit 15 を落とし MVBASE 0）。mk_aux の判定に要る nal_unit_type と nuh_temporal_id_plus1、
  sps_max_sub_layers_minus1 を読む（17 番）。

### 17.（高）slice header の parse の欄が足りない・既定値の受け継ぎが無い

- 根拠: 設計 phase.md:44 の列挙。Linux が使う値（hevc_d_h265.c:596-890、1755-1840）と H.265 7.3.6.1・7.4.7.1。
- 足りない物: NAL header（nal_unit_type・nuh_temporal_id_plus1。IRAP の no_output_of_prior_pics の有無と mk_aux に要る）、
  num_extra_slice_header_bits の bit、short_term_ref_pic_set_idx の bit 数、slice_segment_header_extension_length とその byte、
  offset_len_minus1、num_ref_idx_active_override が無い時の PPS の既定、deblocking_filter_override が無い時の PPS の
  disabled・beta・tc、slice_loop_filter_across_slices_enabled_flag の PPS の既定、dependent slice segment の時に前の独立の segment から
  全ての値（slice_type・参照・QP・重み・offset）を受け継ぐこと、pred_weight_table の各参照の flag を読む条件（版 2 以降の
  「同じ POC でない」条件。単層の Main では常に真だが、参照の list を先に作る必要がある）。
- 起こる状況: DSLICE・ENTP・WPP・DBLK・CAINIT・WP の適合の stream で値がずれ、命令の FIFO が誤り framehash が合わない。
- 直し方: 設計の欄の表を「欄・条件・既定・受け継ぎ」の表にし、host 試験で全ての欄を ffmpeg の trace_headers と照合する（24 番）。
  nvrtx の SPS・PPS の構造体（nvrtx-video.h:214-262）は NVDEC が要る欄だけなので、写す時に sps_max_sub_layers_minus1・
  lists_modification・cabac_init_present・slice_segment_header_extension・deblocking_override などを足す。

### 18.（中）header の長さを 512 byte に限ると、entry point・重み・extension の多い header が読めない

- 根拠: nvrtx の `H265_HEADER_BYTES 512` と unescape の固定の buffer（vk-video-h265.c:33-34、508-511）。設計は header の終わりの byte の
  位置が要る（phase.md:44）ので、entry point と extension の後まで読む必要がある。
- 起こる状況: 2160p の CTB 16 の WPP（135 行、entry point 134 個 × 最大 32 bit）、15 参照 × 2 list の重み、extension 255 byte の header で
  512 byte を越え EINVAL。
- 直し方: header を固定の buffer に unescape せず、escaped の byte 列の上で emulation prevention を外しながら読む reader（読んだ escaped の
  byte の位置を返す）にし、上限は slice segment の長さだけにする。kernel の stack に大きな配列を置かない。

### 19.（高）slice segment の数と長さ、範囲の検べ

- 根拠: nvrtx は H.265 の slice の offset を表の大きさで切り捨て「NVDEC が start code で探す」とする（vk-video.c:2616-2622、3296-3318）。
  HEVC の block は segment ごとに BFBASE・BFNUM を要る（hevc_d_h265.c:559-579、Linux は bit_size と data_byte_offset を検べる 1805-1816）。
- 起こる状況: nvrtx の reader をそのまま写すと、表を越える slice が黙って落ち、絵の下が欠ける。offset が srcBufferRange を越える、前後が
  逆、segment が header より短い、を kernel が読まずに block に渡すと範囲外を読む。
- 直し方: segment の数の上限（例 PicSizeInCtbsY か 600）を決め、越えたら ERROR。各 segment の長さ = 次の offset（最後は range の終わり）−
  この offset、start code と次の NAL の前の trailing zero を除く。全ての offset・長さを buffer の範囲と srcBufferOffset・Range で検べる。
  data offset ≥ 長さなら ERROR。

### 20.（中）Vulkan の memory は uncached（NC）。「dcache clean」と「kernel の map から読む」は誤り・危険

- 根拠: Vulkan の memory は全て `bcm2711_buffer_create_uncached`（vulkan-memory.c:119、417）、HOST_COHERENT の 1 種類
  （vulkan-query.c:157）、NC の alias だけを使う（buffer.c:163-173）。設計 phase.md:47 は「CPU が書いた範囲を decode の前に dcache clean」
  「header の parse は kernel の map から読む」。
- 起こる状況: cached の direct map から読むと、属性の違う alias の混在（arm64 で未定義）で古い line を読む恐れ（推測）。clean は意味が無い。
  parse の後に app が同じ memory を書き換える（TOCTOU）と、kernel の値と block の読む bitstream が食い違う。
- 直し方: header の parse は `buffer->address`（NC の alias）から、header の範囲を kernel の buffer に一度写してから行う。
  bitstream の clean は不要と書き、代わりに 14 番の barrier を書く。FIFO は NC（毎絵 CPU が書く）、PU・係数・MV・pool は NC か CPU 不触。

### 21.（中）phase 1 の取り直しに上限が無い

- 根拠: Linux は exhausted のたびに `next_size` で伸ばし直す（hevc_d_h265.c:2093-2130、62-95）。設計 phase.md:29・35 も「足りなければ一段大きく」。
- 起こる状況: 壊れた（悪意の）stream で毎回 exhausted を返させ、kernel の連続の memory を伸ばし続ける・step を占める。
- 直し方: 取り直しの回数（例 3 回）と大きさの上限（例 幅×高さ×4 と 64 MiB の小さい方）を決め、越えたら ERROR。取り直しの確保は
  session に結びつけ、budget に数える（22 番）。

### 22.（高）pool の memory が隠れた kernel の確保で、16 slot × 4096² では 400 MiB を越える

- 根拠: phase.md:42（maxDpbSlots 16、最大 4096×4096）、phase.md:45（session の pool、slot ごと）。NV12 の 4096×4096 は 24 MiB、16 + 予備で
  約 410 MiB、MV・PU（12 MiB）・係数（48 MiB、`hevc_d_round_up_size(2^24)` = 3·2^24）を足して 470 MiB の連続の memory。Pi 4 は session の
  memory を 1 page の名目で報告する（vulkan-video.c:149・1596 `VIDEO_BIND_BYTES`）。
- 起こる状況: (a) 連続の確保が失敗し、4K の session が作れない（maxCodedExtent と矛盾）。(b) どの process でも session を作るだけで
  数百 MiB の kernel の memory を budget の外で取れ、他を止められる。
- 直し方: pool・PU・係数・MV の大きさを session の maxCodedExtent・maxDpbSlots から計算し、`vkGetVideoSessionMemoryRequirementsKHR` で
  報告して app が bind した memory を使う（Vulkan の普通の形。charge も既存の Vulkan の budget に乗る）。kernel が自分で取るなら、session の
  作成で確保し失敗を VK_ERROR_OUT_OF_DEVICE_MEMORY で返し、budget に数える。maxCodedExtent は実機で確かめる大きさ（例 4096×2304、
  level 5.1 の MaxLumaPs に合う）に下げることも考える。

### 23.（中）DPB の slot と decode の引数の検べ

- 根拠: phase.md:45、R7。vulkan-video.c:2595-2632（Pi 4 は slot を読み捨てる）。
- 足りない検べ: slot の番号 < session の maxDpbSlots かつ < 16（kernel の配列の index）、setup の slot が同じ decode の参照の slot と同じでない、
  dst の codedOffset が 0（または granularity の倍数で TFU の出力の番地に足す）、dst の codedExtent と SPS の大きさが合う、dst の image の
  extent ≥ codedExtent、参照の数 ≤ maxActiveReferencePictures、COINCIDE の時 setup の picture resource が dst と同じ。
- 直し方: 上を decode の記録の時（record_command）に検べ、範囲外は ENOTSUP・EINVAL、stream の誤りは ERROR。

---

## C. 出力の image・TFU・Video Player

### 24.（中）TFU の写しの時と generation の進め方、cross-device の snapshot

- 根拠: 設計 phase.md:46（写した後に generation を進める）。snapshot は buffer の generation と host_mapped で決まる（buffer.h:41-50、
  native-storage.c:504-507）。generation は `bcm2711_v3d_memory_written`（v3d-memory.c:324-345）。TFU の job は同期（v3d71-job.c:536-588、
  vulkan-native-execute.c:500-520）。Video Player は別の device で同じ memory を import して sample する（present.c:1440-1475）。
- 起こる状況: TFU を出してすぐ EINPROGRESS で step を返し、完了の前に generation を進めると、presenter の device の draw がその間に古い
  （書きかけの）raster を変換し、新しい generation の snapshot として使い回す。輝度と色差の 2 job の間で返しても同じ。
- 直し方: 2 つの TFU job を同じ step の中で完了まで走らせ、両方の後に plane ごとの box か NULL で `bcm2711_v3d_memory_written` を呼ぶ。
  TFU が失敗した時（retired false）は v3d-job.h:96-100 の隔離に従い、pool と出力の image を解放しない。

### 25.（中、推測）HEVC が書いた pool を TFU が読む時の V3D の cache

- 根拠: 既存の TFU の変換は CPU が書いた raster を読む。HEVC の block は V3D の外の master。V3D の L2T が TFU の読みを cache するかは
  未確認（v3d71-job.c:551 は TFU に cache の invalidate を使わない）。
- 起こる状況: slot を使い回した時、前の絵の pool の line が V3D の cache に残っていれば古い絵が出る（推測）。
- 直し方: p003 で 2 つの slot を交互に使う stream と、同じ slot を連続して使う stream で framehash を照合し、ずれたら SAND の TFU の前に
  L2T を invalidate する。設計の R に入れる。

### 26.（低）TFU の検べを開く範囲

- 根拠: v3d71-job.c:251-262（入力・出力は 64 byte 揃え、出力は tiled の形だけ）。Mesa の SAND は 1 KiB（v3dx_tfu.c:267-270）、raster の出力の
  stride は IOC の bit 16 に texel で（v3dx_tfu.c:186-195）。
- 直し方: raster の出力を許す時は出力の範囲を `(H-1)×stride + W×cpp` で検べる。SAND の入力（ICFG の IFORMAT 1）の時だけ 1 KiB 揃えと
  IIS の 8 の倍数・0x7fff 以下を検べる。既存の UIF の変換の道は変えない。

### 27.（低）一絵に TFU が 4 回（SAND→raster 2 回、presenter の raster→UIF 2 回）

- 根拠: phase.md:46、vulkan-native-draw.c:488-540（sample の時に raster→UIF の TFU）。
- 起こる状況: 1080p60 で HEVC + TFU 4 回 + 合成。R4・R5 と合わせて 16 ms の枠を圧迫する（推測、TFU の速さは未計測）。
- 直し方: p003 で decode・TFU・present の時間を分けて計る（Pi 4 の cost line と同じ形）。足りなければ SAND→UIF を sampled の image の
  snapshot へ直接、を後の Phase の案にする。

### 28.（中）Pi 4 の部品を Pi 5 で使う時の切り分け（link と意味）

- 根拠: vulkan-buffer-copy.c:17-21・293-331（NV12 の readback が `drv_rpi4_mmal_copy_samples` を呼び `rpi4-mmal-wire.h` を include）、
  vulkan-video.h:56-115（`image_raw`・`readback`・`readback_done`・`settle_mark`・`mark_*`・`available`・`session`・`promise`・`event`・
  `query_dispatch` も口）、render-runtime.c:121-135（capset の native word に H.264 を固定で書く）、Makefile:396-409（Pi 4 以外で
  BCM2711_VIDEO を n に上書き）。設計 phase.md:41 は「派生の macro」と口の一部（dispatch・run・settle・mark・nv12_layout・readback）だけを書く。
- 起こる状況: 派生の macro で囲むだけだと、Pi 5 の build が rpi4 の symbol で link に失敗するか、`image_raw` が true を返し Pi 5 の
  readback に firmware の footroom の補正を掛けて framehash が全て合わない。capset が H.264 を約束し libvulkan が H.265 の queue を出さない。
- 直し方: 設計に口の全ての一覧と Pi 5 での意味（image_raw は false、readback は false＝image から、settle・mark は decode が step の中で
  終わるので 0）を書く。rpi4 の補正の呼び出しは Pi 4 の file の側の関数に移す。capset の word は video の file が返す値にする。派生の macro は
  Makefile の -D で定義（header で定義すると include しない file で 0 になる）。

### 29.（低）新しい config の値の登録

- 根拠: config/drivers/architecture/arm64.drivers:11・14、Makefile:1012-1013（-D の列）、plan/tools/menuconfig-target-host-test.py、
  config/ci/*.mk（全 config が BCM2711_VIDEO を書いている）。config/ci/config-amd64-server.mk はユーザーの物で担当は変えられない（AGENTS §7）。
- 直し方: arm64.drivers に `CONFIG_DRIVER_BCM2712_VIDEO|bool|…|rpi5|y|`、Makefile に既定と -D、Pi 5 以外での上書き。全 config に行を足す
  慣習なら config-amd64-server.mk は Q1 に頼む、と Phase に書く。

---

## D. 出典・規則

### 30.（中）命令の FIFO の組み立ての「手順の引用」が code の構造の写しになる危険

- 根拠: guardrail.md:128-137（GPL の値は block ごとの出典で可、code・comment・構造は写さない。手順の明示の引用は WS039 で認められた形）。
  設計 phase.md:13・32（tile・WPP・dependent slice の手順は Linux の decode_slice・wpp_decode_slice の順に従う）。Linux の該当は
  wpp_entry_fill・wpp_end_previous_slice・new_entry_point などの細かい関数の分け（hevc_d_h265.c:901-1200）。
- 起こる状況: 同じ関数の分けと分岐の順で書くと、WS の最後の類似の監査で「構造の写し」になる。
- 直し方: 設計で、命令の列を「CTB の走査（6.5.1 の ts 順）と register の意味の表」から自分の形で出す（例: 絵の entry point の列を先に
  作り、各 entry の境で出す命令の表）と決め、Linux は「Procedure referenced:」で順と値の照合にだけ使う。CABAC の初期値の式は
  H.265 9.3.2.2、表の値は Table 9-5〜9-37 を出典にし、Linux からは並びの順だけを引く。WS の最後に Linux の file との類似の監査を置く（34 番）。

### 31.（低）DT の出典の file と license の誤り

- 根拠: hevc の node は `bcm2712-ds.dtsi:372-384`（SPDX GPL-2.0）にあり、`bcm2712.dtsi`（GPL-2.0 OR MIT）には無い。IOMMU2 の node も
  bcm2712-ds.dtsi:283-290（aperture-size 4 GiB）。設計 phase.md:14。
- 直し方: 出典の表を bcm2712-ds.dtsi（GPL-2.0、値だけ）に直し、axi の ranges・dma-ranges（恒等）は bcm2712.dtsi:429-444 と分けて書く。

### 32.（低）ws.md の記録の規則の違反と食い違い

- 根拠: ws.md:9-13 に「原文」としてユーザーの発言をそのまま載せている（AGENTS §8、公開の repo に出さない）。ws.md:7 の目標は
  `src/drivers/gpu/bcm2712/` だが設計は `src/drivers/gpu/bcm2711/`（phase.md:41）。
- 直し方: 発言を「（日付）ユーザーから○○という趣旨の指示を受けた」の形に書き直す（担当の WS の範囲なので P3 が直せる。誰が書いたか
  不明なら Q1 に確かめる）。目標の directory を実態に合わせる。

---

## E. 試験の計画と Phase の分け

### 33.（中）host 試験の照合の相手が同じ project の parse で、命令の FIFO は自分の出力との照合

- 根拠: phase.md:54（libmedia の h265-stream・h265-std と照合、命令の FIFO は「固定の trace」と照合）。WS324 の適合の道具は ffmpeg の
  trace_headers も使う（plan/ws324/tests/conformance/run.sh:1-20）。
- 起こる状況: libmedia と kernel の parse が同じ誤解（既定の受け継ぎ、0xFF の数え）を持てば host 試験は通り、p003 で初めて壊れる。
  自分の code が作った trace と照合しても誤りは見つからない。
- 直し方: slice header の全ての欄（entry point の数、header の終わりの escaped の byte の位置を含む）を ffmpeg の trace_headers と照合する。
  命令の FIFO は「register ごとの意味の表から手で作った小さな例」（I の 1 slice、P、B の重み、tile 2×2、WPP、dependent）で照合し、
  hardware の照合の順も同じ順に並べて、p003 で壊れた所を一段で絞れるようにする。

### 34.（中）Phase の分けと依存

- 根拠: phase.md:72-75（p002 が ①〜④ を一つ、p003 が vkvideo-probe と Video Player）、ws.md の Phase の表（p003 の依存は p002 だけ）、
  R8（phase.md:66、Video Player の H.265 の backend は ws322-p006）、AGENTS §2（code を作る WS は最後に「規約の全文の見直し」の Phase）、
  phase.md:70（review を p002 と並べて流す）。
- 起こる状況: p002 が大きすぎ、Pi 4 の非退行・security の検べ・TFU の検証が一つの Queue に入る。p003 が ws322-p006 の未完で uncleared に
  なり WS が閉じない。規約の見直しの Phase が無い。review の反映の前に p002 の実装が進み手戻りになる。
- 直し方: p002 を「純粋な parse・命令の列・geometry の検べと host 試験」と「block の driver・executor・build」に分け、p003 を vkvideo-probe の
  受け入れ、p004 を Video Player（依存 ws322-p006）、最後に規約の全文の見直しと GPL の類似の監査の Phase を足す。ws.md の表に
  cross-WS の依存を書く。p002 は本 review の重大・高の反映の後に始める。

### 35.（中）p003 に hardware の壊れた入力・同時の session・slot の使い回しの試験が無い

- 根拠: phase.md:55（適合の stream と clip だけ）。R6（立て直し不明）。
- 起こる状況: 最初の hang が現場で起き、block が boot まで使えなくなる。二つの process の取り合い（12 番）や slot の使い回し（25 番）の
  誤りが受け入れを通る。
- 直し方: p003 に (a) bit を壊した stream（timeout と ERROR の道、その後に正しい stream が decode できること）、(b) vkvideo-probe 2 本の
  同時、(c) probe と Video Player の同時、(d) slot を交互・連続に使う stream、(e) 32 未満の大きさ・WPP+tile の PPS が ERROR で返ること、
  を足す。Pi 4 の vkvideo-probe の H.264 を 1 本（28 番の切り分けの非退行）を Pi 4 の実機が割り当てられる時に流す。

### 36.（低）WPP と tile の同時の PPS を断る所

- 根拠: hevc_d_h265.c:2487-2505（Linux は PPS の control で断る）。Vulkan は session の作成では PPS を知らない。
- 直し方: session parameters の作成・更新でその PPS を受けた時に断るか（VK_ERROR_INITIALIZATION_FAILED など、libvulkan の wire の
  返し方を確かめる）、その PPS を使う decode を ERROR にする、を決めて書く。

### 37.（低、推測）割り込みの線を GIC で開けたままにしない

- 根拠: phase.md:27（EN を立て、GIC は使わず poll）。dtsi の割り込みは level high（bcm2712-ds.dtsi:379）。
- 起こる状況: zedBSD の GIC で SPI 98 が有効のままなら、EN を立てた block の INT が level で上がり続け、受け手の無い割り込みが続く（推測）。
- 直し方: 最初の版では SPI 98 を GIC で無効のままであることを確かめる（kern_irq_register を呼ばない限り無効か）と書く。

---

## まとめ（重要な順）

1. 7・8: app の SPS・PPS と geometry を検べないまま IOMMU 無しで block に渡すと kernel の memory を DMA で壊せる。IOMMU を使うかは
   ユーザーの判断。
2. 1・3: pool の column の高さを 16 に揃えないと 1080p の色差の TFU が通らず、pool を V3D の MMU に map しないと TFU が読めない。
3. 4: ICTRL の INT を消す時に EN を落とすと 2 枚目から全て timeout。
4. 11・12・13: block が動いている間の buffer の寿命、EINPROGRESS の再入と session の取り合い、誤りで job 全体を落とす道。
5. 15・17・18・19: 参照の無い位置、header の欄と既定の受け継ぎ、512 byte の上限、slice の切り捨て（nvrtx からの写しで入る誤り）。
6. 22: pool の memory を session の memory として報告せず kernel が隠れて取る形は、4K で確保できず DoS にもなる。
7. 28・34: Pi 4 の部品の切り分けの抜け（footroom の補正、capset）と、Phase の分け・依存・規約の見直しの Phase の抜け。
