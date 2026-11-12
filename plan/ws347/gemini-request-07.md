<!-- awesome-plan project=zedbsd record=ws347-gemini-request-07 -->

# Gemini への作業依頼 07（まとめ版、下書き）: U1〜U9 の残りと、そこで見つかった誤り

- 依頼元: zedBSD の Q1（Claude、監査の役。下書きは担当 P3）。2026-11-12。対象: `~/amaterrace-gpu` main b58d4edd。
- **この文書は依頼 01〜06 を置き換える。読むのはこの文書だけでよい。** 作業の番号は V1〜V9 に振り直した。
- 今回の範囲は、依頼 06 の U1〜U9 の残りと、その監査で見つかった誤りだけ。**依頼 06 の U10〜U15（tb・harness の GHCI 1.0 化、死んだ論理、tb の期待値、lint、timing、`spec/gpu/`）は今回は触らない。** 次の依頼で、依頼 06 の文のまま（番号を振り直して）出す。
- 監査の記録（zedBSD 側）: `plan/ws347/audit-b58d4edd.md`。

## 場所・規則・読む物（文脈の無い process で起動された時もこの節から始める）

- 作業する repo: `/home/awe/amaterrace-gpu`（branch `main`）。push しない。
- 仕様の正本: `/home/awe/amaterrace-gpu/docs/ghci.md`。**書き換えない。**
- 最初に読む物: この文書、`docs/ghci.md` の §3.3（CTRL と STATUS、:328-389）・§3.4（IRQ、:391-411）・§3.5（FAULT_INFO、:413-450）・§5.2（:564-575）・§7.3 の `CMD_FENCE`・`CMD_WAIT_FENCE`（:960-991）・§13.2 の Kind の表（:2640-2655）。下の各項目が指す RTL の file:line。
- 進み具合の記録: 区切り（1 次回答を書いた・項目を 1 つ終えた・試験を流した・commit した）ごとに、`/home/awe/amaterrace-gpu/build/agy/request-07.md` に書き足す（時刻、した事、commit の SHA、試験のコマンドと件数、残り）。前の行は消さない。次の process はこの file を読んで続きから始める。
- Vivado は host で同時に 1 process・2 thread、実行の後に `sync`。他の Vivado が動いていれば待つ。

## 進め方（先に 1 次回答、その後に実装）

1. **まず実装せずに、各項目の指摘が正しいかを仕様（`docs/ghci.md`）と RTL で確かめ、1 次回答を返す。** 項目ごとに「正しい」「誤り（仕様の行を示す）」「一部（どこが違うか）」を書く。
   - 監査の側の指摘にも 15% ほど誤りがありうる。誤りは遠慮なく仕様の行を示して返してほしい。特に V3 は監査の側の確信が低い（確かめの依頼）。
2. Q1 が 1 次回答を仕様で照らし、必要ならこの依頼を改訂する。
3. 改訂の後（または Q1 が「そのまま」と返した後）に実装し、報告を返す。

## 0. 守ってほしい事

1. `docs/ghci.md` が正。**仕様は書き換えない。** 仕様が黙っている所は推測で埋めず、報告に書く（下の「今回触らない物」はユーザーに上げた問い）。
2. 値は実際に instance した回路と board・top の parameter から作る。macro が無い時の既定は「無い」（0）。
3. **旧い仕様の名前・offset・機構を、試験を通すために戻さない・別名で残さない。** 名前を消したら、使う所を全部 grep して追従させる（rtl・sim/tb・sim/verilator・fw・include・tools）。新しい別名（旧い名前を新しい意味に結ぶ物、`GHCI_LEGACY_*` の新しい macro）を足さない。
4. **`SYNTH_KINTEX7` の経路と sim の経路の両方を直す。** sim と lint はふつう `SYNTH_KINTEX7` を定義しないので、合成の経路の誤りは試験に出ない。直した module は `verilator --lint-only -Wall -DSYNTH_KINTEX7 -Iinclude sim/lint/xilinx_stubs.v <file>` でも通す。
5. Verilog-2005、`rtl/library/arith/` の外に算術の演算子を書かない。
6. **commit message は `WIP` ちょうど。** `Co-Authored-By` などの行を付けない（前回は守られていた）。
7. tb の期待値は仕様の文から作る。RTL の今の式・値を写さない。tb が答えを入力として与える形（例: `src_kind` を tb が FENCE_ERROR にして FENCE_ERROR を期待する）は、配線の試験にはなるが source の試験にならない。source の試験は top の tb で。
8. 依頼に無い変更は入れない。入れた時は報告に書く。B の行（今回触らない物）の値は、ユーザーが決めるまで今の値のまま変えない。

