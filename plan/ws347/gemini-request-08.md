<!-- awesome-plan project=zedbsd record=ws347-gemini-request-08 -->

# Gemini への作業依頼 08（まとめ版、下書き）: V1〜V9 の残りと、その監査で見つかった誤り

- 依頼元: zedBSD の Q1（Claude、監査の役。下書きは担当 P3）。2026-11-13。対象: `~/amaterrace-gpu` main fe6431b1。
- **この文書は依頼 01〜07 を置き換える。読むのはこの文書だけでよい。** 作業の番号は X1〜X9 に振り直した（各項目に元の V の番号を書いた）。
- 今回の範囲は、依頼 07 の V1〜V9 の残りと、その監査で見つかった誤りだけ。**依頼 06 の U10〜U15（tb・harness の GHCI 1.0 化、死んだ論理、tb の期待値と preset、lint の `-Wno-*`、timing、`spec/gpu/`）は今回も触らない**（X1 の回帰を先に直すため。次の依頼で出す）。
- 監査の記録（zedBSD 側）: `plan/ws347/audit-fe6431b1.md`（所見 A1〜A15）。

## 場所・規則・読む物（文脈の無い process で起動された時もこの節から始める）

- 作業する repo: `/home/awe/amaterrace-gpu`（branch `main`）。push しない。
- 仕様の正本: `/home/awe/amaterrace-gpu/docs/ghci.md`。**書き換えない。**
- 最初に読む物: この文書、`docs/ghci.md` の §3.3（:328-389、特に :363-366 の lifecycle と :368-389 の session の fault）・§3.5（:413-450）・§5.2（:564-575）・§7.3 の `CMD_FENCE`・`CMD_WAIT_FENCE`（:960-991）。下の各項目が指す RTL の file:line（fe6431b1 の行）。
- 進み具合の記録: 区切り（1 次回答を書いた・項目を 1 つ終えた・試験を流した・commit した）ごとに、`/home/awe/amaterrace-gpu/build/agy/request-08.md` に書き足す（時刻、した事、commit の SHA、試験のコマンドと件数、残り）。前の行は消さない。次の process はこの file を読んで続きから始める。
- Vivado は host で同時に 1 process・2 thread、実行の後に `sync`。他の Vivado が動いていれば待つ。

## 進め方（先に 1 次回答、その後に実装）

1. **まず実装せずに、各項目の指摘が正しいかを仕様と RTL で確かめ、1 次回答を返す。** 項目ごとに「正しい」「誤り（仕様の行・RTL の行を示す）」「一部（どこが違うか）」を書く。
   - 監査の側の指摘にも 15% ほど誤りがありうる。誤りは遠慮なく仕様の行を示して返してほしい。**特に X3 と X5 (c) は監査の側の確信が低い**（確かめの依頼）。
2. Q1 が 1 次回答を仕様で照らし、必要ならこの依頼を改訂する。
3. 改訂の後（または Q1 が「そのまま」と返した後）に実装し、報告を返す。

## 0. 守ってほしい事

