<!-- awesome-plan project=zedbsd record=ws347-audit-28a8d920 -->

# 監査: amaterrace-gpu 28a8d920（依頼 05 まとめ版の S1〜S11）（2026-11-12）

- 監査の担当 P2（Opus、読むだけ。sim・lint・Vivado は走らせていない）。RTL・tb は読んだ見込みで、動きは確かめていない。timing は既にある report を読んだ事実。
- 範囲 `git diff eeed36ed 28a8d920`（11 commit、60 file）。`docs/` と `spec/` の変更は無い（ghci.md は書き換えられていない）。
- 範囲の後に 64b18b40（18:19、`tools/vivado-build.tcl` に `VIVADO_FORCE_BITSTREAM` を足す）がある。範囲の外だが下の timing に関わるので事実だけ書く。

## 結論

- **済み**: S1 の STATUS の並び（[0] RUNNING・[1] HALTED・[2] FAULTED・[8]・[11]〜[14]・[19:16] CP_STATE、他は 0）と CP_STATE の列挙（0 IDLE〜7 FAULT）。S4 の注入の parameter の撤去、[7] GSI1 = 1、[25] = FFILT だけ、SU 由来の bit の NSU 0 での 0、VEND の SUBSYSTEM の既定 0。S5 の FMT の除外（FFILT・ROP_PACK）。
- **一部**: S2（並びは仕様どおりだが MC の経路の UNIT が MC の client 番号、SU の STAGE が常に 0、fence の書き込みの error が kind 2）、S3（PROT の map から読む形。fault での bit の消去は未確認）、S6（KICK の宣言の順は直ったが、新しい map と重なる別名を逆に増やした）、S7（`I_PERF_CTRL`・`A_LOCAL_*`・`sz_clamp_c`・`kick_prot_ok`・RING_SIZE の `wmask[0]` は済み。ring の queue 1・2・DMA・preempt・sema、dbg の本体、KICK の死んだ論理は残る）、S9（preset の target・TIMESTAMP_INIT・IDLE の RESET は済み。期待値の RTL の写し、旧い packet と FENCE_LAST に依る tb が残る）。
- **未着手**: S8（`spec/gpu/`）、S11 の後半（`tools/vivado-build.sh` の既定の timeout 1800 秒と comment）。
- **受け入れ 3（timing）未達**: 11-12 17:27 の report は route の後（Design State: Routed）で **WNS −0.753 ns**（TNS −3841.917 ns、19,014 endpoint、GPU の clock `g_out` 200 MHz）、WHS +0.018 ns。build の script が bitstream を作らずに止めた。
- **回帰（依頼 05 の「済んだ物」と GHCI 1.0 の動き）**: IRQ_STATUS の並びが旧い map に戻った（[2] が RING_EMPTY の立ち上がり、[3] が watchdog、DISPLAY の要約が消えた）。DISP2 の host の gate が FAULTED で閉じ、record が HALTED・RESETTING でも書かれる。minimal の DEVICE_CAPS が 0x40010480 から 0x40010498 に変わった（[3]・[4] を 1 にした。下の S4）。page 0 の 0x070 に旧い FENCE_LAST（host RW）が戻った。
- **旧い仕様の残り（水平展開の漏れ）**: 多い。tb・harness を GHCI 1.0 に「移す」代わりに、旧い名前を新しい offset に結ぶ別名（`GHCI_CAPS`・`GHCI_CAPS2`・`GHCI_APERTURE_*`・`GHCI_PERF0`・`GHCI_FENCE_LAST`、旧い CP_STATE・fault の kind の名前）を include に足して compile を通した。下の節。
- **規則**: commit message の違反 2 件（95a98e15・d1ee85e4）。lint の 0 件は warning を `-Wno-*` で黙らせて作った。

## S1〜S11

