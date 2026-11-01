<!-- awesome-plan project=zedbsd record=ws324-design-review -->

# WS324 design の敵対的レビュー

Parent: [design.md](design.md)、Phase [ws324-p001](phase001/phase.md)。

## review 1（design-reviewer、2026-11-01）

- 対象: design.md 第 1 版（commit `adcd8821c7`）、proposed/gpu-op-video-encode.md、phase001/phase.md。
- reviewer の確かめ方: open-gpu-doc の 2 版の `nvenc_drv.h`・`nvdec_drv.h` を host で compile し pahole で全 field の bit 位置を比べた、570.144 の `gpu_engine_type.h` の GET、Vulkan の refpage 2 つ（`vkGetEncodedVideoSessionParametersKHR`・`VkQueueFamilyVideoPropertiesKHR`）の GET、worktree の code の読み。
- 件数: 31（blocker 1・major 14・minor 16）。全件を第 2 版に反映した（取り下げ 0）。
- 食い違い無しと確かめられた物: NVENC の class 0xC4B7、engine type 0x1b・0x25、NVDEC 0x13、C5B7 の method の番号、APPLICATION_ID、magic、H.264 の版のずれ（272 → 276、472 → 476、504 → 508）、NVDEC の HEVC の method と式、`nvdec_hevc_pic_s` の配置が 2 版で同じ、chid 48、gpu-op の番号、capset の bit 0・1、H.265 decode の sType が BETA の外、HAL を変えない主張。

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| 1 | blocker | §11 段 2 の「BETA の値に static const で KHR の名前」は C として成り立たない（23 個は既に同名の KHR の enumerator で再定義、static const は `case` に使えない、Khronos に無い宣言、`VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` の漏れ、noct の照合と dispatch の数） | **採る**。段 2 をユーザーの決定どおり `vulkan_core.h` の作り直し（1.4.309、`maintain-api.noct`）に変えた（§11、D9）。BETA の囲みの 65 個の値を 1.4.309 と照合（食い違い 0）。dispatch 192 → 195、T9（`case` の label の compile、ABI の照合）を p003 の受け入れに |
| 2 | major | Q324-2 の A はユーザーの決定（Q318-3 の A、`vulkan_core.h` の作り直し）と違う案なのに「沿う」と書いた | **採る**。A をユーザーの決定どおりの 2 段に直した（phase.md Q324-2、§0・§11） |
| 3 | major | feedback の query pool を今の `CREATE_QUERY_POOL` の chain で作るのは今の wire（chain の presence 0、全 executor が前提）と合わない。結果の語の数も | **採る**。新しい番号 `0x10012` `CREATE_VIDEO_QUERY_POOL`（feedback だけ）、libvulkan が結果の語の数を flag から数える、T7 に i915・bcm2711 の `CREATE_QUERY_POOL` の非退行（§10、proposed §1） |
| 4 | major | rate control の状態を記録の時に比べるのは誤り（実行の時に変わる） | **採る**。状態の更新と比べを実行の時に、食い違いは encode を ERROR に（§8.6、§13） |
| 5 | major | rate control の DEFAULT の扱いが無い（constantQp 0 で QP 0 になる） | **採る**。DEFAULT = CQP 26 と定義、quality level の preferred の値も合わせた（§7.4、§8.2、D14） |
| 6 | major | 版のずれは末尾だけでない（H.265 も 744 → 748・776 → 780、H.264 の rc の byte 185〜188 の並びも違う） | **採る**。§3.3 を書き直し、0 に固定する範囲を H.264 472〜511 と 185〜188、H.265 744〜783 にした。T5 に 2 版の全 field の差の一覧（§7.4、§7.5、§14.1） |
| 7 | major | history の式の 128 が古く、header の `H264_HIST_BLOCK_SIZE` は 224 | **採る**。224 byte/列で確保、`hist_buf_size` は確保の値、後ろの印の pattern（§3.3、§8.4、R13） |
| 8 | major | HEVC の作業の buffer の設計が足りない（binding の数の矛盾、filter の中の並び、最悪の tile 数、`VIDEO_BINDINGS` 3 と run 8 KiB の置き場、MBHIST と HEVC の method） | **採る**。§8.4 を profile ごとの binding の表と run の配置（16 KiB）に書き直し、最悪の tile 数（20×22）で計算、HEVC の push（0x580〜0x594、MBHIST 無し）、不明な並びを R4 に（§3.2、§6、§8.4、R4） |
| 9 | major | HEVC の picture の番号と DPB の slot の対応、参照の数の上限、初期の参照の list、`sw_hdr_skip_length` が無い | **採る**。slot = picidx（0〜15）、maxDpbSlots 16・参照 15、`RefDiffPicOrderCnts`・`longtermflag` の作り方、8.3.4 の初期の list、`sw_hdr_skip_length` 0 で p007 に確かめ（§8.2、§8.6、T1） |
| 10 | major | CTB の端・入力の粒度に対して行の揃え 16 が足りない（decode の CTB 64、encode の 32、16x16 の tile の chroma の形） | **採る**。NV12 の video の image の行を luma 64・chroma 32 に、encode の DPB は 32 に揃え、後ろの印の pattern の試験、chroma の tile の形は p009b・p012 で（§3.4、§8.5、R12）。H.264 の非退行を p007・p014 に |
| 11 | major | H.265 の temporal MVP と coloc の食い違い | **採る**。v1 は temporal MVP を使わず override で返す（§7.2、§7.5、§16） |
| 12 | major | timeout の後に engine が書く memory の寿命（今の code に R7 が無い、§9 の文の矛盾、WS250 の回復の時） | **採る**。隔離の約束（関わる object の hold を boot の間返さない）を NVDEC・NVENC 共通に、WS250 の回復の前に video を止める規則（§9、§13、D13） |
| 13 | major | p009 の関門で判定と費用が成り立たない恐れ（64×64 は小さすぎる、試行ごとに再起動、入力の配置、PSNR の値の食い違い、log の 79 桁、RC の通知の道） | **採る**。p009 を p009a（channel、RC の通知の記録）と p009b（関門）に分け、256×128、入力は CPU で書ける 16x16 の tile、版・設定は boot の引数、bitstream は VRAM に残し log は行ごとの offset・checksum・全体の SHA-256、PSNR 35 dB に統一、上限は p009a の後から（§3.3、§15、R1・R2） |
| 14 | major | 依存の誤り（p012 は p007 が要る、p008 の T7 は nvrtx の reader が無い） | **採る**。p012 の依存に p007・p008b、T7 を libvulkan の側（p004・p008）と nvrtx の reader（p005・p010）に分けた（§10、§15） |
| 15 | major | Q324-6 は ws.md の目標を狭める範囲の判断なのに Q1 としていた。WS318 §13.6 の項目も受け入れに無い | **採る**。Q324-6 をユーザーへ、推しを「残す」（p016）に変え、WS318 §13.6 の項目を p016 に（§14.3、§15、§18、phase.md） |
| 16 | minor | capset の門が `video_h264` の 1 つだけで encode の flag が漏れる | **採る**。門を bit ごとに（§10、proposed §2） |
| 17 | minor | 番号の並び（reply 付きを記録の後に置くと範囲の振り分けの前提が崩れる） | **採る**。reply 付き 0x10010〜、記録 0x10020〜 に分け、範囲の約束を comment に（§10、proposed §1） |
| 18 | minor | `GET_ENCODED_VIDEO_SESSION_PARAMETERS` の「0 なら大きさだけ」は pData NULL と小さい buffer を区別できない | **採る**。pData の有無の語を別に（§7.6、proposed §1） |
| 19 | minor | flag を 1 語に詰める規則は H.265 の HRD の 8 bit の field に合わない | **採る**。幅のある field は 1 語ずつ（§10） |
| 20 | minor | 値が不正な時の error は `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` | **採る**（§13） |
| 21 | minor | encode の capabilities の表の不足（SEPARATE_REFERENCE_IMAGES、QP の範囲、quality level の properties、requiresGopRemainingFrames、constantQp は slice ごと） | **採る**（§8.2、§8.5） |
| 22 | minor | R2 の一覧に `codec`・`nal_unit_type`・`sao_*_mode`・`ceil_log2_pic_size_in_ctbs` が無い | **採る**（§3.3） |
| 23 | minor | Turing はまず 2021 版の配置で試す | **採る**（§3.3、R1） |
| 24 | minor | NVDEC の HEVC の誤りの判定に `error_status`・`slice_header_error_code`、単位は CTB | **採る**（§13） |
| 25 | minor | Q324-1 の A と標準廉価実装手順の食い違い（設計の Phase を別に、1 実装 = 1 起動、置き場と cache） | **採る**。p006 を p006a（設計）・p006b〜d（S、1 本ずつ）・p006e（probe）に分け、cache を WS318 と同じ `build/sources/h265-conformance/`、所有の記録を Q1 に（§5、§15、§20、phase.md） |
| 26 | minor | Q324-3 の範囲に WS318 §10.3（WS319 を想定）の記録の直しが無い | **採る**（§10、§18、phase.md） |
| 27 | minor | Q324-5 の前提: refpage に禁止は無い | **採る**。答えを書き、R6 を低い確率の退路に（§8.1、R6、phase.md） |
| 28 | minor | encode の出力の bitstream の形（start code、`e4byteStartCode`）が書かれていない | **採る（一部）**。p008a の最初に仕様と照らして決めると明記（§7.3）。仕様の照合はこの Phase では未実施 |
| 29 | minor | driver の中の probe の comment と guardrail「開発の管理情報」 | **採る**。「driver の自己診断」として振る舞いだけを書くと明記（§5） |
| 30 | minor | p004（8 LW）・p008（16 LW）の見積りが少ない | **採る**。p004 を 14、p008 を p008a 14・p008b 10 に（§15） |
| 31 | minor | ws.md の「ユーザーの決定（原文）」の節と phase.md の鉤括弧の引用 | **一部採る**。phase.md の鉤括弧を「という趣旨」の形に直した。ws.md の節は Q1 が書いた節なので P1 は変えず、Q1 に直しを頼む（phase.md の「記録の注意」） |