## 済んだ物（壊さない）

- 依頼 06 までの済んだ物: Slim の page 0、`HCR_IDX_*` の index、STATUS（0x010）の並び（[0] RUNNING、[1] HALTED、[2] FAULTED、[8] MC_IDLE、[11] RING_EMPTY、[12] MEM_READY、[13] AXI_STUCK、[14] QUIESCE、[19:16] CP_STATE、他は 0）、FAULT_INFO の field の位置と W1C、RING_SIZE・WPTR・RPTR・LOCKED_REG、64 bit の TIMESTAMP、RESIDENT_WARPS、NATIVE_ISA_ID、EXTCAP の見出しと MEM・VEND・PERF_SRC、FMT の bit の並びと除外、CAPS の注入の撤去、[7] GSI1・[25]・SU 由来の bit、前処理、GCLK_HZ の既定、minimal の EXT_CAP_FEATURE 0x00110000・EXT_CAP_FMT0 0x00001200、AXI の ID、28a8d920 の誤りの直し（`seg_n`・`load_res_w_lo`・`sub_ci`・`atg_rx_edge.v` の carry）、`atg_cp_multi_queue_tb.v` の削除。
- b58d4edd で済んだ物:
  - IRQ_STATUS の並び（`atg_cp.v:1104`、`atg_cp_irq.v`）: [0] FENCE、[1] FAULT、[2] WATCHDOG、[3] DISPLAY（DISP2 の要約）、[5] SOFT、[6] HALTED、他は 0。RING_EMPTY の検出の撤去。
  - CTRL.IRQ_GLOBAL（`atg_cp_ctl.v:624,640-655,835`）と `irq` の gate（`atg_cp_irq.v:90`）、CTRL.RESET で RUN・HALT・WDT_EN・IRQ_GLOBAL を 0（IRQ_ENABLE は保つ）。
  - lifecycle の state を `lc_state` で出し（`atg_cp_ctl.v:211`）、DISP2 の `active`・`host_s`・`rec_ok` をそれで作る形（`atg_cp_disp.v:128-130`）。
  - fence の書き込みの error が KIND 5（`atg_cp_fence.v:209`）。旧い kind の名前（`GHCI_FAULT_APERTURE` ほか）の撤去。
  - CMD_FENCE の dword 5 の flags の取り込み（sim の経路、`atg_cp_seq.v:1043-1048`）、MEM_WRITE = 0 で memory に書かない形、64 bit の {seq_hi, seq_lo} の書き込み（`atg_cp_fence.v:166,178`）。
  - CP_STATE: CMD_FENCE の書き込みは EXEC、CMD_WAIT_FENCE の待ちだけ WAIT_FENCE、CMD_BARRIER の drain は WAIT_MEM（`atg_cp_ctl.v:196-199`、seq の Q_BARRIER）。
  - include の旧い名前の撤去（`GHCI_CAPS`・`GHCI_CAPS2`・`GHCI_APERTURE_*`・`GHCI_PERF0`・`GHCI_REG_FENCE_LAST`・旧い lifecycle の `GHCI_ST_*`）。

## 1. 合成の経路の誤り（最優先、KC705 の合成が壊れる見込み）

### V1. `atg_cp_seq.v` の `flags` の多重駆動と、HOST_FLUSH（§7.3）

