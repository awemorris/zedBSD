<!-- awesome-plan project=zedbsd record=ws326-design -->

# WS326 libmedia の H.264 の software の decoder の設計（p001）

- 親: [ws.md](ws.md)。作成: ws326-p001（P1、2026-10-30）。正本は header（`userland/desktop/libmedia/h264sw*.h`）の comment。この文書は全体の地図・決定・試験の道。
- 範囲: progressive の frame、8 bit、4:2:0、Constrained Baseline・Baseline（slice group と data partitioning を除く）・Main・High。
  他の profile_idc でも中身がその範囲の SPS（Extended で constraint_set0 か 1 が 1、High 10・High 4:2:2・High 4:4:4 Predictive で 8 bit 4:2:0 と lossless 無し）は受ける（design-reviewer の指摘 22、A.2）。
  interlace（field・PAFF・MBAFF、frame_mbs_only_flag 0 の SPS）・4:0:0・4:2:2・4:4:4・9 bit 以上・lossless・slice group（FMO）・data partitioning・SP/SI は外（ENOTSUP、libmedia では次の backend へ）。SVC・MVC の stream は base layer（base view）だけを decode する。

## 1. 出典

- 仕様: **ITU-T H.264 (08/2021)**（本文は ISO/IEC 14496-10 と共通。ISO の版との対応は未照合）。header と source の節・表・式の番号はこの版の物。
- PDF: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない、著作物）。
  印刷の頁 N は PDF の頁 N + 22（例: 8.5.9 は印刷 183 頁 = PDF 205 頁）。Read の tool の `pages` で PDF を直に読める（1 回 20 頁まで）。`pdftotext -layout` の文字は表・行列の列が崩れる（式 8-315 の v の列の順など）ので、表の値は必ず PDF の頁の画で確かめる。
- 他の実装（FFmpeg・openh264・JM・x264 など）の code・comment・構造・名前を写さない。参照の値は仕様の表と、手で計算した小さな例だけ。
  conformance の zip の trace（JM が出した syntax の記録）は data として debug に読んでよい（code ではない）。
- 表の値は data として、block ごとに直前の複数行の comment で「Values referenced: ITU-T H.264 (08/2021) Table 9-5」の形で出典を書く（[guardrail](../guardrail.md)「MIT・GPL の出典のデータの引用」の書式を仕様書にも使う）。file の先頭には書かない。
- source の comment・log に WS・Phase・Queue の番号、plan/ への参照、試験の進め方を書かない（AGENTS.md §10）。[coding-style.md](../coding-style.md) の全文（ANSI C、宣言は関数の先頭、if ごとの comment など）。

## 2. 決定（p001、Opus）

