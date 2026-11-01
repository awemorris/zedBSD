<!-- awesome-plan project=zedbsd record=ws324-p006d -->

# ws324-p006d H.265: h265sw-slice・h265sw-poc の実装（標準廉価実装手順、Sonnet 5.5 high）

Status: 実装済み・試験 PASS（2026-11-02 Sonnet q1899-i01。cleared は Q1。元は 2026-11-01 P1 が p006a で作成）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D1・D4・D12・§5・§7・§8）。
- 基点 BASE: `1f16db0e6`（2026-11-02 p006a 段 2 の commit 1f16db0e619a9cd23c74f7f36a8729efdba4ecb3）。前提: p006b・p006c が main にある。

## 1. 読む物

- AGENTS.md の §4・§7・§9・§10。`plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」「開発の管理情報」。`docs/agent/spec-module-delegation.md` の §5・§7。
- 設計の §1・§4（D1・D4・D12）・§5・§6・§7・§8。
- 実装する header: `userland/desktop/libmedia/h265sw-slice.h`、`h265sw-poc.h`。使う物: `h265sw-nal.h`・`h265sw-ps.h`・`h265sw-rps.h`（実装済み）、`vcodec-bitreader.h`。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。
  - h265sw-slice: 7.3.2.12 byte_alignment（PDF 59）、7.3.6.1〜7.3.6.3（PDF 64〜67）、7.4.7.1〜7.4.7.3（7-52〜7-58、PDF 113〜120）、Table 7-7（slice_type）。
  - h265sw-poc: 8.3.1（8-1〜8-4、PDF 137〜138）、8.3.2 の 8-5（PDF 138〜139）、prevTid0Pic の定義（8.3.1）、Table 7-1 の SLNR（PDF 84〜85）。

## 2. 作る物

- `userland/desktop/libmedia/h265sw-slice.c`、`h265sw-poc.c`。
- `plan/ws324/tests/unit/test-slice.c`、`test-poc.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

期待の値は仕様と手の計算から。入力は `h265write.h`（slice segment header の書き手）と、試験の中で組んだ SPS・PPS（`h265sw_ps_read_*` で store に入れる）。

- test-slice:
  - 全分岐の往復: IDR・CRA・BLA・TRAIL・RASL・RADL の各型、first と first でない（address の Ceil(Log2(PicSizeInCtbsY)) bit を PicSizeInCtbsY 1・2・3・2^k・2^k + 1 で）、dependent（PPS の dependent_slices 0 では flag を読まない、1 で写し）、reserved の flag 0〜7 本、output_flag の有無、colour_plane_id（separate_colour_plane 1 の SPS）、short-term の SPS の set（idx の bit 長を num_st_rps 1・2・3・64 で）と header の set（予測あり・無し、st_syntax の bits と ref_num_delta_pocs）、long-term（num_lt_sps 0・1・2・32 で lt_idx_sps の bit 長、poc_lsb_lt、msb の cycle の累積）、temporal MVP、SAO の luma と chroma（ChromaArrayType 0 で chroma を読まない）、P と B の num_ref_idx の override、list の modification（NumPicTotalCurr 1 では読まない、2・3・5 の bit 長）、mvd_l1_zero、cabac_init、collocated（from_l0 の推定、ref_idx の読む条件）、重み（P・B、luma と chroma の flag、負の重みの 7-58 の floor）、max_merge_cand、qp と chroma の offset、deblocking の override、loop filter across slices の読む条件、entry point（WPP だけ・tile だけ・両方の上限の境、offset_len 1・32）、extension の長さ 0・256・257（EINVAL）、byte_alignment（1 の bit が 0 の誤り、0 でない詰め物の誤り）、data_offset。
  - 導いた値: slice_addr_rs、num_pic_total_curr、qp（26 + init_qp_minus26 + slice_qp_delta の境）。
  - 範囲の端の EINVAL: address の PicSizeInCtbsY、slice_type 3、IRAP の P・B、idx、lt_idx_sps、num_long_term の和、DeltaPocMsbCycleLt の上限（各値と和）、collocated_ref_idx、five_minus_max_num_merge_cand 5、SliceQpY 52 と −QpBdOffsetY − 1、cb・cr の offset ±13、beta・tc ±7、offset_len_minus1 32、pps の id 64。
  - dependent の segment で independent NULL の EINVAL。
  - 途中で切れた RBSP の全ての切り口で EINVAL（短い写しの場合を含む）、乱数の入力で落ちない。
