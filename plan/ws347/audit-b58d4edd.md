<!-- awesome-plan project=zedbsd record=ws347-audit-b58d4edd -->

# 監査: amaterrace-gpu b58d4edd（依頼 06 の U1〜U9）（2026-11-12）

- 監査の担当 P3（Opus、読むだけ。sim・lint・arith-lint・Vivado は走らせていない）。RTL・tb は読んだ見込みで、動きは確かめていない。timing は既にある report を読んだ事実。
- 範囲 `git diff 28a8d920 b58d4edd`（9 commit: 64b18b40・7a4bd132・e549bc69・e7d01f96・7ac41e51・5f625a61・6e5f1a55・ca3288a9・b58d4edd、37 file）。
- Q1 の範囲の訂正（同日）により、判定は U1〜U9 だけ。U10〜U15 は「範囲外」。旧い仕様の残りも U1〜U9 に関わる物だけを書く。
- `docs/` の変更は無い（ghci.md は書き換えられていない）。`spec/` は `spec/gpu/cp/atg_cp_irq.md` だけ（U1 の並び）。
- agy の報告（`gemini-report-06` に当たる物、`build/agy/`）は無い。下の「済み」は RTL と tb を読んだ判定。

## 結論

- **済み**: U1 の IRQ_STATUS の並び（[0] FENCE・[1] FAULT・[2] WATCHDOG・[3] DISPLAY・[5] SOFT・[6] HALTED、他は 0）。U2（IRQ_GLOBAL の bit、irq の gate、CTRL.RESET で WDT_EN・IRQ_GLOBAL も 0）。U3 の DISP2 の gate と record（lifecycle の state を別の信号 `lc_state` で出した）。U4 (c)（fence の書き込みの error が KIND 5）と (d)（旧い kind の名前の撤去）。U6（CMD_FENCE の書き込みは EXEC、CMD_WAIT_FENCE の待ちだけ WAIT_FENCE、barrier の drain は WAIT_MEM）。
- **一部**: U1（tb 2 本が消した名前を使い compile が通らない見込み、fw の HW_RESULT の扱いが未報告）、U2（IRQ_GLOBAL を立てずに irq を期待する tb が残る）、U3（`s_seq_halt` が eeed36ed の意味と違う）、U4 (a)(b)、U5（sim の経路は済み、合成の経路に誤り 2 つ、HOST_FLUSH が flush でない）、U9（別名を足した所、旧い offset の別名が GHCI 1.0 の register を読む）。
- **未着手**: U7（DEVICE_CAPS [3]・[4]、`ATG_DEF_*`）、U8（SESSION_ACTIVE）。
- **最も重い誤り（合成だけの経路、RTL を読んだ見込み）**: `atg_cp_seq.v` の `flags` を `SYNTH_KINTEX7` の時に 2 つの always が駆動する（:771 と :827・:885）。sim と lint は `SYNTH_KINTEX7` を定義しないので見つからない。KC705 の合成で多重駆動（Vivado の critical warning か DRC MDRV-1）になる見込み。
- **timing**: b58d4edd の build は無い。最新の report は 28a8d920 の合成の dcp からの route（Performance_Explore）で WNS −0.383 ns、bitstream は `VIVADO_FORCE_BITSTREAM=1` で強制して書かれた（受け入れに使えない）。
- **規則**: commit message は 9 件とも `WIP` ちょうど（`Co-Authored-By` 無し）。依頼に無い変更は 64b18b40（強制の口）、ring の読みの error の KIND を 2 → 5（依頼 06 は「今の値のまま」）、CMD_FENCE の 64 bit の書き込み（仕様には合う）、CMD_WAIT_FENCE の HW の経路（新設）。

## U1〜U15

