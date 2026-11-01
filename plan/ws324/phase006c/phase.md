<!-- awesome-plan project=zedbsd record=ws324-p006c -->

# ws324-p006c H.265: h265sw-rps・h265sw-ps の実装（標準廉価実装手順、Sonnet 5.5 high）

Status: in-progress（2026-11-02 Sonnet q1883-i01 が実装済み・試験 PASS。cleared は Q1 の受け入れ後）。前: planned（2026-11-01 P1 が p006a で作成。p006b の cleared の後に起動）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D1・D6・D7・D14・§5・§7・§8）。
- 基点 BASE: `1f16db0e6`（2026-11-02 p006a 段 2 の commit 1f16db0e619a9cd23c74f7f36a8729efdba4ecb3）。前提: p006b（nal・scaling・vui）が main にある。

## 1. 読む物

- AGENTS.md の §4・§7・§9・§10。`plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」「開発の管理情報」。`docs/agent/spec-module-delegation.md` の §5・§7。
- 設計の §1・§4（D1・D6・D7・D14）・§5・§6・§7・§8。
- 実装する header: `userland/desktop/libmedia/h265sw-rps.h`、`h265sw-ps.h`。使う物: `h265sw.h`・`h265sw-scaling.h`・`h265sw-vui.h`（p006b の実装）・`vcodec-bitreader.h`。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。
  - h265sw-rps: 7.3.7（PDF 68）、7.4.8（7-59〜7-71、PDF 120〜122）。
  - h265sw-ps: 7.3.2.1〜7.3.2.3.3（PDF 52〜58）、7.3.3（PDF 60〜62）、7.4.3.1（PDF 90〜94）、7.4.3.2（PDF 94〜101）、7.4.3.3（PDF 101〜106）、7.4.4（PDF 107〜110）、6.2 の SubWidthC・SubHeightC（Table 6-1、PDF 40〜41）、7-10〜7-19 の導出、A.3.2〜A.3.4（VUI の誤りの規則の profile、PDF 265〜267）、Table A.8（PDF 282〜283）。

## 2. 作る物

- `userland/desktop/libmedia/h265sw-rps.c`、`h265sw-ps.c`。
- `plan/ws324/tests/unit/test-rps.c`、`test-ps.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

期待の値は仕様と手の計算から。入力は `h265write.h`（st_ref_pic_set の両方の形、PTL、VPS、SPS、PPS の書き手）。

- test-rps:
  - explicit の set: num_negative・num_positive の 0〜max_pictures の境（和の上限を含む、+1 で EINVAL）、delta_poc の 0・2^15 − 1・2^15（EINVAL）、7-67〜7-71 の累積を手の計算と照合。
  - 予測の set（7-61・7-62）: 手で計算した例を 6 通り以上（deltaRps の負・正、use_delta_flag の 0 を含む、元の set が空・negative だけ・positive だけ・満ち、j = NumDeltaPocs の最後の組が新しい entry を作る場合と作らない場合）。結果の entry の数が max_pictures を超える時の EINVAL（配列の外に書かない）。
  - slice header の set（index == num_sets）: delta_idx_minus1 の 0・index − 1・index（EINVAL）。SPS の set（index < num_sets）では delta_idx_minus1 を読まない。index 0 では inter_ref_pic_set_prediction_flag を読まない。
  - syntax: 各形で全 field と mask、`bits` が書いた bit 数と一致。syntax NULL でも同じ set。
  - 途中で切れた入力の全ての切り口、乱数の入力。
