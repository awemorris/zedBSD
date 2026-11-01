<!-- awesome-plan project=zedbsd record=ws324-p006h -->

# ws324-p006h H.265 の解析の適合の試験（HEVC_v1、host、Opus）

Status: in-progress（2026-11-02 P6 q1910-i01 が道具を作り、適合 147 本と派生 1 本が全部 PASS。cleared は Q1 が決める）。前: planned（2026-11-01 P1 が p006a で作成）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md) §2.3・§8.4・§12。型 O（Opus、phase-runner）。手順 §4.6 の「統合・適合」の Phase。

## やる事

1. 適合の bitstream を取る: `H265SW_CONFORMANCE_DIR=/home/awe/zedBSD-claude1/build/sources/h265-conformance sh plan/tools/h265/conformance/fetch.sh`（パラメータの無い GET、SHA-256 の記録と照合、zip でない答えで即止めて Q1 に）。
2. host の道具 `plan/ws324/tests/conformance/conform.c` と `run.sh` を作る（design §8.4: h265-stream・h265-dpb（17 slot、出力ごとに release）・h265-std に通し、parameter set と最初の slice header の値を syntax element の名前で、出力の POC の列を書く。zip は `unzip -p` で読む）。比べの script（`compare.py`、`/usr/bin/python3 -I`）: host の `ffmpeg -bsf:v trace_headers` の値と、`ffmpeg -threads 1 -loglevel debug` の `Output frame with POC` の列。各 command に時間の上限。
3. 比べる field の一覧を決めて記録する（VPS・SPS・PPS の全 syntax element のうち trace_headers が出す物、slice header の poc_lsb・RPS・long-term・pps id・型）。
4. 全 in の stream で design §8.4 の 1〜4 を確かめる。prefix SEI（decoding unit information など）を絵の segment の間に持つ stream（HRD_A_Fujitsu_3 など、名前と txt から選ぶ）を明示して確かめ、記録する。食い違いは仕様で原因を確かめ、module の .c の誤りなら直す（header は変えない。header の comment の誤りは記録して規約の見直しの Phase へ）。ffmpeg と仕様が違う場面（NoOutputOfPriorPics、EOS、RASL）は例外の表に理由を書く（ffmpeg を正としない）。
5. out の stream（10 bit など）が parse で落ちずに読め、範囲の判定に必要な値が正しいことも確かめる（decode はしない）。

## 受け入れ

- run.sh の最後の行 `conformance: PASS (<in の数> streams)`、例外の表、比べた field の一覧と件数。
- `sh plan/ws324/tests/run-unit.sh all` と `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` が PASS のまま。
- 直した .c は phase.md に file・要点・SHA。

## 結果

担当 P6（Opus 5.5、q1910-i01、worktree `p6-ws324h`、基点 main 4eae22020）、2026-11-02（host の `date` で 06:12〜07:10）。cleared は Q1 が決める。

### 作った物（`plan/ws324/tests/conformance/`）