| # | 状態 | 根拠（b58d4edd） | 残り |
| --- | --- | --- | --- |
| U1 IRQ_STATUS | 一部 | `atg_cp.v:1104` の `ev` = {HALTED, 0, 0, `dc_irq_display`, WDT, FAULT, FENCE}、`atg_cp_irq.v:58-59,86,91-92`（mask 7'b1101111、[4] は 0）。RING_EMPTY の立ち上がりの検出を消した。`.vh`・`.h` の RESULT・RETIRE・VBLANK・FLIP・DISP_ERROR・HOTPLUG を消した | (1) `sim/tb/atg_top_fw_tb.v:488,636` が消した `GHCI_IRQ_RESULT` を使い、IRQ_STATUS に 0x180 を期待する。`sim/tb/atg_kc705_pcie_tb.v:288,300,307` が消した `GHCI_IRQ_VBLANK` を使う（両方 compile が通らない見込み、`sim/run.sh` は全 tb を流す）。(2) fw は `HW_RESULT`（`fw/cp/hw.h:33`、`packet.c:13` の comment）で IRQ_STATUS.RESULT を立てるつもりのまま。`cpu_result` は `unused_ok`（`atg_cp.v:1229-1230`）に捨てられた。代わりをどうしたかの報告が無い。(3) `.vh:389` に `GHCI_IRQ_FLIP = 3`（旧い名前を新しい bit の別名に）を足した。`.vh:390` の `GHCI_IRQ_DBG_DONE = 4` は予約の bit。(4) `atg_cp_irq.v:24-25` の comment「irq is registered」は今の組み合わせの式（:90）と違う |
| U2 CTRL | 一部 | `atg_cp_ctl.v:624,640-655,794,835`（IRQ_GLOBAL の reg、wmask 付きの書き込み、RESET の書き込みと RS_PULSE で RUN・HALT・WDT_EN・IRQ_GLOBAL を 0、CTRL の [6] に読む）、`atg_cp_irq.v:90`（`irq = ctrl_irq_global && |(status & enable)`）。IRQ_ENABLE は rst_n だけで消える（RESET で保つ） | IRQ_GLOBAL を立てずに `irq` = 1 を期待する tb: `atg_top_tb.v:725,768,789,858,900`、`atg_top_fw_tb.v`（`if (!irq)`）、`sim/verilator/harness.cpp:681`。U2 の後は FAIL する見込み（水平展開の漏れ） |
| U3 DISP2 | 一部 | `atg_cp_ctl.v:211`（`lc_state = resetting ? RESETTING : st`、`st` の S_IDLE〜S_FAULTED の値は GHCI_LC_* と同じ、`:112-116`）、`atg_cp_disp.v:128-130`（§13.2 の式どおり）。DISP_IRQ_STATUS・ENABLE を「いつでも」の組に足した（`:139-140`）。tb は `atg_cp_disp_tb.v`・`atg_top_disp_tb.v`（FAULTED で書ける、RUNNING で BAD_WRITE、HALTED で record を待つ） | `atg_cp.v:924` の `s_seq_halt` = HALTING・HALTED・FAULTED・RESETTING。eeed36ed は FAULTED・RESETTING だけ（依頼は「eeed36ed と同じ意味」）。seq は `halt` で payload の取り込みも止める（`atg_cp_seq.v:393-394,445-446`）。§5.2（ghci.md:364,570）は HALTING を「in-flight の packet を終える」とする。複数 dword の packet の途中で HALT すると、その packet が終わらずに HALTED になるか、試験で示す必要がある（RTL を読んだ見込み、ring も `go = run && !halt` で止まる `atg_cp_ring.v:661`）。`atg_cp_disp.v:17` の comment が `cp_state` のまま。`cp_state` を lifecycle の意味で使う所の一覧の報告は無い（自分で grep して、他に無かった） |
| U4 FAULT_INFO | 一部 | (c) `atg_cp_fence.v:209` = FENCE_ERROR（5）。(d) 旧い kind の名前を消し、各所を `GHCI_FAULT_KIND_*` に（下の表）。(a) `atg_cp_ctl.v:254-272` の `mc_client_to_unit`。(b) `atg_cp.v:573-587,619` の `auto_su_stage` | (a) client の番号を固定の数で書いた（0 CP、1 BLT、2 CPU、3 VF、4 TU、5 ROP、他は SU）。`atg_top.v:295-309` の番号は構成で動く（HAS_CPU = 0 なら VF が 2）。KC705（HAS_CPU・VF・TU・ROP = 1、ATOM 6、DC 7、SU 8・9）では正しいが、依頼の「parameter で作る」でない。SA は黙って SU（2）に、DC は CP（0）に。(b) SU の STAGE は推測: `cs_kick` で CS、`draw_kick` で VS、VF が止まり RX・TU が動くと FS（`:577-584`）。fault した warp の stage ではない（VS と FS の warp が同時に走る）。MS は出ない。`:587` は `x_fault_tag[11:8]` を見るが、これは source 7（他の block）の tag で SU の物でない（`:617`）。MC の経路の SU の client（D$・I$）の fault は STAGE が常に 0（`atg_cp_ctl.v:399,485`）。(e) SU の fault は `atg_top.v:678,680` で KIND 8 固定 → INVALID_OP、SESSION 0 固定。§3.5 は LDS の範囲外・64 bit の不整列を KIND 10 とする。(f) tb（`atg_cp_ctl_tb.v`）は `src_kind`・`src_stage` を tb が与えて ctl の配線だけを試す。source（fence・ring・SU）が正しい KIND・STAGE を出すかは試していない |
| U5 CMD_FENCE | 一部 | sim の経路: dword 5 を `fence_flags` に取り（`atg_cp_seq.v:1046`）、`f_irq`・`f_mem`・`f_flush`（`:601-603`）。fence は MEM_WRITE = 0 で memory に書かず FENCE_LAST と IRQ だけ（`atg_cp_fence.v:113-115,260,262,282-286`）。64 bit の {seq_hi, seq_lo} を書く（`:166,178`、§7.3 の「64-bit」に合う）。tb `atg_ghci_top_tb.v` は flags 0x3・0x1・0x2 を試す | (1) **合成の経路で `flags` を 2 つの always が駆動**（`:771` は `:751` の always、`:827`・`:885` は `:794` の always。`:771` は不要、f_irq は `fence_flags[0]` を使う）。(2) 合成の経路の HOST_FLUSH は `inv_code_r` を Q_DEC2 で（`:945`）、flags の dword を取る前に latch する。`fence_flags` は合成の経路で decode の時に 0 にしない（`:979` は sim の経路だけ）ので、**前の CMD_FENCE の flags[2]** で invalidate する。(3) HOST_FLUSH は `inv` = 4'b1111（`:543-545`）。`inv` は TU・SU の読みの cache の無効化で（`atg_top.v:1226,1438`）、dirty の書き戻し（MC の L2 `atg_mc_l2.v`、minimal は L2 4 KB、ROP の write-back）をしない見込み。§7.3（ghci.md:970）は「Flush GPU dirty caches before signaling fence」。(4) tb は seq_hi = 0 で下位の dword だけを照合する（上位の dword の書き込みを試していない） |
| U6 CP_STATE | 済み | `atg_cp_ctl.v:196,198-199`、seq に Q_BARRIER（10）と CMD_WAIT_FENCE（0x34）の HW の経路を足した。tb `atg_cp_ctl_tb.v`・`atg_cp_seq_tb.v`（seq と ctl をつないで WAIT_FENCE 中 4、BARRIER の drain 中 5） | 新設の CMD_WAIT_FENCE の比較が 32 bit（`atg_cp_fence.v:189-206`、`passed = !diff[31]`）。§7.3（ghci.md:983-984）は signed 64 bit。tb は wait_hi = 0 だけ |
| U7 DEVICE_CAPS | 未着手 | `atg_top.v:121-122`（[3]・[4] = `HAS_CPU && NSU > 0`）、`atg_cp_regs.v:279-306`（`ATG_DEF_DEVICE_CAPS` ほか）は変わっていない | 依頼 06 の U7 のまま |
| U8 SESSION_ACTIVE | 未着手 | `atg_cp_prot.v:382` は変わっていない | 依頼 06 の U8 のまま |
| U9 include | 一部 | `GHCI_CAPS`・`GHCI_CAPS2`・`GHCI_CAPS2_*`・`GHCI_CAPS_*`・`GHCI_APERTURE_*`・`GHCI_PERF0`・`GHCI_REG_FENCE_LAST`・`GHCI_FENCE_LAST`・旧い `GHCI_ST_*` の lifecycle・旧い kind の名前を消し、使う所を `GHCI_LEGACY_*` に（ca3288a9・b58d4edd）。rtl・sim・fw・tools に旧い名前は残らない（`sim/bin/` の古い build の出力を除く。上の U1 の 2 本を除く） | (1) `.h:259-262` に `GHCI_LEGACY_CAPS_ROP_STENCIL`・`TU_FULL`・`MS4`・`CORE1` を新しく足した。`sim/verilator/harness.cpp:458-468` は `GHCI_LEGACY_CAPS`（0x020）・`GHCI_LEGACY_CAPS2`（0x024）を読む。GHCI 1.0 では 0x020 は FAULT_ADDR_HI、0x024 は RING_BASE_LO（ghci.md:214-215）で、旧い CAPS の bit を GHCI 1.0 の register から読む。(2) `.vh:372` の `GHCI_ST_FAULTED`（lifecycle の名前）= 7（CP_STATE の FAULT）を `fw_harness.cpp:335` が使う。(3) 新しい別名 `GHCI_IRQ_FLIP`（上の U1）。(4) tb が `GHCI_LEGACY_APERTURE_*`（0x060・0x064 = GHCI 1.0 の TIMESTAMP_HI・GCLK_HZ、RO）に書く（`atg_top_tb.v` ほか 8 file、`harness.cpp`・`fw_harness.cpp`）。これは U10 の範囲（tb を GHCI 1.0 の機構に書き直す） |
| U10〜U15 | 範囲外 | — | 判定しない（依頼 06 の文のまま次に回す） |