1. `docs/ghci.md` が正。**仕様は書き換えない。** 仕様が黙っている所は推測で埋めず、報告に書く（下の「今回触らない物」はユーザーに上げた問い）。
2. 値は実際に instance した回路と board・top の parameter から作る。macro が無い時の既定は「無い」（0）。
3. **旧い仕様の名前・offset・機構を、試験を通すために戻さない・別名で残さない。** 名前を消したら、使う所を全部 grep して追従させる（rtl・sim/tb・sim/verilator・fw・include・tools）。新しい別名（旧い名前を新しい意味に結ぶ物）を足さない（C の `#define` も同じ）。
4. **`SYNTH_KINTEX7` の経路と sim の経路の両方を直す。** sim と lint はふつう `SYNTH_KINTEX7` を定義しないので、合成の経路の誤りは試験に出ない。直した module は `verilator --lint-only -Wall -DSYNTH_KINTEX7 -Iinclude sim/lint/xilinx_stubs.v <file>` でも通す。**報告に lint のコマンドと出力の全文（warning の行）を貼る。**
5. Verilog-2005、`rtl/library/arith/` の外に算術の演算子を書かない。
6. **commit message は `WIP` ちょうど。** `Co-Authored-By` などの行を付けない（前回は守られていた）。
7. tb の期待値は仕様の文と構成（`config/*.mk`、board top の parameter）から作る。RTL の今の式・値を写さない。**依頼した試験を足していない時は「足していない」と書く**（前回は足していない試験を「追加した」と報告した所がある。下の X7）。
8. 依頼に無い変更は入れない。入れた時は報告に書く。**tb の照合を消す・弱める時は、消した行と理由を報告に書く**（前回 `atg_top_prot_tb.v` の「a is not written」を理由なしに消した。下の X5）。B の行（今回触らない物）の値は、ユーザーが決めるまで今の値のまま変えない。

## 済んだ物（壊さない）

- 依頼 07 までの済んだ物（Slim の page 0、`HCR_IDX_*`、STATUS の並び、FAULT_INFO の field と W1C、RING_*、LOCKED_REG、64 bit の TIMESTAMP、RESIDENT_WARPS、NATIVE_ISA_ID、EXTCAP の見出しと MEM・VEND・PERF_SRC、FMT の並び、CAPS の注入の撤去、[7] GSI1、前処理、GCLK_HZ、minimal の EXT_CAP_FEATURE 0x00110000・EXT_CAP_FMT0 0x00001200、AXI の ID、IRQ_STATUS の並び、CTRL.IRQ_GLOBAL と irq の gate、CTRL.RESET の消す bit、`lc_state` と DISP2、fence の書き込みの error の KIND 5、CP_STATE の EXEC・WAIT_FENCE・WAIT_MEM、include の旧い名前の撤去）。
- fe6431b1 で済んだ物:
  - V2: CMD_WAIT_FENCE の 64 bit の比較（`atg_cp_fence.v` の `f_64`・`f_wide_r`・`am_sub #(.W(64))`、8 byte に揃えた読み）。
  - V5: `GHCI_IRQ_RESULT` ほかの名前の撤去（grep 0 件）、`atg_kc705_pcie_tb.v` の DISP2 の VSYNC、fw の `HW_RESULT` の撤去と hex の作り直し。
  - V1 (a): `atg_cp_seq.v` の `flags` の多重駆動の撤去、合成の経路で dword 5 を取った後に HOST_FLUSH を決める形（`:942-952`）、`fence_flags` を decode で 0 に（`:778`）。CMD_FENCE の seq_hi の書き込み（tb で照合済み）。
  - V3 の HALTING の直し（HALTING の間は今の packet の payload を取る）。**ただし X1 の回帰を直す。**
  - V4 (a)(b)(d): MC の client → UNIT を `C_*` の parameter から、SU の warp ごとの stage（`atg_su.v:1270-1283`）、SU の KIND 1・SESSION は `prot_unit_tag`、`auto_su_stage` の撤去。
  - V6: tb・harness の CTRL.IRQ_GLOBAL。
  - V7: [3] = 0、[26] = `HAS_ROP_PACK && HAS_ROP`、`ATG_DEF_DEVICE_CAPS` ほか 4 つの撤去。
  - V8: session の fault で SESSION_ACTIVE[i] = 0（`atg_cp_prot.v:174-176`）、STATUS.FAULTED を立てない（`atg_cp_ctl.v:541,732`）。
  - V9: `GHCI_LEGACY_CAPS_*`・`.vh` の `GHCI_ST_FAULTED` の撤去。

## 1. 回帰（最優先）

### X1. seq が `halt` を全く見なくなった（元 V3、監査 A4・A14）

