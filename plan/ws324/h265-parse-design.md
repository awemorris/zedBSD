<!-- awesome-plan project=zedbsd record=ws324-h265-parse-design -->

# WS324 p006a 設計: H.265（HEVC）の bitstream の解析（NAL・VPS・SPS・PPS・slice segment header・POC・RPS・DPB）を StdVideoH265 へ

- 親: [ws.md](ws.md)、Phase [ws324-p006a](phase006a/phase.md)、WS の設計 [design.md](design.md)（§0 の libmedia の項、Q324-1）。手順は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)、以下「手順」）の §4。
- 作成: 2026-11-01 P1（q1864-i01）。調べた tree: main 00ad53445（worktree `p1-ws324`）。
- 正本: 各 module の仕様は [proposed/h265/](proposed/h265/) の header の comment（11 個）。この文書は地図・決定・試験・Phase の割り振り。source は変えていない（header を source に置くのは p006a の段 2、§9）。
- HAL（`include/hal/hal.h`）は関係しない。toolchain も変えない（Vulkan の H.265 の std の header を tree に入れるのは p002、§2.3）。

## 0. 要約

- Q324-1（Q1 の判断）: WS318 p016〜p019 の範囲（H.265 の NAL・VPS・SPS・PPS・slice segment header・POC・RPS・DPB を StdVideoH265 へ）を、手順で先に作る。libmedia の build と runtime には繋がない。使い手は WS324 の `vkvideo-probe` の H.265（nvrtx の NVDEC の確かめ、p007）と WS322 の VF2 の Wave511（p006）、後で WS318 p020 の libmedia の Vulkan Video の道。
- **土台は WS330 の proposed の解析の header**（`h265sw-nal`・`-scaling`・`-rps`・`-vui`・`-ps`・`-slice`・`-poc`）。WS330 では Q1 が Q330-2 の A（H.265 の解析は WS318 と 1 つに）を決め、WS330 の設計が「WS318 p016 がこの header を採り、Vulkan の道が要る生の値（VPS・HRD・st_ref_pic_set の生の syntax・slice header の RPS の生の field）を足す」と書いている（WS330 design D4・§11）。その通りに採り、足した（D1）。
- Vulkan の道だけの物は新しい 3 つ: `h265-dpb`（Vulkan の DPB の slot と、8.3.2 の marking と C.5.2 の出力の順）、`h265-stream`（Annex B から picture を組み、NoRaslOutputFlag・POC・RPS を出す）、`h265-std`（StdVideoH265 の構造体へ）。H.264 の `h264.c`・`h264-dpb.c` と同じ分け方（D2）。
- 実装は Sonnet 5.5 high の 6 Phase（p006b〜g）、その前に header を source に置く段 2（p006a の 2 回目、Opus）、後に適合の試験（p006h、Opus、HEVC_v1 の 147 本の Main の範囲）と `vkvideo-probe` の H.265（p006i、Opus、今の ws.md の案の p006e）。§9。
- ユーザーの判断が要る点は無い。Q1 の技術の判断が 5 つ（§13）。

## 1. 範囲と範囲の外

| 項目 | 範囲 | 範囲の外と扱い |
| --- | --- | --- |
| 層 | base layer（nuh_layer_id 0） | 他の層の NAL は読み飛ばし数える（`other_layer`） |
| 解析が保つ形式 | parameter set は有効な値を全部保つ（chroma_format_idc 0〜3、bit depth 8〜16、SPS の range extension の 9 つの flag）。WS330 D4 と同じ「解析は範囲で拒まない」 | 読まない syntax: SPS・PPS の screen content・3D・multilayer の拡張、PPS の range extension の道具 → ENOTSUP（`*_refused`）。CTB 16 未満・64 超、level 6.2 を超える大きさ・tile 数 → ENOTSUP |
| decode する範囲 | driver が決める（nvrtx・Wave511 は Main、8 bit 4:2:0、WS318 Q318-8 の Q1 の判断）。`vkvideo-probe` が profile と形式を driver の能力で確かめる（p006i） | — |
| 絵の構造 | progressive の frame。field_seq_flag は値として保つ | field の列の表示の扱いは使い手（libmedia・probe） |
| 出力 | picture ごとの Vulkan の情報（StdVideoDecodeH265PictureInfo、参照の slot と StdVideoDecodeH265ReferenceInfo、slice segment の offset）、session の parameters（VPS・SPS・PPS の StdVideoH265）、表示の順（C.5.2 の bumping） | crop（conformance window）は値を渡すだけ |
| 入力 | Annex B の byte stream。container の hvcC は使い手が Annex B に直して渡す（`h265_stream_parameters`） | 長さ前置の NAL の直読み |
| 生成されない参照 | 8.3.3 は行わない（D10）。壊れた stream の Curr の欠けは報告だけ | 欠けた参照の代わりの灰の絵（WS330 D13）は Vulkan の道では使い手が決める |

## 2. 仕様と資料の入手

### 2.1 ITU-T H.265

