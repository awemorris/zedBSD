<!-- awesome-plan project=zedbsd record=ws347-p003 -->

# ws347-p003 driver を最新の ghci.md に合わせる（実装と host 試験）

Status: cleared（2026-11-13 Q1 判定: driver を ghci.md ad23fe01 に追従（VA − 30、新しい CAP の bit、TU_BILINEAR 0 で nearest、BLT_SCALE 0 の blitter は B1 が決まるまで Vulkan の転送に使わない）。host 試験 全 9 群 PASS、vf2・x86_64 の kernel warning 0。仕様の所見は台帳の A12・B30・B31。mk の 2 つの key の案はユーザーへ。CLIP_POLY 0 の software の clip は Q1 の案としてユーザーへ）

- 親: [WS347](../ws.md)。依存: p002（[spec-diff.md](../spec-diff.md)）。
- 2026-11-11 ユーザーから、driver は仕様（ghci.md）に合わせて直す、という趣旨の指示を受けた。
- 正本: amaterrace-gpu の commit 済みの `docs/ghci.md`（2026-11-13 に読んだ版: repository の HEAD 2ca25db2、ghci.md の最後の変更 ad23fe01）。
  作業ツリーは Gemini が RTL を直している最中なので読まない。
- 仕様の問い（仕様が黙っている・表せない・設計の意図が要る所）は推測で埋めず、[ghci-rtl-questions.md](../ghci-rtl-questions.md) の B に足す案を Q1 に送り、Q1 経由でユーザーが決める。
  決まっていない B に依る所は、どちらの答えでも動く形にする。

## 範囲（`src/drivers/gpu/ghci/`、HAL・Makefile・`mk/` は変えない）

2026-11-13 P4 が下の「乖離の確かめ」で今に直した範囲。

1. EXT_CAP_FEATURE の VA_BITS_ENC を VA − 30 で読む（決定 D2）。VA が 32 bit より狭い GPU では、driver が配る GPU の address（2 MiB の chunk）を VA の内に限り、CP_HEAP の見積もりもその chunk の数で数える。
2. 新しい CAP の bit を `ghci-regs.h`・`caps.c`（`ghci_caps_has` と decode）に足し、attach の log（`ghci.c`）と Vulkan の shortfall の log（`vk/vk-caps.c`）に出す:
   EXT_CAP_FEATURE [30] CLIP_POLY・[31] BLT_SCALE、EXT_CAP_FMT0 [10] FFILT_FP16（CAP_TEX_FFILT の mirror）・[11] FFILT_FP32・[15:12] MAX_COLOR_ATTACHMENTS（MAX_MRT の mirror）・[16] TU_BILINEAR・[17] BLT_BLEND・[18] BLT_TILE。
3. 各 bit が 0 の時の扱い:
   - TU_BILINEAR 0: 全 format の FILTER_LINEAR を報告しない。sampler の linear（min・mag・mip）と blit の linear は nearest に落とす（FILTER_LINEAR の無い format での linear は Vulkan の誤用なので、落としても規則に反しない）。
   - FFILT_FP32 0: 32 bit float の format の FILTER_LINEAR を報告しない（FMT の窓の無い GPU の表の道）。
   - BLT_SCALE 0: Vulkan の転送（copy・blit・fill・resolve）を blitter に出さず、blitter の無い GPU と同じ driver の program・描画の道にする（B1 が未決のため。下の「B に依る所」）。
   - BLT_BLEND 0・BLT_TILE 0: driver はどちらも使わない（下の根拠）ので動きは変わらない。decode と問い合わせだけ。
   - CLIP_POLY 0: driver に software の clip は無い。attach の log の Vulkan 1.0 の shortfall に出す（下の案）。（p008 で shortfall から外した: 2026-11-13 の決定）
4. N79（attachment・MSAA の row_pitch_bytes）・N81（occlusion query の BEGIN・END を同じ流れに）・VS の属性の register の連続の割り当て・STATUS の CP_STATE [19:16] に今の code が合っているかを確かめる。
5. host 試験（`plan/ws290/tests/`、phase の指定で WS290 の試験の道具を直す）に新しい bit の 0・1 と VA 30 bit の場面を足し、全部 PASS。riscv64（VF2、`CONFIG_DRIVER_PCI_GHCI=y`）と amd64 の kernel が warning 0。