| 決定 | 内容と理由 |
| --- | --- |
| 既存の h264.c と分ける | h264.c は SPS・PPS・slice header を Vulkan の StdVideo の構造体に読む（`vulkan/vulkan.h` が要る）。software の decoder は全 field を自分の単位で持ち、Vulkan の header 無しで host の単体試験をしたい。syntax の読みは小さく、共有の改造で Vulkan Video の経路を壊す危険の方が大きい。|
| 使い回す物 | `bits.h` の `media_rbsp_unescape`（emulation prevention の除去）。ただしこれは NAL の末尾の 0x000003（cabac_zero_word の後に付く 0x03、7.4.1 で正しい）を誤りとするので、`h264sw_nal_parse` が末尾の 0x03 を先に落とす（bits.c は変えない）。`media_bitstream_*`（container の packet を Annex B に）、`picture.h` の pool（出力の picture）。|
| bit reader は別 | `media_bits` は 1 bit ずつで遅く、more_rbsp_data・peek が無い。CAVLC の毎 macroblock の経路なので h264sw-bits を新しく持つ。|
| frame だけ | SPS の frame_mbs_only_flag 0 を open の時点で拒む（ENOTSUP）。interlace の可能性がある stream を最初から libavcodec に回し、direct・deblocking・neighbour を frame の式だけにする（MBAFF の表 6-4 などを持たない）。|
| 出力 | 内部の frame は 3 plane（border 無し、MC は座標を clamp）。出力の時に libmedia の `media_picture`（今の pool の配置）へ複写。複写の費用は 1080p で 3 MB/frame、速さの Phase で見直す。|
| thread | decoder は global の状態を持たず 1 thread で動く。名前の付いた thread（WS309 の方針）・slice や frame の並列は速さの Phase で設計する。|
| 誤りの方針 | bitstream は信用しない。範囲外の値は配列の添字に使う前に検査、loop は buffer か上限で必ず終わる。slice の中の誤りはその slice を止め、残りの macroblock は picture の終わりに concealment（RefPicList0[0] からの複写、無ければ前の出力、無ければ灰）。decoder は次の IDR を必ず受けられる。|
| 開始と seek | 最初と reset の後は IDR か最初の slice が I の picture まで捨てる（recovery point の SEI は読まない。container の key frame の印がその役）。非 IDR から始めた時は、その後の POC が開始の picture より小さい picture（open GOP の leading）を decode はするが出力しない（次の IDR か MMCO 5 まで）。参照の欠けは stand-in で埋め concealed を数える。|
| SPS の活性化 | SPS は IDR（と開始の picture）でだけ替わる。その他の slice が別の SPS（id か世代）を指す・first_mb が今の大きさ以上・picture の途中で PPS が替わる、は EINVAL で捨てる（配列を別の SPS の値で添字しない）。大きさか dpb_frames が替わる IDR でも、no_output_of_prior_pics_flag が 0 なら古い frame を出力する（C.4.4 NOTE 3）。|
| VUI | 読めない・範囲外の VUI は捨てて SPS は残す（実際の stream の VUI の誤りで再生を止めない）。表 A-1 に無い level_idc は DPB 16 枚。|
| 算術 | bitstream が何でも未定義の動作をしない: level と scale の積は int64、負の値の左 shift は乗算、仕様が範囲を述べる中間値はその場で clamp、狭い型へは明示の clamp か wrap（h264sw.h）。|
| AU の境 | prefix NAL（14）と拡張の slice（20、21）は AU の境にしない（7.4.1.2.3 NOTE 1、SVC・MVC の base の複数 slice を切らない）。backend は packet ごとに AU を閉じない（TS の packet は AU の途中で切れうる）、次の AU の最初の NAL で閉じる。|
| 出力の時刻 | container が decode の順の時刻しか持たない時（decode_order_times）、decoder が出力の時に、出力する frame と、queue と DPB の出力待ちの frame の中の最小の時刻を入れ替える（vkvideo.c と同じ考え、h264sw_decoder_output）。|
| DPB の詰まり | 全 buffer が出力済みの参照で埋まった時（壊れた stream）、FrameNumWrap 最小の short（無ければ LongTermFrameIdx 最小の long）を外して loop を必ず終える（h264sw_dpb_make_room）。|
| backend の順 | 2026-10-30 ユーザー「libmedia の backend の順番は、Vulkan Video --> 独自ソフトウェアdecoder --> libavcodecです。」→ decoder.c の表で `media_vkvideo_ops` の直後に `media_h264sw_ops`、その後に libavcodec（WS318 p002 で libmedia の中へ移る）。|
| 遅れた拒否の切り替え | Q1 の追加（2026-10-30）: decoder.c が track（private data の複写）・今の backend・受け入れた packet の数を持つ。open の拒否（DEVICE・PROFILE・BUSY）は次へ。send の拒否（ENOTSUP・ENODEV・EBUSY）は、受け入れ 0 なら閉じて次を開き同じ packet から、受け入れ後なら旧 backend を drain して閉じ、次を開いて key frame の packet まで捨て（数を counter と media_log に）、そこから送る。key frame が来ないまま stream が終わった時だけ誤り。詳細は `h264sw-backend.h` の comment。|

## 3. module

全部 `userland/desktop/libmedia/`。関数の接頭辞 `h264sw_`、定数 `H264SW_`。各 header の comment が仕様（入力・出力・前提・誤り・節）。
codec に依らない部分（p026 で移した、H.265 と WS329 の encoder も使う）は `vcodec.h`（共通の約束・誤りの値）、`vcodec-bitreader`（旧 h264sw-bits）、`vcodec-nal`（Annex B の分割と emulation prevention の除去）、`vcodec-cabac`（算術の engine・表 9-44・9-45・式 9-5）。下の表の h264sw-bits は vcodec-bitreader、h264sw-nal と h264sw-cabac は H.264 の部分（header・context）と読む（p026、[ws.md](ws.md) の「結果」）。