- **ITU-T H.265 (V11) (01/2026)**（ISO/IEC 23008-2 と本文が共通）。WS330 p001 が ITU の site からパラメータの無い GET で取った物を、Q1 が main の消えない場所に置いた:
  - path: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（734 頁、12 246 649 byte、git に入れない）。
  - **SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`**（2026-11-01 に `sha256sum` で照合、WS330 の記録と一致）。
  - **印刷の頁 N = PDF の頁 N + 18**（印刷 32 = PDF 50、印刷 42 = PDF 60 で確かめた。WS330 の 8.6.4.2・7.4.7.1 の 2 か所とも合う）。
  - 読み方: `pdftotext -layout -f N -l M`。上付き（2^15 が「215」、2^31 が「231」）と表の折り返しは崩れるので、値は頁の画（Read の tool の `pages`）でも確かめる（WS326 の教訓）。
- 読む節（印刷の頁 → PDF の頁）:

| 節 | 内容 | 印刷 | PDF |
| --- | --- | --- | --- |
| 3.67・3.73・3.132 | IDR・IRAP・reference picture の定義 | 8・11 | 26・29 |
| 7.2・7.3.1・7.3.2.1〜7.3.2.3 | 記述子、NAL、VPS・SPS・PPS の syntax | 33〜40 | 51〜58 |
| 7.3.2.11・7.3.2.12 | rbsp_trailing_bits・byte_alignment | 41 | 59 |
| 7.3.3・7.3.4 | profile_tier_level・scaling_list_data | 42〜45 | 60〜63 |
| 7.3.6.1〜7.3.6.3・7.3.7 | slice segment header・list の modification・pred_weight_table・st_ref_pic_set | 46〜50 | 64〜68 |
| 7.4.2.2・7.4.2.4.4・7.4.2.4.5 | NAL の型（Table 7-1）、access unit と順 | 64〜72 | 82〜90 |
| 7.4.3.1〜7.4.3.3 | VPS・SPS・PPS の semantics | 72〜89 | 90〜107 |
| 7.4.4・7.4.5 | PTL・scaling list の semantics、Table 7-5・7-6 | 89〜94 | 107〜112 |
| 7.4.7.1〜7.4.7.3・7.4.8 | slice header・modification・重み・RPS の semantics（7-52〜7-71） | 95〜104 | 113〜122 |
| 8.1.3・8.3.1・8.3.2・8.3.3 | NoRaslOutputFlag・PicOutputFlag、POC、RPS、生成 | 117〜125 | 135〜143 |
| A.4.1・Table A.8 | level の上限（MaxDpbSize、slice segment 600、tile 20×22、NumPicTotalCurr 8） | 262〜266 | 280〜284 |
| B.2・B.3 | byte stream | 273〜274 | 291〜292 |
| C.5.2.1〜C.5.2.4 | 出力の順の DPB | 288〜289 | 306〜307 |
| E.2.1〜E.2.3・E.3.1〜E.3.3 | VUI・HRD | 427〜451 | 445〜469 |

### 2.2 Vulkan Video の H.265 decode

- 構造体: Khronos の `vulkan_video_codec_h265std.h`・`vulkan_video_codec_h265std_decode.h`（Apache-2.0、host の Debian `libvulkan-dev 1.4.309.0-1` の `/usr/include/vk_video/`）。zedBSD の tree の `include/libc/vulkan/vk_video/` に入るのは WS324 p002（Q324-2、Q1 の toolchain の許可）。p006b〜g の host 試験は host の header で compile する（tree の物と同じ版の写しになる、design.md §11）。
- 意味（Vulkan の仕様の本文、docs.vulkan.org の `chapters/videocoding.html` と refpage `VkVideoDecodeH265PictureInfoKHR` をパラメータの無い GET で 2026-11-01 に読んだ）: IrapPicFlag・IdrPicFlag・IsReference は H.265 の 3.73・3.67・3.132、PicOrderCntVal は 8.3.1、RefPicSet* は 8.3.2 の意味で DPB の slot か `STD_VIDEO_H265_NO_REFERENCE_PICTURE`、NumBitsForSTRefPicSetInSlice は sps の flag が 0 の時の st_ref_pic_set の bit 数、NumDeltaPocsOfRefRpsIdx は「`short_term_ref_pic_set_sps_flag` が 1 の時の NumDeltaPocs[RefRpsIdx]、他は 0」と書かれている（D8 で扱う）。scaling list の 4 つの配列は ScalingList[0]〜[3]、RPS の used_by_curr_pic_flag などは bit i が [i] の bit mask、PPS の鍵は VPS・SPS・PPS の id の組、pSliceSegmentOffsets は picture の各 slice segment の始まり。
- 本文の引用は短い要約だけにした（Khronos の仕様の文の写しは置かない）。

### 2.3 適合の試験の data

- ITU-T H.265.1 の HEVC_v1 の bitstream（JCT-VC、ITU の公開の mirror `https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1/`）。2026-11-01 に一覧を GET で読み 147 本（WS330 の 2026-10-31 の一覧と同じ名前の組）。zip の中は `.bit`/`.bin`、復号の出力全体の `.md5`（`MD5 (dec.yuv) = …` や `… *NAME_dec.yuv` の形）、説明の `.txt`、一部に macOS の `.DS_Store` などの余り物。
- 一覧と取得の script は WS330 が作った物 [plan/tools/h265/conformance/](../tools/h265/conformance/)（`list.txt` 147 行、名前から in 136・out:main10 11 の推定、`fetch.sh` はパラメータの無い GET・SHA-256 の記録と照合・zip でない答えで即止める、展開しない）。WS324 はこれを使い、二重に作らない（Q1 への問い Q-D: plan/tools/ へ移す）。
- cache: main の `/home/awe/zedBSD-claude1/build/sources/h265-conformance/`（WS318 §13.3・WS330 §10.3 と同じ場所。まだ無い。Q1 が作る、zip で 129 MB、disk は 2026-11-01 に 275 GB 空き）。
- 2026-11-01 に P1 が 3 本（`ipcm_A_NEC_3`・`POC_A_Bossen_3`・`RPS_A_docomo_5`）を scratchpad に取り、中身の形と host の ffmpeg 7.1.5 の 2 つの副の oracle（§8.4）が使えることを確かめた（`RPS_A` は 44 frame、`-threads 1 -loglevel debug` の `Output frame with POC 0/N.` が 44 行）。

## 3. 今の姿（main 00ad53445 で読んだ事実）

| 物 | 事実 | 出所 |
| --- | --- | --- |
| H.265 の code | libmedia・libvulkan・driver に無い | WS324 design §2 |
| 共通の bit reader・NAL | `vcodec-bitreader.h`（u・ue・se・te・more_rbsp_data・trailing。**byte_alignment は無い**）、`vcodec-nal.h`（`vcodec_nal_find`・`vcodec_nal_unescape`）、`vcodec.h`（誤りの値の約束）。H.264 の software の decoder と共有、WS326 の基点 77ed48ac4 の header の照合（`BASE=77ed48ac4 sh plan/tools/h264sw/check-skeleton.sh`）が `vcodec*.h` の不変を見る | `userland/desktop/libmedia/vcodec*.h`、`plan/tools/h264sw/check-skeleton.sh:56-62`、`plan/tools.md` |
| H.264 の Vulkan の道の形 | `h264.c`（Annex B → picture、StdVideoH264 を直に持つ）と `h264-dpb.c`（論理の参照と Vulkan の slot を分ける、`busy` の mask） | `libmedia/h264.h`・`h264-dpb.h` |
| probe の形 | `vkvideo-probe` は自分の `h264.c`・`dpb.c` を持ち、slice を 3 byte の start code の後ろに写して offset を渡す。表示の順は POC で並べ直す | `userland/tests/vkvideo-probe/main.c:1103-1151,1426-1500` |
| WS330 の proposed | decoder の header 29 個・共通 7 個。解析の 7 個（nal・scaling・rps・vui・ps・slice・poc）と `h265sw-dpb`（`vcodec-dpb`・`vcodec-frame` の上、WS330 p002 の共通化に依る）。p003 で source に置く予定。Q330-2 の A | `plan/ws330/proposed/`、`plan/ws330/design.md` D4・§11 |
| WS330 の共通の変更 | `vcodec_bitreader_byte_alignment`・`vcodec_asr32` などを WS330 p002（WS329 completed の後）で足す案。まだ source に無い | `plan/ws330/proposed/common/` |
| WS326 の試験の補助 | `plan/tools/h264sw/unit/check.h`（判定）・`bitwriter.h`（u・ue・se・trailing を書く）。回帰の道具として登録済み | `plan/tools.md` の h264sw の行 |

## 4. 決定（根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案 |
| --- | --- | --- | --- |
| D1 | 解析の header は WS330 の proposed の `h265sw-nal`・`-scaling`・`-rps`・`-vui`・`-ps`・`-slice`・`-poc` を採り、Vulkan の道が要る物を足す（§5.2 の差の表）。名前も `h265sw-` のまま | Q330-2 の A（解析は 1 つ）と WS330 design §11 の段取りそのもの。WS330 の Sonnet の Phase（p004・p006〜p011）は「main にある実装を確かめるだけ」になる | WS318 D19 の「Vulkan 用に別の `h265*.c`」（同じ節を 2 回実装し 2 回照合する。Q330-2 で取り消された） |
| D2 | Vulkan の道だけの物は `h265-dpb`・`h265-stream`・`h265-std` の 3 つ（接頭辞 `h265_`）。`h265-dpb` と `h265-stream` は Vulkan の型に依らず、Vulkan の header に依るのは `h265-std` だけ | H.264 の `h264.c`・`h264-dpb.c` と同じ分け方。WS330 の `h265sw-dpb` は frame の buffer（`vcodec-dpb`・`vcodec-frame`）の上で、それは WS330 p002（WS329 completed の後）まで無い。Vulkan の DPB は画を持たず slot の番号だけ。Vulkan の型を 1 つの module に閉じると、他の 2 つは Vulkan の header 無しで host 試験できる | `h265sw-dpb` を先に作る（WS329・WS330 の共通化を前倒しすることになり範囲の外）、StdVideo の型を DPB に直に持つ（H.264 の形。試験に Vulkan の header が要り、slot の約束と Vulkan の写しが混ざる） |
| D3 | 置き場所は `userland/desktop/libmedia/`（WS318 D19 の場所）。`sources.mk` に足さない（libmedia の build・runtime に繋がない、Q324-1）。使い手（`vkvideo-probe`、p006i）は自分の Makefile で path を名指して compile する | WS318 p020 は `sources.mk` に足すだけで使える（移しが無い）。WS330 も同じ場所に置く予定 | 別の directory（`userland/lib/libhevc` など。WS318・WS330 が後で移し、Makefile と試験の path を直すことになる） |
| D4 | `vcodec*.h` は変えない。slice header の byte_alignment() は `h265sw-slice.c` の中で読む。負の値の shift と `&` は module の中で floor の算術にする（`h265sw.h` の約束） | `vcodec*.h` の変更は WS326 の回帰の照合（基点 77ed48ac4）を壊す。WS330 p002 が共通の補助を足した後に置き換えればよい | WS330 の proposed/common の `vcodec.h`・`vcodec-bitreader.h` を今入れる（WS330 p002 の前倒し、H.264 の回帰の基点の付け替えが要る） |
| D5 | `h265sw-scaling.h` から量子化の行列（`h265sw_scaling_factors`・`_derive`・`_matrix`）を、`h265sw-ps.h` から `h265sw_ps_scaling` を外す | どれも software の decoder だけが使い、`h265sw-scan`（WS330 p005）に依る。実装しない関数を header に置かない（骨格の ENOSYS が残る）。WS330 p003 が足す | 骨格のまま置く（使えない関数が source に残る） |
| D6 | VPS を読む（`h265sw_vps`、`h265sw_ps_read_vps`）。HRD は最初の 1 つだけ保つ | Vulkan の session parameters は VPS を持ち、Wave511 は VPS を作り直して bitstream に置く（WS322 §7）。StdVideoH265VideoParameterSet の HRD の pointer は 1 つ | WS330 D16 の「VPS は読まない」（Vulkan の道が成り立たない）。VPS の HRD を全部（最大 1024 個、7 KiB ずつ） |
| D7 | 短期の RPS は StdVideo に 2 つの形を両方書く（予測の syntax と、導いた set を explicit の形で）。`h265sw-rps` は導いた set（`h265sw_st_rps`）と signalled の syntax（`h265sw_st_rps_syntax`）を両方出す | Vulkan の本文は「syntax の意味で」とだけ言い、予測の set でどちらを読むかを決めていない。NVDEC は slice header を自分で読み、SPS の set の数と slice の値で足りる。Wave511 は SPS を作り直すので、explicit の形で書けば予測の連鎖を持たずに済み、予測の形で書くなら生の値が要る。両方あれば driver が選べる | 片方だけ（driver の書き方を縛る） |
| D8 | NumDeltaPocsOfRefRpsIdx は「slice header が自分の set を持ち（sps の flag 0）、それが予測の時の NumDeltaPocs[RefRpsIdx]」、他は 0 | この値の用途は hardware が slice header の予測の st_ref_pic_set を読むこと（j を 0..NumDeltaPocs[RefRpsIdx] で回す、7.3.7）。sps の flag が 1 なら slice に st_ref_pic_set は無く、RefRpsIdx も定まらない。Vulkan の本文の「flag が 1」の書き方はこの用途と合わない。使い手の driver は zedBSD の nvrtx と Wave511 だけで、約束は `h265-std.h` に書いた | 本文の字句どおり（sps の flag 1 の時に値を入れる: 入れる値が定まらない） |
| D9 | IsReference は常に 1 | 3.132 と 8.1.3 の step 4: decode した絵は全部「used for short-term reference」にされ、読まれるかは後の絵の RPS が決める。Vulkan は IsReference が 0 の絵の setup（slot への書き込み）をしない。SLNR の絵も上の sub-layer の絵から読まれ得る | SLNR を 0 に（上の sub-layer の参照が壊れる）、最上位の sub-layer の SLNR だけ 0（得は slot 1 つ、規則が増える） |
| D10 | 8.3.3（使えない参照の生成）を行わない。CRA・BLA に付く RASL の絵（NoRaslOutputFlag 1 の時）は `h265-stream` が捨てる。壊れた stream の Curr の欠けは `missing` として報告し、使い手が決める | 8.3.3.1 の NOTE: 生成は RASL の絵の制約を書くためだけで、その RASL の絵は出力されず、出力される絵のどれも読まない。Vulkan の道で灰の絵を作るには GPU の操作が要り、作り方は使い手の物 | 生成して slot を当てる（画の無い slot は Vulkan では使えず、代わりの画の準備が DPB の module に入り込む） |
| D11 | end of sequence（と end of bitstream）の NAL の後の絵に `after_eos` を立て、使い手は DPB を出力付きで空にしてから（`h265_dpb_flush`）その絵を始める | WS330 D11 と同じ。C.5.2.2 では EOS の後の CRA は NoOutputOfPriorPicsFlag 1 で前の絵を出さずに捨てるが、HM の参照の出力（適合の MD5）は EOS で全部を出す | 規定どおり捨てる（適合の出力と frame の数が合わない流れがある） |
| D12 | slice segment の header は NAL の先頭 64 KiB の RBSP の写しから読み、足りなければ先頭 1 MiB の写しで読み直す（malloc で 1 回取り、stream が保つ） | 大きな I の slice（数 MB）を絵ごとに全部写すのは無駄。header はほぼ数十 byte。7.4.7.1 の entry point の上限（tile の列 × PicHeightInCtbsY − 1、h265sw.h の範囲で 20 × 1 056 − 1）で、有効な header は 90 KiB 未満なので 1 MiB の写しで必ず読める | 全部写す（4K60 で毎秒数百 MB の複写）、header の長さを固定の上限で切る（正しい stream を誤って拒む） |
| D13 | DPB が出力した絵の slot は、使い手が読み終えて `h265_dpb_release` を呼ぶまで busy で、新しい絵の setup に使わない | COINCIDE（出力と DPB が同じ image）では、出力した絵の image を読む前に次の decode が上書きすると壊れる。H.264 の `h264-dpb` の `busy` と同じ考え | 使い手が自分で避ける（約束が呼び手に散る） |
| D14 | 同じ id の parameter set の再送は、構造体を 0 で埋めてから組んで byte で比べ、中身が同じなら generation を変えない | 7.4.2.4.2 は CVS の中の同じ中身の再送を許す。Vulkan の session parameters は同じ鍵の更新を許さないので、使い手は generation が変わった時だけ parameters を作り直す。WS330 の store の約束と同じ | 受け取るたびに作り直す（IDR ごとに parameters の object を作る） |
| D15 | StdVideo の profile は general_profile_idc が 1・2・3・4・9 ならその値、他は compatibility の flag の 1・2・3 の最小（無ければ ENOTSUP）。level は 30 倍の値を enum に写し、表に無い値は次の上の level（186 を越える値は 6.2、0 は 1.0） | Vulkan の enum に無い値は driver に渡せない。Main 互換の flag を持つ古い stream（profile_idc 0）を拾える。level は driver の能力の確かめに使うだけなので、表の外の値で decode を止めない | 値をそのまま cast（enum の外の値を driver に渡す）、表の外の level を ENOTSUP（その stream を全く decode できない） |
| D16 | 実装の Phase は Sonnet の 6 つ（§9）。各 1〜3 header | 手順 §4.2（1 Phase = 1 回の起動、header 1〜3、仕様の 1〜2 節）と WS326・WS330 の大きさ。Q1 の「3 本程度」より多いのは、ps（VPS・SPS・PPS・PTL）、DPB（C.5.2）、std（StdVideo の約 250 field）、stream（AU と活性と捨ての状態）がそれぞれ 1 回分の大きさだから（review 2 の 6） | 3 つに詰める（1 回の起動に 4 header・4 節以上、WS326 の教訓に反する） |
| D17 | 絵の終わりは、AU を始め得る NAL（AUD・VPS・SPS・PPS・prefix SEI など）を見た時でなく、その後の base layer の VCL の first_slice_segment_in_pic_flag が 1 の時に決め、cursor をその AU を始める最初の NAL へ戻す。EOS・EOB は今の AU に属し、次の絵に after_eos と NoRaslOutputFlag 1 | 7.4.2.4.4 は AU の始まりを次の絵の最初の VCL を基準に定め、parameter set と prefix SEI（decoding unit information など、D.3）は 1 つの絵の segment の間にも置ける（review 2 の 1・3） | その NAL を見た時に絵を切る（segment の間の SEI で絵が割れ、後ろの segment が壊れた絵になる） |

## 5. module

### 5.1 一覧と依存（一方向）

```
vcodec.h  vcodec-bitreader  vcodec-nal                 （今の共通、変えない）
   └ h265sw.h（上限と約束）
       ├ h265sw-nal（NAL の header、型、RBSP、AU の始まり）
       ├ h265sw-scaling（scaling_list_data、既定の表）
       ├ h265sw-vui（VUI、HRD）
       ├ h265sw-rps（st_ref_pic_set、導出と生の syntax）
       ├ h265sw-ps（PTL、VPS、SPS、PPS、store、組の確かめ）  ← scaling・vui・rps
       ├ h265sw-slice（slice segment header、modification、重み） ← nal・ps・rps
       └ h265sw-poc（POC、RPS の POC の 5 つの list）            ← slice
   h265-dpb（Vulkan の DPB: 8.3.2 の marking、C.5.2、slot）     ← poc（型）・ps（型）
   h265-stream（Annex B → picture、NoRaslOutputFlag、AU）       ← nal・ps・slice・poc
   h265-std（StdVideoH265 へ、<vulkan/vulkan.h>）               ← ps・dpb・stream（型）