- 仕様: HALTING は「CTRL.HALT == 1 だが in-flight の packet を終えている途中」、HALTED は「Engine has fully stopped execution」（ghci.md:363-365）。FAULTED は「halts all pipeline blocks」（:389）。§5.2（:570）。
- 今: fe6431b1 で `atg_cp.v:938` の `s_seq_halt` から HALTING を外したのは依頼どおり。だが同じ commit（31c7039c）で `atg_cp_seq.v` の `hdr_take`（`:328`）、`n_rdy_idle`・`fw_rdy_idle`・`n_rdy_pay`・`fw_rdy_pay`（`:394-397`）、sim の経路の `n_ready`・`fw_ready`（`:448-449`）から `!halt` を全部消した。`halt` は port（`:72`）と comment（`:47`）にしか残らない。よって HALTED・FAULTED・RESETTING でも seq は header と payload を取る:
  - ring が R_N で流している途中の packet（ring は `go` を R_IDLE でしか見ない、`atg_cp_ring.v:661,1623`）の残りを、FAULTED の後に実行して unit を kick しうる。
  - firmware の経路（`fw_valid`、`atg_cp.v:1004`）の µop を HALTED・FAULTED でも取る。
  - `atg_cp.v:934` の `lc_halting` は使われない。`-Wall` の lint で UNUSEDSIGNAL が出る見込み（報告の「Warning 0 件」と合わない）。
- 直し方: seq の止め方を lifecycle ごとに分ける。HALTING: 今の packet を終え、次の header を取らない。HALTED: header を取らない（STEP の 1 packet を除く）。FAULTED・RESETTING: 直ちに止める（payload も取らない。eeed36ed の意味）。ring の経路と firmware の経路の両方に効かせる。comment（`:47`）を今の形に合わせる。
- 期待値（top の tb）:
  - CMD_FENCE（6 dword）を ring の途中まで（ring の line の境で 3 dword 目まで）見える形にして CTRL.HALT = 1 → 残りが届いた後に fence の値が memory に書かれてから STATUS.HALTED = 1。その後の packet は HALT = 0 まで実行されない（依頼 07 の V3 の試験。前回は足していない）。
  - FAULTED の後に ring に積んだ CMD_FENCE が実行されない（memory に書かれない、RPTR が進まない）。
  - firmware の構成（`atg_top_fw`）で、HALTED の間に firmware の µop が実行されない。

## 2. V1・V4・V7 の残り

### X2. HOST_FLUSH の要求が捨てられる、CP_STATE、試験（元 V1、監査 A1・A2・A3）

- 仕様: §7.3 HOST_FLUSH「Flush GPU dirty caches before signaling fence」（ghci.md:972）。§3.3 CP_STATE 5「Waiting on memory controller flush/quiesce」（:355）。
- 今:
  - (a) 要求は 1 cycle の pulse（`f_flush = (st == Q_FLUSH) && settle`、`atg_cp_seq.v:599,611` → `l2_flush`、`atg_cp.v:642`）。L2 は `S_IDLE` で、しかも high の要求が無い時だけ `ctrl_inv_valid` を受ける（`atg_mc_l2.v:397,465-485`）。high の client は DC（`atg_mc.v:582`）で、KC705 は表示が常に読む。pulse の cycle に L2 が他を処理中か DC の要求があれば要求は捨てられ、Q_FLUSH は `!settle && l2_idle`（`atg_cp_seq.v:1110-1112`）で進む → flush 無しで fence を書く。`ctrl_inv_done` は使われていない（`atg_mc.v:633`）。
  - (b) Q_FLUSH（seq_st 11）の間の CP_STATE が 5 にならない（`atg_cp_ctl.v:208-219` の表に 11 が無い）。
  - (c) 依頼 07 の試験が無い: dirty な data が fence の前に memory にある事、合成の経路の sim。