- test-poc:
  - 8-1 の 3 つの枝を、MaxPicOrderCntLsb 16 と 65536 で境（差が Max/2 ちょうど・± 1）を含む手の例。IRAP の NoRaslOutputFlag 1 で Msb 0。IDR の POC 0。
  - finish: TemporalId 0 の TRAIL_R は記録、RASL・RADL・SLNR（TRAIL_N など）・TemporalId 1 は記録しない。
  - rps: IDR で全部空。short-term の used で Before・After・Foll への分け（8-5 の順）。long-term の msb の有無、負の PicOrderCntVal での `&` の floor の算術（例: POC −1、Max 16 で 15）、DeltaPocMsbCycleLt の大きな値。int32 を越える和で EINVAL。
- わざと壊す確かめ（例: 8-1 の >= と > の取り違え、dependent の写しの 1 field、list_entry の bit 長、7-58 の shift）。

## 4. 注意

- 試験の道具の口は設計の §8.1.1（p006a 段 2 の結果）。試験は `plan/ws324/tests/unit/test-<name>.c` に置き、include は `"check.h"`・`"h265write.h"`・`"userland/desktop/libmedia/<module>.h"`（include の道は tree の root）、`main` の最後は `check_finish("<name>")` の値を返す。大きな構造体（`h265sw_ps_store`・`h265_stream`・`h265_picture`・`h265w_sps` など）は static か calloc に置く。
- 骨格の関数は本体ごと置き換え、「Not implemented yet」の comment も実装の comment に書き換える。module の中の補助は static 関数（一行の前方宣言を付ける）。外へ見える名前は `h265sw_`・`h265_` で始まる物だけ（check-skeleton の nm の照合）。
- libmedia の .c は check-skeleton で `-std=c89 -Wpedantic` と zedBSD の clang（x86_64・aarch64・riscv64、tree の libc の header だけ）にも通す: `LL`・`ULL` の接尾辞（`-Wlong-long`）、`//` の comment、文の後の宣言、host にしか無い関数を使わない。64 bit の定数は `(uint64_t)1U << n` か乗算で作る。
- `run-unit.sh` の `H265_UNIT_DIR` は道具の自己確かめ用。使わない。
- byte_alignment は `vcodec-bitreader.h` に無い。この module の中で読む（`vcodec*.h` を変えない、design D4）。
- 負の値の右 shift（7-58 の `( 128 * ChromaWeight ) >> denom`）は floor の除算で。負の値の `&`（8-5）は `x − Floor( x / m ) * m` を int64_t で（`h265sw.h` の約束）。
- slice header の short-term の set は `h265sw_rps_read( bits, num_st_rps, num_st_rps, sps->st_rps, max, &st_rps, &st_syntax )`。SPS の set を使う時は `st_syntax` を 0 のままに。
- 7.4.7.1 の entry point の上限は tile と WPP の組で 3 通り（header の comment）。
- 重みの presence の条件の PicOrderCnt の比較は常に真として読む（header の約束）。

## 5. 変えてはいけない物

全ての header、他の module の .c、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh slice poc
sh plan/ws324/tests/run-unit.sh all
BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ。

## 結果

実装の担当（Sonnet 5.5、q1899-i01、worktree `s-ws324-p006d`）、2026-11-02（host の `date` で 03:24 JST）。cleared は Q1 が決める。

### 作った物

- `userland/desktop/libmedia/h265sw-slice.c`・`h265sw-poc.c`（骨格を実装で置き換え）。header・道具・他の module は変えていない（check-skeleton の BASE 照合 PASS）。
- `plan/ws324/tests/unit/test-slice.c`・`test-poc.c`。
- commit: 実装と最初の試験 `bc98663480`、壊す確かめで見逃した 2 件を塞ぐ試験の追加は次の commit（Q1 への報告の SHA）。