- 仕様（ghci.md:960-972）: CMD_FENCE の dword 5 の [2] HOST_FLUSH は「Flush GPU dirty caches before signaling fence」。
- 今:
  - (a) `SYNTH_KINTEX7` の時、`flags` を 2 つの always が駆動する: `:771`（`:751` の always、`if (fflags_we_b[0]) flags <= in_data[7:0];`）と `:827`・`:885`（`:794` の always）。`:771` は要らない（合成の経路の `f_irq` は `fence_flags[0]` を使う、`:590`）。
  - (b) 合成の経路の `inv_code_r` は Q_DEC2 で latch する（`:936-945`）。その時はまだ dword 5 を取っていない。しかも合成の経路の `fence_flags` は decode の時に 0 にならない（0 にするのは sim の経路の `:979` だけ）。よって HOST_FLUSH は**前の CMD_FENCE の flags[2]** で決まる。sim の経路は `exec_go` の時の `inv_code_dec`（`:555`）なので正しい。
  - (c) HOST_FLUSH は `inv` = 4'b1111（`:543-546`）に結ばれる。`inv` は TU と SU の読みの cache の無効化（`atg_top.v:1226,1438`）で、dirty の書き戻しではない見込み。minimal の KC705 は MC の L2（`atg_mc_l2.v`、`CONFIG_MC_L2_CACHE_KB = 4`、dirty の line を持つ）と ROP の write-back（`atg_rop_wb.v`・`atg_rop_cache.v`）を持つ。
- 直し方: (a) `:771` を消す。(b) 合成の経路でも HOST_FLUSH の判断を dword 5 を取った後の値で行う（fence の要求の前に flush を起こし、その完了を待ってから fence を書く）。(c) HOST_FLUSH で、構成にある dirty を持つ cache（L2、ROP の write-back）を書き戻し、完了の後に fence を書く。無効化だけで足りる理由があるなら、その理由（どの cache が dirty を持たないか）を報告に。
- 期待値（tb、sim と `-DSYNTH_KINTEX7` の lint）:
  - `verilator --lint-only -Wall -DSYNTH_KINTEX7` で `atg_cp_seq.v` に MULTIDRIVEN が出ない。
  - flags = 0x7 の CMD_FENCE の前に ROP（か L2）に書いた値が、fence の値が memory に見えた時点で memory にある。
  - flags = 0x4 の CMD_FENCE の次の flags = 0x3 の CMD_FENCE で flush が起きない（前の flags を引きずらない。合成の経路の形を sim でも試せるよう、`SYNTH_KINTEX7` の経路を通す tb か、両経路の同じ試験を）。
  - seq_hi ≠ 0（例 0x00000001）の CMD_FENCE で、`fence_addr + 4` に seq_hi が書かれる（今の tb は seq_hi = 0 で下位の dword だけを照合）。

### V2. CMD_WAIT_FENCE の 64 bit の比較（§7.3）

- 仕様（ghci.md:974-991）: `(int64_t)(current_seq - wait_seq) >= 0` で通る。dword 3・4 が `wait_value` の下位・上位。
- 今: b58d4edd で CMD_WAIT_FENCE（0x34）を seq の HW の経路に足した（`atg_cp_seq.v` の `fence_op`・`is_fence_cmd_r`、Q_FREQ → Q_FWAIT）。fence の比較は 32 bit（`atg_cp_fence.v:189-206`、`am_sub #(.W(32))`、`passed = !diff[31]`）で、`f_aux`（wait の上位）を使わない。読みの address は 4 byte に揃える。
- 直し方: memory の 64 bit（下位・上位の 2 word）を読み、`{hi, lo} − {wait_hi, wait_lo}` の 64 bit の差の符号で判定する（`rtl/library/arith/` の減算器で）。揃えは B28 が決まるまで 8 byte に揃えた読みでよい（CMD_FENCE の `fence_addr` と同じ）。どうしたか報告に。
- 期待値（tb）: memory = 0x00000001_00000000、wait = 0x00000000_00000001 → 待たずに通る（32 bit では待ってしまう）。memory = 0x00000000_00000005、wait = 0x00000001_00000001 → 待つ（32 bit では通ってしまう）。待つ間 STATUS[19:16] = 4。

## 2. U1〜U4 の残り

