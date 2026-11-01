<!-- awesome-plan project=zedbsd record=ws324-p006g -->

# ws324-p006g H.265: h265-stream の実装（Annex B から decode する picture へ。標準廉価実装手順、Sonnet 5.5 high）

Status: planned（2026-11-01 P1 が p006a で作成。p006d の cleared の後に起動）

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

（実装の担当が書く）