### 確かめた command と最後の行（2026-11-02 03:24）

| command | 最後の行 |
| --- | --- |
| `sh plan/ws324/tests/run-unit.sh slice poc` | `test-slice: PASS (3904937 checks)`、`test-poc: PASS (1788280 checks)`、`run-unit: PASS` |
| `sh plan/ws324/tests/run-unit.sh all` | h265write 51・nal 816099・poc 1788280・ps 1447981・rps 1891521・scaling 5499・slice 3904937・vui 2432070 checks、全部 PASS、`run-unit: PASS` |
| `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` | `check-skeleton: PASS`（h265-std は Vulkan の header が tree に無いため飛ばし、p006b・c と同じ） |
| `git diff --check` | 出力なし（終了 0） |

試験は ASan・UBSan 付き（`-std=c89 -Wall -Wextra -Werror`）。test-slice の実行は約 1 秒。

### 試験の中身（要点）

- test-slice: read_start（全 64 の nal type、flag の組、pps id 0〜70、巨大な ue と 32 個超の 0、切り口、NULL）、IDR の既定値、全 NAL type（IRAP の no_output の有無、IDR の poc 0）、slice type（IRAP は I のみ、3〜7 と巨大値は EINVAL）、address（PicSizeInCtbsY 1〜1025 の 24 通りの bit 長と範囲の端、2 次元の 35）、dependent（flag を読まない場合、全 field の写し、SliceAddrRs、連鎖、independent NULL・PPS 違いの EINVAL、nal type と temporal id の違い）、reserved flag 0〜7 本の全 pattern、pic_output、colour_plane_id（0〜3）と ChromaArrayType 0（sao chroma・重み）、SPS の短期 set（1・2・3・4・5・33・64 個の idx の bit 長と範囲）、header の短期 set（explicit の手の値と bits 18/19、予測 2 例を 7-61・7-62 で手計算、delta_idx_minus1 の境、DPB サイズの境と最上位 sub-layer）、long-term（SPS 候補 0・1・2・3・5・32 個の idx の bit 長、写し、7-52 の累積の手の例 3 通り、cycle と和の上限を log2 4・8・12・16 で、個数の和の上限、巨大な個数で wrap しない）、tmvp・SAO の全組、inter（既定・override・14/15 の境）、list modification（NumPicTotalCurr 1〜10 で読む条件・bit 長・範囲）、collocated（list の大きさ 1・2・4 の全組）、重み（手の値 5 例、7-58 の負の重みと clip、P・B の条件、高精度の ENOTSUP、範囲の端）、merge・QP（bit depth 8・10・12・16 で −QpBdOffsetY〜51 の境）、chroma offset の和の境、deblocking の全組と loop filter の読む条件、entry point（3 通りの上限の境、offset_len 1〜32 と 33、最大の画 439）、extension（0・1・256・257 ほか）、byte_alignment（誤りと padding 0〜7 の全長）、引数の誤り。
- 乱数の往復 4000 通り（乱数の SPS・PPS の組、各 field を `h265write.h` で書いて読み、期待の header を表の条件で別に組んで全 field を比べる。独立・dependent の両方、全ての byte の切り口が EINVAL、bit 反転で落ちず受理の時は不変式）と、乱数の byte 6 万通り（受理は 700 件超で不変式）。
- test-poc: init、8-1 の手の例（Max 16・65536、半分の境 ±1、負の Msb、全長 4〜16）、「最も近い値、同じ距離は上」という別の式との照合（Max 16・32 の全 lsb の組、乱数 10 万通り）、int32 の端、prevTid0Pic（全 nal type 0〜31 × TemporalId 0・1）、IDR・CRA・BLA・RASL・RADL・SLNR・higher sub-layer を並べた 34 枚の列（毎回「記録した場合と違う値」になる列）、8-5 の手の例（短期の Before・After・Foll の順、長期の msb 有無、cycle）、負の POC の `&`（−1・−17・−16・−65537）、cycle の大きな値と int32 の端、15 個の上限、別の式（remainder を算術で）との乱数 10 万通り。
- 入力は全部 `h265write.h`。期待の値は仕様と手の計算（コメントに計算を書いた）。他の実装の出力は使っていない。