| module（.h/.c） | 内容（節） | 使う module |
| --- | --- | --- |
| h264sw.h | 上限（level 6.2 の MaxFS など）・誤りの値の約束・UNUSED_PARAMETER | — |
| h264sw-bits | u(n)・ue・se・te・more_rbsp_data・trailing（7.2、9.1） | — |
| h264sw-nal | Annex B の分割（B.1、B.2）・NAL header（7.3.1）・RBSP（7.4.1） | bits.h（libmedia） |
| h264sw-tables | zig-zag（表 8-13、8-14）・既定の scaling list（表 7-3、7-4）・QPC（表 8-15）・normAdjust（式 8-315、8-318） | — |
| h264sw-ps | SPS・VUI の一部・PPS・scaling list と fall-back（7.3.2.1.1、7.3.2.2、表 7-2）・store | bits、tables |
| h264sw-slice | slice header・list の modification・weight の表・marking（7.3.3）・新しい picture の判定（7.4.1.2.4） | bits、nal、ps |
| h264sw-frame | frame（3 plane と macroblock ごとの motion）・pool（参照の数） | — |
| h264sw-poc | POC の type 0・1・2（8.2.1） | ps、slice |
| h264sw-dpb | marking（8.2.5）・frame_num の gap（8.2.5.2）・C.4 の格納と bumping・出力の queue | frame、poc、ps、slice |
| h264sw-reflist | PicNum・初期の list・modification（8.2.4） | dpb、frame、slice |
| h264sw-mb | macroblock の状態（struct h264sw_mb）・mb_type の表（表 7-11〜7-18）・block の位置と neighbour（6.4） | frame |
| h264sw-context | picture・slice の context、macroblock の syntax（構造体だけ、.c 無し） | 上の多く |
| h264sw-cavlc | residual_block_cavlc（9.2、表 9-5〜9-10）・nC・cbp の対応（表 9-4） | bits、mb、context |
| h264sw-cabac-init | context の初期値 (m, n) の表（表 9-12〜9-24、ctxIdx 0〜459、4 組） | cabac（定数だけ） |
| h264sw-cabac | 算術復号の engine・context の初期化（9.3.1、9.3.3.2、表 9-44・9-45） | bits、cabac-init |
| h264sw-cabac-se | CABAC の各 syntax element の 2 値化と ctxIdxInc・residual_block_cabac（9.3.2、9.3.3.1、7.3.5.3.3） | cabac、mb、context |
| h264sw-macroblock | slice_data の loop・macroblock_layer・mb_pred・sub_mb_pred・residual（7.3.4、7.3.5） | cavlc、cabac-se、mb、recon |
| h264sw-transform | LevelScale（8.5.9）・逆 scan・4x4/8x8 の逆量子化と逆変換・DC（8.5.6〜8.5.14） | tables、ps |
| h264sw-intra | Intra4x4/8x8 の mode の導出・端の sample・4x4/8x8/16x16/chroma の予測（8.3） | mb、context |
| h264sw-mvpred | 隣の partition と MV の予測・P_Skip（6.4.11.7、8.4.1.1、8.4.1.3） | mb、context |
| h264sw-direct | B の direct の spatial と temporal、co-located（8.4.1.2） | reflist、mb、context、mvpred |
| h264sw-interp | luma の 6 tap・chroma の bilinear（8.4.2.2） | — |
| h264sw-weight | default・explicit・implicit の重み（8.4.2.3、8.4.3） | ps、reflist、slice |
| h264sw-recon | 1 macroblock の再構成: 動き・予測・残差の組み立て（8.3〜8.5） | intra、mvpred、direct、interp、weight、transform |
| h264sw-deblock | 境界の強さ・閾値・filter（8.7、表 8-16、8-17） | mb、context |
| h264sw-decoder | 全体の流れ: NAL → picture の開始・slice・終わり（concealment・deblocking・DPB）・出力 | 全部 |
| h264sw-backend | libmedia の `media_decoder_ops`（open・send・receive…）と出力の picture への複写 | decoder、picture.h、bitstream |