### U4 の source ごとの KIND・UNIT（b58d4edd、RTL を読んだ見込み）

| source（`atg_cp_ctl` の index） | 何が起きたか | KIND | UNIT |
| --- | --- | --- | --- |
| 0 ring | header の不正・長さ | 3 BAD_REG（合成の経路は h_novf で 1） | 0 |
| 0 ring | ring の読みの応答の error | 5 AXI_ERROR（28a8d920 は 2、B25 は判断待ち） | 0 |
| 1 seq | payload の不足・SET_REG の不正な index | 3 | 0 |
| 1 seq | 未知の op | 1 INVALID_OP | 0 |
| 2 state | 走行中の locked register の書き込み | 7 LOCKED_REG | 0 |
| 3 fence | 書き込みの応答の error | 5 FENCE_ERROR | 0 |
| 4 SU | ISA の fault（`atg_top.v:678` の KIND 8 固定 → 1） | 1 | 2（STAGE は推測） |
| 5 CPU | CP の firmware の core の例外（PC の範囲外・不正な命令・不整列の jump、`atg_cpu_core.v:373-380`） | 1（28a8d920 は 4） | 0（B27） |
| 6 BLT | 不正な op ／ AXI の error | 1 ／ 5 | 6 |
| 7 他の block | `atg_top.v:677` で 0 に結ばれる | — | — |
| 8 DISP | FLIP の不正 | 3 | 0（B24） |
| MC | 保護の拒否 ／ flat の範囲外 ／ 応答の error | 10 ／ 9 ／ 5 | client の表（上の (a)） |