### わざと壊す確かめ（git の外で、元の file は変えていない）

- 252 か所（slice 207、poc 45）。script は `scratchpad/p006d-mut/runner.py`・`mutants*.py`（Q1 が消した別名の directory の後に作り直した）。各回を file に書き、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB 超か 600 秒で kill、並列 4（ASan のため RLIMIT_AS は使わず RSS 監視）。
- 結果（最後の全件の再実施）: 243 が試験の失敗（`FAIL`）で落ちた。build の誤りは 0（未使用変数・関数の警告で build が通らなかった 25 か所は、`(void)` や `(x & 0)` を使う形に作り直して再実施し、全部試験の失敗で落ちた）。生き残り 9 は全部等価:
  - slice s040: 複写の後の `num_entry_points = 0` の削除。entry point を読む PPS では必ず後で代入され、読まない PPS では複写元も 0。
  - slice s111: `delta_pocs > 15` の検べの削除。`h265sw_rps_read` が和 15 以下を保つので届かない守り。
  - slice s187: 7-58 の floor（`quotient -= 1`）の削除。`128 * ChromaWeight` は常に `2^ChromaLog2WeightDenom`（最大 128）で割り切れるので、切り捨てと floor は同じ。
  - slice s189・s190: 7-58 の clip の閾値を 1 ずらす壊し。ずらした側の端の値は代入しても同じ値で、観測が変わらない。
  - slice s266: `offset_len_minus1` の上限 32 への緩め。`vcodec_bitreader_u` が 33 bit で誤りにするので結果が同じ。
  - poc p010・p011: 8-1 の `lsb < prev_lsb`・`lsb > prev_lsb` の削除。他方の差の条件が既に同じ向きを含む。
  - poc p035: `poc_push` の満杯の検べ `>=` を `>` へ。長さ 15 の list の 16 個目は呼び出し側の検べで届かない。
- 最初の実行で見逃した 2 件を塞いだ: (1) 複写の後の nal_type の戻し（独立と dependent で同じ type だった）→ dependent に別の nal type（TRAIL_N）を使った。(2) extension 長 257 の拒否（後ろの byte が alignment の誤りで偶然 EINVAL になっていた）→ 長さを緩めると header として読める byte を後ろに置く形にした。

### 自分で決めた点（header が黙っている所、または仕様と header の間）