- 直し方: (a) 要求を `ctrl_inv_done` まで保つ（L2 の側で要求を latch してもよい）。Q_FLUSH は done を待ってから Q_FREQ へ。L2 の無い構成（`g_no_l2`）は done を直ちに 1 に。(b) Q_FLUSH を WAIT_MEM に。(c) 下の試験を足す。
- 期待値（tb）:
  - L2 を持つ構成で、CMD_FENCE の前に local の address へ書いた値（L2 に dirty で残る物。L2 に入ったことを tb で確かめる）が、flags 0x7 の fence の値が memory に見えた時点で memory（MIG の model）にある。flags 0x3 なら（追い出されない限り）まだ無い事も照合して、試験が flush を見分ける事を示す。
  - DC が scan out している最中（test pattern でなく frame の読み）に flags 0x7 の fence を 20 回流し、毎回 flush が完了してから fence が書かれる（`ctrl_inv_done` の数と fence の数が等しい）。
  - flush の待ちの間 STATUS[19:16] = 5。
  - 合成の経路: `-DSYNTH_KINTEX7` で seq（か top）の tb を通す方法を用意し、flags 0x4 → 0x3 の順で flush が 1 回だけ起きる事を照合する。用意できないなら理由を報告に。

### X3. local の fence_addr の書き込みが L2 に残る（新規、監査 A15、確かめの依頼）

- 仕様: §7.3 CMD_FENCE は 64 bit の値を「memory に」書き、N12 で [LOCAL_BASE, LOCAL_BASE + LOCAL_SIZE) は local に振り分ける（ghci.md:961-972）。IRQ_FENCE は「wrote 64-bit sequence value to memory」（:400）。
- 監査の読み: CP の書き込みは local の address なら L2 に入る（`atg_mc.v:566,597`、`nc = g_host`）。L2 は write-back（`dirty_mem`）。host の BAR2 の local の窓は GPU を通らず MIG の第 2 port へ行く（`boards/kc705/atg_kc705_top.v:16-18,234`）。よって local の fence の値は L2 から追い出されるまで host から見えない。HOST_FLUSH は fence を書く前の flush で、fence 自身の書き込みは L2 に残る。
- まず確かめて 1 次回答に書く: (1) L2 は write の miss で allocate するか、hit の時だけ L2 に残すか。(2) host の BAR2 の窓の読み書きが L2 を通らない事。(3) 上の読みが正しいか。
- 直し方（読みが正しい時）: fence（と CMD_WRITE_TIMESTAMP の host への書き込み）の書き込みを、local の address でも memory まで届けてから IRQ_FENCE と完了にする（non-cacheable で書く、か書いた line を clean する）。
- 期待値（tb）: fence_addr を local にした flags 0x2 の CMD_FENCE で、fence の完了の時点で MIG の model の memory に値がある。
- host の書き込み（ring・vertex を窓から書く）と L2 の古い line の問題は仕様の問い（B35）。今回は直さず、L2 が local の line を持ち続ける場合を報告に書く。

### X4. MC の経路の SU の STAGE と V4 の試験（元 V4、監査 A5）

- 仕様: §3.5 SHADER_STAGE は UNIT = 2 の時「the active shader pipeline stage」（ghci.md:439-444）。
- 今: `atg_cp_ctl.v:418,506` は MC の経路で UNIT が SU の時 `su_stage` を入れるが、`su_stage` は SB の fault と同じ cycle だけ値を持ち、他は 0（`atg_top.v:1576-1588`）。MC の拒否（D$・I$・ATOM）は SB の fault と別の時に来るので STAGE は常に 0。
- 直し方: MC の要求に warp の stage（か warp の番号）を載せて fault に運ぶ。運べないなら理由と、運ぶ時の LUT の見積りを報告に。mesh・task の warp を MS（3）にする（今は CS になる。mesh を有効にする preset が無いなら、その事を報告に書き、回路だけ用意する）。
- 期待値（top の tb、tb が kind・stage を与えない形で。**依頼 07 の V4 の試験。前回は足していない**）: FS の shader に不正な命令 → KIND 1・UNIT 2・STAGE 1・FAULT_ADDR = FS の入口からの命令の番号。CS で同じ → STAGE 2。VS と FS が同時に走る draw で FS の warp が fault → STAGE 1。FS の load が範囲外（MC の拒否）→ UNIT 2・STAGE 1。fence の書き込みに AXI の error → KIND 5・UNIT 0。HAS_CPU = 0 の構成で VF の MC の拒否 → UNIT 1。