| file | 中身 |
| --- | --- |
| `conform.c` | bitstream（file か `-` で標準入力）を `h265-stream`・`h265-dpb`（17 slot、出力ごとに即 release）・`h265-std` に通し、各 picture の `@PIC` 行（型・TemporalId・POC・PicOutputFlag・NoRaslOutputFlag・first・after_eos・NoOutputOfPriorPicsFlag・IRAP・IDR・id・generation・segment 数）、その picture が使う VPS・SPS・PPS（新しいか中身が変わった時）と最初の slice segment header を syntax element の名前で（7.3 の syntax が持つ所だけ、parse が保った値から）、導出の値（`=` の行）、DPB の出力の順（`@OUT`）、skip の数（`@SKIP`）を書く。StdVideo の自己の照合: dump した VPS・SPS・PPS ごとに h265-std で StdVideo にし、StdVideo の値だけから `h265write.h` で RBSP に書き戻し、h265sw-ps で読み直して byte で比べる（StdVideo が持たない値は名指して元から: VPS の base_layer 2 flag・max_layers・max_layer_id・layer set・extension、PTL の profile_space・compatibility・level_idc）。各 picture の `h265_std_picture`・`_references`・`_setup` の値を picture と plan と照合 |
| `compare.py` | 3 つの物差しと比べる（`/usr/bin/python3 -I`）: 1. bitstream そのものの NAL header の走査と、この script 自身の 8.1.3（NoRaslOutputFlag、捨てる絵）・8.3.1（POC）・7.4.5（scaling list）・7.4.7.1（long-term）・7.4.7.3（重み）・7.4.8（短期 set）の読み、2. `ffmpeg -bsf:v trace_headers`（VPS・SPS・PPS・slice segment header の値）、3. `ffmpeg -threads 1 -loglevel debug` の `Decoded frame with POC`（decode 順）と `Output frame with POC`（出力の順）。stream ごとに 1 行と、食い違いの詳細の file |
| `run.sh` | 道具の build（host cc、`-std=c89 -Wall -Wextra -Werror`、ASan・UBSan）、zip ごとに `unzip -p` で読み（展開しない）conform・ffmpeg 2 本・compare、各 command に `timeout`、最後に `sync`。出力 `build/tmp/h265-conformance`（fresh_out）。最後の行 `conformance: PASS (<in> streams, <out> out of range, <派生> derived)` |
| `reorder-ps.py` | ffmpeg が読めない時だけ ffmpeg の入力にする写し: 最初の絵の前の parameter set を VPS・SPS・PPS の順に（型ごとの順は保つので、同じ id の最後の物が勝つ）。ffmpeg 7.1 は PPS を読むのにその SPS を先に要るが、仕様の 7.3.2.3 は SPS の値を読まない（PPS が SPS の前にあってよい）。conform には元の bitstream を渡す |
| `interleave-du.py` | 派生の stream: picture の segment k（k ≥ 1）の前に decoding unit information SEI（D.2.21、payloadType 130）の prefix SEI、segment 1 の前に直前の PPS の写しを置く（7.4.2.4.4 が許す形。HEVC_v1 の 147 本には無い） |

### 確かめた command と最後の行

| command（worktree の root で） | 最後の行 |
| --- | --- |
| `H265SW_CONFORMANCE_DIR=/home/awe/zedBSD-claude1/build/sources/h265-conformance sh plan/tools/h265/conformance/fetch.sh` | `fetch: PASS (147 zips)`（cache の SHA-256 が全部記録と一致、取得なし） |
| `sh plan/ws324/tests/conformance/run.sh`（07:10 終了、約 25 分） | `conformance: PASS (136 streams, 11 out of range, 1 derived)` |
| `sh plan/ws324/tests/run-unit.sh all`（06:45） | `run-unit: PASS`（h265write 51・nal 816099・scaling 5499・vui 2432070・rps 1891521・ps 1447981・slice 3904937・poc 1788280・dpb 40617273・std 1680・stream 1331307 checks） |
| `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh`（06:46） | `check-skeleton: PASS`（zedBSD の clang の h265-std は tree に Vulkan の H.265 の header が無いので飛ばし、p002 の後） |

### stream ごとの結果（最後の実行 `build/tmp/h265-conformance.run.5pnp35/summary.txt`）