- slice read_start: `out`・`bits`・`nal` NULL と decodable でない type（10〜15、22 以上）は EINVAL。先に `*out` を 0 で埋める（失敗の時は、type が違う場合は埋めずに返す）。
- read_rest の引数の検べ: NULL、`pps->id != out->pps_id`、`pps->sps_id != sps->id`（header の通り）に加え、手作りの SPS が配列の外になる場合（`pic_size_ctbs` 0、log2_max_poc_lsb が 4〜16 の外、max_sub_layers_minus1 7 以上、num_st_rps 65 以上、num_lt_sps 33 以上）と、P・B で `pps->num_ref_idx_default` が 1〜15 の外は EINVAL（parse が作る set では起きない守り）。
- dependent: independent の pps_id が自分の pps_id と違えば EINVAL（7.4.7.1 は全 segment が同じ PPS）。independent が dependent の header でも、その field が複写済みなので同じに動く。nal_type・temporal_id・first_in_pic・no_output_of_prior_pics・pps_id・address は自分の値、`slice_addr_rs` は independent の値、`num_entry_points`・`offset_len`・`data_offset` は 0 にして後で読む。
- `slice_addr_rs`: header の約束どおり independent の値（7.4.7.1 の CtbAddrTsToRs の式は tile 走査が要るので使わない）。非 first の segment の address は 1 以上（0 は最初の segment の物、7.4.7.1 の「同じ address の segment は無い」）。
- slice_type 以外の条件: `sps_max_dec_pic_buffering_minus1[TemporalId] == 0` なら I（7.4.7.1）は検べない（header の範囲に挙げていない）。P・B で NumPicTotalCurr が 0 は検べない（復号の欠けの問題）。
- 初期値: independent の header は `pic_output` 1・`collocated_from_l0` 1 を先に入れる（I slice も）。I slice の `max_merge_cand`・`num_ref_idx` は 0。P slice の `num_ref_idx[1]`・list 1 の entry・重みは 0。
- long-term: `num_long_term_pics` の上限は header の約束（`H265SW_MAX_RPS - 1 - NumDeltaPocs`）で、SPS の DPB サイズでは検べない。`lt_msb_cycle` は msb が無い entry でも 7-52 のとおり前の値を引き継ぐ（delta 0 を足す）。`delta_poc_msb_cycle_lt` の上限は uint64 の和で、各値と累積の両方が 2^(32 − log2_max_poc_lsb) 以下。
- 重み: 高精度（range_extension の bit 6）の ENOTSUP は、pred_weight_table を読む時だけで、読む前（denom より先）に返す。ChromaArrayType 0 では chroma の denom 差・flag・値を読まず、`chroma_log2_denom` は luma と同じ、chroma の weight・offset は 0。flag 0 の entry は weight が `1 << denom`（luma・chroma）、offset 0。
- 量子化: `qp` の下限は `bit_depth_luma` から（`QpBdOffsetY = 6 * (bit_depth_luma − 8)`）。slice の cb・cr offset は PPS の offset との和も −12〜12。
- deblocking: override が無い時の `deblocking_disabled`・beta・tc は PPS の値。override で disabled 1 の時の beta・tc も PPS の値（slice は持たない）。
- entry point: 上限は int64 で作る。`pps->tiles` の時の `tile_columns`・`tile_rows` を使い、tile 無しの時は使わない。offset の値は読んで捨てる。
- extension: 長さ 256 まで読み飛ばし、257 以上は EINVAL（読み飛ばし中に RBSP の終わりを越えれば EINVAL）。
- byte_alignment: 1 bit が 1、続く 0 bit が 0（最大 7）。`data_offset` は読み終えた位置 / 8。
- poc: `h265sw_poc_compute` は IDR（nal type）も `irap_no_rasl` 1 と同じく Msb 0 にする（header の poc_lsb は 0）。`h265sw_poc_finish` は `poc − lsb` が int32 の外なら端に丸める（compute が作った poc では起きない。手作りの poc だけ）。compute は Msb の int32 の検べを足さず、poc が int32 の外の時だけ EINVAL。`h265sw_poc_init`・`h265sw_poc_finish` は NULL を無視する。
- `h265sw_poc_rps`: IDR は header の中身を見ずに全部空で成功。長期の個数は `num_long_term_sps + num_long_term_pics`（uint64 の和）が 15 以下（`num_long_term` field は見ない）。短期の `num_negative`・`num_positive` はそれぞれ 15 以下で和も 15 以下、超えたら EINVAL。msb 無しの長期は `pocLt = lt_poc_lsb` で、int32 の外（手作りの lsb）は EINVAL。

### 未実施

- QEMU・実機（この Phase の範囲外）。適合の stream での確かめ（p006h）。規約の全文の見直し（p015）。coding-style の checklist で自分の diff は見直した（関数を条件に使わない、関数の先頭の宣言、段落の comment、forward 宣言、成功の return の comment）。試験の .c は host の C89 の試験で、`CHECK` の中の式や一部の添字の計算に条件演算子を使っている。
- 表の値（Table）を持つ module ではないので、値の照合の節は無い。

### header について（変えていない、規約の見直しの Phase へ）

- `h265sw-slice.h`: `h265sw_slice_read_rest` は header の `independent` の pps_id 違いと NULL を EINVAL にする（実装した）が、pps_id の違いは header に書いていない。`H265SW_MAX_RPS` を使う long-term の和の上限の説明は SPS の DPB サイズの検べを含まない事も書いていない。
- `h265sw-poc.h`: IDR の `irap_no_rasl` の扱い（実装は IDR を常に Msb 0）と、`h265sw_poc_finish` の丸めは書いていない。