### X5. session の fault（元 V8、監査 A9・A10・A11）

- 仕様: §3.5「FAULT_INFO latches the error classification of the first fault. Subsequent faults while FAULT_INFO is uncleared are dropped」（ghci.md:415）。§3.3 N05（:375-381）: fault した session i の後の CMD_DRAW*・CMD_DISPATCH・CMD_COPY*・CMD_BLIT・CMD_EXECUTE_COMMANDS は stall も起動も無しに NOP。後の CMD_FENCE は普通に実行して memory に書き IRQ_FENCE。
- 今と直し方:
  - (a) session の fault で CP が止まらなくなったので、後の fault が FAULT_INFO を上書きする（`atg_cp_ctl.v:599-613` は `take_fault_info` の度に書く。session の fault では `can_fault_r` を下げない）。FAULT_INFO・FAULT_ADDR を「0 の時だけ書く」first-fault の latch にする（W1C で再び書ける）。whole-device の fault が latch 中に来た時の扱いは B34（今回は FAULTED への遷移は今のまま行い、FAULT_INFO は上書きしない）。
  - (b) N05 の NOP が無い。fault した session（SESSION_ACTIVE[i] = 0）の tag の上の command を NOP にする（ring の経路と firmware の経路の両方）。CMD_FENCE・CMD_SESSION_CLOSE は実行する。
  - (c) **確かめの依頼（監査の確信は低い）**: `map_v[tag] = 0` の後、その tag の ring の µop は `map_u = 0`（`atg_cp_prot.v:146`）、つまり slot 0「unchecked work」（`:27`）で走るように読める。fault した session の後の仕事（(b) を直す前の今）が保護の検査無しで走るか、RTL と試験で確かめて報告に。走るなら (b) と合わせて、閉じた・fault した session の仕事が slot 0 で走らない形にする。
  - (d) `atg_top_prot_tb.v` の refuse の組で消した `check(mem.peek32(AREA_A) == DATA_A, "a is not written")` を戻す。戻して FAIL するなら、a に何が書かれたか（誰が、どの slot で）を報告に。
  - (e) open(A=3)・open(B=6)・open(C=7) の後の SESSION_ACTIVE を照合する（期待は仕様から: open で有効になった session の bit）。今の tb は A の fault と close(B) の後に 0x00000001 を期待して PASS するので、C の bit が 0 である理由を報告に。
- 期待値（balanced の tb）: (a) session 3 の fault の後に、別の拒否をもう 1 つ起こしても FAULT_INFO が最初の値のまま。W1C の後の次の fault は書かれる。(b) session 3 の fault の後の session 3 の CMD_COPY（か draw）が memory を変えず、FAULT_INFO・IRQ_FAULT を再び立てず、その後の CMD_FENCE は memory に書かれ IRQ_FENCE が立つ。他の session（B）の仕事は普通に走る。
- KIND 4（watchdog）・KIND 5（AXI_ERROR）の session の fault を FAULTED にするかは B33（今の「SESSION ≠ 0 なら session の fault」のまま）。

### X6. DEVICE_CAPS の残り（元 V7、監査 A7・A8）

