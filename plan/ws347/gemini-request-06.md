<!-- awesome-plan project=zedbsd record=ws347-gemini-request-06 -->

# Gemini への作業依頼 06（まとめ版、下書き）: GHCI 1.0 の残りと回帰の直し

- 依頼元: zedBSD の Q1（Claude、監査の役。下書きは担当 P2）。2026-11-12。対象: `~/amaterrace-gpu` main 28a8d920。
- **この文書は依頼 01〜05 を全部置き換える。読むのはこの文書だけでよい。** 作業の番号は U1〜U15 に振り直した。
- 監査の記録（zedBSD 側）: `plan/ws347/audit-28a8d920.md`。

## 進め方（先に 1 次回答、その後に実装）

1. **まず実装せずに、各項目の指摘が正しいかを仕様（`docs/ghci.md`）と RTL で確かめ、1 次回答を返す。** 項目ごとに「正しい」「誤り（仕様の行を示す）」「一部（どこが違うか）」を書く。
   - 監査の側の指摘にも 15% ほど誤りがありうる（前回、STATUS の CP_STATE の行を見落とした）。誤りは遠慮なく仕様の行を示して返してほしい。
2. Q1 が 1 次回答を仕様で照らし、必要ならこの依頼を改訂する。
3. 改訂の後（または Q1 が「そのまま」と返した後）に実装し、報告を返す。

## 0. 守ってほしい事

1. `docs/ghci.md` が正。**仕様は書き換えない。** 仕様が黙っている所は推測で埋めず、報告に書く（下の「今回触らない物」はユーザーに上げた問い）。
2. 値は実際に instance した回路と board・top の parameter から作る。macro が無い時の既定は「無い」（0）。
3. **旧い仕様の名前・offset・packet・機構を、試験を通すために戻さない。** 試験が旧い物に依っていたら、試験の方を GHCI 1.0 の仕様の機構に書き直す。名前を変えた・意味を変えた物は、使う所を全部 grep して追従させる（RTL・tb・harness・fw・include・`spec/gpu/`）。
4. Verilog-2005、`rtl/library/arith/` の外に算術の演算子を書かない、Vivado は host で 1 process・2 thread、実行の後に `sync`。
5. **commit message は `WIP` ちょうど。** `Co-Authored-By` などの行を付けない（前回 95a98e15 は `Co-Authored-By` の 1 行だけ、d1ee85e4 は `WIP` の後に `Co-Authored-By`）。
6. tb の期待値は仕様の文から作る。RTL の今の式・値を写さない。
7. lint の warning は直して 0 件にする。`-Wno-*` や file 全体の `lint_off` で黙らせない。
8. 依頼に無い変更（README など）は入れない。入れた時は報告に書く。

## 済んだ物（壊さない）