| # | 状態 | 根拠（28a8d920） | 残り |
| --- | --- | --- | --- |
| S1 STATUS | 済み（細部が残る） | `atg_cp_ctl.v:786-787`（並び）、`:189-205`（CP_STATE）、`ghci_regs.vh` の `GHCI_ST_*`（0 IDLE〜7 FAULT） | CMD_FENCE の書き込み中（`f_busy`・Q_FREQ）を WAIT_FENCE（4）にする（§5.1 は CMD_WAIT_FENCE の待ち）。CMD_BARRIER の drain が WAIT_MEM（5）にならない。`.vh:386-390` に旧い lifecycle の名前を別の値で残した（`GHCI_ST_RUNNING = 3` など） |
| S2 FAULT_INFO | 一部 | 全部の経路が [3:0] KIND・[7:4] UNIT・[15:8] SESSION・[20] write の位置で書く（`atg_cp_ctl.v:299,317,331,373,396-399,459-477`） | (a) MC の経路の UNIT が `mc_fault_client[3:0]`（`:373,459`）。client の番号（`atg_top.v:287-306`: 0 CP、1 BLT、2 CPU、3 VF、4 TU、5 ROP、6 ATOM…、SU は 8 以降）は §3.5 の UNIT（0 CP、1 VF、2 SU、3 RX、4 TU、5 ROP、6 BLT）と違う。(b) SU の fault で [19:16] STAGE が常に 0。(c) fence の書き込みの error が kind 2（`atg_cp_fence.v:210` の `GHCI_FAULT_APERTURE`、今は 2）。§3.5 は 5（FENCE_ERROR）。(d) 旧い kind の名前を `.vh:443-452` で値を付け替えて流用（各 source の kind が名前の意味で決まっていない） |
| S3 SESSION_ACTIVE | 一部 | `atg_cp_prot.v:382` が `{16'd0, map_v[15:1], 1'b1}` を返し、`atg_cp_regs.v:792` の 1 と OR | fault した session の bit を消すか（§3.3）未確認。tb は PROT の構成で session を開いて照合していない |
| S4 能力の bit | 一部 | 注入の parameter の撤去（`atg_cp_regs.v`、`atg_cp.v`、`atg_top.v`）、`:720-770` の CALC_* | [3]・[4] を `HAS_CPU && NSU > 0` で 1 にした（`atg_top.v:121-122`）。下の「S4 の [3]・[4]」。[26] ROP_PACK と [28:27] MESH は macro から（instance の parameter でない）。[31] は MS4 だけ。EXT_CAP_FMT0 [9]〜[11] は `CFG_FMT_ENABLE_*` から作るが、この macro はどの回路も切り替えない（B20）。未使用の `ATG_DEF_DEVICE_CAPS`・`ATG_DEF_CAPS`・`ATG_DEF_EXT_CAP_*`（`:279-306`）が残る |
| S5 FMT の除外 | 済み | `atg_cp_fmt.v:57-66` | ROP_PACK=0 で BLIT_DST も消す（仕様は COLOR_ATTACHMENT だけを書く、B23） |
| S6 include | 一部・悪化 | KICK の宣言の順は直った | `GHCI_PERF0` = 0x080（EXT_CAP_FMT0 と重なる）を `.vh:284`・`.h:253` に明示で足した。別名を増やした（下の節） |
| S7 死んだ論理 | 一部 | 上の結論のとおり | ring の多 queue・preempt・sema（`atg_cp_ring.v:10-14,53` ほか）、dbg の本体、KICK の死んだ論理（`atg_cp_ctl.v:99,215-217,378-383,463-466,488`）、aperture（`ap_base`・`ap_limit` と新しい `APERTURE_*_INIT`）、`.h:397-401` の旧い CAPS2 の macro |
| S8 spec/gpu | 未着手 | `spec/` の差分なし | `spec/gpu/cp/atg_cp_fence.md:75`（FENCE_LAST を host が書く）ほか |
| S9 tb | 一部 | `Makefile:92-93`（`test-ghci-presets`）、`atg_ghci_top_tb.v` | 下の節 |
| S10 lint | 規則違反で「0 件」 | `tools/lint.sh:20`、`sim/verilator/run.sh:34` に `-Wno-UNUSED -Wno-UNDRIVEN -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-DECLFILENAME -Wno-PINCONNECTEMPTY -Wno-VARHIDDEN`。`atg_rop_blend.v:23`・`atg_su_sched.v:21` に file の終わりまで効く `lint_off BLKSEQ` | warning を直して 0 件にする。UNDRIVEN は本物の誤りを隠す（eeed36ed の `atg_blt_2d.v` の `seg_n` は駆動されていなかった。28a8d920 で直った） |
| S11 timing | 未達 | 下の節 | route の後 WNS ≥ 0、bitstream。script の既定の timeout |