- test-ps:
  - PTL: max_sub_layers_minus1 0〜6、sub_layer_profile_present・level_present の全組（6 sub-layer で 2^12 通り）で読んだ bit 数が手の計算（2 + 1 + 5 + 32 + 4 + 43 + 1 + 8 の general、reserved の 2 bit の数、各 sub-layer の 88 + 8）と一致。general の flag の写し。
  - VPS: reserved の 16 bit の後の PTL の位置、HRD 0・1・3 個（2 番目と 3 番目で cprms_present_flag 0 と 1）、保つのが最初の 1 つだけ、hrd_layer_set_idx の範囲と重複の EINVAL、layer_id_included の skip（num_layer_sets_minus1 1023・max_layer_id 63 の最大）、timing の 0 の EINVAL、sub-layer の写し下げ、extension data の skip。
  - SPS: 全分岐の往復（conformance window、sub-layer の ordering の有無、scaling の無効・既定・signalled、PCM、short-term set 0・1・64 個（予測を含む）、long-term 0・32、VUI、range extension の flag）。導いた値（crop、CTB の数、min CB の数）を手の計算と照合。SPS の log2 の各 syntax element の上限の境と、ue(v) の大きな値（2^31 など）を足して wrap させる組の EINVAL・ENOTSUP、PCM の下限 Min( MinCbLog2SizeY, 5 ) の境、範囲の端（width・height の MinCbSize の倍数でない・0・H265SW_MAX_SIDE + 1、MaxLumaPs を超える積、log2_max_poc_lsb 4・16・17、max_dec_pic_buffering 15・16、reorder > dec、CTB 8・128 の ENOTSUP、TB の上限、PCM の上限）の EINVAL・ENOTSUP。multilayer・3D・SCC の flag の ENOTSUP と `sps_refused`。VUI の誤り: profile_idc 1・2・3 で VUI を落とし SPS を保つ（vui_present 0、拡張の flag を読まない）、4 で EINVAL。
  - PPS: 全分岐の往復（tile の uniform と非 uniform、deblocking の control、scaling、extension）。init_qp の -48・51 で可・-49・52 で EINVAL（check_pair では 8 bit の SPS で 0 が下限、-1 で EINVAL）、tile の 20×22 で可・21 列で ENOTSUP、column_width の 1055・1056（EINVAL）、range extension の道具の各 ENOTSUP。
  - check_pair: 各条件の可と EINVAL の境（SPS の scaling_list_enabled 0 で PPS の scaling_list_present 1、init_qp の -QpBdOffsetY、diff_cu_qp_delta_depth、parallel merge、tile の数、非 uniform の和、sps の id の不一致）。
  - store: 同じ中身の再送で generation と sps_generation が変わらない、違う中身で +1、失敗した読みで store が前のまま（byte で比べる）、refused の立ち下げ。
  - 途中で切れた入力の全ての切り口（VPS・SPS・PPS）、乱数の入力で落ちない。
- わざと壊す確かめ（例: 7-61 の符号、sub-layer の写し下げ、PTL の reserved の bit 数、VUI の誤りの規則の profile）。

## 4. 注意

- 試験の道具の口は設計の §8.1.1（p006a 段 2 の結果）。試験は `plan/ws324/tests/unit/test-<name>.c` に置き、include は `"check.h"`・`"h265write.h"`・`"userland/desktop/libmedia/<module>.h"`（include の道は tree の root）、`main` の最後は `check_finish("<name>")` の値を返す。大きな構造体（`h265sw_ps_store`・`h265_stream`・`h265_picture`・`h265w_sps` など）は static か calloc に置く。
- 骨格の関数は本体ごと置き換え、「Not implemented yet」の comment も実装の comment に書き換える。module の中の補助は static 関数（一行の前方宣言を付ける）。外へ見える名前は `h265sw_`・`h265_` で始まる物だけ（check-skeleton の nm の照合）。
- libmedia の .c は check-skeleton で `-std=c89 -Wpedantic` と zedBSD の clang（x86_64・aarch64・riscv64、tree の libc の header だけ）にも通す: `LL`・`ULL` の接尾辞（`-Wlong-long`）、`//` の comment、文の後の宣言、host にしか無い関数を使わない。64 bit の定数は `(uint64_t)1U << n` か乗算で作る。
- `run-unit.sh` の `H265_UNIT_DIR` は道具の自己確かめ用。使わない。
- `h265sw_rps_read` は SPS の set i を読む時 `sets[0..i)` だけを使う。予測の元 RefRpsIdx は index − (delta_idx_minus1 + 1)。
- 7-61・7-62 は新しい set の entry を一つずつ足すので、足す前に数を max_pictures と比べる（header の約束）。
- SPS・VPS の読みは、0 で埋めた作業の構造体（malloc でその呼び出しの間だけ取る、header の約束）に組み、全部の検べが済んでから store へ写す（失敗で store を変えない、generation の byte の比べのため）。ENOMEM の道は、試験で malloc を失敗させる手段が無いので code の読みで確かめ、結果に書く。
- VPS の HRD の 2 番目以降は作業の 2 つの `h265sw_hrd` を交互に使う（header の約束）。
- PTL の general の 4 つの source の flag の後の 43 bit（constraint と reserved）と、その後の 1 bit（general_inbld_flag か general_reserved_zero_bit）の計 44 bit は値を保たずに読み飛ばす。general の部分は general_level_idc の前で 88 bit。
- VPS の vps_temporal_id_nesting_flag の後に vps_reserved_0xffff_16bits（u(16)、値は見ない）がある。
- SPS の log2 の大きさは、各 syntax element を足す前に上限と比べる（header の数値。ue(v) の和で wrap させない）。PCM の下限は Min( MinCbLog2SizeY, 5 )。
- SPS の lt_ref_pic_poc_lsb_sps は log2_max_poc_lsb bit の u(v)。