### V3. HALTING の間の seq の停止（§5.2、1 次回答で確定）

- 仕様（ghci.md:364）: HALTING は「CTRL.HALT == 1 だが CP は in-flight の packet・draw を終えている途中」。:570「current command が終わり memory の access が静まったら HALTED」。
- 今（1 次回答の解析で確定）: CTRL.HALT = 1 で `s_seq_halt`（`atg_cp.v:924`）が立ち、seq が `Q_PAY` でも `n_ready = 0`（`atg_cp_seq.v:393,445`）で payload を拒む。ring は `R_N` で止まり `ring_busy` が落ちず、`atg_cp_ctl.v:754` の HALTED への遷移が起きない。**packet の途中の HALT で HALTING のまま固まる。**
- 直し方: `s_seq_halt` から HALTING を外し（HALTED・FAULTED・RESETTING だけ）、HALTING の間は今の packet の payload を取り込んで実行を終え、packet の境で次の header を取らない。
- 期待値（tb）: CMD_FENCE（6 dword）の 3 dword 目を ring に置いた所で CTRL.HALT = 1、残りの 3 dword を置く → fence の値が memory に書かれてから STATUS.HALTED = 1。その後の packet は HALT = 0 まで実行されない。

### V4. FAULT_INFO の中身（§3.5）

- 仕様（ghci.md:413-450）: [7:4] UNIT（0 CP、1 VF、2 SU、3 RX、4 TU、5 ROP、6 BLT）。[15:8] SESSION_ID。[19:16] SHADER_STAGE は UNIT = 2 の時「the active shader pipeline stage」（0 VS、1 FS、2 CS、3 MS）。KIND 10 は「LDS out-of-bounds, or misaligned 64-bit access」も含む。FAULT_ADDR は SU の ISA の fault で「stage の入口からの PC」（:450）。
- 今と直し方:
  - (a) MC の client → UNIT（`atg_cp_ctl.v:254-272` の `mc_client_to_unit`）は固定の番号（0 CP、1 BLT、2 CPU、3 VF、4 TU、5 ROP、他は SU）。`atg_top.v:295-309` の番号は構成で動く（HAS_CPU = 0 なら VF が 2、HAS_TU = 0 なら ROP が 4）。`atg_top` の `C_CPU`・`C_VF`・`C_TU`・`C_ROP`・`C_ATOM`・`C_SA`・`C_DC`・`C_SU` を parameter で `atg_cp` → `atg_cp_ctl` に渡し、表をそれから作る。SA・DC の UNIT は B24 が決まるまで今の値（SA 2、DC 0）のまま。
  - (b) SU の STAGE は推測（`atg_cp.v:573-584` の `auto_su_stage`: `cs_kick` で CS、`draw_kick` で VS、VF が止まり RX・TU が動くと FS）。VS と FS の warp は同時に走るので、fault した warp の stage にならない。MS が出ない。`:587` は `x_fault_tag[11:8]` を見るが、これは source 7（他の block）の tag（`:617`）で SU の物でない。直し方: SU が fault した warp の stage を出し（warp ごとに stage を持つ）、`x_fault` の経路で運ぶ。推測の `auto_su_stage` と `:587` の式を消す。運べないなら理由を報告に。
  - (c) MC の経路の SU の client（D$・I$）の fault は STAGE が常に 0（`atg_cp_ctl.v:399,485` の `4'd0`）。UNIT = 2 の時は (b) の stage を入れる。
  - (d) SU の fault の KIND と SESSION は `atg_top.v:678,680` で KIND 8 固定（→ INVALID_OP）と tag 0 固定。**1 次回答で確定: 今の SU が検出するのは未定義の命令（`atg_su_sb.v:774` の `ill`）だけなので KIND 1。SESSION は SU が warp ごとに持たないので、CP が追っている今の session を入れる（Q1 の判断）。LDS の範囲外・不整列の検出（仕様 §12.6 は KIND 10）は今回は実装せず、検出の回路を足した時の LUT の見積りを報告に書く。** 以下は元の文:SU が区別できる fault（不正な命令、LDS の範囲外、不整列）を KIND 1・10 に分け、SESSION は fault した warp の session を入れる。SU が区別できない物は報告に。
  - (e) SU の FAULT_ADDR が stage の入口からの PC になっているかを確かめ、報告に。
  - CPU（CP の firmware の core）の fault の KIND（今 1）は B27、ring の読みの error の KIND（今 5）は B25 が決まるまで今のまま。