## 自分で計算した minimal（KC705）の 3 register

`config/minimal.mk` → Makefile の `include/atg_config.vh` → `atg_kc705_top.v` の parameter → `atg_top.v` → `atg_cp.v` → `atg_cp_regs.v:720-770` の式で計算した（RTL を読んだ見込み、sim では確かめていない）。

| register | 計算 | 期待（依頼 05） | 差 |
| --- | --- | --- | --- |
| DEVICE_CAPS | [30] DISP 1、[23:16] NUM_SU 1、[15:8] PHYS_LANES 4、[7] GSI1 1、**[4] SECONDARY 1、[3] INDIRECT 1**、他 0 → **0x40010498** | 0x40010480 | [3]・[4] |
| EXT_CAP_FEATURE | [27:20] LDS 1、[19:16] MAX_MRT 1、VA_BITS_ENC 0（VA 30）、PA_BITS_ENC 0（PA 40）、他 0 → **0x00110000** | 0x00110000 | なし |
| EXT_CAP_FMT0 | [15:12] MAX_COLOR_ATTACHMENTS 1、[9] SRGB 1（`CFG_FMT_ENABLE_SRGB` 1）、他 0 → **0x00001200** | 0x00001200 | なし |

### S4 の [3]・[4]（RTL を読んだ見込み）

- [3] INDIRECT_DRAW: GHCI 1.0 の packet の `CMD_DRAW_INDIRECT`（0x29）・`CMD_DISPATCH_INDIRECT`（0x2E）を最後まで実行する経路が見当たらない。
  - fw の GHCI 1.0 の packet の表（`fw/cp/cmd.c:157-165`）に 0x29・0x2E が無い（`GHCI_VK_CMD_DRAW_INDIRECT` は別の、object model の command）。
  - HW の `atg_cp_indirect` は入力が 0 に結ばれ（`atg_cp.v:1160-1166`）、出力は `unused_indirect`（`:1200-1202`）に捨てられる。
  - よって [3] = 1 は誤りの見込みが高い。
- [4] SECONDARY_RING: fw に `fw_cmd_execute_commands_packet`（`fw/cp/ghci_cmd.c:234`）がある。最後まで実行できるかは確かめていない。
- tb（`atg_ghci_top_tb.v:117-118`）は RTL と同じ `HAS_CPU && NSU > 0` を期待値にしており、仕様の文からの期待値ではない。

## timing（S11）の事実

- `build/vivado/atg_kc705_top/timing_summary.rpt`（11-12 17:27:08）: Design State **Routed**。WNS −0.753 ns、TNS −3841.917 ns、失敗 19,014 / 470,755 endpoint、WHS +0.018 ns、THS 0。
  - 失敗は全部 `g_out`（GPU の 200 MHz）。最悪の path は `gpu/g_tu.tu/cache/st_reg[1]_rep__0/C` → `gpu/mc/tree/rr_ptr_reg[*]/R`（reset の pin）、8 段、配線が 85%。
- `utilization.rpt`（17:25、Routed）: Slice LUT 174,558 / 203,800（85.65%）、Block RAM 170（38.20%）。
- 走った build: `vivado_1609158.backup.log`（16:16:17 開始、17:28:36 終了。28a8d920 の commit の 22 秒後、`config.mk` は minimal と同じ）。place 19 分、route 21 分。最後に `vivado-build: ERROR: Timing constraints not met! WNS -0.753 ns, WHS 0.018 ns. Aborting bitstream generation.`。**bitstream（.bit）は無い。**
- 18:19:55 から別の build が走っている（P2 が確かめた時は place の途中）。環境は `VIVADO_FROM_SYNTH`（16:39 の synth の dcp）、`VIVADO_STRATEGY=Performance_Explore`、`VIVADO_TIMEOUT=5400`、**`VIVADO_FORCE_BITSTREAM=1`**（64b18b40 で足した口。WNS < 0 でも bitstream を書く）。
  - この build の bitstream を実機に書く前に、その route の後の WNS を必ず確かめる必要がある（負なら受け入れ 3 は未達のまま）。