- Slim の page 0（SLIM_MMIO の撤去）、各 offset を `HCR_IDX_*` の index 1 つに送る形、読みの混ざりの撤去。
- STATUS（0x010）の並び: [0] RUNNING、[1] HALTED、[2] FAULTED、[8] MC_IDLE、[11] RING_EMPTY、[12] MEM_READY、[13] AXI_STUCK、[14] QUIESCE、[19:16] CP_STATE、他は 0（`atg_cp_ctl.v:786-787`）。CP_STATE の値の列挙 0 IDLE〜7 FAULT（`GHCI_ST_IDLE`〜`GHCI_ST_FAULT`）。走行中に doorbell を待つ間は CP_STATE = 0。
- FAULT_INFO の field の位置: [3:0] KIND、[7:4] UNIT、[15:8] SESSION、[19:16] STAGE、[31:20] SYNDROME（[20] は page・perm の fault の write）。W1C。
- RING_SIZE を byte の数で（4 KiB〜16 MiB の 2 の冪、他は前の値を保つ。書き込みの条件は sim と合成で同じ）、WPTR・RPTR 22 bit、走行中の RING_*・CP_HEAP の書き込みは LOCKED_REG（kind 7）。
- 64 bit の TIMESTAMP（`TIMESTAMP_INIT` の parameter で桁上がりを試す形）、RESIDENT_WARPS、NATIVE_ISA_ID。
- EXTCAP の見出しが 0x100〜0x13F、見出しは RO、MEM（0x1E0〜）、VEND の SUBSYSTEM（汎用 module の既定 0、KC705 の board が 0x10EE・0x0007 を渡す）、PERF_SRC。
- FMT_OPTIMAL・LINEAR の bit の並び、FILTER_LINEAR は bilinear が有る時だけ、FFILT が無い時の FP16・FP32 の FILTER_LINEAR の除外、ROP_PACK が無い時の Path 2 の COLOR_ATTACHMENT の除外（`atg_cp_fmt.v:57-66`）。
- CAPS を外から注入する parameter の撤去。DEVICE_CAPS [7] GSI1 = 1 固定、[25] TEX_FFILT = FFILT だけ、SU 由来の bit（FP16・SUBGROUP・LDS）は NSU 0 で 0。macro の名前の統一（`CFG_ENABLE_SUBGROUP`・`CFG_ENABLE_ETC2`）。
- 前処理（`` `elsif ``）の直し、汎用 module の GCLK_HZ の既定 0、config.mk.template の key と VF の comment。
- minimal（KC705）の EXT_CAP_FEATURE 0x00110000、EXT_CAP_FMT0 0x00001200（DEVICE_CAPS は U7 で決まる）。
- AXI の ID（host の access で ID 1）、tb の AXI の model の ID の queue。
- 28a8d920 の誤りの直し: `atg_blt_2d.v:427` の `seg_n` の駆動、`atg_su_xu.v` の `load_res_w_lo`、`am_addsub.v` の `sub_ci`、`atg_rx_edge.v` の carry の index。
- tb の削除 `atg_cp_multi_queue_tb.v`（GHCI 1.0 に多 queue は無い）。

## 1. 回帰と driver が直に見る物（最優先）

### U1. IRQ_STATUS の並びを §3.4 に戻す（回帰、96af1397 で壊れた）

- 仕様（ghci.md §3.4、:391-418）: [0] IRQ_FENCE、[1] IRQ_FAULT、[2] IRQ_WATCHDOG、[3] IRQ_DISPLAY（DISP2 の要約）、[5] IRQ_SOFT、[6] IRQ_HALTED、他は予約（0 を読む）。DISP2 の個々の事象（VSYNC・FLIP_DONE・HOTPLUG・UNDERFLOW・DISP_ERROR）は DISP2 の `DISP_IRQ_STATUS`（0x218）で読み消す（N21）。
- 今: `atg_cp.v:1077-1078` の `ev` は [2] が `s_ring_empty`、[3] が `s_ev_wdt`、[9]〜[12] が DISP2 の事象。`atg_cp_irq.v:12-15,51,83` は [2] を RING_EMPTY の立ち上がりとして扱う（旧い map）。`dc_irq_display` は `unused_ok` に入れられた。eeed36ed は [2]・[3] が正しかった。
- 直し方: [2] = watchdog、[3] = `dc_irq_display`（DISP2 の要約）に戻す。`atg_cp_irq.v` の RING_EMPTY の立ち上がりの検出（`empty_q`）を消す。[4] DBG_DONE、[7] RESULT、[8] RETIRE、[9]〜[12] は top の IRQ_STATUS に出さない（読むと 0）。fw が RESULT（`fw/cp/hw.h:33` の `HW_RESULT`）に依るなら、どう代えるかを報告に書く。`include/ghci_regs.vh:404-407`・`.h:148-153` の VBLANK・FLIP・DISP_ERROR・HOTPLUG・RESULT・RETIRE の top の bit の名前を消す（DISP2 の名前は DISP2 の register の側に）。
- 期待値: watchdog の発火で IRQ_STATUS == 0x00000004（他の要因が無い時）。DISP2 の vsync（DISP_IRQ_ENABLE の該当 bit を 1）で IRQ_STATUS[3] == 1 と DISP_IRQ_STATUS の該当 bit。ring が空になっても IRQ_STATUS は変わらない。IRQ_SET[5] で 0x20。

### U2. CTRL.IRQ_GLOBAL と CTRL.RESET（§3.3・§3.4、範囲の前からの誤り）

- 仕様: CTRL[6] IRQ_GLOBAL = master の割り込みの許可。`irq = CTRL.IRQ_GLOBAL && ((IRQ_STATUS & IRQ_ENABLE) != 0)`（§3.4）。CTRL[0] RESET は CTRL の RUN・HALT・STEP・WDT_EN・IRQ_GLOBAL を 0 にする（§3.3）。
- 今: IRQ_GLOBAL は RTL のどこにも無い（CTRL の読みは `atg_cp_ctl.v:794` で [6] = 0、`atg_cp_irq.v:72` の irq は IRQ_GLOBAL を見ない）。RS_PULSE（`atg_cp_ctl.v:746-760`）は RUN・HALT だけを消し、WDT_EN を消さない。
- 直し方: ctl に IRQ_GLOBAL の bit を持たせ、CTRL の読みに出し、irq の線を IRQ_GLOBAL で gate する。CTRL.RESET で WDT_EN・IRQ_GLOBAL も 0 に。IRQ_ENABLE は RESET で保つ（今のまま）。
- 期待値: IRQ_ENABLE と IRQ_STATUS が立っていても IRQ_GLOBAL = 0 なら irq = 0、1 で irq = 1。CTRL に 0x60 を書いて 0x60 を読む。RESET の後に CTRL を読んで 0。

### U3. DISP2 の gate と record、seq の停止（§13.2、回帰）

- 仕様（ghci.md §13.2 の Kind の表、:2647-2655 付近）: S の register は、DISP_SET（FB_NEXT は FLIP だけ）が RUNNING・HALTING の間、host が IDLE・HALTED・FAULTED の間に書く。他の state の host の書き込みは無視して STATUS.BAD_WRITE。DISP_SET と FLIP は RUNNING・HALTING の外では何もしない。record は IDLE・RUNNING・HALTING の時に書き、HALTED・FAULTED・RESETTING の間は待つ。
- 今: `atg_cp_disp.v:118-120` は `active = !cp_open`、`host_s = cp_open`、`rec_ok = cp_state != GHCI_ST_FAULT`。`open` は FAULTED で 0 なので FAULTED の host の書き込みが BAD_WRITE になり、FAULTED・RESETTING で `active` になり、HALTED・RESETTING で record を書く。CP_STATE の意味を micro の state に変えた時に、lifecycle を使っていた所が追従していない。`atg_cp.v:900` の `s_seq_halt` も、RESETTING の全体（eeed36ed）から RS_PULSE の 1 cycle だけに縮んだ（読んだ見込み）。
- 直し方: `atg_cp_ctl` から lifecycle の state（IDLE・RUNNING・HALTING・HALTED・FAULTED・RESETTING、§5.2）を CP_STATE と別の信号で出し、`atg_cp_disp` の `active`・`host_s`・`rec_ok` と `s_seq_halt` をそれで作る（eeed36ed の式と同じ意味）。他に旧い `GHCI_ST_*` の lifecycle の意味で `cp_state` を使っていた所が無いか grep し、報告に一覧を書く。
- 期待値（tb）: FAULTED で host が FB_BASE を書けて BAD_WRITE が立たない。RUNNING で host の S の書き込みは無視され BAD_WRITE。HALTED で FLIP の record が書かれず、RUNNING に戻ると書かれる。

### U4. FAULT_INFO の中身（§3.5）

- (a) MC の経路の UNIT: `atg_cp_ctl.v:373,459` は `mc_fault_client[3:0]` をそのまま [7:4] に入れる。MC の client の番号（`atg_top.v:287-306`: 0 CP、1 BLT、2 CPU、3 VF、4 TU、5 ROP、6 ATOM、SA、EXT/DC、SU は 2 つずつ）は §3.5 の UNIT（0 CP、1 VF、2 SU、3 RX、4 TU、5 ROP、6 BLT）と違う。client → UNIT の表を parameter で作り変換する（CP・CPU → 0、BLT → 6、VF → 1、TU → 4、ROP → 5、ATOM と SU の client → 2）。SA・DC は §3.5 に UNIT が無いので、今の値を報告に書く（B24）。
- (b) SU の fault の [19:16] STAGE: 今は常に 0（`atg_cp_ctl.v:299,317,331,476`）。仕様は UNIT = 2 の時 0 VS、1 FS、2 CS、3 MS。SU から fault した warp の stage を運ぶ。運べない場合は理由を報告に。FAULT_ADDR が SU の ISA の fault で stage の入口からの PC（§3.5）になっているかも確かめて報告。
- (c) fence の書き込みの error の KIND: `atg_cp_fence.v:210` は `GHCI_FAULT_APERTURE`（今は 2）。§3.5 の 5（`GHCI_FAULT_KIND_FENCE_ERROR`、「fence memory write failure」）に。
- (d) 旧い kind の名前の撤去: `include/ghci_regs.vh:443-452` の `GHCI_FAULT_APERTURE`・`OPCODE`・`RING`・`WATCHDOG`・`AXI`・`KICK`・`LOCKED`・`ISA`・`GPU_*` を消し、使う所（`atg_cp_disp.v:196`、`atg_cp_fence.v:210`、`atg_cp_ring.v:1697,1703,1776,1787`、`atg_cp.v:593-594`、`atg_cp_seq.v:969,979,990,1001`、`atg_blt_2d.v:3351,3655`、`atg_blt_rsv.v:403,470`、`atg_blt.v:225`）を、その fault の意味に合う `GHCI_FAULT_KIND_*` に書き直す。src 5（CPU）の kind が `GHCI_FAULT_WATCHDOG` 固定（`atg_cp.v:593`）なのは、CPU の fault の意味を確かめて選び直す。**source ごとの「何が起きたか → KIND・UNIT」の表を報告に書く。** 仕様で決まらない物（ring の読みの error、MC の拒否、DISP2 の FLIP）は今の値のまま報告に（B24・B25）。
- 期待値（tb）: BLT の MC の拒否で UNIT 6、VF の拒否で UNIT 1。fence の書き込みに AXI の error を返して KIND 5・UNIT 0。ring の header の不正で KIND 3。watchdog で KIND 4。SU の不正な命令で KIND 1・UNIT 2・STAGE がその stage。

### U5. CMD_FENCE の flags（§7.3、範囲の前からの誤り）

- 仕様（ghci.md:960-972）: CMD_FENCE は 6 dword。dword 5 が flags: [0] IRQ（IRQ_FENCE を立てる）、[1] MEM_WRITE（0 なら memory に書かない、割り込みだけの fence）、[2] HOST_FLUSH（signal の前に dirty な cache を flush）。
- 今: seq は flags を header の [31:24] から取る（`atg_cp_seq.v:862,951`）。dword 5 は取り込まない（`:1011-1016` は dword 1〜3 だけ）。MEM_WRITE = 0 でも書く。`atg_ghci_top_tb.v:563` は flags = 1（IRQ だけ）で memory の書き込みを期待して PASS している。
- 直し方: dword 5 を取り込み、[0]・[1]・[2] をその意味で使う。HOST_FLUSH で flush する cache が無い構成ならそう報告。tb を直す。
- 期待値（tb）: flags = 0x3 で memory に seq が書かれ IRQ_STATUS[0] = 1。flags = 0x1 で memory は変わらず IRQ_STATUS[0] = 1。flags = 0x2 で memory に書かれ IRQ_STATUS[0] = 0。

### U6. CP_STATE の細部（§5.1）

- 仕様（ghci.md:551-562）: 4 WAIT_FENCE は CMD_WAIT_FENCE の条件の待ち。5 WAIT_MEM は MC の flush・quiesce・barrier の drain の待ち。
- 今（`atg_cp_ctl.v:190-195`）: CMD_FENCE の書き込み中（`f_busy`、seq の Q_FREQ）も WAIT_FENCE。CMD_BARRIER の drain は EXEC。
- 直し方: WAIT_FENCE は CMD_WAIT_FENCE の待ち（Q_FWAIT）だけ。CMD_FENCE の書き込みは EXEC（か WAIT_MEM。どちらにしたか報告）。barrier の drain は WAIT_MEM。
- 期待値（tb）: CMD_WAIT_FENCE で条件が満たされない間 STATUS[19:16] = 4。CMD_BARRIER の drain の間 5。

## 2. 能力・session

### U7. DEVICE_CAPS の残り（§3.2.1）

- [3] CAP_INDIRECT_DRAW: 今は `HAS_CPU && NSU > 0` で 1（`atg_top.v:121`）。GHCI 1.0 の packet の `CMD_DRAW_INDIRECT`（0x29）・`CMD_DISPATCH_INDIRECT`（0x2E）を最後まで実行する経路が見当たらない: fw の GHCI 1.0 の packet の表（`fw/cp/cmd.c:157-165`）に 0x29・0x2E が無い（`GHCI_VK_CMD_DRAW_INDIRECT` は別の command）。HW の `atg_cp_indirect` は入力が 0 に結ばれ（`atg_cp.v:1158-1166`）、出力は `unused_indirect`（`:1200-1202`）に捨てられる。実行できないなら 0。
- [4] CAP_SECONDARY_RING: fw の `fw_cmd_execute_commands_packet`（`fw/cp/ghci_cmd.c:234`）がある。**ring の試験（CMD_EXECUTE_COMMANDS で secondary の CMD_FENCE を実行して memory に書かれる）で示せたら 1、示せなければ 0。**
- [26] ROP_PACK・[28:27] MESH・TASK は macro でなく、instance した回路の parameter から（`atg_cp_regs.v:720-737`。[26] は `HAS_ROP_PACK && HAS_ROP`）。[31] MSAA は MS4 だけを見る。2 sample の構成の回路があるかを報告に。
- 使われていない `ATG_DEF_DEVICE_CAPS`・`ATG_DEF_CAPS`・`ATG_DEF_EXT_CAP_FMT0`・`ATG_DEF_EXT_CAP_FEATURE`（`atg_cp_regs.v:279-306`）を消す。
- 期待値（minimal の KC705）: [3] = 0・[4] = 0 なら DEVICE_CAPS 0x40010480、[4] だけ 1 なら 0x40010490。今の RTL は 0x40010498（監査の計算）。

### U8. SESSION_ACTIVE（§3.3・§3.5・§11.6）

- 今: `atg_cp_prot.v:382` が `{16'd0, map_v[15:1], 1'b1}` を返す（`atg_cp_regs.v:792` の 1 と OR）。
- 確かめて直す: session i（1〜15）の fault で bit i を 0 にする（§3.3「The CP immediately clears SESSION_ACTIVE[i]」）。CMD_SESSION_CLOSE で 0。
- 期待値（balanced の tb）: CMD_SESSION_OPEN(3) の後 0x00000009、session 3 の fault の後 0x00000001、STATUS.FAULTED = 0。

## 3. 旧い仕様の残り

### U9. include の別名と旧い名前

- 消す・`GHCI_LEGACY_*` の名前で旧い offset に戻す物（`include/ghci_regs.vh:122-160`、`.h:257-293`）:
  - `GHCI_CAPS`（DEVICE_CAPS に結んだが、使う所は旧い CAPS の bit の意味で読む）、`GHCI_CAPS2` = 0x084、`.h:397-401` の `GHCI_CAPS2_*` の field の macro。
  - `GHCI_APERTURE_BASE`・`LIMIT` = 0x060・0x064（TIMESTAMP_HI・GCLK_HZ と同じ offset）。
  - `GHCI_PERF0` = 0x080（EXT_CAP_FMT0 と同じ、`.vh:284`・`.h:253`）。
  - `GHCI_REG_FENCE_LAST` = 0x070（`.vh:259`・`.h:289`。仕様に無い register に GHCI 1.0 の `GHCI_REG_` の接頭辞を付けない）。
  - `.vh:386-390` の旧い lifecycle の名前（`GHCI_ST_RUNNING` = 3、`HALTED` = 0、`HALTING` = 5、`RESETTING` = 5）。
  - U4 (d) の旧い kind の名前。
- 新しい map の短い別名（`GHCI_STATUS`・`GHCI_CTRL` など、ghci.md の名前と同じ意味・同じ offset の物）は残してよい。
- 期待値: `grep` で上の名前が rtl・sim・fw・tools に残らない（`GHCI_LEGACY_*` は除く）。

### U10. 廃止の機構を戻さない（§7.5）、tb・harness を GHCI 1.0 の機構に

- 仕様（ghci.md:1194-1203）: 旧い class の gpu-op、aperture の管理は GHCI 1.0 で廃止。
- 今: tb・harness が GHCI 2.0 の packet（`GHCI_PKT_CLASS_STD` の `GHCI_OP_SET_REG`・`GHCI_OP_BLIT`・`GHCI_OP_FENCE`）を ring に置き、完了を FENCE_LAST（0x070）で待つ（`atg_kc705_top_tb.v`、`atg_top_scene.vh`、`atg_top_tb.v`、`atg_top_fw_tb.v`、`atg_top_cs_tb.v`、`atg_top_disp_tb.v`、`atg_top_mesh_tb.v`、`atg_top_prot_tb.v(h)`、`sim/verilator/harness.cpp`、`fw_harness.cpp`）。そのために 96af1397 で 0x070 の host の読み書き（`atg_cp_regs.v:415,432,587`、`atg_cp_fence.v:87,109,256-257,326`）と `APERTURE_BASE_INIT`・`APERTURE_LIMIT_INIT`（`atg_cp_ctl.v:56-57,599-600`、`atg_cp.v`、`atg_top.v`）を足した。`harness.cpp:458-468` は旧い CAPS の bit を別名で新しい register に読んで照合する。
- 直し方:
  - tb・harness を §7.3 の packet（`CMD_FILL_BUFFER`・`CMD_COPY_BUFFER`・`CMD_COPY_IMAGE`・`CMD_BLIT`・`CMD_DRAW*`・`CMD_FENCE` など）に書き直し、完了は CMD_FENCE の memory の書き込み（と IRQ_FENCE）で待つ。BLT の無い構成（minimal）は BLT の packet を試さない。
  - `harness.cpp` の能力の照合を DEVICE_CAPS・EXT_CAP_FEATURE・EXT_CAP_FMT0 の §3.2 の bit に書き直す。
  - `APERTURE_*_INIT` を消す。aperture の検査を残すなら固定の全域。範囲の検査が要る所は §11 の MEM（HOST_LIMIT・local の窓）で。どうしたか報告。
  - 0x070 の host の読み書きは、tb が使わなくなったら消す（0x070 が何を返すべきかは B21 で決まるまで、消して 0 を読む形でよい）。
- 期待値: 上の tb・harness が旧い packet・FENCE_LAST・aperture を使わずに PASS。

### U11. 死んだ論理と comment（依頼 05 の S7 の残り）

- ring の queue 1・2・DMA・preempt・sema（`atg_cp_ring.v:10-14,53` ほか、0 を入れて `unused_ok` に入れただけの物）、dbg の本体のうち GHCI 1.0 で使わない物、KICK の死んだ論理（`atg_cp_ctl.v:99,215-217,378-383,463-466,488` の `host_kick`・`kick_bad`・`kick_absent`）、`unused_ok` の古い幅（ring `:1888`）を消す。
- `atg_cp_ctl.v` の先頭の comment から KICK・aperture の記述を消す（U10 の後）。

## 4. 試験・lint・timing・文書

### U12. tb（`sim/tb/atg_ghci_top_tb.v` ほか）

1. **期待値を仕様の式で**: DEVICE_CAPS [3]・[4]（`:117-118`、今は RTL と同じ式）を U7 の結論で。EXT_CAP_FMT0 [11] を 0 固定にせず（`:129`）、[10]・[11] を §3.2.2 の式で、[9] は macro が無い時 0（`:131`）。FMT_OPTIMAL・LINEAR の 0xEF・0xED（`:496,498`）を §4.1 の bit の定義と構成から計算。MEM_CACHE_CFG を §11 の MEM の表の式で。
2. **EXTCAP**: 「DBG」の検査が ID 1（DISP）を見ている（`:461`）。仕様の DBG は ID 2。DBG を list に載せるなら指す先は 0。PERF_SRC の 4 つの値、CPFW、DISP2（DISP_IRQ_STATUS・ENABLE を含む）の register を照合。
3. **STATUS・FAULT_INFO・IRQ**: STATUS の予約の bit が 0、HALT の後の HALTED・QUIESCE、packet の取り込み中の CP_STATE（FETCH 以降）。U1・U2・U4・U5・U6・U8 の期待値。
4. **preset の仕組み**: `make test-ghci-presets`（`Makefile:92-93`）は `config.mk`・`include/atg_config.vh` を上書きし balanced のまま終わる。続く `make vivado` が balanced を build する。preset ごとの header を `build/` の下の別の directory に作り `-I` で渡す形にし、`config.mk` を変えない。
5. **KC705 の board と同じ DUT**: `atg_ghci_top_tb` に board の CPU の firmware（`CPU_IMEM_INIT`・`CPU_DMEM_INIT`・`IMEM_WORDS` 32768・`DMEM_WORDS` 16384）を渡す。`atg_kc705_top_tb.v:68-69` の `HAS_BLT(1)`・`HAS_L2(0)` の上書きをやめ、minimal の board の形で試す（BLT が要る試験は balanced で）。`:323` の DEVICE_CAPS を mask でなく全 bit で。
6. 試験で消した・書き直した tb の一覧を報告に。

### U13. lint と sim・合成の一致

- `tools/lint.sh:20` と `sim/verilator/run.sh:34` の `-Wno-UNUSED -Wno-UNDRIVEN -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-DECLFILENAME -Wno-PINCONNECTEMPTY -Wno-VARHIDDEN` を戻す（eeed36ed の形）。`atg_rop_blend.v:23`・`atg_su_sched.v:21` の file の終わりまで効く `lint_off BLKSEQ` を消し、順序回路の中の blocking の代入を直す。warning を直して 0 件に。どうしても残す物は、行を限った `lint_off`/`lint_on` と理由を報告に。
- `atg_rx_setup.v:65-76` の `ifndef SYNTHESIS` の initial（scissor の配列を sim だけ 0・0x7FFF に）: 合成の版は reset の無い register で FPGA の初期値 0。sim と実機で動きが違う。reset で初期化するか、sim の initial を消して書かれる前に読まれない事を示す。
- `tools/arith-lint.py` 0 件。

### U14. timing（minimal の KC705、route の後）

- 事実（`build/vivado/atg_kc705_top/timing_summary.rpt`、11-12 17:27、Routed）: WNS −0.753 ns、TNS −3841.917 ns、19,014 endpoint（全部 `g_out` 200 MHz）、WHS +0.018 ns。最悪の path は `gpu/g_tu.tu/cache/st_reg[1]_rep__0/C` → `gpu/mc/tree/rr_ptr_reg[*]/R`（reset の pin、8 段、配線 85%）。LUT 85.65%。bitstream は無い。
- 求める物: minimal の KC705 で route の後 **WNS ≥ 0・WHS ≥ 0** と bitstream。`VIVADO_FORCE_BITSTREAM=1`（64b18b40）で作った bitstream は受け入れに使わない（作ったなら報告に「WNS < 0 で強制」と明記し、実機に渡さない）。
- `tools/vivado-build.sh` の既定の timeout（1800 秒、`:15,62`）が KC705 の build に足りない件: 既定を延ばすか、使い方の comment に KC705 の値（例 5400）を書く。どちらにしたか報告。

### U15. `spec/gpu/` の文書（RTL の直しの後、最後に）

- `atg_cp_regs.md` から RTL に無い物（PERF_CTRL 0x160、PERF_SRC 0x168..、CPFW_DOORBELL 0x1C0・RESET 0x1C4、WINDOW_OFFSET 0x1F4（RW）。0x1F4 は ghci.md で MEM_CACHE_CFG（RO））を消し、RTL と ghci.md に合わせる。
- `atg_cp_fence.md:75`（FENCE_LAST を host が書く）など、U1〜U10 で変えた物の記述。STATUS・FAULT_INFO・IRQ_STATUS・CTRL の並び。

## 今回触らない物（仕様の問いとしてユーザーに上げた・上げる）

- CPFW の 11 個の register が §4.1 の CPFW の窓に入らない件（今の配置 0x170・0x1A0〜0x1BC のまま）。SA（id 7）の指す先 0x140。
- RING_SIZE のリセット値、size を変えた時の WPTR・RPTR、RING_BASE_HI が fetch に効かない件、RING_BASE の整列の mask（今 32 byte）。
- TIMESTAMP の LO・HI の読みの原子性、VEND の VENDOR_ID の値、PERF0〜3 の counter の置き場所、MEM_CACHE_CFG の field の幅と minimal の値、FMT_BUFFER の並び、SESSION_ACTIVE のリセットの関係、DBG の窓。
- 新しい問い（監査 28a8d920）: `CONFIG_FMT_ENABLE_FP16`・`FP32`・`SRGB` がどの回路も切り替えない件（EXT_CAP_FMT0 [9]〜[11] は今の式のまま）。page 0 の仕様に無い offset の読み（0x070 を含む）。PROT の時の LOCKED_REG で FAULTED を立てるか（今のまま）。ROP_PACK = 0 の時の Path 2 の BLIT_DST・BLEND（今のまま）。DISP2 の FLIP の不正の KIND・UNIT、MC の拒否と ring の読みの error の KIND（今のまま）。PERF_SRC の「STATUS の busy の bit の番号」（今のまま）。

## 受け入れ

1. U12 の tb が minimal と balanced で全部 PASS（仕様の式の期待値、KC705 の board と同じ DUT、`config.mk` を変えない preset の仕組みで）。
2. 既存の tb・harness が旧い packet・FENCE_LAST・aperture・旧い CAPS の別名を使わずに PASS。`tools/lint.sh`（warning を黙らせない形）・`tools/arith-lint.py` 0 件。
3. minimal の KC705 で route の後 WNS ≥ 0・WHS ≥ 0、bitstream ができる（強制でなく）。
4. `git diff` で `docs/ghci.md` が変わっていない。commit message が全部 `WIP` ちょうど。

## 報告の書き方

- 1 次回答: U1〜U15 の各項目に「正しい・誤り（仕様の行）・一部」と短い理由。
- 実装の報告: U1〜U15 のそれぞれ、済み・一部・未着手、変えた file、確かめた試験（tb が何をどう照合したか）。
- commit の SHA、試験のコマンドと件数、minimal・balanced の 3 register の 16 進、route の後の WNS・WHS、LUT、bitstream の path と SHA-256。
- U4 の source ごとの KIND・UNIT の表、U3 の `cp_state` を使う所の一覧、U7 の [3]・[4] の判断と根拠（試験の名前）、U10 で書き直した・消した tb の一覧、決められなかった所。
- 順: U1・U2・U3 → U5 → U4 → U6 → U9・U10 → U7・U8 → U11 → U12 → U13 → U14 → U15。区切りごとに commit してよい。