- 148 本全部 PASS: in 136 本、out 11 本（`*MAIN10*` の 10 bit と `TSUNEQBD_A_MAIN10_Technicolor_2` の luma 10・chroma 9。class は conform が読んだ SPS で決め、list.txt の名前からの推定 in 136・out 11 と一致）、派生 1 本（`HRD_A_Fujitsu_3+du`）。
- 合計: picture 9331（返した絵）、出力 9299、比べた値 310 739（syntax element の値と導出の値）、照合の数: fields 222 842（name の組）、derived 78 356、pictures 102 921（8.1.3 の flag・型・segment 数・skip）、poc 9323、ffmpeg の decode 順 148・出力の順 148、StdVideo の照合 50 573（失敗 0）。
- conform の数: 全 stream で broken_pictures 0・refused_sets 0・missing 0・evicted 0・DPB の誤り 0・StdVideo の失敗 0（受け入れの判定 1）。
- 捨てた絵（8.1.3、compare.py の独立の導出と一致）: rasl_skipped は RAP_A_docomo_6 7・RAP_B_Bossen_2 7・NoOutPrior_A_Qualcomm_1 7・NUT_A_ericsson_5 2。before_irap・reserved_type・other_layer は全部 0。
- NoOutputOfPriorPicsFlag 1 で出力せず空にした絵（C.5.2.2、ffmpeg の出力の順と一致）: BUMPING_A_ericsson_1 17・RAP_B_Bossen_2 3・NoOutPrior_A_Qualcomm_1 3・NoOutPrior_B_Qualcomm_1 3。
- 型の範囲: BLA_W_LP（BUMPING_A・NUT_A）・BLA_W_RADL・BLA_N_LP（NUT_A）、CRA 32 本、IDR、RASL・RADL・TSA・STSA。long-term の絵（slice の num_long_term_*）は LTRPSPS_A・RPS_D・RPS_E、slice header の中の短期 set は 83 本、予測の短期 set は 114 本。§8.4 の名の組（RPS・POC・BUMPING・NoOutPrior・RAP・CRA・BLA・LTRPSPS・RPS_E・VPSID・SLIST・TILES・WPP・DSLICE・PPS・SLPPLP・NUT・OPFLAG・EXT・HRD）は全部入る。

### 絵の segment の間の NAL unit（手順 4 の明示の確かめ）

- 147 本全部を NAL header で走査し、1 つの絵の 2 つの segment の間に non-VCL の NAL unit を持つ stream は **0 本**（HRD_A_Fujitsu_3 は 1 AU 4 DU だが、SEI（active parameter sets・buffering period・picture timing・recovery point）は全部最初の segment の前）。
- そこで派生の stream `HRD_A_Fujitsu_3+du`（`interleave-du.py`）を作り、96 絵の全部で segment の間に PPS 96 個・decoding unit information SEI 288 個を置いた。PASS（各絵の segment 数 4 が NAL の走査と一致、値・POC・出力の順が ffmpeg と一致）。ffmpeg の demuxer はこの PPS で packet を切るが、trace は stream の順で、decode の POC の列も一致。

### 比べた field の一覧