- 期待値（top の tb、tb が kind・stage を与えない形で）: FS の shader に不正な命令 → FAULT_INFO の KIND 1・UNIT 2・STAGE 1、FAULT_ADDR = FS の入口からの PC。CS で同じ → STAGE 2。VS と FS が同時に走る draw で FS の warp が fault → STAGE 1。fence の書き込みに AXI の error → KIND 5・UNIT 0（source の fence から）。HAS_CPU = 0 の構成（tb）で VF の MC の拒否 → UNIT 1。

### V5. U1 の水平展開の漏れ（§3.4）

- 消した名前を使う tb（compile が通らない見込み）:
  - `sim/tb/atg_top_fw_tb.v:488,636`: `GHCI_IRQ_RESULT` と IRQ_STATUS に 0x180（[7]・[8]）を期待。§3.4 で [7]・[8] は予約（0）。
  - `sim/tb/atg_kc705_pcie_tb.v:288,300,307`: `GHCI_IRQ_VBLANK`。DISP2 の DISP_IRQ_ENABLE の VSYNC（[0]）を 1 にして、IRQ_STATUS[3] と DISP_IRQ_STATUS[0] を見る形に書き直す。
- fw: `fw/cp/hw.h:33` の `HW_RESULT`（「raise IRQ_STATUS.RESULT」）と `fw/cp/packet.c:13` の comment。RTL は `cpu_result` を捨てる（`atg_cp.v:1229-1230` の `unused_ok`）。fw の結果の通知を GHCI 1.0 の機構（CMD_FENCE の memory の書き込みと IRQ_FENCE）に代えるか、`HW_RESULT` を使わない形にし、どうしたか報告に。`fw/cp/images/*.hex` を作り直したら、作り直しのコマンドを報告に。
- include: `.vh:389` の `GHCI_IRQ_FLIP = 3`（旧い名前を新しい bit に結ぶ別名）と `.vh:390` の `GHCI_IRQ_DBG_DONE = 4`（[4] は予約）を消す。
- comment: `atg_cp_irq.v:24-25` の「irq is registered」を今の形（`:90` の組み合わせの式）に合わせる。
- 期待値: `grep -rnw 'GHCI_IRQ_RESULT\|GHCI_IRQ_RETIRE\|GHCI_IRQ_VBLANK\|GHCI_IRQ_FLIP\|GHCI_IRQ_DISP_ERROR\|GHCI_IRQ_HOTPLUG\|GHCI_IRQ_DBG_DONE' rtl sim/tb sim/verilator fw include tools` が 0 件。`sim/run.sh atg_top_fw atg_kc705_pcie` が compile して PASS。

### V6. U2 の水平展開の漏れ（§3.3・§3.4）

- 仕様: `irq = CTRL.IRQ_GLOBAL && ((IRQ_STATUS & IRQ_ENABLE) != 0)`（ghci.md:393-394）。driver の初期化は IRQ_GLOBAL を 1 にする（:2855）。
- 今: IRQ_GLOBAL を立てずに `irq` = 1 を期待する tb: `sim/tb/atg_top_tb.v:725,768,789,858,900`、`sim/tb/atg_top_fw_tb.v`（`if (!irq)`）、`sim/verilator/harness.cpp:681`。
- 直し方: これらの tb の初期化で CTRL.IRQ_GLOBAL を 1 にする（RUN と同じ書き込みで）。CTRL.RESET の後は 0 に戻るので、RESET の後にも立て直す。top の tb に「IRQ_GLOBAL = 0 で IRQ_STATUS & IRQ_ENABLE ≠ 0 でも irq = 0」を 1 つ足す。
- 期待値: 上の tb が PASS。