## 受け入れ

- 上の 1〜4 の実装・確かめ、host 試験 PASS、両 arch の kernel の build warning 0。実機は p004。

## 乖離の確かめ（2026-11-13 P4）

この phase.md と spec-diff.md は 2026-11-11 の記述。その後の決定（ws.md・台帳の「決定済み」）・今の ghci.md・今の driver の code と比べた。

### 仕様の差（a7f5ca47 → ad23fe01）

- 6949b248: VA_BITS_ENC = VA − 30（0x0 = 30 bit、0x2 = 32 bit、0xA = 40 bit）。page table の説明は変わらない（決定 D2）。
- ad23fe01: KC705 の GCLK は 200 MHz（WDT の既定の秒数・timestampPeriod の例の文）、MAX_MRT・EXT_CAP_FMT0[15:12]・FMT_LIMITS[3:0] は「実装数」（minimal は 1）、DEVICE_CAPS[3]・[4] の構成の欄。
  driver への影響: GCLK・WDT は GCLK_HZ を読むので変更なし、maxColorAttachments は前から MAX_MRT。

### 直した記述

| 記述 | 今 | 直し |
| --- | --- | --- |
| 「Q1 は今は仕様の持ち主」 | 仕様の問いはユーザーが決める（台帳の B、Q1 経由） | 上の前置きに書き直した |
| 範囲 1 の「`check.c` の表示」 | `check.c` は ring の空きの計算だけで CAP を表示しない | 表示は attach の log（`ghci.c`）と shortfall（`vk-caps.c`・`vk-node.c`）に |
| 範囲に VA_BITS_ENC が無い | spec-diff §3 は「driver の decode は P2」としたが、main の code は 32 + raw のまま（P2 の差分は未 commit） | 範囲 1 に足した |
| EXT_CAP_FMT0 の [10]・[11]・[15:12] | 未 decode | 範囲 2 に足した |
| 試験の道具 | `plan/ws290/tests/ghci-host-test.sh` は今は無い `platform/amd64/vmunix.mk` を読み、`set -e` で driver の試験の後に止まっていた（main で vk・pipeline・execute・meta の試験が流れない） | `mk/kernel.mk` の GHCI の節を読む形に直した |
| model の profile | VA_BITS_ENC 0 は今の decode で 30 bit | 32 bit の profile は 2 に直し、30 bit の profile を足した |
| kernel の build の手順 | `plan/ws290/tests/config-*-ghci.mk` の `ZEDBSD_CONFIG=` と `config/ci/` は今の build system の形でない | `PLATFORM=… CONFIGMK=…` で読む config を [tests/](../tests/) に置いた |

### 今の code が仕様どおりで変更の無い所（根拠）

- STATUS の CP_STATE [19:16]: `GHCI_STATUS_CP_STATE_SHIFT 16`・mask 0xF で定義済み。driver は CP_STATE を判定に使わない（RESET の後は CTRL.RESET の自己解除と MEM_READY、停止は HALTED を待つ。§3.3・N80）。
- N80（RESET の後の MEM_READY）: `ghci.c`・`recover.c` で待っている（spec-diff のとおり）。
- N79: `layout.h` の `ghci_layout_row` は MSAA で roundup(width, 2) × S × ps、1 sample で width × ps を 64 byte に切り上げる。
  1 sample の P_min = ⌈width/2⌉ × 2 × ps を下回るのは width が奇数で width × ps が 64 の倍数の時だけで、ps < 64 では起きない。row_pitch はそのまま RT・DS の stride と texture の row_pitch に入る。
- N81: `vk-execute.c` は render pass の外で始まった query が active の間、提出を切らない（`cut_mark`）。event の待ちの切れ目は query の始まりへ動かす（`wait_events`）。
  1 つの piece は gate の中で BIND_SESSION から fence と BIND_SESSION(0) まで続けて置かれ（`vk-queue.c` の `ring_place`）、間に他の session の packet は入らない。Vulkan は BEGIN・END を同じ command buffer に置く規則。