- VPS: vps_video_parameter_set_id、vps_base_layer_internal_flag、vps_base_layer_available_flag、vps_max_layers_minus1、vps_max_sub_layers_minus1、vps_temporal_id_nesting_flag、PTL の general（general_profile_space、general_tier_flag、general_profile_idc、general_profile_compatibility_flag[0..31]、general_progressive_source_flag、general_interlaced_source_flag、general_non_packed_constraint_flag、general_frame_only_constraint_flag、general_level_idc）、vps_sub_layer_ordering_info_present_flag、vps_max_dec_pic_buffering_minus1[i]・vps_max_num_reorder_pics[i]・vps_max_latency_increase_plus1[i]（signal された i）、vps_max_layer_id、vps_num_layer_sets_minus1、vps_timing_info_present_flag、vps_num_units_in_tick、vps_time_scale、vps_poc_proportional_to_timing_flag、vps_num_ticks_poc_diff_one_minus1、vps_num_hrd_parameters、最初の hrd_parameters()（前置の一致。parse が最初の 1 つだけ保つ）、vps_extension_flag。
- SPS: sps_video_parameter_set_id、sps_max_sub_layers_minus1、sps_temporal_id_nesting_flag、PTL の general（同上）、sps_seq_parameter_set_id、chroma_format_idc、separate_colour_plane_flag、pic_width_in_luma_samples、pic_height_in_luma_samples、conformance_window_flag と conf_win_*_offset 4 つ、bit_depth_luma_minus8、bit_depth_chroma_minus8、log2_max_pic_order_cnt_lsb_minus4、sps_sub_layer_ordering_info_present_flag、sps_max_dec_pic_buffering_minus1[i]・sps_max_num_reorder_pics[i]・sps_max_latency_increase_plus1[i]、log2 の 4 つ（min・diff の形）、max_transform_hierarchy_depth_inter・intra、scaling_list_enabled_flag、sps_scaling_list_data_present_flag、amp_enabled_flag、sample_adaptive_offset_enabled_flag、pcm_enabled_flag と PCM の 5 つ、num_short_term_ref_pic_sets、各 st_ref_pic_set() の全 element（inter_ref_pic_set_prediction_flag、delta_rps_sign、abs_delta_rps_minus1、used_by_curr_pic_flag[j]、use_delta_flag[j]、num_negative_pics、num_positive_pics、delta_poc_s0/s1_minus1[i]、used_by_curr_pic_s0/s1_flag[i]）、long_term_ref_pics_present_flag、num_long_term_ref_pics_sps、lt_ref_pic_poc_lsb_sps[i]、used_by_curr_pic_lt_sps_flag[i]、sps_temporal_mvp_enabled_flag、strong_intra_smoothing_enabled_flag、vui_parameters_present_flag、VUI の全 element（E.2.1）と HRD の全 element（E.2.2・E.2.3）、sps_extension_present_flag、sps_range_extension_flag、sps_multilayer_extension_flag、sps_3d_extension_flag、sps_scc_extension_flag、range extension の 9 flag。
- PPS: 7.3.2.3.1 の全 element（pps_pic_parameter_set_id から pps_extension_present_flag と 4 つの extension flag まで、tile の column_width_minus1[i]・row_height_minus1[i]、deblocking の 4 つ）。
- 各 picture の最初の slice segment header: first_slice_segment_in_pic_flag、no_output_of_prior_pics_flag、slice_pic_parameter_set_id、slice_reserved_flag[i]、slice_type、pic_output_flag、colour_plane_id、slice_pic_order_cnt_lsb、short_term_ref_pic_set_sps_flag、slice の st_ref_pic_set() の全 element（delta_idx_minus1 を含む）、short_term_ref_pic_set_idx、num_long_term_sps、num_long_term_pics、lt_idx_sps[i]、poc_lsb_lt[i]、used_by_curr_pic_lt_flag[i]、delta_poc_msb_present_flag[i]、delta_poc_msb_cycle_lt[i]（累積の DeltaPocMsbCycleLt から差に戻して）、slice_temporal_mvp_enabled_flag、slice_sao_luma_flag、slice_sao_chroma_flag、ref_pic_list_modification_flag_lX、list_entry_lX[i]、mvd_l1_zero_flag、cabac_init_flag、collocated_from_l0_flag、collocated_ref_idx、luma_log2_weight_denom、delta_chroma_log2_weight_denom、five_minus_max_num_merge_cand、slice_qp_delta、slice_cb_qp_offset、slice_cr_qp_offset、deblocking_filter_override_flag、slice_deblocking_filter_disabled_flag、slice_beta_offset_div2、slice_tc_offset_div2、slice_loop_filter_across_slices_enabled_flag、num_entry_point_offsets、offset_len_minus1。
- 導出の値（compare.py が trace の syntax から自分で導く）: SPS・PPS の ScalingList[sizeId][matrixId] と DC（既定の表 7-5・7-6 を含む）、SPS の各短期 set（7-61・7-62、explicit の累積）、slice の短期 set、NumBitsForSTRefPicSetInSlice（trace の bit の位置の差）、PocLsbLt・UsedByCurrPicLt・DeltaPocMsbCycleLt、NumRefIdxActive[X]（override か PPS の既定）、NumPicTotalCurr（7-57）、LumaWeightLX・luma_offset・ChromaWeightLX・ChromaOffsetLX（7-56）、slice_segment_data() の始まりの byte（RBSP の中、NAL の 2 byte の header の後）。
- 比べない物（conform が書かない: 名前の一覧は compare.py の `NOT_WRITTEN`）: NAL の header、trailing と alignment の bit、reserved の bit、sub-layer の PTL（parse が保たない）、VPS の layer_id_included・hrd_layer_set_idx・cprms_present、extension data、`*_extension_4bits`、scaling list の生の syntax（導出の値で比べる）、num_ref_idx_active_override_flag と num_ref_idx_lX_active_minus1（導出の値で）、重みの flag と生の delta（導出の値で）、entry_point_offset_minus1、slice header extension、2 つ目以降の segment の header（picture は最初の header だけを持つ）。trace にあって conform が書かない名前がこの一覧の外にあれば FAIL にする。

