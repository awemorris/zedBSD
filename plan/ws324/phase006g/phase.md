<!-- awesome-plan project=zedbsd record=ws324-p006g -->

# ws324-p006g H.265: h265-stream の実装（Annex B から decode する picture へ。標準廉価実装手順、Sonnet 5.5 high）

Status: cleared（2026-11-02 06:09 Q1、main で test-stream PASS）。前: in-progress（2026-11-01 P1 が p006a で作成。2026-11-02 Sonnet q1906-i01 が実装・試験 PASS。cleared は Q1 が決める）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D3・D10〜D12・D14・§5.3・§6・§7・§8）。
- 基点 BASE: `1f16db0e6`（2026-11-02 p006a 段 2 の commit 1f16db0e619a9cd23c74f7f36a8729efdba4ecb3）。前提: p006b〜d が main にある（h265-dpb・h265-std は使わない）。

## 1. 読む物

- AGENTS.md の §4・§7・§9・§10。`plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」「開発の管理情報」。`docs/agent/spec-module-delegation.md` の §5・§7。
- 設計の §1・§4（D3・D10〜D12・D14）・§5.3・§6・§7・§8。
- 実装する header: `userland/desktop/libmedia/h265-stream.h`。使う物: `h265sw-nal.h`・`h265sw-ps.h`・`h265sw-slice.h`・`h265sw-poc.h`（実装済み）、`vcodec-nal.h`（`vcodec_nal_find`）。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。
  - 7.4.2.4.2〜7.4.2.4.5（parameter set の順、AU と順、PDF 87〜90）、7.4.3.2.1 の SPS の活性（PDF 94〜95）、8.1.3（NoRaslOutputFlag・HandleCraAsBlaFlag・PicOutputFlag、PDF 135〜137）、B.2・B.3（PDF 291〜292）、C.5.2.2 の NoOutputOfPriorPicsFlag（PDF 306）、D.3 の decoding unit information の SEI の位置（PDF 324 前後）。

## 2. 作る物

- `userland/desktop/libmedia/h265-stream.c`。
- `plan/ws324/tests/unit/test-stream.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

入力は `h265write.h` で組んだ Annex B の stream。

- 始まりと捨て: IDR から始まる IPPP、CRA から始まり RASL を捨て RADL を返す、途中から（IRAP の前の絵を before_irap に数える）、reset の後の CRA（BLA として、RASL を捨てる、first 1）、IRAP の no_output_of_prior_pics の導出（CRA は 1、IDR は flag の値、first の絵では 0）。
- end of sequence・end of bitstream: EOS で終わる AU の絵は after_eos 0、次の絵が after_eos 1 で、それが CRA なら no_rasl_output 1（RASL を捨てる）。EOB でも同じ。AU の外（絵を集めていない時）の EOS。
- AU の境（7.4.2.4.4）: 1 つの絵の 2 つの segment の間の prefix SEI・PPS（別の id）・SPS（同じ中身の再送）が同じ AU に入り（絵が切れない、壊れた絵 0）、その PPS が store にあり、picture.generation がその後の値。絵と絵の間の AUD・VPS・SPS・PPS・prefix SEI では cursor がその最初の NAL で止まり、次の picture の前に store が変わらない。suffix SEI は前の AU。reserved の 41〜44 と 48〜55、他の層の NAL（other_layer）、reserved の VCL の型（reserved_type）。
- parameter set: 再送で generation が変わらない、違う中身で変わる、`h265_stream_parameters`。
- SPS の活性: CVS の途中で別の SPS id を名指す絵は壊れた絵、同じ id で中身が変わった SPS（IRAP 以外で）も壊れた絵、次の IRAP で立ち直る。
- slice segment: 600 個で返り 601 個で壊れた絵、VPS の欠けで壊れない、dependent、PPS の不一致・nal_unit_type の不一致の壊れた絵、first_slice_segment_in_pic_flag 0 から始まる絵の飛ばし、offset と size が NAL の bytes（start code の後）を指す、access_unit の範囲。
- 長い slice header: tile と WPP の両方と高い絵（CTB 16、高さ 13 000 以上）で entry point を多くし、header が 64 KiB を越える segment を 2 つ目の写しで読む。
- `h265_stream_input` を AU ごとに分けて与えても、全体を 1 回で与えた時と同じ picture の列。
- 全ての切り口で切った stream と乱数の byte 列で落ちない（ASan・UBSan）。
- わざと壊す確かめ（例: RASL の捨ての条件、EOS の後の NoRaslOutputFlag の付く絵、au_start の戻し、cursor の止め位置）。

