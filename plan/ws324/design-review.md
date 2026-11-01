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