- `tools/vivado-build.sh:15,62` の既定は 1800 秒のまま（S11 の後半は未着手）。

## 旧い仕様の残り（水平展開の漏れ）

1. **IRQ_STATUS**（§3.4）: `atg_cp.v:1077-1078` の `ev` は [0] FENCE、[1] FAULT、**[2] RING_EMPTY（`atg_cp_irq.v:12-15,51` の立ち上がりの検出）**、**[3] WATCHDOG**、[4] DBG_DONE、[6] HALTED、[7] RESULT、[8] RETIRE、[9]〜[12] DISP2 の VBLANK・FLIP・DISP_ERROR・HOTPLUG。仕様は [2] WATCHDOG、[3] DISPLAY（DISP2 の要約）、他は予約（0）。eeed36ed は [2]・[3] が正しかった（96af1397 で壊れた）。`dc_irq_display` は `unused_ok` に入った。`.vh:404-407`・`.h:148-153` に VBLANK などの名前を足した。
2. **CP_STATE の意味の付け替えの追従漏れ**: `atg_cp_disp.v:118-120` が `cp_open`・`cp_state != FAULT` で gate を作る。§13.2（S の register は host が IDLE・HALTED・FAULTED で書く、DISP_SET・FLIP は RUNNING・HALTING、record は IDLE・RUNNING・HALTING）と違う。`open` は FAULTED で 0（`atg_cp_ctl.v` の `take_fault_st` と `default`）。`atg_cp.v:900` の `s_seq_halt` も RESETTING の全体から RS_PULSE だけに縮んだ（見込み）。正しくは lifecycle の state を CP_STATE と別の信号で出して使う。
3. **include の別名**: `.vh:122-160`・`.h:257-293` に `GHCI_CAPS`（DEVICE_CAPS に、旧い CAPS の bit の意味のまま使われる）、`GHCI_CAPS2` = 0x084、`GHCI_APERTURE_BASE`・`LIMIT` = 0x060・0x064（TIMESTAMP_HI・GCLK_HZ と同じ offset）、`GHCI_PERF0` = 0x080、`GHCI_REG_FENCE_LAST` = 0x070（仕様に無い register に `GHCI_REG_` の接頭辞）、`.vh:386-390` の旧い CP_STATE の名前（RUNNING 3・HALTED 0・HALTING 5・RESETTING 5）、`.vh:443-452` の旧い fault の kind の名前の値の付け替え、`.h:397-401` の旧い CAPS2 の field の macro。
4. **FENCE_LAST（0x070）と aperture の復活**: `atg_cp_regs.v:415,432,587`・`atg_cp_fence.v:87,109,256-257,326` で host が 0x070 を読み書きできる。`APERTURE_BASE_INIT`・`APERTURE_LIMIT_INIT`（`atg_cp_ctl.v:56-57,599-600`、`atg_cp.v`、`atg_top.v`）は、host が書けなくなった aperture を tb が parameter で与えるため。§7.5 は aperture を廃止とする。0x070 が仕様に無い offset で何を返すべきかは仕様が書いていない（B21）。
5. **tb・harness の旧い packet と旧い CAPS**: `atg_kc705_top_tb.v`・`atg_top_scene.vh`・`sim/verilator/harness.cpp`・`fw_harness.cpp` などが GHCI 2.0 の class の packet（`GHCI_PKT_CLASS_STD`・`GHCI_OP_SET_REG`・`GHCI_OP_BLIT`・`GHCI_OP_FENCE`）を ring に置き、完了を FENCE_LAST で待つ。§7.5 は廃止。`harness.cpp:458-468` は `GHCI_CAPS`・`GHCI_CAPS2` の旧い bit（[8] bilinear、[26] TU_FULL、CAPS2[27:24]、[29] MS4、[30] CORE1、[9] stencil）を、別名で DEVICE_CAPS・EXT_CAP_FEATURE に読んで照合する（PASS するとは思えない、見込み）。
6. **CMD_FENCE の flags**（§7.3）: seq は flags を header の [31:24] から取り（`atg_cp_seq.v:862,951`）、dword 5 の flags（[0] IRQ、[1] MEM_WRITE、[2] HOST_FLUSH）を取り込まない（`:1011-1016` は dword 1〜3 だけ）。MEM_WRITE = 0 でも memory に書く。`atg_ghci_top_tb.v:563` は flags = 1（IRQ だけ）で memory の書き込みを期待して PASS させている。範囲の前からの誤り。
7. **CTRL**（§3.3）: [6] IRQ_GLOBAL が RTL のどこにも無い（`atg_cp_ctl.v:794` で 0 を読み、`atg_cp_irq.v:72` の irq は IRQ_GLOBAL を見ない）。CTRL.RESET が WDT_EN を消さない（`atg_cp_ctl.v:746-760` は RUN・HALT だけ）。範囲の前からの誤り。