```

| module | header（proposed） | 仕様 | 中身 |
| --- | --- | --- | --- |
| h265sw | [h265sw.h](proposed/h265/h265sw.h) | A.4、各節の上限 | 定数（大きさ・id・tile・slice segment・sub-layer・CPB）と算術の約束。.c 無し |
| h265sw-nal | [h265sw-nal.h](proposed/h265/h265sw-nal.h) | 7.3.1、7.4.2.2、Table 7-1、7.4.2.4.4 | header の読みと検べ、RBSP の全体の写しと先頭の写し、型の分類 |
| h265sw-scaling | [h265sw-scaling.h](proposed/h265/h265sw-scaling.h) | 7.3.4、7.4.5、Table 7-5・7-6 | 既定の list、scaling_list_data の読み（予測・DC・差分） |
| h265sw-vui | [h265sw-vui.h](proposed/h265/h265sw-vui.h) | E.2.1〜E.2.3、E.3.1〜E.3.3、Table E.1 | VUI の全 field と推定、HRD（共通部と sub-layer）、SAR の表 |
| h265sw-rps | [h265sw-rps.h](proposed/h265/h265sw-rps.h) | 7.3.7、7.4.8（7-59〜7-71） | 短期の set の読み、予測（7-61・7-62）、生の syntax と bit 数 |
| h265sw-ps | [h265sw-ps.h](proposed/h265/h265sw-ps.h) | 7.3.2.1〜7.3.2.3、7.3.3、7.4.3.1〜7.4.3.3、7.4.4 | PTL、VPS、SPS（VUI の誤りの規則）、PPS、store と generation、組の確かめ |
| h265sw-slice | [h265sw-slice.h](proposed/h265/h265sw-slice.h) | 7.3.6.1〜7.3.6.3、7.4.7.1〜7.4.7.3、7.3.2.12 | slice segment header の全 field、dependent の写し、byte_alignment |
| h265sw-poc | [h265sw-poc.h](proposed/h265/h265sw-poc.h) | 8.3.1、8.3.2 の 8-5 | POC、prevTid0Pic、RPS の POC の list |
| h265-dpb | [h265-dpb.h](proposed/h265/h265-dpb.h) | 8.3.2（8-6・8-7、step 1〜4）、C.5.2.1〜C.5.2.4、8.1.3 step 4 | 絵の entry、marking、bumping と出力、slot と busy、flush・clear・abort |
| h265-stream | [h265-stream.h](proposed/h265/h265-stream.h) | 7.4.2.4.4・7.4.2.4.5（D17）、8.1.3（NoRaslOutputFlag・HandleCraAsBlaFlag・PicOutputFlag）、7.4.3.2.1（SPS の活性） | NAL の走査、parameter set の保管、slice segment を picture に、捨てる絵の数え |
| h265-std | [h265-std.h](proposed/h265/h265-std.h) | Vulkan の H.265 の std（§2.2） | VPS・SPS・PPS・PTL の StdVideo、picture info、reference info、約束（D7〜D9、D15） |

### 5.2 WS330 の proposed からの差（D1、WS330 p003 が取り込む物）

| header | 足した・変えた物 | 理由 |
| --- | --- | --- |
| h265sw.h | 冒頭の説明を「解析は Vulkan の道と software の decoder が共有」に。`H265SW_MAX_CPB`。負の値の `&` の約束 | HRD の配列、POC の 8-5 |
| h265sw-nal.h | `struct h265sw_nal` に `complete`、`h265sw_nal_parse_prefix`、`H265SW_MAX_NAL_BYTES` の検べと、ENOBUFS を unescape の前に capacity と比べる約束を明記 | D12、review 2 の 19 |
| h265sw-scaling.h | 行列の derive・matrix・factors を外した（D5）。entry 0 は EINVAL（7.4.5「greater than 0」）、DC の推定（予測 0 → 16、他 → 参照の DC）を明記 | D5、仕様 |
| h265sw-vui.h | VUI の全 field（Vulkan の StdVideoH265SequenceParameterSetVui の全部）、`struct h265sw_hrd`・`h265sw_sub_layer_hrd`、`h265sw_vui_read_hrd`（VPS の共通部の引き継ぎ） | D6、Vulkan |
| h265sw-rps.h | `struct h265sw_st_rps_syntax`、`h265sw_rps_read` の末尾の引数 `syntax`（NULL 可） | D7、D8 |
| h265sw-ps.h | PTL の 2 flag と general の 44 bit の読み飛ばし、VPS の reserved の 16 bit、SPS の log2 の上限の数値（和の前に検べ）と PCM の下限 Min( MinCbLog2SizeY, 5 )、check_pair の scaling の規則、`h265sw_vps`・`h265sw_ps_read_vps`・`h265sw_ps_read_ptl`、SPS の vps_id・temporal_id_nesting・sub_layer_ordering_info_present・conformance_window と生の offset・scaling_list_data_present・st_syntax・extension の 2 flag、PPS の deblocking_control_present・extension の 2 flag、init_qp の範囲を bit depth に依らない形（-48..51、組の確かめで SPS と）、store の VPS と `generation`。`h265sw_ps_scaling` を外した（D5） | Vulkan、「形式で拒まない」の一貫 |
| h265sw-slice.h | high_precision_offsets_enabled_flag 1 の重みを ENOTSUP、delta_chroma_weight の範囲、reserved_flags・colour_plane_id・st_rps_sps・st_rps_idx・st_syntax・num_long_term_sps/pics・lt_idx_sps・deblocking_override、byte_alignment は module の中、entry point の上限の 3 通り、式の番号の直し（NumPicTotalCurr 7-57） | Vulkan、D4、仕様 |
| h265sw-poc.h | 変えず（負の POC の `&` の算術を明記） | 算術 |

### 5.3 公開の口の要点（使い手の 1 picture の流れ）

```
h265_stream_init / h265_stream_input(packet か file)
for each: h265_stream_next(&picture)                          → ENOENT で終わり
  generation が変わった → h265_std_vps/sps/pps で session parameters を作り直す
  picture.after_eos → h265_dpb_flush（出力を表示し release）
  h265_dpb_begin(sps, &picture.rps, picture.no_rasl_output, picture.no_output_of_prior_pics, &plan)
    plan.output を表示の順に読み、release
    plan.missing > 0 → 使い手が決める（probe は数えて飛ばす: h265_dpb_abort）
  h265_dpb_take_slot(&plan)（EBUSY → 出力を読み終えて release、再試行）
  h265_std_picture・h265_std_references・h265_std_setup → vkCmdBeginVideoCodingKHR（plan.reference_slot と setup）・vkCmdDecodeVideoKHR（Curr の slot、slice の offset）
  h265_dpb_end(&plan, poc, pic_output, index, out, &n)（失敗なら h265_dpb_abort）
