<!-- awesome-plan project=zedbsd record=ws324-p006c -->

# ws324-p006c H.265: h265sw-rps・h265sw-ps の実装（標準廉価実装手順、Sonnet 5.5 high）

Status: planned（2026-11-01 P1 が p006a で作成。p006b の cleared の後に起動）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D1・D6・D7・D14・§5・§7・§8）。
- 基点 BASE: `<p006a 段 2 の SHA>`。前提: p006b（nal・scaling・vui）が main にある。

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
  - VPS: HRD 0・1・3 個（2 番目と 3 番目で cprms_present_flag 0 と 1）、保つのが最初の 1 つだけ、hrd_layer_set_idx の範囲と重複の EINVAL、layer_id_included の skip（num_layer_sets_minus1 1023・max_layer_id 63 の最大）、timing の 0 の EINVAL、sub-layer の写し下げ、extension data の skip。
  - SPS: 全分岐の往復（conformance window、sub-layer の ordering の有無、scaling の無効・既定・signalled、PCM、short-term set 0・1・64 個（予測を含む）、long-term 0・32、VUI、range extension の flag）。導いた値（crop、CTB の数、min CB の数）を手の計算と照合。範囲の端（width・height の MinCbSize の倍数でない・0・H265SW_MAX_SIDE + 1、MaxLumaPs を超える積、log2_max_poc_lsb 4・16・17、max_dec_pic_buffering 15・16、reorder > dec、CTB 8・128 の ENOTSUP、TB の上限、PCM の上限）の EINVAL・ENOTSUP。multilayer・3D・SCC の flag の ENOTSUP と `sps_refused`。VUI の誤り: profile_idc 1・2・3 で VUI を落とし SPS を保つ（vui_present 0、拡張の flag を読まない）、4 で EINVAL。
  - PPS: 全分岐の往復（tile の uniform と非 uniform、deblocking の control、scaling、extension）。init_qp の -74・51 で可・範囲外で EINVAL、tile の 20×22 で可・21 列で ENOTSUP、column_width の 1055・1056（EINVAL）、range extension の道具の各 ENOTSUP。
  - check_pair: 各条件の可と EINVAL の境（init_qp の -QpBdOffsetY、diff_cu_qp_delta_depth、parallel merge、tile の数、非 uniform の和、sps の id の不一致）。
  - store: 同じ中身の再送で generation と sps_generation が変わらない、違う中身で +1、失敗した読みで store が前のまま（byte で比べる）、refused の立ち下げ。
  - 途中で切れた入力の全ての切り口（VPS・SPS・PPS）、乱数の入力で落ちない。
- わざと壊す確かめ（例: 7-61 の符号、sub-layer の写し下げ、PTL の reserved の bit 数、VUI の誤りの規則の profile）。

## 4. 注意

- `h265sw_rps_read` は SPS の set i を読む時 `sets[0..i)` だけを使う。予測の元 RefRpsIdx は index − (delta_idx_minus1 + 1)。
- 7-61・7-62 は新しい set の entry を一つずつ足すので、足す前に数を max_pictures と比べる（header の約束）。
- SPS・VPS の読みは、0 で埋めた作業の構造体（malloc でその呼び出しの間だけ取る、header の約束）に組み、全部の検べが済んでから store へ写す（失敗で store を変えない、generation の byte の比べのため）。ENOMEM の道は、試験で malloc を失敗させる手段が無いので code の読みで確かめ、結果に書く。
- VPS の HRD の 2 番目以降は作業の 2 つの `h265sw_hrd` を交互に使う（header の約束）。
- PTL の general の 43 bit（constraint と reserved）は値を保たずに読み飛ばす。
- SPS の lt_ref_pic_poc_lsb_sps は log2_max_poc_lsb bit の u(v)。

## 5. 変えてはいけない物

全ての header、他の module の .c（p006b の物を含む）、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh rps ps
sh plan/ws324/tests/run-unit.sh all
BASE=<p006a 段 2 の SHA> sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ（全 PASS を自分で読む、規約の checklist、commit、結果の節、ws.md の行）。

## 結果

（実装の担当が書く）