## 5. 変えてはいけない物

全ての header、他の module の .c（p006b の物を含む）、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh rps ps
sh plan/ws324/tests/run-unit.sh all
BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ（全 PASS を自分で読む、規約の checklist、commit、結果の節、ws.md の行）。

## 結果

実装の担当（Sonnet 5.5、q1883-i01、worktree `s-ws324-p006c`）、2026-11-02（host の `date` で 02:41 JST）。cleared は Q1 が決める。

### 作った物

- `userland/desktop/libmedia/h265sw-rps.c`・`h265sw-ps.c`（骨格を実装で置き換え）。header・道具・他の module は変えていない（check-skeleton の BASE 照合 PASS）。
- `plan/ws324/tests/unit/test-rps.c`・`test-ps.c`。
- commit: 実装と最初の試験 `e51f4e72c5`、試験の追加（壊す確かめで見逃した 2 件を塞いだ物）`30a3fa3b37`。

### 確かめた command と最後の行（2026-11-02 02:41）

| command | 最後の行 |
| --- | --- |
| `sh plan/ws324/tests/run-unit.sh rps ps` | `test-rps: PASS (1891521 checks)`、`test-ps: PASS (1447981 checks)`、`run-unit: PASS` |
| `sh plan/ws324/tests/run-unit.sh all` | h265write 51・nal 816099・ps 1447981・rps 1891521・scaling 5499・vui 2432070 checks、全部 PASS、`run-unit: PASS` |
| `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` | `check-skeleton: PASS`（h265-std は Vulkan の header が tree に無いため飛ばし、p006b と同じ） |
| `git diff --check` | 出力なし（終了 0） |

試験は ASan・UBSan 付き（`-std=c89 -Wall -Wextra -Werror`）。test-ps の実行は約 20 秒。

### 試験の中身（要点）

- test-rps: explicit の count の全組（0〜16 × 0〜16、max_pictures 0・1・2・5・14・15）、delta の 0・2^15 − 1・2^15・65535・0x7FFFFFFF、手で解いた explicit の例（cumulative と used mask、bits 26）、予測の手の例 6 通り以上（deltaRps の負・正・極値 ±2^15、use_delta 0、元が空・負だけ・正だけ・15 個、新しい entry を作る・作らない、結果の数が max_pictures と 1 違いで EINVAL、deltaRps −100 で 16 個になり EINVAL）、乱数 2 万通りを「残る entry を集めて並べ替える」第二の式と照合（SPS の set と slice header の set、delta_idx_minus1 の指す元）、delta_idx_minus1 の 0・index − 1・index（EINVAL）、SPS の set は delta_idx_minus1 を読まない（bits が一つ短い）、index 0 は予測の flag を読まない、syntax の全 field と mask と `bits`、syntax NULL、引数の誤り、caller が作った大きすぎる元の set（和が 16 以上）の EINVAL、全ての切り口、乱数 10 万通り。
- test-ps: PTL（max_sub_layers_minus1 0〜6 の全 flag の組 5461 通りで読んだ bit 数を 88 + 8 + 2 × max + 2 × (8 − max) + 88 × 部 + 8 × 部 の手の式と照合、general の field の写し）、VPS（id、reserved の 16 bit、ordering の写し下げ 7 × 2 通り、layer set の skip、HRD 0・1・3 個の cprms の全組を 1 と 3 sub-layer で、最初の 1 つだけ保つ、hrd_layer_set_idx の範囲・重複・base_layer_internal 0 の下限、timing 0 の EINVAL、extension data 0〜32 bit）、SPS（既定、sub-layer ordering、scaling 3 通り、手で解いた導出 8 例、サイズの境 23 例と window の境、log2 の全組と wrap させる ue(v)、PCM の全組、set の鎖 0・1・2・3・7・64 個を手で解いた A・B・C の鎖で、max_pictures の出所、long-term 0・32・33、extension の flag・data、VUI の誤りの規則を profile 0〜9 で、切れた VUI、sps_supported）、SPS の乱数 3000 通り、PPS（既定、各範囲の境、tile の全組（20 × 22 可・21 列 ENOTSUP・1 × 1 EINVAL）と 1055・1056、range extension の各道具、ENOTSUP の 3 つの extension、手書きの PPS）、PPS の乱数 3000 通り、check_pair の各条件の境、store（再送で generation が変わらない・違う中身で +1・id ごとの sps_generation・失敗で byte が前のまま・refused の立て下げ）、全ての切り口（乱数の VPS・SPS・PPS 各 40 組で全ての切り口が EINVAL かつ store が byte で前のまま）、壊した入力の乱数 6 万通り（成功なら不変式、失敗なら store が byte で前のまま）。
- 入力は全部 `h265write.h`。期待の値は仕様と手の計算（コメントに計算を書いた）。他の実装の出力は使っていない。