### 道具の確かめ（壊した入力で FAIL になるか）

- conform の出力を 20 通りに壊して compare.py に流した。最初は 18 通りが FAIL、見逃しが 1 通り（`sps drop field`: conform が field を書かない）、1 通りは壊しの script の誤り（segment 数でなく generation を変えていた）。trace にある名前を conform が書かない時も FAIL にする検べを足し、壊しの script を直して、20 通り全部 FAIL、壊さない入力は PASS（WP_A・SLIST_A・RPS_A の出力で、slice_qp_delta・重み・chroma offset・出力の順・POC・NoRaslOutputFlag・segment 数・skip・data offset・SPS の field の欠け・PPS・VPS の値・scaling list と DC・SPS と slice の短期 set・短期 set の syntax・NumPicTotalCurr・broken の数・StdVideo の失敗）。script は scratchpad の物で repo に入れていない。

### 例外の表（ffmpeg と仕様・道具の都合が違う所）

| stream | 何が | 理由と確かめ |
| --- | --- | --- |
| RPS_C_ericsson_5、VPSSPSPPS_A_MainConcept_1 | ffmpeg の入力だけ `reorder-ps.py` の写し | PPS が その SPS の前にある（VPSSPSPPS_A の txt「The order of the headers is arbitrary」）。仕様は許す（7.3.2.3 は SPS の値を読まない）が ffmpeg の trace_headers と decoder は SPS を先に要る。並べ替えは同じ id の最後の物を変えないので、最初の絵が見る値は同じ。conform は元の順で読み、全部の照合が PASS |
| TSUNEQBD_A_MAIN10_Technicolor_2（out） | ffmpeg の物差しが無い（structure・ffmpeg-decode・ffmpeg-output を waive、compare.py の `EXCEPTIONS`） | ffmpeg 7.1 は luma と chroma の bit depth が違う SPS（10 と 9）を読まない。範囲の判定に要る値を zip の txt と HM の `.trc` と手で照合: general_profile_idc 2（Main 10）、general_level_idc 153（5.1）、chroma_format_idc 1、1024x768、bit_depth_luma_minus8 2・chroma 1、log2_max_pic_order_cnt_lsb_minus4 4、transform_skip_enabled_flag 1、8 絵（decode 順の POC 0・4・2・1・3・6・5・7、.trc の最初の 2 絵の POC 0・4 と一致）。8.1.3 の flag と segment 数の照合は PASS |
| NoOutPrior_A・NoOutPrior_B・RAP_B・BUMPING_A | 例外でない（ffmpeg と一致） | C.5.2.2 で NoOutputOfPriorPicsFlag 1 の絵の前の絵を出力せず空にする。ffmpeg も HM も同じ。下の「直した事」の 2 を参照 |

ffmpeg を正としなかった場面: EOS の後の出力（下の 2）は仕様（C.5.2.2）と HM の動作で確かめ、ffmpeg とたまたま同じだった。ffmpeg と仕様が違って残った場面は無い。

### 直した事