- 未実施: 反映の後の第 2 版への 2 回目の review（Q1 が要ると判断すれば）。Vulkan の仕様の encode の bitstream の形の照合（p008a）。

## review 2（p006a、H.265 の解析の設計）（design-reviewer、2026-11-02）

- 対象: [h265-parse-design.md](h265-parse-design.md)、[proposed/h265/](proposed/h265/) の header 11 個、phase006a〜g の phase.md（commit `b3735c259a`）。
- reviewer の確かめ方: H.265 V11 の PDF（PDF 40〜160・270〜360・440〜475）を `pdftotext -layout` で、WS330 の proposed との差、`vcodec*.h`・`vcodec-nal.c`・`bits.c`（`media_rbsp_unescape`）、`vkvideo-probe/main.c`、host の Vulkan の std の header、docs.vulkan.org の refpage 1 つ。
- 件数: 20（blocker 2・major 5・minor 13）。全件を反映した（取り下げ 0）。P1 が PDF で 7.3.3（general の 88 bit）、7.4.3.2.1（PCM の下限）、7.4.3.3.1（init_qp・scaling の規則）を確かめ直した。
- 食い違い無しと確かめられた物: Table 7-1 と AU の始まりの型の一覧、先頭の写しの unescape の規則、scaling list（7-42・7-43、範囲、DC の推定、> 0）、VUI・HRD の syntax と推定、VPS の HRD の範囲と重複、RPS の式の番号と範囲と 16 bit の mask、slice header の順と条件と式の番号、8.3.1・8-5、8.3.2 の step 1〜4、C.5.2.2〜C.5.2.4、evict の規則、setup の slot と busy、D8 の記述、D9、Curr の 8 の上限、StdVideo の tile・DecPicBufMgr・HRD の mask の大きさ、module の依存の向き、`vcodec*.h` の不変。

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| 1 | blocker | `h265_stream_next` が AU を始め得る NAL（prefix SEI・PPS）を見た所で絵を切る。7.4.2.4.4 は次の絵の最初の VCL を基準に AU の始まりを定め、parameter set と prefix SEI（D.3 の decoding unit information など）は 1 つの絵の segment の間にも置ける | **採る**。au_start を覚え、次の base layer の VCL の first_slice_segment_in_pic_flag（最初の payload の byte の最上位の bit）で絵の終わりを決め、1 なら cursor を au_start へ戻す。間の parameter set は読んだ時に保管。generation はその後の値（h265-stream.h、design D17）。試験に segment の間の prefix SEI・PPS（phase006g） |
| 2 | blocker | PTL の general は 4 つの source の flag の後に 43 bit と 1 bit（inbld か reserved）の 44 bit。header は 43 と書いた | **採る**。44 bit・general は 88 bit と明記（h265sw-ps.h、phase006c の注意） |
| 3 | major | EOS は今の AU に属するので、eos_pending の付け方では EOS の前の絵に after_eos と NoRaslOutputFlag 1 が付く | **採る**。eos_seen（集めている絵の AU の中の EOS・EOB）を足し、その絵を返した時に eos_pending へ。NoRaslOutputFlag は最初の segment を読んだ時の状態から（h265-stream.h）。試験（phase006g） |
| 4 | major | VPS の vps_reserved_0xffff_16bits が header に無い | **採る**（h265sw-ps.h、phase006c） |
| 5 | major | SPS の log2 の大きさの範囲が開いていて、ue(v) の和が wrap する。PCM の下限は Min( MinCbLog2SizeY, 5 ) | **採る**。各 syntax element を足す前に数値の上限で検べる形に書き直した（h265sw-ps.h）。試験に wrap させる組（phase006c） |
| 6 | major | p006f（h265-std と h265-stream）が 1 回の起動に大きすぎる。h265-std は型だけに依る | **採る**。p006f を h265-std（段 2 の後に並べて）、p006g を h265-stream（p006d の後）に分け、適合を p006h、probe を p006i に（design §9、D16）。p006c の分割は R6 の備えのまま |
| 7 | major | 試験に 1・3 を出す場面と、slots 16（nvrtx・Wave511 の maxDpbSlots）の DPB が無い | **採る**。phase006g に segment の間の SEI・PPS、EOS・EOB の付く絵。phase006e に slots 16 の busy・EBUSY・再試行と reference_count ≤ 15、setup_slot が reference_slot に無い不変条件。phase006h に HRD_A などの名指し |
| 8 | minor | h265-std の「拡張の field は 0」が SPS の range extension の flag と食い違う | **採る**。sps・pps の range_extension_flag と 9 つの flag を写す。SCC などだけ 0（h265-std.h、phase006f） |
| 9 | minor | slice header の読み直しが「NAL の全体」で、1 MiB を越える NAL で壊れる。大きさの理由も誤り（7.4.7.1 で 90 KiB 未満） | **採る**。2 つ目の写しを `h265sw_nal_parse_prefix(limit = 1 MiB)` に、理由を 7.4.7.1 の上限に（h265-stream.h、D12）。試験を「64 KiB を越える header（tile と WPP、高い絵）」に |
| 10 | minor | end of bitstream が扱われていない | **採る**。EOS と同じに（h265-stream.h） |
| 11 | minor | 「*picture は成功の時だけ書く」と、作業の場所が無いことの矛盾 | **採る**。*picture を作業の場所とし、0 以外の時の中身は決めない（h265-stream.h） |
| 12 | minor | high_precision_offsets_enabled_flag 1 の重みの規則が無い。delta_chroma_weight の範囲が無い | **採る**。その重みは ENOTSUP、delta_chroma_weight −128..127 を明記（h265sw-slice.h） |
| 13 | minor | init_qp の範囲は −48..51（init_qp_minus26 の範囲を init_qp に当てていた） | **採る**（h265sw-ps.h、phase006c の試験） |
| 14 | minor | phase006a・ws.md が「review の反映済み」と書くが review 2 の表が無い | **採る**。この表を足し、状態の文を直した |
| 15 | minor | h265-stream.h の「see the design」は source の comment から plan/ への参照 | **採る**。理由そのもの（参照の decoder が EOS で全部を出す）に |
| 16 | minor | 表に無い general_level_idc で SPS・VPS の変換が止まる | **採る**。次の上の level（186 超は 6.2、0 は 1.0）に写す（h265-std.h、D15） |
| 17 | minor | 構造体の byte の比べは padding を見得る | **採る**。memset した領域に member の代入だけで組み、比べの前に構造体の代入をしない、最悪でも余分な generation（h265sw-ps.h） |
| 18 | minor | StdVideo の VUI の sar の出所が無い | **採る**。signalled の値と明記（h265-std.h） |
| 19 | minor | h265sw-nal の ENOBUFS が escape の数で変わる | **採る**。unescape の前に capacity と size − 2（と limit）を比べる（h265sw-nal.h） |
| 20 | minor | check_pair に「SPS の scaling 無効で PPS の scaling は 0」が無い。design §8.3 の 2^14 は 2^12 | **採る**（h265sw-ps.h、design §8.3） |

- 未実施: 反映の後の 3 回目の review（Q1 が要ると判断すれば）。header は段 2 の前にもう一度 cc・clang で単独の compile を通した（2026-11-02、P1）。