## 4. 注意

- 試験の道具の口は設計の §8.1.1（p006a 段 2 の結果）。試験は `plan/ws324/tests/unit/test-<name>.c` に置き、include は `"check.h"`・`"h265write.h"`・`"userland/desktop/libmedia/<module>.h"`（include の道は tree の root）、`main` の最後は `check_finish("<name>")` の値を返す。大きな構造体（`h265sw_ps_store`・`h265_stream`・`h265_picture`・`h265w_sps` など）は static か calloc に置く。
- 骨格の関数は本体ごと置き換え、「Not implemented yet」の comment も実装の comment に書き換える。module の中の補助は static 関数（一行の前方宣言を付ける）。外へ見える名前は `h265sw_`・`h265_` で始まる物だけ（check-skeleton の nm の照合）。
- libmedia の .c は check-skeleton で `-std=c89 -Wpedantic` と zedBSD の clang（x86_64・aarch64・riscv64、tree の libc の header だけ）にも通す: `LL`・`ULL` の接尾辞（`-Wlong-long`）、`//` の comment、文の後の宣言、host にしか無い関数を使わない。64 bit の定数は `(uint64_t)1U << n` か乗算で作る。
- `run-unit.sh` の `H265_UNIT_DIR` は道具の自己確かめ用。使わない。
- 絵の終わりは、AU を始め得る NAL の後に来る次の base layer の VCL の first_slice_segment_in_pic_flag で決まる（header の comment）。その NAL だけで絵を切らない。
- EOS・EOB は今集めている絵の AU に属する。その絵ではなく次の絵に after_eos と NoRaslOutputFlag 1 が付く。
- 捨てる絵は POC の状態を変えない。返す絵だけ `h265sw_poc_finish`。
- `slice_rbsp` の malloc の失敗の道（ENOMEM）は code の読みで確かめ、結果に書く。
- `*picture` は作業の場所。0 以外を返す時の中身は決めない（header）。

## 5. 変えてはいけない物

全ての header、他の module の .c、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh stream
sh plan/ws324/tests/run-unit.sh all
BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ。

## 結果

実装の担当（Sonnet 5.5、q1906-i01、worktree `s-ws324-p006g`）、2026-11-02（host の `date` で 04:43 JST）。cleared は Q1 が決める。

### 作った物

- `userland/desktop/libmedia/h265-stream.c`（骨格を実装で置き換え）。header・道具・他の module は変えていない（check-skeleton の BASE 照合 PASS）。
- `plan/ws324/tests/unit/test-stream.c`。
- commit: 実装と最初の試験 `bc42a69b45`、試験の追加（壊す確かめで見つけた見逃しを塞ぐ）`f0f3cb665e` とその後（Q1 への報告の SHA）。

### 確かめた command と最後の行（2026-11-02 04:43）

| command | 最後の行 |
| --- | --- |
| `sh plan/ws324/tests/run-unit.sh stream` | `test-stream: PASS (1331307 checks)`、`run-unit: PASS` |
| `sh plan/ws324/tests/run-unit.sh all` | h265write 51・nal 816099・poc 1788280・ps 1447981・rps 1891521・scaling 5499・slice 3904937・std 1680・stream 1331307・vui 2432070 checks、全部 PASS、`run-unit: PASS` |
| `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` | `check-skeleton: PASS`（h265-std は Vulkan の header が tree に無いため飛ばし、p006b〜f と同じ） |
| `git diff --check` | 出力なし（終了 0） |

試験は ASan・UBSan 付き（`-std=c89 -Wall -Wextra -Werror`）。test-stream の実行は約 2 秒。

### 実装の形