## 3. 能力・session（依頼 06 の U7・U8、未着手のまま）

### V7. DEVICE_CAPS の残り（§3.2.1）

- [3] CAP_INDIRECT_DRAW: 今は `HAS_CPU && NSU > 0` で 1（`atg_top.v:121`）。GHCI 1.0 の packet の `CMD_DRAW_INDIRECT`（0x29）・`CMD_DISPATCH_INDIRECT`（0x2E）を最後まで実行する経路が見当たらない: fw の GHCI 1.0 の packet の表（`fw/cp/cmd.c:157-165`）に 0x29・0x2E が無い。HW の `atg_cp_indirect` は入力が 0 に結ばれ、出力は `unused_indirect`（`atg_cp.v` の `unused_ok`）に捨てられる。実行できないなら 0。
- [4] CAP_SECONDARY_RING（`atg_top.v:122`）: fw の `fw_cmd_execute_commands_packet`（`fw/cp/ghci_cmd.c:234`）がある。**ring の試験（CMD_EXECUTE_COMMANDS で secondary の CMD_FENCE を実行して memory に書かれる）で示せたら 1、示せなければ 0。**
- [26] ROP_PACK・[28:27] MESH・TASK は macro（`ATG_DEF_ROP_PACK`・`ATG_DEF_MESH`）でなく、instance した回路の parameter から（`atg_cp_regs.v:720-737`。[26] は `HAS_ROP_PACK && HAS_ROP`）。[31] MSAA は MS4 だけを見る。2 sample の構成の回路があるかを報告に。
- 使われていない `ATG_DEF_DEVICE_CAPS`・`ATG_DEF_CAPS`・`ATG_DEF_EXT_CAP_FMT0`・`ATG_DEF_EXT_CAP_FEATURE`（`atg_cp_regs.v:279-306`）を消す。
- 期待値（minimal の KC705）: [3] = 0・[4] = 0 なら DEVICE_CAPS 0x40010480、[4] だけ 1 なら 0x40010490。今の RTL は 0x40010498（監査の計算）。tb の期待値は §3.2.1 の文と構成から作る（`atg_ghci_top_tb.v:117-118` の今の式は RTL の写し）。

### V8. SESSION_ACTIVE（§3.3・§3.5・§11.6）

- 今: `atg_cp_prot.v:382` が `{16'd0, map_v[15:1], 1'b1}` を返す。
- 仕様: §3.5（ghci.md:447-448）「bit i は session i が open で有効な時 1、inactive か fault で止まった時 0」。§3.3 は session i（1〜15）の fault で SESSION_ACTIVE[i] を直ちに 0 にする。
- 確かめて直す: session i の fault で bit i を 0 にする。CMD_SESSION_CLOSE で 0。
- 期待値（balanced の tb）: CMD_SESSION_OPEN(3) の後 0x00000009、session 3 の fault の後 0x00000001、STATUS.FAULTED = 0。

## 4. include の残り

### V9. U9 の残り

- `include/ghci_regs.h:259-262` に足した `GHCI_LEGACY_CAPS_ROP_STENCIL`・`TU_FULL`・`MS4`・`CORE1` を消す。これを使う `sim/verilator/harness.cpp:458-468` は `GHCI_LEGACY_CAPS`（0x020）・`GHCI_LEGACY_CAPS2`（0x024）を読む。GHCI 1.0 では 0x020 は FAULT_ADDR_HI、0x024 は RING_BASE_LO（ghci.md:214-215）で、旧い CAPS の bit の照合は意味を持たない。この 5 つの照合を §3.2 の DEVICE_CAPS・EXT_CAP_FEATURE・EXT_CAP_FMT0 の bit（と FMT の窓）の照合に書き直す（harness の他の部分は U10 で次に）。
- `.vh:372` の `GHCI_ST_FAULTED`（lifecycle の名前を CP_STATE 7 に結ぶ）を消し、使う所（`sim/verilator/fw_harness.cpp:335`）を `GHCI_ST_FAULT`（CP_STATE）か STATUS.FAULTED（[2]）に。
- 期待値: `grep -rnw 'GHCI_LEGACY_CAPS_[A-Z0-9_]*\|GHCI_ST_FAULTED' rtl sim/tb sim/verilator fw include tools` が 0 件。