### わざと壊す確かめ（git の外で、戻して `cmp` と `git diff --quiet` で確認）

- 142 か所（rps 29、ps 113）。script は `build/tmp/p/mutate.py`・`mutate2.py`。各回を file に書き、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB 超か 600 秒で kill の上限を付けた（Q1 の指示。ASan のため RLIMIT_AS は使えず RSS 監視で代えた）。
- 7-61・7-62 の各条件（符号・等号）、deltaRps の符号、flag の index、max_pictures の比較（S0 と S1 の別）、delta_idx_minus1 の境と読む条件、delta の上限、予測の flag を読む条件、use_delta の既定、bits、cumulative の符号、PTL の compatibility の順・reserved・44 bit・88 bit・8 bit、VPS の 16 bit・写し下げ・HRD の previous・scratch の交互・layer set の bit 数と上限・hrd_layer_set_idx の範囲と重複・timing の 0、SPS の各 log2 の守りと上限・サイズの各比較・SubHeightC・window・ctb の切り上げ・PCM の各境・set 数・long-term の bit 数・VUI の誤りの規則の profile・extension の各 flag と skip・refused と generation の記録、PPS の各範囲・tile の数と大きさ・range extension の各 ENOTSUP・既定の写し、check_pair の各条件、sps_supported の各条件など。
- 結果: 140 か所が試験の失敗で落ちた（build の誤りでなく `FAIL`。build の誤りになった 9 か所は、未使用変数や紛らわしい字下げを避ける形に作り直して再実施し全部試験の失敗で落ちた）。生き残ったのは 2 か所で、どちらも観測できる振る舞いが変わらない壊し（等価）:
  - VPS の `num_hrd_parameters` の上限を 1 緩める壊し: 件数が増えた分は hrd_layer_set_idx の範囲と重複の検べで必ず EINVAL になるので結果が同じ。
  - SPS の `num_short_term_ref_pic_sets` の上限を 64 → 65 にする壊し: 65 は `h265sw_rps_read` の引数の検べ（num_sets 64 超）で EINVAL になるので結果が同じ。
- 最初の実行で見逃した 2 件を試験で塞いだ: (1) PPS の chroma_qp_offset_list_len_minus1 の上限（writer が 6 組しか書けず、7 組の入力が作れなかったので偶然 EINVAL になっていた）→ 手で書いた PPS を足した。(2) rps の元 set の和（16 個）の検べ → 全 entry を落とす 16 個の元の set を足した。
- 実装の誤りを試験が捕まえた 1 件: PTL の `compatibility` を MSB 先頭で読んでいた（header は flag[j] が bit j）。test-ps の PTL の field の試験が見つけ、読みを 1 bit ずつ bit j に置く形に直した。
- 試験の道具の問題を 1 件見つけた（直していない）: `h265w_vps` の layer set の loop は num_layer_sets_minus1 を配列で区切らず、2^31 を書くと約 2^37 回回る。試験は 1024・1100・8000 だけを書き、2^31 と ue の最大は書いていない（reader 側の上限は 1023 超で EINVAL の分岐を 1024 で確かめた。巨大な値で reader が wrap しない事は、`layer_sets_minus1 <= 1023` の検べの後にだけ積を作る code の読みで確かめた）。

### 追加の確かめ（試験の外、使い捨て）