- `h265_stream_next` は NAL を 1 つずつ読む（`stream_step`）。picture の収集の状態は呼び出しごとの局所の構造体（`struct stream_gather`: open・dead・nomem・au_first・au_end・join_end・sps_generation）。`*picture` は作業の場所。
- 絵の終わり: 次の base layer の VCL NAL の first_slice_segment_in_pic_flag（NAL の 3 byte 目の最上位 bit）が 1 なら cursor をその NAL に戻して終える。segment の後に AU を始め得る NAL（`h265sw_nal_starts_access_unit`）が来たら先読み（`stream_lookahead`）: 何も保存・計数せずに次の VCL まで進み、flag 1・EOS・EOB・buffer の終わりなら絵を終えて cursor をその最初の NAL に戻す（その parameter set は絵を返した後の呼び出しで保存される）。flag 0 なら cursor を戻し、その VCL の位置を `join_end` に置いて同じ NAL を通常の道で読み直す（parameter set はここで保存、再び先読みしない）。
- 捨てる絵・壊れた絵は `dead` にして、後の segment は AU の境を見るだけで歩く。型で捨てる物（before_irap・RASL）は header を読む前に決める。壊れた物は見つけた時点で数える（絵ごとに 1 回）。
- 状態の更新は絵を返す時（`stream_finish`）だけ: `h265sw_poc_finish`・started・first_pending・eos_pending（= eos_seen）・rasl_skip・cra_as_bla・SPS の活性（IRAP で NoRaslOutputFlag 1 の時に active_sps と active_generation）。捨てた絵は eos_seen を eos_pending に足すだけ。
- 2 回目の複写（`stream_read_long`）は slice_rbsp を 1 回 malloc して保つ。header が短い複写で読めず `complete` 0 の時だけ使う。

### 試験の中身（要点）

- 始まり: IDR からの IPPP（offset・size・access_unit・generation・index を NAL の位置から作った期待と比較）、3 byte の start code、RPS の order count（手の計算）、POC の折り返し（8-1: lsb 100→200→3 で 259、SLNR と TemporalId 1 は prevTid0Pic でない、壊れた絵が状態を変えない: 200 が -56 になる）、PicOutputFlag。CRA の始まり（RASL を捨て RADL・TRAIL を返す）、BLA の RASL の捨て、途中の CRA は RASL を返す、途中から（before_irap、start code の前の byte の読み飛ばし）、reset（CRA を BLA として扱う、reset 無しとの対照、reset が started・pending の EOS・cra_as_bla を戻す、parameter set は保つ）、NoOutputOfPriorPicsFlag（first・IDR・BLA・CRA の場合）。
- EOS・EOB: AU 内の EOS の次の絵に after_eos と NoRaslOutputFlag（CRA の RASL を捨てる）、AU の外の EOS、suffix SEI の後の EOS、PPS の後の EOS（PPS は絵を返した後に保存）、buffer の境の EOS、捨てた RASL の AU の EOS、非 IRAP が EOS の後、EOS の後の SPS の活性。
- AU の境: segment 間の prefix SEI・別 id の PPS・SPS の再送で絵が切れない（generation はその後の値）、絵の間の AUD・VPS・SPS・PPS・prefix SEI で cursor が最初の NAL で止まり store が変わらない、suffix SEI は前の AU。非 VCL の型 32〜63（EOS・EOB 以外）の全てを、絵の間と segment の間に置いて、AU を始める型かで AU の範囲と cursor の位置を（7.4.2.4.4 の型の一覧から別に作った期待で）比較。他の層の NAL・reserved の VCL（10〜15・22〜31 の 32 通り）の計数と無影響。
- parameter set: 再送で generation 不変、違う中身で変わる、VPS 無し、`h265_stream_parameters`（最初の VCL まで、他の NAL・3 byte の start code・長すぎる・unescape できない・header が壊れた set の EINVAL と refused_sets・reason）、64 KiB 超の set、ENOTSUP の PPS（絵は壊れではなく refused に数える）。
- SPS の活性: CVS の途中の別 id・同じ id で中身が変わった SPS・CRA（NoRaslOutputFlag 0）・segment の間の SPS の変更・segment の間に PPS が別の SPS を名指す、が壊れた絵、次の IRAP で立ち直る。
- slice segment: 599・600 個が返り 601 個が壊れた絵、dependent（stream.independent が最後の独立 segment、dependent の header を入れない）、PPS・nal_unit_type の不一致、first flag 0 から始まる絵（1 つの壊れた絵として数える）、読めない header・欠けた PPS・欠けた SPS・refused の SPS・PPS と SPS の不一致・範囲の外の重み（high_precision）、3 つ以上の segment の捨てる絵、2・3 byte の VCL NAL（先読みの中も）、1 byte の slice、header が壊れた NAL（forbidden bit・temporal id plus1 0）が絵の外・絵の中・segment の間にある時。
- 長い header: tile 20 列と WPP、CTB 16、高さ 13 600 で 16 999 個の entry point（header 68 KB 超、offset の値 3 通りで escape も含む）を 2 つ目の写しで読む、entry point 16 300〜16 400 個の全て（1 つ目の写しに収まる・収まらない境）、写しを 1 回だけ取る（pointer が同じ）、収まる header では写しを取らない、17 000 個（上限超）は壊れ、2 つ目の写しの unescape の誤り、destroy。header の bit 列は syntax writer が 64 KiB までなので、entry point の部分だけ試験の中で組んだ。
- order count の端: int32 の端の POC（2^31 - 1 は返る、2^31 + 2 と RPS の +1 は壊れた絵）。
- AU ごとに分けて与えても、全体を 1 回で与えた時と同じ列（14 AU の scenario の 12 枚、offset を補正して全 field と counts を比較。2 AU ずつの buffer も）。
- 全ての切り口で切った scenario、1〜4 byte を変えた物 6 000 通り、先頭 700 byte の全 bit 反転、乱数の byte 列 6 000 通りで、落ちない（ASan・UBSan）ことと、返した絵の不変式（segment が buffer の中・AU の中・順序通り・id の範囲）。
- 失敗の reason の文字列を各経路で比較（`rd.reason` は実装の文字列。経路ごとに別の試験で 1 つずつ起こす）。
- 入力は全部 `h265write.h`。期待の値は仕様と手の計算と、試験が追加した NAL の位置。他の実装の出力は使っていない。