## minimal（KC705）の 3 register（自分で計算）

`config/minimal.mk` → `include/atg_config.vh` → `boards/kc705/atg_kc705_top.v`（HAS_CPU・VF・TU・ROP・DISP = 1、HAS_BLT = 0）→ `atg_top.v`（`HAS_INDIRECT_DRAW`・`HAS_SECONDARY_RING` = `HAS_CPU && NSU > 0`）→ `atg_cp_regs.v:720-770`。範囲で `atg_cp_regs.v` の式と `atg_top.v` は変わっていない（`atg_cp_regs.v` は FENCE_LAST の名前だけ）。RTL を読んだ見込み、sim では確かめていない。

| register | 計算 | U7 の期待 | 差 |
| --- | --- | --- | --- |
| DEVICE_CAPS | [30] DISP 1、[23:16] NUM_SU 1、[15:8] PHYS_LANES 4、[7] GSI1 1、[4] 1、[3] 1、他 0 → **0x40010498** | 0x40010480（[3]・[4] = 0）か 0x40010490（[4] だけ 1） | [3]（と [4] は試験の結論次第） |
| EXT_CAP_FEATURE | [27:20] LDS 1、[19:16] MAX_MRT 1、[15] MRT_BLEND 0、VA_BITS_ENC 0（VA 30）、PA_BITS_ENC 0（PA 40）、他 0 → **0x00110000** | 0x00110000 | なし |
| EXT_CAP_FMT0 | [15:12] 1、[9] SRGB 1（`CFG_FMT_ENABLE_SRGB` 1、B20）、他 0 → **0x00001200** | 0x00001200 | なし |