依存の順（実装の Phase の順）: bits・nal → tables・ps → slice → frame・poc → dpb → reflist → mb → cavlc → cabac-init → cabac → cabac-se → transform → intra → interp・weight → mvpred → direct → macroblock → recon → deblock → decoder → 適合 → backend → 速さ → 規約。

## 4. 所有と寿命

- `struct h264sw_decoder` が全てを持つ: parameter set の store（埋め込み）、RBSP の buffer、frame の pool、DPB と出力の queue（埋め込み）、picture と slice の context、macroblock の配列（sequence の大きさで確保）。
- frame は参照の数で生きる（今の picture・DPB の entry・出力の queue・最後の出力・呼び手）。pool は decoder の close で dead になり、最後の frame と一緒に消える。
- reference list の frame は DPB から借りる（slice の間、marking は picture の後なので消えない）。
- struct h264sw_mb の entry は `h264sw_mb_reset` で初期化する（全 0 ではない: intra4x4_mode 2、ref_idx -1）。picture の始めに全部を slice -1 で、macroblock を decode する時に slice の番号で。per-macroblock の slice context（decoded_4x4・direct.spatial_ready）は h264sw_slice_data が毎 macroblock 消す。
- struct h264sw_mb の field の書き手: **parser**（h264sw-macroblock）が slice・kind・cbp・transform_8x8・qp・chroma_pred_mode・direct・total_coeff・coded_flags・mvd・motion.intra・送られた ref_idx を、
  **recon** が intra4x4_mode・nonzero・motion.mv・motion.ref_id・direct の ref_idx を書く。CABAC と CAVLC の context は、同じ macroblock の中の先に読んだ block の値を読むので、parser は block ごとに即書く。
- picture の終わりに、macroblock の motion を frame へ写す（後の B の direct が読む）。

## 5. 単体試験の仕様（host）

- 置き場: `plan/tools/h264sw/unit/test-<name>.c`。流し方: `sh plan/tools/h264sw/run-unit.sh <name>...`（`all` で全部）。全部の h264sw-*.c（backend を除く）と bits.c を ASan・UBSan で link（未実装の module は ENOSYS の骨格）。最後の行 `test-<name>: PASS (...)`、全体 `run-unit: PASS`。
- 補助: `check.h`（CHECK と最後の行）、`bitwriter.h`（u・ue・se・trailing・NAL の escape。入力は仕様の syntax から自分で組む）。`test-bitwriter.c` が補助自体を表 9-2・9-3 で確かめる（PASS 済み）。
  CABAC の engine の Phase が `cabacwriter.h`（9.3.4 の informative の符号化 engine）を足し、後の CABAC の試験が共有する。
- 表の写し間違い: 表を test にもう一度写して比べると同じ読み違いが通るので、表には形の検査を足す（VLC の表は列ごとに prefix-free で Kraft の和が 1、表 9-44・9-45 の単調さと形、(m, n) は pdftotext からの独立の抜き出しと全組の照合）。
- 期待の値の作り方: (a) 仕様の表の値そのもの（例: 表 9-5 の符号語 → TotalCoeff・TrailingOnes）、(b) 手で計算した小さな例（式を紙で追った数値を test に書き、comment に式の番号）、(c) 仕様が informative に与える逆の処理（CABAC の符号化 9.3.4 を test に書いて往復）、(d) 性質（逆変換の DC だけの入力は平らな出力、など）。他の decoder の出力や source を期待の値にしない（全体の試験の副の oracle を除く、§7）。
- 誤りの試験: 各 module で、範囲外の値・途中で切れた入力・巨大な数で、落ちず（ASan・UBSan が黙る）決められた誤りを返すこと。
- 各 Phase の phase.md に module ごとの試験の項目を書いた。CABAC の macroblock 全体の符号化は test に書くには大きいので、CABAC の macroblock の syntax は stream の試験（ffmpeg で作る CABAC の stream）と適合の試験で確かめる。

## 6. 骨格の確かめ