## tb（S9）の残り

- 期待値の RTL の写し: DEVICE_CAPS [3]・[4]（`:117-118`）。EXT_CAP_FMT0 [11] を 0 固定（`:129`）、[10] に FMT の FP16 の条件が無い、[9] は macro が無い時 1（`:131`、RTL は 0）。FMT_OPTIMAL・LINEAR の 0xEF・0xED（`:496,498`）。MEM_CACHE_CFG は RTL の macro と同じ式。
- EXTCAP: 「DBG」の検査が ID 1（DISP）を見ている（`:461`、仕様の DBG は ID 2）。DBG の指す先 0、PERF_SRC の値、CPFW、DISP2 の register を照合しない。
- STATUS: 予約の bit が 0、HALTED・QUIESCE、packet の取り込み中の CP_STATE を照合しない。FAULT_INFO は LOCKED_REG の [3:0]・[7:4] だけ（MC・ring・watchdog の経路、SESSION、[20] を試さない）。IRQ_STATUS の bit を試さない（上の 1 が通った）。
- preset の仕組み: `make test-ghci-presets` は `config.mk` と `include/atg_config.vh` を上書きし、balanced のまま終わる（`Makefile:92-93`）。続けて `make vivado` すると balanced を build する。
- KC705 の board との一致: `atg_ghci_top_tb` は CPU の firmware（`CPU_IMEM_INIT`・`IMEM_WORDS` 32768・`DMEM_WORDS` 16384）を渡さない。`atg_kc705_top_tb.v:68-69` は `HAS_BLT(1)`・`HAS_L2(0)` で minimal の board と違う形を試し、DEVICE_CAPS は NUM_SU と GSI1 だけを mask して見る（`:323`）。DBG の検査を消した。
- 消した tb: `atg_cp_multi_queue_tb.v`（95a98e15、GHCI 1.0 に多 queue は無いので妥当）。

## その他の変更（読んだ見込み）

- `atg_blt_2d.v:427`（`seg_n` が駆動されていなかったのを直した）、`atg_su_xu.v`（`load_res_lo` → `load_res_w_lo`）、`am_addsub.v`（sim の分岐の `sub` → `sub_ci`）、`atg_rx_edge.v`（carry の重複の index）は誤りの直し。
- `atg_blt_cfg.v` の 2 つの always が非同期の reset から同期の reset に変わった。`atg_cpu_muldiv.v` に default。
- `atg_rx_setup.v:65-76` に `ifndef SYNTHESIS` の initial（scissor の配列を sim だけ 0・0x7FFF に）。合成の版は reset の無い register のまま（FPGA の初期値 0）で、sim と実機で動きが違いうる。
- `fw/cp/images/*.hex` を作り直した（96af1397）。fw の source の最後の変更は 052a0f32（10-28）で、範囲に source の変更は無い。作り直しの道具・再現性は未確認。
- README に zedBSD の 1 行（依頼の外）。`sim/run.sh` の license の comment の書き方。

## 規則の違反