## 今回触らない物

- 依頼 06 の U10〜U15（次の依頼で出す）: tb・harness の旧い packet・FENCE_LAST・aperture の書き直しと `APERTURE_*_INIT` の撤去（U10）、死んだ論理（U11）、tb の期待値と preset の仕組み・KC705 の board と同じ DUT（U12）、lint の `-Wno-*` の撤去と sim だけの initial（U13）、timing の収束と `tools/vivado-build.sh` の timeout（U14）、`spec/gpu/` の文書（U15）。
  - ただし V1〜V9 の試験に要る範囲で tb を直すのはよい。
  - timing の事実（監査 b58d4edd）: 最新の report は 28a8d920 の合成からの route で WNS −0.383 ns、WHS +0.018 ns。`build/vivado/atg_kc705_top/atg_kc705_top.bit`（11-12 19:30）は `VIVADO_FORCE_BITSTREAM=1` で強制した 28a8d920 の物で、受け入れに使えない。
- 仕様の問い（ユーザーの判断待ち。今の値のまま）: CPFW の register の配置、SA の指す先、RING_SIZE のリセット値ほか ring の細部、TIMESTAMP の読みの原子性、VEND の VENDOR_ID、PERF0〜3 の置き場所、MEM_CACHE_CFG、FMT_BUFFER、SESSION_ACTIVE のリセット、DBG の窓、`CONFIG_FMT_ENABLE_*`、page 0 の仕様に無い offset（0x070 を含む）、PROT の時の LOCKED_REG の FAULTED、ROP_PACK = 0 の BLIT_DST・BLEND、DISP2 の FLIP の不正の KIND・UNIT と SA・DC の UNIT、MC の拒否と ring の読みの error の KIND、PERF_SRC の番号。
  - 新しい問い（監査 b58d4edd）: CP の firmware の core の例外の KIND・UNIT（今 1・0）。CMD_WAIT_FENCE の `fence_addr` の揃え（V2 は 8 byte で仮に）。

## 受け入れ

1. V1〜V9 の tb が minimal と balanced で PASS（仕様の式の期待値、source の試験は top の tb で）。
2. `sim/run.sh` で、V5・V6 で挙げた tb（`atg_top_fw`・`atg_kc705_pcie`・`atg_top`）が compile して PASS。他の tb は今の状態より悪くならない（流した tb と件数を報告に）。
3. 直した module が `-DSYNTH_KINTEX7` の lint でも MULTIDRIVEN などを出さない。`tools/arith-lint.py` 0 件。
4. `git diff` で `docs/ghci.md` が変わっていない。commit message が全部 `WIP` ちょうど。
5. KC705 の bitstream は今回の受け入れに含めない（U14 は次）。作ったなら route の後の WNS・WHS を報告に書き、WNS < 0 の物は「強制」と明記して実機に渡さない。

## 報告の書き方

- 1 次回答: V1〜V9 の各項目に「正しい・誤り（仕様の行）・一部」と短い理由。V3 は RTL で確かめた結果（ring が packet の境で止まるか、HALTING で payload が止まるか）。
- 実装の報告: V1〜V9 のそれぞれ、済み・一部・未着手、変えた file、確かめた試験（tb が何をどう照合したか）。
- commit の SHA、試験のコマンドと件数、`-DSYNTH_KINTEX7` の lint の結果、minimal・balanced の 3 register の 16 進。
- V1 の HOST_FLUSH で書き戻す cache の一覧、V2 の揃えの扱い、V4 の source ごとの「何が起きたか → KIND・UNIT・STAGE・SESSION」の表、V5 の fw の結果の通知をどうしたか、V7 の [3]・[4] の判断と根拠（試験の名前）、決められなかった所。
- 順: V1 → V2 → V5・V6 → V3 → V4 → V9 → V7・V8。区切りごとに commit し、`build/agy/request-07.md` に書き足してよい。