- VS の属性: compiler（`compiler/gsi1-io.c` の `prepare_attributes`）は location の順に base_i = 前の成分数の和、60 register まで。§12.10.3 の式と同じ。
- SFU: CAP の bit は無く仕様上必須（決定 D1）。driver の変更は無い。minimal の実機は SFU 無しなので、compiler が transcendental を出す shader は止まる見込み（p004 の試験で避ける）。

### 仕様の側の食い違い（Q1 へ、台帳に足すかの判断）

1. `include/ghci_regs.h:193` の `GHCI_FEATURE_VA_BITS` の comment が「Virtual Address bits (32..40)」のまま（ghci.md は VA − 30）。A の候補（header の comment の直し）。
2. §11 の page table は VA 32 bit（2-level）と 40 bit（3-level）の walk だけを書き、30・31 bit や 33〜39 bit の walk を書かない（決定 D2 で説明は変えない）。
   driver の扱い: VA ≤ 32 は 2-level の root（16 KiB、使う chunk は VA の内）、VA > 32 は 3-level で数える。どちらも CP_HEAP の見積もりは多めに倒れ、使う address は VA の内なので、walk の形がどうでも害は無い。minimal（flat）には関わらない。
3. EXT_CAP_FMT0 [11] FFILT_FP32 の文は一行だけで、CAP_TEX_FFILT = 1 で [11] = 0 の時に FP32 の linear filter が無いのかを書かない（§8.5.1 の表は FP32 も CAP_TEX_FFILT とだけ書く）。
   driver は「無い」と読む（報告を減らす側で、どちらの答えでも誤った報告にはならない）。B の候補。

### B に依る所と、どちらの答えでも動く形

- **B1（CAP_BLT_SCALE = 0 の copy の address の揃え）**: §3.2.2 [31] は BLT_SCALE = 0 で「unaligned byte window alignment」も無いと読めるが、要る揃えを書かない。
  決まるまで、BLT_SCALE = 0 の blitter には Vulkan の転送（copy・blit・fill・resolve）を出さず、blitter の無い GPU と同じ driver の program・描画にする（`ghci_vk_caps_blitter`）。
  B1 が決まったら、その揃えを満たす 1:1 の copy・fill だけを blitter に戻す（P2 の差分の `blit_takes` の形）。KC705 の minimal は blitter が無いので疎通には影響しない。
  display の frame の fill（local memory の時だけ、64 byte に揃った行の 1:1 の fill）は今のまま blitter。

### 案（Q1 へ）と、その決定

- **CLIP_POLY = 0 の software の代替**: 2026-11-13 ユーザーの決定（Q1 経由）で作らない。clipper の無い GPU は何も言わずに欠けてよい（minimal は実用ではない）。attach の log の shortfall からも CLIP_POLY を外した（[p008](../phase008/phase.md) で実装）。
- **mk の 2 つの key**: 2026-11-13 ユーザーの決定（Q1 経由）で案を取り下げた。割り込みの有無と DMA の coherent は driver が bus から調べて決める基本の機能なので、`CONFIG_GHCI_POLL`・`CONFIG_GHCI_HOST_UNCACHED` を source から外し、検出だけで決める（[p008](../phase008/phase.md) で実装）。

### 前の担当（P2、2026-11-11 に止めた worktree `p2-ws347`）の差分の扱い

読むだけにして、今の仕様と照らして自分の worktree で書き直した。

- 使った形: 新 bit の定義、`caps_mirror` の表（FFILT_FP16 の mirror）、`ghci_va_chunks` と vm.c の `end_chunk`、`max_color` の log、shortfall の 2 行、format と sampler の bilinear の扱い、host-test.sh の mk/kernel.mk、model の profile（kc705-min・va30）。
- 変えた所: BLT_SCALE = 0 の 1:1 の blit を blitter に出す `blit_takes` は B1 に依るので使わず、上の「blitter に出さない」形にした。format の blit の報告も同じ判定（`ghci_vk_caps_blitter`）にそろえた。subpass の resolve も同じ判定に。
  FFILT_FP32 の扱い・VA の log の行・`CONFIG_GHCI_*` の既定・blt-min の profile と各試験の場面は P2 の差分に無く、足した。

## 実装（2026-11-13 P4、commit c57782d252）