### わざと壊す確かめ（git の外で、元の file は変えていない）

- 994 か所（比較・論理演算子の置換、整数の定数の ±1 と 0/1、`++`/`--`、if の条件を 0・1 に、文の削除、return の値）。script は `scratchpad/mut.py`（build/tmp/p006g-mut に写しと exe を作る。並列 4、各 exe は RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒、RSS 6 GB 超か 90 秒で kill（600 秒の上限より短くした: 試験は 2 秒で終わり、無限 loop の mutant の待ちを減らすため）、ASan のため RLIMIT_AS は使わない）。
- build の誤りは 0（警告は `-w` で無視して build。未使用の変数などで通らない物は無かった）。
- 最初の試験で 763 が試験の失敗で落ち 231 が生き残った。生き残りを調べて試験を足した（reset 後の started、damaged header の全型、1・2・3 byte の NAL と先読み、reason の文字列、`h265_stream_parameters` の経路、dependent の independent、EOS の後の AU の始まり、order count の端、第 2 の写しの誤り、dead の picture の 3 segment、他の層・damaged の NAL が segment の間にある時、dead の picture の EOS の後始末、など）。最後の全件の結果: 889 が試験の失敗で落ち、105 が生き残った。
- 生き残り 105 の内訳（全部、観測できない、または等価。個別の理由）:
  - 12: malloc の失敗の道（`stream_read_long` の NULL 検べ・reason・return、`stream_spoil` の NOMEM、`h265_stream_next` の ENOMEM の return）。malloc を失敗させる道が単体試験に無いので、code の読みで確かめた（下の「未実施」）。
  - 17: 初期化の削除・変更で、memset か後の代入で冗長な物、または初期化されない局所変数を読む mutant（`gather_init`、`h265_stream_init` の `h265sw_ps_init`・`h265sw_poc_init`、`reset` の `rasl_skip`・`eos_seen`・`poc_init`（IRAP が返る時に上書きされ、started 0 の間は参照されない）、`h265_stream_parameters` の `cursor = 0`、`first_flag` の既定の削除など。後者は stack の値が偶然 0 の時に通る）。
  - 17: 0 でない値の置換（`ended = 2`、`dead = 2`、`open = 2`、`return 2`、`failed = 2`、`no_rasl = 2`、`expect.check = 2` など。真偽でしか使わない）。
  - 7: `return STREAM_SEGMENT_OK` を `return 0` に（OK は 0）。
  - 8: `find_sets` の PPS id・SPS id の範囲の検べ（`h265sw_slice_read_start` が PPS id 63 以下を、`h265sw_ps_read_pps` が SPS id 15 以下を保つので届かない守り）。
  - 7+7: `stream_start_code` の `nal_offset < 3`（NAL は start code の 3 byte の後にしか無い）、`stream_damaged_set` の `nal_size < 1`（`vcodec_nal_find` は空の NAL を返さない）。
  - 4: `picture->generation = 0`・`picture->index = 0`（`stream_finish` が必ず上書き）。
  - 他: `ended = 0` の初期値（両枝で代入）、`before >= join_end` の `>`（その位置の NAL は VCL で先読みを起こさない）、`stream_nal_header` の `nal_size < 2`（1 byte の NAL の nal[1] は次の start code の 0 で、temporal id plus1 が 0 になり同じ結果）、先読みの `type < STREAM_FIRST_NON_VCL` の `<=`（VPS は先読みで読み飛ばす点で同じ）、先読みの `if (error != 0)`（初期化されない type を使う）、`stream_read_long` の `read_start` の失敗と ENOTSUP の道（同じ bytes で read_start は失敗せず、重みの ENOTSUP は 1 つ目の写しの中で先に返るので 2 つ目の写しには来ない）、`stream_read_segment` の read_start の失敗の `if (0)`（失敗した時は pps_id が 0 のままで、続く read_rest が同じ bit で失敗し同じ数え）、`cra_as_bla` の分岐（reset の後は first_pending も 1 なので冗長）、`type & 0x3e` の 2 か所（型の組は decodable・IRAP・RASL かの判定が同じ）、`if (gather->open != 0)` を `if (1)` に（au_end は絵を開く時に上書き）、`stream_finish` の `open == 0` の守りと `stream_step` の EOS の `return 0`（呼ばれない道）、`no_output_of_prior_pics`・`active_sps` の `if (1)`（IRAP でない絵の active_sps は同じ値の代入）。