- `sh plan/tools/h264sw/check-skeleton.sh`: 各 header の単独 compile（cc と clang、`-std=c89 -Wall -Wextra -Werror -Wpedantic`）と全 .c の compile。`BASE=<p001 の SHA>` で header が変わっていないこと（実装の Phase は header を変えない）。
- zedBSD amd64: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so`（worktree の中、warning 0）。

## 7. 全体の試験と適合の試験

- **小さな全体の試験**（decoder の Phase）: `plan/ws083/tests/streams/*.h264` と `plan/ws236/tests/*.h264`（既存の試料）と、host の ffmpeg（libx264）で作る短い stream（CAVLC と CABAC、I・P・B、8x8、weighted、複数 slice）。
  期待は host の ffmpeg（`ffmpeg -i x.h264 -f rawvideo -pix_fmt yuv420p`）の frame ごとの MD5（副の oracle。他の decoder の出力を正とするのはここだけで、主の判定は JVT の参照の YUV）。
- **適合の試験**（適合の Phase）: ITU-T H.264.1 の JVT conformance の bitstream。
  - 入手: `sh plan/tools/h264sw/conformance/fetch.sh [<dir>/<name>...]`。`https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/{AVCv1,FRExt}/<name>.zip` のパラメータの無い GET（master.md「統合の注意」の web の取得の条件）。
    zip でない答え（bot の確認など）は即止めて Q1 に報告、別の取り方で再試行しない。SHA-256 は `conformance/sha256.txt` に記録（初回に書き、以後照合）。cache は `$H264SW_CONFORMANCE_DIR`（既定 `build/tmp/ws326-conformance/zips`、全部で約 3.1 GB: AVCv1 135 本 1.7 GB、FRExt 69 本 1.4 GB）。
    p001 で 3 本（BA1_Sony_D・CABA1_SVA_B・BA_MW_D）を取得して確かめた（2026-10-30、PASS、hash を記録）。
  - zip の中: bitstream（`.264`・`.jsv`・`.26l`・`.jvt`・`.avc` など）、参照の復号の出力（`.yuv`・`_rec.yuv`・`_rec.qcif` など、I420 8 bit、出力の順、crop の後）、readme（Frame/Field Coding・Entropy・Slice groups などの記述）、trace。
  - 一覧: `conformance/list.txt`（204 本、名前からの推測の範囲 `out:interlace` など、119 本は `?`）。適合の Phase で、decoder 自身の SPS・PPS・slice header の読み（範囲の外は ENOTSUP の理由）で各 stream を `in` か `out:<理由>` に分け、readme と食い違えば調べて記録する。
  - 判定: 道具 `conform`（適合の Phase で作る host の program）が bitstream を NAL ごとに decoder に与え、出力の frame を crop して I420 で並べ、参照の YUV と frame ごとに比べる（大きさ・frame 数・各 frame の MD5）。
    受け入れ: `in` の全部が全 frame で一致。`in` の中に CAVLC の Baseline、CABAC の Main、High の 8x8 変換と scaling matrix、weighted prediction（explicit・implicit）、B の temporal・spatial direct、MMCO と long-term、複数 slice と ASO、PCM、frame cropping の各々が 1 本以上あること。
- 実機・QEMU: libmedia の backend の Phase で Video Player（WS318 の後は libmedia 経由）で再生の試験。Pi 5 などの速さは速さの Phase。

## 8. 速さ（見通しだけ）

正しさの後に別の Phase で設計する（Opus）: CAVLC・CABAC の表引き、MC の端の扱いの fast path、IDCT の SIMD（NEON・SSE2）、deblocking の分岐の削減、slice・行の並列（WS309 の名前の付いた thread）、出力の複写の削減（WS318 の picture の形）。目標: Pi 5 で 1080p30 の High（ws.md の見積もり 80〜120 LW の残り）。

## 9. design-reviewer（2026-10-30）

design-reviewer を 1 本（header・design.md・phase.md・既存の libmedia・仕様の PDF と pdftotext を読む敵対的レビュー）。指摘 23 件（blocker 2・major 10・minor 11）と反映:

| # | 指摘 | 反映 |
| --- | --- | --- |
| 1 | struct h264sw_mb の「全部 0」の初期化は intra4x4_mode 0・ref_idx 0 になり誤り。P_Skip の ref_idx・ref_id の書き手が無い | `h264sw_mb_reset` を mb.h に足し、slice_data・decoder・recon の comment をそれに。P_Skip は recon が ref_idx 0 と RefPicList0[0] の id |
| 2 | 別の大きさの SPS を指す非 IDR の slice が mbs の外を書く。picture->sps の持ち主が 2 通り | decoder.h の手順 4 に SPS の id・世代・first_mb・PPS の検査（EINVAL）。picture->sps は decoder の写し |
| 3 | decoded_4x4・spatial_ready を消す者が無い | h264sw_slice_data が毎 macroblock 消す |
| 4 | gap の frame の POC（type 1・2）と motion が無い | dpb_gaps が poc_compute で導く（type 0 は直前の値）。fill_grey が motion を intra に。新しい frame の motion も intra |
| 5 | bumping が終わらない場合（全部が出力済みの参照） | `h264sw_dpb_make_room` を足し、外す frame の規則を決めた |
| 6 | 算術の未定義の動作（積の溢れ、負の左 shift、int16 への格納、POC の int64） | h264sw.h に全体の規則、transform・poc・direct・macroblock（CAVLC の mvd の範囲）に個別に |
| 7 | decode_order_times の時刻の持ち方が書けない | decoder の field と h264sw_decoder_output の入れ替えの規則 |
| 8 | prefix NAL（14）で picture が切れる | 14・20・21 を AU の境にしない |
| 9 | VUI の誤りで再生が止まる、level 1b（9）と未知の level | VUI は捨てて SPS を残す、level 9、未知は 16 |
| 10 | backend の出力の道（frame の持ち方、EBUSY・ENODEV を返さない、pool の失敗） | backend.h を書き直した |
| 11 | p020〜p022 が自分の規則で終われない、deblock の依存に tables が無い | 該当の Phase に変えてよい file を明示、decoder の Phase は他の .c の修正可（Opus を推す）、deblock の依存に p003 |
| 12 | (m, n) の 1840 組を 1 回で入れるのは大きい、表の写しの試験は同じ誤りを通す、CABAC の macroblock の試験の符号化、recon の手計算 | (m, n) を h264sw-cabac-init（新しい Phase p010）に分け独立の抜き出しと照合、表の形の検査、cabacwriter.h の共有、CABAC の macroblock は stream の試験へ、recon は性質の試験 |
| 13 | changed（大きさの変化）の扱いの食い違い | 古い frame を出力（C.4.4 NOTE 3）に統一、開始の非 IDR の picture でも SPS を活性化 |
| 14 | MMCO 5 の POC を frame へ写す手順が無い | decoder.h の手順 6 |
| 15 | 色の決め方が vkvideo.c と違う、present の flag が無い | sps・frame に signal_present・colour_present、backend は vkvideo.c と同じ規則 |
| 16 | CABAC の terminate と error の定義 | cabac.h を 9-bit の model で書き直した |
| 17 | 重なる・重複の slice | 既に decode された address で EINVAL、slice の数の上限で EINVAL |
| 18 | frame.h の clamp の式の番号 | 8-239・8-240（と chroma 8-262〜8-269） |
| 19 | recon の nonzero の規則が曖昧 | mb.h の規則（8x8 は 4 bit 全部）に揃えた |
| 20 | h264sw.h の先頭の comment の仕様の版（guardrail の「file の上部」） | 版の記述を file の説明から外し、定数の block の comment（節の番号の読み方）に移した。Q1 に判断を返す |
| 21 | open GOP の leading の picture、TS の packet と AU | leading は出力しない規則、backend は end_access_unit を呼ばない |
| 22 | A.2 で Baseline・Main に適合する他の profile_idc | 受ける規則を ps.h に |
| 23 | 作れない試験の項目（幅 0）、cycle 0 の欠け | p003・p005 の項目を直した |

レビューが「確かめて誤り無し」とした所: 式の番号（frame.h 以外）、CABAC の ctxIdxOffset と ctxIdxInc の条件、I_PCM の QP と CABAC の bit の位置、CAVLC の nC と level_prefix、zig-zag の向き、8.5.9〜8.5.12 の式、direct の 2 方式、重み、P_Skip と MV の予測、intra 8x8 の n、list の初期化と modification、C.4.4 と C.4.5.2、deblocking の qP、上限。確かめていない所: 表の値（9-5、9-12〜9-24、9-44・9-45、8-15〜8-17）、2 値化の表 9-36〜9-38、intra の各 mode の式、filter の式、試験の script と補助、規約の全文（各実装の Phase と規約の Phase で）。