- 適合の stream 147 本（`/home/awe/zedBSD-claude1/build/sources/h265-conformance` の zip の先頭の .bit・.bin）の全部の VPS・SPS・PPS（nuh_layer_id 0）を `build/tmp/conf/psdump` で読み、失敗 0 件（147 本、各 stream の最初の 64 MB まで）。これは p006h の範囲の下見で、この Phase の受け入れの試験ではない。

### 自分で決めた点（header が黙っている所、または仕様と header の間）

- rps: `bits`・`out` が NULL、num_sets 65 以上、index が num_sets 超、max_pictures 16 以上、index が 0 でない時の sets NULL は EINVAL。元の set の num_negative・num_positive・和が 15 超（caller が作った不正な set）は EINVAL。delta の和は int64 で作り int32 の外は EINVAL（7-61・7-62 の各 entry）。成功の時も失敗の時も先に `*out` と `*syntax` を 0 で埋める。
- ps（共通）: `store`・`bits` が NULL は EINVAL。trailing bits が合わない時と extension data の途中で切れた入力は仕様どおり（extension data は stop bit まで読み飛ばす。切れた extension data は短い extension として成功する）。
- PTL: 先に `*ptl` を 0 で埋める。`compatibility` の bit j は flag[j]。
- generation: その id の set がまだ無い時の読みも「中身が違う」として generation と sps_generation を 1 進める。同じ中身の再送は進めず store も書き換えない。
- SPS: ENOTSUP の検べ（CTB の大きさ）は、後ろにある EINVAL の検べより先（読む順）。width・height・window の検べは block size の後。log2_min_cb_minus3・log2_diff_max_min_cb が 3 超は ENOTSUP（足す前）。sub-layer の ordering の単調増加（i > 0 で dec ≥ 前）・nesting flag・latency の VPS との大小は検べない（header が範囲に挙げていない）。PCM の chroma の深さは ChromaArrayType が 0（monochrome と separate_colour_plane 1）の時は検べない（7.4.3.2.1 が無視を求める）。VUI を落とす時は vui を 0 にし extension の flag と trailing を読まない（extension_present・range_extension_present・range_extension は 0）。profile_idc だけで判定し compatibility flag は見ない。
- VPS: `vps_max_layers_minus1`・`vps_max_layer_id` は 63 まで受ける。layer_id_included の bit は個数だけ読み飛ばす。
- PPS: tile が有効で num_tile_columns_minus1 と num_tile_rows_minus1 が両方 0 は EINVAL（7.4.3.3.1）。ENOTSUP の tile の数の検べは EINVAL の検べより先。tile_columns・tile_rows は tile 無しの時 1、uniform_spacing は 1（推定）。diff_cu_qp_delta_depth は 3 超、log2_parallel_merge_level_minus2 は 4 超を読みの時点で EINVAL にした（SPS が取りうる最大。足す前の wrap を避ける。SPS ごとの上限は check_pair）。range extension は、各値の範囲（log2_max_transform_skip_block_size_minus2 4 以上、chroma_qp_offset_list の長さ 6 以上・深さ 4 以上・offset 12 超、log2_sao_offset_scale 7 以上）の EINVAL を先に出し、範囲内で道具が on の時の ENOTSUP は最後。multilayer・3D・SCC の flag は range extension を読んだ後に ENOTSUP。
- check_pair: init_qp が 51 超も EINVAL にした（header は下限だけ）。`sps_supported` は NULL で EINVAL。
- ENOMEM の道（VPS の 2 回の malloc、SPS の malloc の失敗）は、試験で malloc を失敗させる手段が無いので code の読みで確かめた: 失敗した時点で、取った物だけを free して ENOMEM を返し、store には触れない。

### 未実施

- ENOMEM の道の実行（上の理由）。QEMU・実機（この Phase の範囲外）。適合の stream を試験として repo に置く事（p006h）。規約の全文の見直し（p015）。coding-style の checklist で自分の diff は見直した（関数を条件に使わない、関数の先頭の宣言、段落の comment、forward 宣言、成功の return の comment）。試験の .c は host の C89 の試験で、`CHECK` の中の式や、一部の添字の計算に条件演算子を使っている。
- 表の値（Table）を持つ module ではないので、値の照合の節は無い。

### header について（変えていない、規約の見直しの Phase へ）

- `h265sw-ps.h` は PPS の `tile_columns`・`tile_rows`・`uniform_spacing` が tile 無しの時の値を書いていない（実装は 1・1・1）。`diff_cu_qp_delta_depth` と `log2_parallel_merge_level` を read が 3 と 4（minus2）で絞る事も書いていない。