- 見逃しで試験を足した例: damaged header の NAL（forbidden bit・tid plus1 0）を `if (0)` にした mutant は、壊れた slice の試験では別の理由でも壊れた絵になり見逃していた。壊れていない slice の header だけを壊した NAL を絵の外に置く試験で塞いだ。reason の文字列の削除は、各失敗を 1 つずつ起こして文字列を比べる試験で塞いだ。

### 自分で決めた点（header が黙っている所）

- 数え方: `reserved_type` と `other_layer` は NAL 単位（header は「絵」と書くが、型が reserved の VCL NAL は絵として扱えないので 1 NAL を 1 と数える）。`refused_sets` は parameter set の拒否（header が壊れた VPS/SPS/PPS、長すぎる、unescape できない、parse が拒む）に加えて、ENOTSUP の PPS・SPS・slice の道具（重みの high_precision）を名指して捨てた絵を 1 枚ごとに足す（`broken_pictures` には数えない。header の「範囲の外であって壊れではない」に従い、counts に他の欄が無いので refused_sets に入れた）。
- NAL の header が読めない（forbidden_zero_bit 1、temporal id plus1 0、2 byte 未満）物は読み飛ばす。VPS/SPS/PPS の型を指す物は refused_sets に数える。その時に絵を集めていて、型が decodable な VCL を指す物なら、その絵を壊れた絵にする（segment が欠けるため）。絵を集めていなければ何も数えない。`h265sw_nal_parse_prefix` が失敗する slice（IRAP の temporal id 0 でない、unescape の誤り）は絵ごと壊れた絵。
- 2 byte の VCL NAL（payload 無し）は first flag が読めないので 1 として扱う（新しい絵を始め、header が読めず壊れた絵）。先読みでも同じ。
- reserved の VCL NAL が絵の最後の segment の後に来たら、その AU に入る（`access_unit_size` に含める）。他の層の NAL は AU の範囲に含めず、AU の境にも影響しない。
- AU の範囲 `access_unit` は、その絵の AU で最初に読んだ base layer の NAL の start code（4 byte の start code の zero_byte を含む。直前の byte が 0 なら 1 byte 戻す）から、最後の NAL の終わりまで。AU の外の EOS（絵を集めていない時）はその NAL だけで 1 つの AU として、次の絵の AU はその後から始める。buffer の終わりで終わる絵は、そこまでの最後の NAL の終わりまで。
- EOS・EOB の先読み: AU を始める NAL の後に EOS・EOB が来たら、絵はその NAL の前で終え（cursor はその最初の NAL）、EOS は次の呼び出しで AU の外の EOS として扱う。AU を始める NAL の後に来た parameter set は、次の VCL が first flag 0 の時だけ（絵が続く時）その絵を返す前に保存する。
- 先読みが VCL に着く前に buffer の終わりが来たら、絵はそこで終え、cursor は先読みの最初の NAL に戻す（それらの parameter set は次の呼び出しで保存される。ENOENT）。
- SPS の活性は `stream_finish`（絵を返す時）で行う。絵を返さなければ active_sps を変えない。later segment は first segment が読んだ SPS の id と sps_generation と同じでなければならない（違えば壊れた絵）。PPS の中身が同じ id のまま絵の途中で変わる場合は検べない（各 segment は header を読む時点の PPS と SPS の組で `h265sw_ps_check_pair` を通る）。
- 非 IRAP の絵が EOS の後に来た時: 返す（`after_eos` 1、`no_rasl_output` 0）。EOS の後の絵が IRAP でない場合の SPS は active のものを使う。
- `h265_stream_parameters`: 先頭から最初の VCL NAL（型 0〜31 の base layer、reserved も）まで。他の層の NAL は other_layer に数える。壊れた parameter set の NAL header（VPS/SPS/PPS の型）は refused_sets に数えて EINVAL。reader の buffer・cursor は触らない。buffer が NULL の時は 0。
- `h265_stream_reset`: buffer と cursor はそのまま。started・first_pending・rasl_skip・eos_pending・eos_seen・cra_as_bla・poc を戻す。counts と returned は保つ。
- `h265_stream_destroy` は buffer の借用（data・size・cursor）も NULL・0 に戻す。`h265_stream_input` は data が NULL の時 size を 0 にする。`h265_stream_init` は構造体全体を memset してから ps と poc を初期化する（slice_rbsp を持つ reader を destroy 無しで init すると漏れる）。
- ENOMEM は絵を歩き終えてから返す（`gather.nomem`）。残りの segment が別の絵（first flag 0 の孤児）として二重に数えられるのを避けるため。その絵は broken_pictures に 1 回数える。