- [4] CAP_SECONDARY_RING: 今は `HAS_CPU && NSU > 0`（`atg_top.v:127`）。KC705 の board top は minimal でも `HAS_CPU(1)`（`boards/kc705/atg_kc705_top.v:328`）なので **minimal の KC705 で 1**。報告は minimal を「CPU 無」として 0x40010480 と書いたが、RTL の式では 0x40010490 の見込み。依頼 07 は「CMD_EXECUTE_COMMANDS で secondary の CMD_FENCE を実行して memory に書かれる試験で示せたら 1、示せなければ 0」、依頼 02 の W4（ユーザーの承認済み）は「KC705・minimal の今の FW と HW では 0」。直し方: その試験を足して PASS を示すか、0 にする。tb の期待値（`atg_ghci_top_tb.v:118`）を RTL の式の写しでなくする。
- [28:27] MESH・TASK: top の parameter `HAS_MESH`（`atg_top.v:121-125`、macro から）でなく、SU の mesh の回路の有無から（SU が mesh を常に持つなら、その事を報告に）。
- [25] と EXT_CAP_FMT0 [11:10]: `(HAS_FFILT != 0) || ATG_DEF_FFILT`（`atg_cp_regs.v:699,718-719`）の `|| ATG_DEF_FFILT` を消す（macro が回路の無い時に bit を 1 にしうる）。
- 使われない `ATG_DEF_MESH`・`ATG_DEF_ROP_PACK`（`atg_cp_regs.v:59-61,183-185`）を消す。
- 期待値: minimal の KC705 の DEVICE_CAPS を、board top の parameter と §3.2.1 から計算した値と照合する tb（[4] = 0 なら 0x40010480）。

## 3. 試験と include の残り

### X7. IRQ_GLOBAL = 0 の試験（元 V6、監査 A6）

- 依頼 07 の V6 の「top の tb に IRQ_GLOBAL = 0 で IRQ_STATUS & IRQ_ENABLE ≠ 0 でも irq = 0 を 1 つ足す」が無い（報告は「追加」と書いたが、`git diff b58d4edd fe6431b1 -- sim/` に無い）。足す。期待値（§3.4、ghci.md:393-394）: IRQ_ENABLE[5] = 1、IRQ_SET[5] = 1、CTRL.IRQ_GLOBAL = 0 → irq = 0。IRQ_GLOBAL = 1 にした cycle の後 irq = 1。

### X8. include の別名と harness の期待値（元 V9、監査 A12）

- `include/ghci_regs.h:259-260` に足した `#define GHCI_ST_FAULT GHCI_CP_STATE_FAULT` と `#define GHCI_STATUS_STATE(s) GHCI_STATUS_CP_STATE(s)` を消し、`sim/verilator/fw_harness.cpp` を `GHCI_STATUS_CP_STATE(...) == GHCI_CP_STATE_FAULT`（か STATUS.FAULTED）で書く。
- `sim/verilator/harness.cpp` の新しい期待値（TEX: EXT_CAP_FMT0[16] = 1・DEVICE_CAPS[25] = 1、CORE: DEVICE_CAPS[26] = 1・EXT_CAP_FEATURE[12] = 1）は構成と結ばれていない。minimal は TU_BILINEAR・FFILT・ROP_PACK・EARLY_Z が 0（`config/minimal.mk:22,34,38,50`）。期待値を build の構成（`CFG_*`）から作る。旧い「CAPS[9] stencil」「CAPS[30] Vulkan 1.0 core」に GHCI 1.0 で当たる bit が無いなら、無いと書いて照合を消す（別の bit で代えない）。
- 期待値: `sim/verilator/run.sh` の basic・tex・msaa・core を、minimal と balanced の各 preset で流して件数を報告に（前回は basic だけ）。

### X9. 小さな物（監査 A13・V2 の小）