- `ghci-regs.h`: 上の bit、`GHCI_FEATURE_VA_BITS_BASE`（30）、`struct ghci_caps` の `max_color`、`ghci_caps_flag` の 6 つ、`ghci_va_chunks`。
- `caps.c`: VA = 30 + [3:0]、mirror の表に FFILT_FP16、`max_color`、`ghci_caps_has` の 6 つ（blitter の 3 つは CAP_BLT が無ければ 0、FFILT_FP32 は CAP_TEX_FFILT が無ければ 0）、`ghci_va_chunks`、`ghci_heap_device_bytes` の chunk の数。
- `vm.c`: 配る chunk の終わりを VA の chunk の数から（`end_chunk`）。
- `ghci.c`: mirror の食い違いの log に FMT0、MAX_MRT と FMT0[15:12] の食い違いの log、VA・PA・colour attachment・shared memory の log の行。
- `pci-attach.c`: `CONFIG_GHCI_POLL`・`CONFIG_GHCI_HOST_UNCACHED` の既定 0（p008 で option ごと外した）。
- `vk/vk-caps.c`・`vk.h`: `ghci_vk_caps_blitter`、shortfall に CLIP_POLY と TU_BILINEAR（CLIP_POLY は p008 で外した）。
- `vk/vk-format.c`: TU_BILINEAR 0 で FILTER_LINEAR を落とす、FFILT_FP32、blit の報告は `ghci_vk_caps_blitter`。
- `vk/vk-execute.c`: `has_blitter` と subpass の resolve を `ghci_vk_caps_blitter` に、TU_BILINEAR 0 で blit の filter を nearest に。
- `vk/vk-layout.c`: TU_BILINEAR 0 で sampler の min・mag・mip を nearest に。

## 試験（2026-11-13 P4）

- host 試験 `sh plan/ws290/tests/ghci-host-test.sh`（worktree で）: words 151・ref 56・model 280・driver 1086・vk 474・pipeline 176・execute 291・meta 98、全部 0 failed。
  未変更の HEAD（host-test.sh の path だけ直した写し）では words 141・ref 56・model 271・driver 1061・gsi1 50・vk 438・pipeline 175・execute 285・meta 98 で全 PASS だった。
  足した場面: words（VA 30・32・40・45、新 bit の decode と CAP_BLT・CAP_TEX_FFILT への依存、FFILT_FP16 の mirror、va30 の chunk と CP_HEAP）、
  model（kc705-min・blt-min の identify）、driver（va30 の CP_HEAP、chunk 510 まで・2 chunk は入らない、kc705-min は flat で local memory を使わない）、
  vk（flag の差: TU_BILINEAR で 45 format の FILTER_LINEAR、FFILT_FP32 で 3 format、BLT_SCALE で depth の BLIT_SRC、CLIP_POLY・BLT_BLEND・BLT_TILE は差無し、kc705-min の shortfall と format、blt-min の blitter 不使用）、
  execute（blt-min で copy・fill・image copy・blit が blitter の packet を出さず dispatch と draw になる）、pipeline（bilinear 無しの sampler の 16 byte）。
- 全部の通し（gsi1 を含む、commit c57782d252 の tree）: `PASS ghci290-host-test`、上の件数に gsi1 の 50 passed と compiler の PASS の行（59 行の PASS）、0 failed。
- kernel: `make PLATFORM=vf2 CONFIGMK=plan/ws347/tests/config-vf2-ghci.mk llvm-toolchain-cached` → `sysroot BUILD_JOBS=3` → `build/vf2/vmunix -j4`、
  `make PLATFORM=x86_64 CONFIGMK=plan/ws347/tests/config-amd64-ghci.mk sysroot BUILD_JOBS=4` → `build/x86_64/vmunix -j4`。両方 rc 0、warning 0、GHCI の 42 file の object がある。
  log は worktree の `build/p4-*-*.log`。

## 未実施・残り

- 実機（VF2＋KC705）と QEMU での確かめは無い（p004）。attach の log の新しい行は host の stub でだけ通った。
- 規約の全文の見直しは p005。
- mk の 2 つの key の案は取り下げ（上の決定）。B1 が決まったら blitter の 1:1 の copy を戻す。