### 未実施

- malloc の失敗（`slice_rbsp` の確保の失敗、ENOMEM）の試験: 単体試験に確保を失敗させる道が無い。code の読みで確かめた: `stream_read_long` は NULL を `STREAM_SEGMENT_NOMEM` で返し、`stream_spoil` が `nomem` を立てて broken_pictures に 1 回数え、その絵は dead のまま最後の segment まで歩き、`stream_finish` が 0 を返した後 `h265_stream_next` が ENOMEM を返す。cursor と状態は整合して次の呼び出しで次の絵に進む。
- QEMU・実機（この Phase の範囲外）。適合の stream での確かめ（p006h）。規約の全文の見直し（p015）。zedBSD の clang でのコンパイルは check-skeleton の通り（x86_64・aarch64・riscv64 の tree の header）。coding-style の checklist で自分の diff は見直した（関数を条件に使わない、3 つ以上の条件は行を分ける、関数の先頭の宣言、段落の comment、forward 宣言、成功の return の comment）。試験の .c は host の C89 の試験で、`CHECK` の中の式や一部の補助は規約の全てには従っていない。
- 表の値（Table）を持つ module ではないので、値の照合の節は無い。

### header について（変えていない、規約の見直しの Phase へ）

- `h265-stream.h`: `reason` の静的な文字列は実装の物（header は文字列を決めていない）。`reserved_type`・`other_layer`・`refused_sets` の数え単位（NAL か絵か）、AU の範囲（`access_unit_size` が最後の NAL の終わりまでで、buffer の終わりまでではない点）、他の層・reserved の NAL の AU への含め方は header に書いていない。ENOMEM の時に絵を歩き終えてから返す点も。header の「絵の終わり」の説明の、`au_start` の記述は実装では先読み（`stream_lookahead`）と `join_end` で行っている（結果は header の通り）。