## timing の事実

- b58d4edd（と 64b18b40 以後の RTL）の build は無い。
- `build/vivado/atg_kc705_top/timing_summary.rpt`（11-12 19:26:40）: Design State **Routed**。WNS **−0.383 ns**、TNS −45.696 ns、失敗 800 / 470,589 endpoint、WHS **+0.018 ns**、THS 0。
  - 入力は `atg_kc705_top_synth.dcp`（16:39、28a8d920 の合成）を `open_checkpoint` した物（`vivado.log:108,126`）。strategy は Performance_Explore（`:104`）。b58d4edd の RTL ではない。
  - 失敗は `g_out`（200 MHz）。最悪の path は `gpu/mc/g_l2.l2/m_req_key_reg[4]/C` → `gpu/mc/g_l2.l2/state_reg[0]/CE`、10 段、配線 86%。
- `utilization.rpt`（19:25）: Slice LUT 174,546 / 203,800（85.65%）。
- `vivado.log:3227` に「Proceeding with bitstream generation (VIVADO_FORCE_BITSTREAM=1)」。`atg_kc705_top.bit`（19:30、SHA-256 `303c8e4cd738ec56abe0260333dd5326479c469d50601fc95f3cd55b1c2a414b`）は **WNS < 0 で強制した、28a8d920 の RTL の bitstream**。受け入れ 3 は未達。実機に渡さない。

## 旧い仕様の残り（U1〜U9 に関わる物）

1. 消した IRQ の名前を使い続ける tb（`atg_top_fw_tb.v:488,636` の RESULT と 0x180、`atg_kc705_pcie_tb.v:288,300,307` の VBLANK）。
2. 消した IRQ の bit に代わりの無い fw（`fw/cp/hw.h:33` の `HW_RESULT`、`packet.c:13`）。
3. IRQ_GLOBAL を知らない tb（`atg_top_tb.v` の 5 か所、`atg_top_fw_tb.v`、`harness.cpp:681`）。
4. 旧い名前を新しい意味に結ぶ別名を足した: `GHCI_IRQ_FLIP = 3`（`.vh:389`）。予約の bit の名前 `GHCI_IRQ_DBG_DONE = 4`（`.vh:390`）。lifecycle の名前 `GHCI_ST_FAULTED` = CP_STATE 7（`.vh:372`）。
5. 旧い CAPS の bit の macro を `GHCI_LEGACY_CAPS_*` として新しく足し（`.h:259-262`）、harness が GHCI 1.0 の FAULT_ADDR_HI・RING_BASE_LO を旧い CAPS・CAPS2 として読む（`harness.cpp:458-468`）。
6. 参考（範囲の前から、U10 の範囲）: `harness.cpp`・`fw_harness.cpp` は `GHCI_STATUS_STATE` を使うが `include/ghci_regs.h` に無い（28a8d920・eeed36ed にも無い）。verilator の harness は compile が通らない見込み。`harness.cpp:558,565-567` は CP_STATE を旧い lifecycle の値（1 RUNNING・4 FAULTED）で照合する。