end: h265_dpb_flush
```

## 6. 所有・寿命・並行性

- 大域の状態は無い（定数の表だけ）。stream・DPB・picture・std の構造体は使い手が確保し持つ。2 つの stream を 2 つの thread で lock 無しに動かせる。
- 大きさ（host の amd64 で `sizeof` を測った）: `h265sw_ps_store` 597 508 byte、`h265_stream` 664 136 byte（store・64 KiB の RBSP を含む）、`h265_picture` 11 016 byte、`h265sw_sps` 22 316 byte、`h265_std_sps` 14 272 byte。stack に置かない（使い手は calloc か static）。`h265sw_ps_read_vps`・`_sps` は作業の構造体（約 22 KiB）をその呼び出しの間だけ malloc で取る（大きな物を stack に置かない、失敗で store を変えない）。
- 借りる物: `h265_stream_input` の buffer は次の input まで借り、picture の offset はその中を指す。`h265_std_*` の出力は自分の中を指す（写さない、動かさない）。
- 確保: `h265-stream` の `slice_rbsp`（D12、`h265_stream_destroy` で返す）と、`h265sw_ps_read_vps`・`_sps` の呼び出しの間の作業の領域だけ。他の module は確保しない。

## 7. 失敗と回復

- 誤りの値は `vcodec.h` の約束（EINVAL = stream の誤り、ENOTSUP = 範囲の外の道具、ENOMEM、ENOBUFS = 呼び手の buffer、EBUSY = 先に出力を取る）。`h265-stream` は ENOENT（buffer の終わり）を足す。
- 信用できない入力: 全関数が範囲の外を読み書きせず、上限のある loop で必ず終わる（各 header の comment）。読みの誤りは bit reader の sticky の誤りで、値を index に使う前に確かめる。
- parameter set の読みの失敗は store を変えない（前の物が残る）。壊れた parameter set を名指す絵は壊れた絵として数えて飛ばす。
- 壊れた絵（slice header の誤り、segment の食い違い、SPS の活性の違反、PPS・SPS の欠け）は返さず数え、`reason` に短い理由。POC の状態は変えない（次の絵が正しく続く）。次の IRAP の絵で立ち直る（活性の SPS もそこで変えられる）。
- DPB: 壊れた stream で満ちたら Curr に無い最小の POC の絵を追い出す（`evicted`、WS330 `h265sw_dpb_before_picture` と同じ規則）。どれも Curr なら EINVAL で、その絵を飛ばす。出力の slot は release まで busy。
- seek: `h265_stream_reset` と `h265_dpb_clear`（出力無し）。次の IRAP から（CRA は BLA として、RASL は捨てる）。

## 8. 試験

### 8.1 道具（p006a の段 2 で作る。実装の Phase は変えない）

| 道具 | 中身 |
| --- | --- |
| `plan/ws324/tests/run-unit.sh NAME...｜all` | `plan/ws324/tests/unit/test-<name>.c` を、`userland/desktop/libmedia/` の `h265sw-*.c`・`h265-*.c`・`vcodec-bitreader.c`・`vcodec-nal.c`・`bits.c` と一緒に、host の cc で `-std=c89 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined` で build し流す（`-I. -Iplan/tools/h264sw/unit -Iplan/ws324/tests/unit`、Vulkan は host の `/usr/include`）。出力は `plan/tools/fresh-out.sh` の新しい directory。最後の行 `run-unit: PASS`／`FAIL`。各試験の最後の行 `test-<name>: PASS (<n> checks)` |
| `plan/ws324/tests/check-skeleton.sh`（`BASE=<sha>`） | 1. 11 個の header が単独で 2 回 include して cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で通る、2. 全 .c が両方で警告無しに compile できる、3. `h265sw-*` の file が `h265-*`・`vulkan` を名指さない（一方向）、`h265-dpb`・`h265-stream` が `vulkan` を名指さない、4. `BASE` からの header の不変、5. `vcodec*.h` が WS326 の基点 77ed48ac4 から不変（D4）。最後の行 `check-skeleton: PASS`／`FAIL` |
| `plan/ws324/tests/unit/h265write.h` | 試験の入力を作る書き手（parser の code を使わず、7.3 の syntax の表から書く）: 2 byte の NAL の header と emulation prevention（7.4.2 の規則）と start code、PTL・VPS・SPS（scaling list・st_ref_pic_set の両方の形・long-term・VUI・HRD・range extension の flag）・PPS（tile・deblocking・scaling・range extension）・slice segment header（全分岐、st_ref_pic_set の予測、long-term、重み、entry point、extension、byte_alignment）を値の構造体から書く。`plan/tools/h264sw/unit/bitwriter.h` の上に置き、自己試験 `test-h265write.c`（書いた物を手で並べた bit 列と比べる）を持つ |
| 再利用 | `plan/tools/h264sw/unit/check.h`（`CHECK`、最後の行）、`bitwriter.h` |

### 8.2 単体試験の方針（全 Phase）

- 期待の値は仕様の表・式と手の計算だけから作る。他の実装（HM・FFmpeg・libde265・x265・Mesa）の出力を使わない（手順 §5.3）。
- 正常・境界（各範囲の端と端 + 1）・誤りの入力・途中で切れた入力（全ての切り口で EINVAL か ENOENT、落ちない）・乱数の入力（seed を固定し 10 万通り以上、ASan・UBSan で落ちない）・往復（`h265write.h` で書いて読み、値が全部一致）。
- わざと壊す確かめ（手順 §5.3）: 試験が通った後に実装を git の外で数通り壊し、FAIL を確かめて戻す。

### 8.3 Phase ごとの試験の項目

各 phase.md の「試験の項目」に詳しく書いた。要点:

| Phase | 試験 |
| --- | --- |
| p006b | test-nal（header の各 field と禁止の組、TSA・IRAP の TemporalId、型の分類の全 64 値の表、unescape と先頭の写しの全ての切り口、`complete`）、test-scaling（Table 7-5・7-6 の全値を PDF から 2 つの方法で取った表と照合、予測 0・参照・DC の推定、nextCoef の巡回、entry 0 の EINVAL、範囲の端）、test-vui（E.2.1 の全分岐の往復、推定の値、Table E.1 の 16 値、HRD の共通部の有無と引き継ぎ・CpbCnt 32・7 sub-layer・sub_pic の分岐、範囲の外の EINVAL） |
| p006c | test-rps（7-61・7-62 の手の例 6 通り以上（deltaRps の正負、use_delta の 0、元の set の空・満ち）、上限 max_pictures の境、delta_idx の範囲、syntax の bit 数と mask）、test-ps（PTL の general の 88 bit と sub-layer の組の全 2^12 の present の組の bit 数、VPS の reserved の 16 bit、SPS の log2 の上限と wrap させる組、VPS の HRD 0・1・3 個と cprms_present の 0、SPS の往復（全分岐）、sub-layer の写し下げ、VUI の誤りの規則（profile 1・2・3 で落とす、4 で EINVAL）、ENOTSUP の分類、PPS の往復、check_pair の各条件、store の generation（同じ中身の再送で不変、違えば +1）、失敗で store が変わらない） |
| p006d | test-slice（全分岐の往復、dependent の写し、PPS・SPS の組ごとの bit 長（address の Ceil(Log2)、lt_idx_sps、list_entry、poc_lsb）、重みの 7-58 の負の重みの floor、範囲の端、byte_alignment の誤り、短い写しで EINVAL）、test-poc（8-1・8-2 の折り返しの 4 通り、IRAP の Msb 0、prevTid0Pic の除外（RASL・RADL・SLNR・TemporalId > 0）、負の POC での 8-5 の LT、int32 の外で EINVAL） |
| p006e | test-dpb（手で書いた POC と RPS の列で: IDR だけ、IPPP、階層の B（reorder 2〜4）、long-term の LSB と全 POC の照合、NoOutputOfPriorPicsFlag 0・1、latency の上限、size の満ちによる bumping、壊れた stream の evicted と EINVAL、slot の busy と release、take_slot の EBUSY と再試行、abort、flush の順、clear。各段の出力の順と marking を C.5.2 の手順から手で作った表と照合） |
| p006f | test-std（各 field の写しを手の値で、range extension の 9 flag、RPS の 2 つの形（予測の set を explicit に直した値を手で計算）、D8 の 3 通り、profile・level の写し（表の外の level の写し）と ENOTSUP、pointer が自分の中を指す、Curr が 8 を超えて ENOTSUP） |
| p006g | test-stream（`h265write.h` で組んだ stream: IDR の始まり、CRA の始まり（RASL を捨て RADL を返す）、途中から（IRAP の前を捨てる）、EOS・EOB の後（次の絵に after_eos と NoRaslOutputFlag）、reset の後の CRA、絵の segment の間の prefix SEI・PPS（絵が切れない）、parameter set の再送（generation）、SPS の活性の違反、多い slice segment と dependent、AU の境（AUD・SEI・suffix SEI）、64 KiB を超える slice header、reserved の型と他の層の読み飛ばし、全ての切り口で落ちない） |

### 8.4 適合の試験（p006h、Opus）

- 入力: §2.3 の HEVC_v1 の 147 本のうち、h265-stream が読む SPS で Main の範囲（8 bit 4:2:0）と分かった物（名前からの推定は in 136）。zip は展開せず `unzip -p` で読む（WS330 の形）。
- host の道具（p006h が作る `plan/ws324/tests/conformance/conform.c`）: file を `h265-stream`・`h265-dpb`（slot は 17 個、出力ごとに即 release）・`h265-std` に通し、(a) VPS・SPS・PPS・各 picture の最初の slice header の値を syntax element の名前で、(b) 出力の POC の列を書く。
- 判定（全部の in で）: 1. 壊れた絵 0・refused 0・missing 0（RAP_A・RAP_B・NoOutPrior などの意図した例外は zip の txt で確かめて記録）。2. **副の oracle 1**: host の `ffmpeg -bsf:v trace_headers` の parameter set と slice header の値が (a) と一致（比べる field の一覧は p006h が決める。ffmpeg は data を出す物差しで、code は読まない）。3. **副の oracle 2**: host の `ffmpeg -threads 1 -loglevel debug` の `Output frame with POC` の列が (b) と一致。4. StdVideo の値の自己の照合（h265-std の出力を h265write.h で SPS・PPS に書き戻し、h265sw-ps で読み直して一致: WS322 T1 の往復の形）。
- 受け入れ: in の全部で 1〜4。RPS・POC・BUMPING・NoOutPrior・RAP・CRA・BLA・long-term（LTRPSPS・RPS_E など）・VPS id・SLIST・TILES・WPP・DSLICE・PPS・SLPPLP・NUT・OPFLAG・EXT・HRD の各々が 1 本以上入る（名前から全部ある）。

### 8.5 QEMU・実機

- この Phase 群（p006b〜h）は host だけ。Vulkan の decode は p006i（`vkvideo-probe` の H.265、amd64 の package の build と、Venus・lavapipe の QEMU で使える物があれば）と p007（RTX 2070 の実機、Q1 の割当）、WS322 p006（VF2）。

## 9. Phase の分け方と受け入れ（案、Q1 の判断 Q-B）

型: O = Opus、S = Sonnet 5.5 high（phase-runner-low、手順の 1 回の起動）。依存は module の依存（§5.1）と、試験が他の module の実装を使うかで決めた。

| Phase | 中身 | 型 | 依存 | 受け入れ |
| --- | --- | --- | --- | --- |
| p006a | この設計（段 1）と、header・骨格・道具を source に置く（段 2） | O | p002 は要らない（host の Vulkan の header） | 段 1: design-reviewer の反映、Q1 の受け入れ。段 2: §9.1 |
| [p006b](phase006b/phase.md) | h265sw-nal・h265sw-scaling・h265sw-vui | S | p006a 段 2 | run-unit nal・scaling・vui・all PASS、check-skeleton（BASE）PASS |
| [p006c](phase006c/phase.md) | h265sw-rps・h265sw-ps | S | p006b | run-unit rps・ps・all PASS、check-skeleton PASS |
| [p006d](phase006d/phase.md) | h265sw-slice・h265sw-poc | S | p006c | run-unit slice・poc・all PASS、check-skeleton PASS |
| [p006e](phase006e/phase.md) | h265-dpb | S | p006a 段 2（型だけを使い、試験は手の値） | run-unit dpb・all PASS、check-skeleton PASS |
| [p006f](phase006f/phase.md) | h265-std | S | p006a 段 2（型だけを使い、試験は手の値） | run-unit std・all PASS、check-skeleton PASS |
| [p006g](phase006g/phase.md) | h265-stream | S | p006d | run-unit stream・all PASS、check-skeleton PASS |
| [p006h](phase006h/phase.md) | 適合の試験（HEVC_v1）と、見つかった食い違いの直し（header の comment の直しは記録して p015 へ） | O | p006e〜g、cache（Q-D） | §8.4 |
| p006i | `vkvideo-probe` の H.265（今の ws.md の案の p006e。WS324 design §14 の形） | O | p006h、p002 | WS324 design の受け入れ（amd64 の build の warning 0、host の解析の試験。Vulkan の decode は p007） |

- ws.md の案の表の名前との対応: 案の「p006b〜d（解析の実装 3 つ）」→ p006b〜g（6 つ）、案の「p006e（vkvideo-probe）」→ p006i。p007 の依存は p006e → p006i。
- 並べ方: 段 2 の後に p006b・p006e・p006f を並べ、p006b → p006c → p006d → p006g と進む。最短で 4 段の直列（b・c・d・g）。
- 見積り（LW、目安）: 段 2 4、p006b 3、p006c 4、p006d 3、p006e 3、p006f 3、p006g 3、p006h 6、p006i 6。

### 9.1 p006a 段 2（header を source に置く、Opus）の受け入れ

1. `plan/ws324/proposed/h265/` の 11 個の header を `userland/desktop/libmedia/` に**変えずに**写す（段 1 の review の後の版）。
2. .c の骨格 10 個（h265sw.h を除く各 module）: include と全関数の本体を未実装の印（int は `return ENOSYS;`、void は何もしない、ポインタを返す関数は無い）に。`sources.mk` には足さない（D3）。
3. §8.1 の道具（`run-unit.sh`・`check-skeleton.sh`・`unit/h265write.h` と自己試験）。
4. 確かめ: `check-skeleton.sh` PASS（1〜3・5）、`run-unit.sh h265write` PASS、`BASE=77ed48ac4 sh plan/tools/h264sw/check-skeleton.sh` と `sh plan/tools/h264sw/run-unit.sh all` が前と同じ（H.264 の非退行）、`git diff --check`。
5. commit の SHA を **基点 BASE** として ws.md・design・各 phase.md の「確かめの command」に書く（同じ commit か直後の plan の commit）。

## 10. 他の WS への影響（Q1 が記録する物）

- **WS330**: proposed の解析の 7 header は WS324 の source の物が正になる。WS330 p003 は source の header に、D5 で外した物（量子化の行列、`h265sw_ps_scaling`）と WS330 の残りを足す。WS330 p004（nal の部分）・p006（scaling の読みの部分）・p007〜p011 は「確かめだけ」（WS330 design §11 の予定どおり）。`h265sw_rps_read` の引数が 1 つ増えた（NULL を渡せばよい）。WS330 p002 の後、`h265sw-slice.c` の byte_alignment を共通の補助に置き換えてよい。適合の一覧と fetch.sh は Q-D。
- **WS318**: p016〜p019 は WS324 p006a〜h で済む（WS324 design R11）。p020 は `sources.mk` に足し、`h265-stream`・`h265-dpb`・`h265-std` を `vkvideo.c` の H.265 の道に使う。
- **WS322**: p006 の T1（H.265 の parameter set の往復）と Wave511 の parameter set の作り直しは `h265-std` の StdVideo を入力にする。D7（RPS の 2 つの形）・D8・D9 を driver の約束として読む。
- **WS324**: p007（NVDEC の H.265 の実機）は p006i の後。design.md §8 の初期の参照の list は `plan.st_before`・`st_after`・`lt_curr`（slot の番号）で作れる。

## 11. HAL・toolchain・出典

- HAL は関係しない（userland の library）。toolchain は変えない。
- 出典: header の comment の節・表・式の番号は H.265 (V11) (01/2026)（`h265sw.h` の定数の block に版を書いた。file の先頭には書かない）。表の値（Table 7-5・7-6・E.1）は実装の Phase が block ごとの comment に「Values referenced: ITU-T H.265 (01/2026) Table …」で書く。
- 他の実装（HM・FFmpeg・libde265・x265・openHEVC・Mesa・GStreamer）の code・comment・構造・名前を写さない。GPL・LGPL の source は読まない（この設計でも読んでいない）。host の ffmpeg は副の oracle として出力だけを使う（§8.4）。
- Vulkan の std の header（Apache-2.0）は include して使うだけ（p002 が tree に入れる）。

## 12. 危険

| # | 危険 | 対処 |
| --- | --- | --- |
| R1 | D8 の約束が他の Vulkan の実装の読み方と違い、zedBSD の driver の外（例えば Mesa の上の別の driver）で使う時に食い違う | 使い手は zedBSD の driver だけ。約束を `h265-std.h` に書いた。p007・WS322 p006 の実機で slice header に予測の set を持つ stream（RPS_A〜F、適合）を通して確かめる |
| R2 | D7 の explicit の形への直しで、予測の連鎖が長い set の差が 16 bit を超える | 有効な stream では起きない（8-4 の範囲）。超えたら EINVAL で、h265-std の試験に境の例 |
| R3 | ffmpeg の出力の順（副の oracle 2）が C.5.2 の順と違う場面（NoOutputOfPriorPics、EOS） | p006h で zip の txt と仕様で確かめ、違いの理由を記録する（例外の表）。ffmpeg を正としない |
| R4 | WS330 p003 が source の header に足す時に、WS324 の実装の Phase の基点が動く | WS324 の実装の Phase（p006b〜g）が終わってから WS330 p003 が足す（WS330 p003 は WS329 completed の後で、時期は先）。Q1 が順を見る |
| R5 | 古い encoder の VUI の誤り（WS330 D17）で、Main 以外の profile の SPS が EINVAL になる | WS330 の規則のまま。p006h の適合で数え、多ければ Q1 に |
| R6 | 1 Sonnet の起動に h265sw-ps（VPS・SPS・PPS・PTL）が大きすぎる | p006c は rps と ps の 2 header。止まったら ps を VPS・PTL と SPS・PPS に分ける直しの Phase を Q1 が作る |

## 13. 問い（Q1 の技術の判断。ユーザーの判断は無い）

- Q-A（推し: A）: 解析の header は WS330 の proposed を採って足す（D1）。A: 採る。B: WS318 D19 のとおり Vulkan 用に別の解析を作る。→ A は Q330-2 の A と WS330 design §11 に沿う。
- Q-B（推し: A）: Phase を §9 の 6 つの Sonnet（p006b〜g）・適合（p006h）・probe（p006i）にし、ws.md の案の p006b〜e の名前を置き換える。B: 3 つに詰める。
- Q-C（推し: A）: 段 2（header を source に置き、骨格と道具と基点）を P1 が p006a の 2 回目の起動で行う。B: 別の Phase の番号にする。
- Q-D（推し: A）: WS330 の `plan/tools/h265/conformance/`（list・fetch・sha256）を `plan/tools/h265/conformance/` に移して plan/tools.md に登録し、cache の `/home/awe/zedBSD-claude1/build/sources/h265-conformance/` を作る（Q1 の作業）。B: WS324 が WS330 の path をそのまま使う（WS330 の完了で消える恐れ）。
- Q-E（推し: A）: D8（NumDeltaPocsOfRefRpsIdx）の約束を採る。B: Vulkan の本文の字句どおり。

## 14. review

[design-review.md](design-review.md) の「review 2（p006a、H.265 の解析の設計）」。20 件（blocker 2・major 5・minor 13）を全部反映した。