- `sim/tb/atg_kc705_pcie_tb.v:37` の comment「(IRQ_STATUS [9], ...」を [3]（DISPLAY）に。
- `sim/tb/atg_ghci_top_tb.v` の CMD_WAIT_FENCE の Case 1 は RPTR の前進（fetch）だけを見る。Case 1 の直後に別の packet を置き、それが実行される事（待たずに通った事）を照合する。

## 今回触らない物

- 依頼 06 の U10〜U15（次の依頼で出す）。ただし X1〜X9 の試験に要る範囲で tb を直すのはよい。
  - timing の事実（前回の監査まで）: 最新の route の report は 28a8d920 の物で WNS −0.383 ns。fe6431b1 の bitstream は無い。
- 仕様の問い（ユーザーの判断待ち。今の値のまま）: CPFW の register の配置、SA の指す先、ring の細部、TIMESTAMP の読みの原子性、VEND の VENDOR_ID、PERF0〜3、MEM_CACHE_CFG、FMT_BUFFER、SESSION_ACTIVE のリセット、DBG の窓、`CONFIG_FMT_ENABLE_*`、page 0 の仕様に無い offset、PROT の時の LOCKED_REG の FAULTED、ROP_PACK = 0 の BLIT_DST・BLEND、DISP2 の FLIP の不正の KIND・UNIT と SA・DC の UNIT、MC の拒否と ring の読みの error の KIND、PERF_SRC の番号、CP の firmware の core の例外の KIND・UNIT、CMD_WAIT_FENCE の揃え（今は 8 byte）、SU の FAULT_ADDR の単位（今は命令の番号）。
  - 新しい問い（監査 fe6431b1）: KIND 4・5 の session の fault を FAULTED にするか（B33、今は SESSION ≠ 0 なら session の fault）、FAULT_INFO の latch 中の別の fault の扱い（B34、X5 (a) のとおり）、BAR の local の窓と L2 の一貫性（B35、X3 の fence の書き込み以外）。

## 受け入れ

1. X1〜X9 の tb が minimal と balanced で PASS（仕様の式の期待値、source の試験は top の tb で）。どの preset で流したかを件数と一緒に書く。
2. `sim/run.sh`（引数無し、全 tb）を 1 回流し、PASS・FAIL の件数と FAIL の名前を報告に。fe6431b1 より悪くならない。
3. 直した module が `-DSYNTH_KINTEX7` の lint でも MULTIDRIVEN・UNUSEDSIGNAL などを出さない（コマンドと出力の全文）。`tools/arith-lint.py` 0 件。
4. `git diff` で `docs/ghci.md` が変わっていない。commit message が全部 `WIP` ちょうど。
5. KC705 の bitstream は今回の受け入れに含めない。作ったなら route の後の WNS・WHS を報告に書き、WNS < 0 の物は「強制」と明記して実機に渡さない。

## 報告の書き方

- 1 次回答: X1〜X9 の各項目に「正しい・誤り（仕様の行）・一部」と短い理由。X3 は (1)〜(3) の事実、X5 (c) は確かめた結果。
- 実装の報告: X1〜X9 のそれぞれ、済み・一部・未着手、変えた file、確かめた試験（tb が何をどう照合したか）。**足していない試験は「足していない」と書く。**
- commit の SHA、試験のコマンドと件数（preset ごと）、`-DSYNTH_KINTEX7` の lint の出力。
- **minimal（KC705）と balanced の DEVICE_CAPS（0x008）・EXT_CAP_FMT0（0x080）・EXT_CAP_FEATURE（0x084）の 16 進**。tb で読んだ値と、§3.2 の bit の名前ごとの内訳（前回の報告は offset と bit の意味が旧い CAPS の物だった）。
- X1 の lifecycle ごとの seq の止め方の表、X2 の flush の要求と完了の handshake、X4 の source ごとの「何が起きたか → KIND・UNIT・STAGE・SESSION」の表、X5 の slot 0 の確かめの結果、X6 の [4] の判断と根拠（試験の名前）、決められなかった所。
- 順: X1 → X2 → X5 → X4 → X3 → X6 → X7・X8・X9。区切りごとに commit し、`build/agy/request-08.md` に書き足してよい。