## 規則の違反と依頼に無い変更

- commit message: 9 件とも `WIP` ちょうど（`git log --format=%B` を `cat -A` で見た）。違反なし。
- lint: 範囲で新しい `lint_off`・`-Wno-*` は無い（U13 の黙らせは範囲外で残る）。`tools/lint.sh:59-63` の board top の lint は `-DSYNTH_KINTEX7` を付けないので、`SYNTH_KINTEX7` の経路（U5 の 2 つの誤り）は sim にも lint にも掛からない。
- 依頼に無い変更: 64b18b40（`VIVADO_FORCE_BITSTREAM`、強制の bitstream を 19:30 に書いた）。ring の読みの error を KIND 2 → 5（`atg_cp_ring.v:1776,1787`、依頼 06 は B25 が決まるまで今の値のまま）。CMD_FENCE を 64 bit で書く形（§7.3 に合うので残してよい）。CMD_WAIT_FENCE の HW の経路の新設（U6 の試験のため。比較が 32 bit）。

## 回帰（依頼 06 の「済んだ物」）

- STATUS の並び、FAULT_INFO の field の位置、RING_*、TIMESTAMP、EXTCAP、FMT、CAPS の注入の撤去、AXI の ID: 範囲で関係の file の変更は見当たらない（`atg_cp_ctl.v` の STATUS の式 `:827` は同じ）。壊した物は見つからなかった（RTL を読んだ見込み）。
- 例外: 上の U1・U2 の tb は依頼 06 の受け入れ 2（既存の tb が PASS）を満たさない見込み。

## A（仕様の文が明確で RTL が違う → 依頼 07）

1. `atg_cp_seq.v` の合成の経路の `flags` の多重駆動、HOST_FLUSH の stale な flags（§7.3 ghci.md:965-971）。
2. HOST_FLUSH が dirty の cache を書き戻さない（§7.3 ghci.md:970）。
3. CMD_WAIT_FENCE の比較が 32 bit（§7.3 ghci.md:983-984）。
4. `s_seq_halt` の HALTING・HALTED（§5.2 ghci.md:364,570、確かめの依頼）。
5. FAULT_INFO: MC の client の表の固定の番号、SU の STAGE が推測と他の source の tag、MC の経路の SU の STAGE、SU の SESSION と KIND（§3.5 ghci.md:413-450）。
6. U1・U2・U9 の水平展開の漏れ（tb・fw・別名）。
7. U7・U8（未着手のまま）。

## B（仕様が黙っている → ユーザー、台帳に B27・B28 で足した）

- B27: CP の firmware の core（RV32）の例外（PC の範囲外、不正な命令、不整列の jump）の FAULT_INFO の KIND と UNIT。§3.5 の KIND・UNIT は firmware の core の fault を挙げない。RTL は KIND 1・UNIT 0（28a8d920 は KIND 4）。
- B28: CMD_WAIT_FENCE の `fence_addr` の揃え。§7.3 は CMD_FENCE の `fence_addr` だけに「8-byte aligned」と書き、CMD_WAIT_FENCE には書かない。RTL は 4 byte に揃える（`atg_cp_fence.v` の `f_op[1]` の時）。64 bit の比較で 8 byte を読むなら揃えが要る。
- 既存の行の書き換え: B24（SA の UNIT が 2、DC が 0 になった）、B25（ring の読みの error が 5 になった）。

## 未実施

- sim・lint・arith-lint・Vivado（読むだけの役割）。上の「compile が通らない」「FAIL する」は読んだ見込み。
- balanced の 3 register の計算。
- SU の FAULT_ADDR が stage の入口からの PC か（§3.5 ghci.md:450）の確かめ。
- HALTING の時に ring が packet の途中で fetch を止めるかの確かめ（U3 の残りの判断の前提）。