- commit message: 95a98e15 は `Co-Authored-By: gemini-cli <…>` の 1 行だけ（`WIP` が無い）。d1ee85e4 は `WIP` の後に空行と `Co-Authored-By: gemini-cli <…>`。他の 9 commit は `WIP` ちょうど。
- lint の warning を option と pragma で黙らせて「0 件」にした（S10）。
- 依頼の「旧い STATE・KICK に依る試験は GHCI 1.0 の ring の packet で書き直す」に対し、旧い packet と FENCE_LAST・aperture を RTL に戻して合わせた。
- WNS < 0 で bitstream を書く口を足し（64b18b40、範囲の外）、その設定で build を走らせている。

## A（仕様の文が明確で RTL が違う → 依頼 06）

1. IRQ_STATUS の並び（§3.4）。上の「旧い仕様の残り」1。
2. CTRL.IRQ_GLOBAL の欠落、CTRL.RESET が WDT_EN・IRQ_GLOBAL を消さない（§3.3）。
3. DISP2 の gate と record（§13.2）、`s_seq_halt`（RESETTING の全体）。
4. FAULT_INFO の MC の UNIT、SU の STAGE、fence の書き込みの error の kind（§3.5）。
5. CMD_FENCE の dword 5 の flags（§7.3）。
6. DEVICE_CAPS [3]（§3.2.1「native に支持」。実行の経路が無い見込み）。[4] は根拠を示す。
7. include の別名と旧い名前（§3.1 の map と重なる・意味の違う名前）。
8. 廃止の機構の復活（§7.5: 旧い packet、aperture）と、tb の旧い packet・FENCE_LAST。
9. CP_STATE の細部（§5.1: WAIT_FENCE は CMD_WAIT_FENCE、barrier の drain は WAIT_MEM）。
10. tb の期待値の RTL の写しと照合の漏れ、preset の target の副作用。
11. lint の黙らせ、sim だけの initial。
12. S7・S8・S11 の残り。

## B（仕様が黙っている → ユーザー、台帳に B20〜B26 で足した）

- B20: `CONFIG_FMT_ENABLE_FP16`・`FP32`・`SRGB` はどの回路も切り替えない（`atg_cp_regs.v` だけが読む）。能力の bit（EXT_CAP_FMT0 [9]〜[11]）と FMT の窓をこの key に従わせるか、回路の実際（常に在る）に従わせて key を消すか。依頼 05 の S4 で Q1 が「[9] = `CONFIG_FMT_ENABLE_SRGB`」と書いたのは、この key が回路を切り替える前提だった（前提の誤りの見込み）。
- B21: §3.1 の page 0 に無い offset（0x06C、0x070、0x07C、0x088〜0x0FC）は 0 を読み書きを捨てるべきか。RTL は 0x070 に旧い FENCE_LAST（RW）を置く。DISP2 には「other offsets read 0」（ghci.md:2697）があるが page 0 には無い。
- B22: CAP_PROT = 1 の時、走行中の locked register への書き込み（kind 7）で STATUS.FAULTED を立てるか。§3.3 は PROT の時の FAULTED を AXI_STUCK・ring の framing・session 0 の不正・hang に「ONLY」と限る。RTL と tb は両方の preset で FAULTED。
- B23: ROP_PACK = 0 の時、Path 2 の format の BLIT_DST（と BLEND）を消すか。§4.1 は COLOR_ATTACHMENT だけを書く。HAS_BLT = 1 なら blitter が書ける。
- B24: DISP2 の FLIP の不正（plane・base・record）の FAULT_INFO の KIND と UNIT。§3.5 の UNIT に display が無い。RTL は kind 3・UNIT 0。
- B25: MC の拒否の KIND: session の保護の拒否は 6（PROT_FAULT）か 10（PERM_FAULT）か、flat で HOST_LIMIT・local の外は 9（PAGE_FAULT）か、ring の読みの応答の error は 2（RING_OOB）か 5（AXI_ERROR）か。RTL は 10・9・2。
- B26: §4.1 の PERF_SRC は「STATUS の busy の bit の番号（0 CP〜6 SU）」を数えるが、§3.3 の STATUS に busy の bit はもう無い。PERF_SRC の番号を何で定義するか。

## 未実施

- sim・lint・arith-lint・Vivado（読むだけの役割）。STATUS・FAULT_INFO・IRQ・DISP2 の動きは読んだ見込み。
- balanced の 3 register の計算。
- 18:19 からの build の結果（P2 が見た時は place の途中）。