1. module の .c の誤り: **0 件**（147 本と派生 1 本で値・POC・出力の順・StdVideo の往復に食い違いが無かった）。header も変えていない。
2. **Q1 が受け入れた直し（2026-11-02 Q1）**: `h265-stream.h` の after_eos の comment（「the caller then outputs and empties its DPB first, as the reference decoder's output does at an end of sequence」）と [h265-parse-design.md](../h265-parse-design.md) §5.3 の「picture.after_eos → h265_dpb_flush」は C.5.2.2 と食い違う。EOS の後の絵は NoRaslOutputFlag 1 の IRAP で、CRA なら NoOutputOfPriorPicsFlag は 1、IDR・BLA なら no_output_of_prior_pics_flag で、1 なら前の絵はまだ出力していない物も出力せずに空にする（HM も ffmpeg も同じ）。先に flush すると仕様が落とす絵を出力する: conform を最初その通りに書いて NoOutPrior_A（3 絵）と RAP_B（3 絵）が出力の順で FAIL、flush を外して PASS。`h265_dpb_begin` が既に正しく扱うので、flush は IDR の flag 0 では冗長、他では誤り。conform.c は after_eos で flush しない。header の comment と design §5.3 は規約の全文の見直しの Phase（p015）で直す。p006i（vkvideo-probe）と libmedia の使い手は after_eos で flush せず `h265_dpb_begin` に任せる。2026-11-02 の時点で `userland/`・`src/` に after_eos で `h265_dpb_flush` を呼ぶ所は無い（`h265-dpb.[ch]`・`h265-stream.[ch]` の定義の他に名前の出現 0）。
3. 道具の直し（compare.py）: ffmpeg の decode の log で、stream の情報の probe の decoder が後の decoder と同じ address に置かれる場合（CAINIT_G・CAINIT_H で POC 0 が 2 回に見えた）に `Stream mapping:` の行より後だけを読む形に、debug の行が別の行の末尾に prefix 無しで付く場合（WPP_B で POC 2 を見落とした）に行の中を探す形に直した。

### 2 回目の起動（2026-11-02、Q1 の main の実行で SAODBLK_B が 1 回だけ FAIL）

- Q1 が main（2a1c95404）で流した全体の実行で `SAODBLK_B_MainConcept_4` が `failed=derived:1`（detail は `picture 221 SH =DataOffset: conform 6, derived 5`）、その stream だけの再実行は PASS。
- 原因は道具（compare.py・run.sh）の側: conform の出力は Q1 の実行と P6 の実行で byte で同じで、valgrind（`--track-origins=yes`）の誤り 0、20 回の実行の出力の md5 が全部同じ（module・conform.c は決定的）。食い違ったのは ffmpeg の trace の file: ffmpeg の進み具合の行（`frame=  221 ...`、改行でなく carriage return で終わる）が stderr の trace の途中に入り、次の trace の行がその後ろに付いた。run.sh の sed は `[trace_headers @ ...]` の prefix を行の頭でしか消さず、compare.py の Python は carriage return でも行を分けるので、分かれた後半（`[trace_headers @ ...] 63 alignment_bit_equal_to_zero ...`）を他の log の行として捨てた。捨てたのがその header の最後の alignment bit だったので、trace から導いた slice data の始まりが 1 byte 短くなった。進み具合の行が出る時刻で決まるので、実行ごとに変わる。
- 直し: run.sh の ffmpeg 3 本に `-nostats`、sed は prefix を行のどこでも消す、compare.py は行を読む時に prefix を消し（carriage return で分かれた行も読める）、field に見えて読めない行があれば `structure` の FAIL にする（`frame=` の統計の行は飛ばす）。FAIL の stream の summary の行に最初の食い違い（`first=[...]`、両方の値）を足した。Q1 の実行の壊れた trace をそのまま新しい compare.py に通して PASS（行を取り戻す）、prefix を直した写しでも PASS。conform.c は main の中の `data`・`size` の初期化だけ（ASan 無しの build の maybe-uninitialized の警告）。

### 未実施・残り

- QEMU・実機は無し（host の Phase）。Vulkan の decode は p006i・p007。
- 2 つ目以降の slice segment header の値は比べていない（picture が最初の header だけを持つ。dependent の写しは test-slice・test-stream の範囲）。
- conformance の 147 本には segment の間の non-VCL NAL が無いので、その場面は派生の 1 本と test-stream だけ。
- header の comment の直し（上の 2）は p015。
