<!-- awesome-plan project=zedbsd record=ws347-audit-fe6431b1 -->

# 監査: amaterrace-gpu fe6431b1（依頼 07 の V1〜V9）（2026-11-13）

- 監査の担当 P3（Opus、読むだけ。sim・lint・arith-lint・Vivado は走らせていない）。RTL・tb は `git show`・`git diff` で読んだ見込みで、動きは確かめていない。
- 範囲 `git diff b58d4edd fe6431b1`（7 commit: 77d6f219 V1、2ca25db2 V2、31c7039c V5・V6・V3、3800cb30 V4、9816446e V9、faf2b32b V7、fe6431b1 V8。30 file）。
- agy の報告: `~/amaterrace-gpu/build/agy/request-07.md`（進捗の log と最終報告）。
- 規則: commit message は 7 件とも `WIP` ちょうど。`docs/` の変更は無い（`git diff --stat b58d4edd fe6431b1 -- docs/` が空）。
- 仕様の行は `docs/ghci.md`（fe6431b1）の行番号。RTL の行は fe6431b1 の file の行番号。
- Claude の所見は 15% ほど誤りうる。下の A は依頼 08 で Gemini に先に妥当性を確かめてもらう。

## 結論

- **済み**: V2（CMD_WAIT_FENCE の 64 bit の比較、tb は 32 bit との違いを見分ける）、V5（消した名前の grep が 0 件、tb 2 本の書き直し、fw の `HW_RESULT` の撤去と hex の作り直し。comment が 1 つ残る）。
- **一部**: V1、V3、V4、V6、V7、V8、V9。
- **最も重い所見**:
  1. **V3 の直しで seq が `halt` を全く見なくなった**（回帰）。HALTED・FAULTED・RESETTING でも seq が header と payload を取る。§3.3 の FAULTED（「halts all pipeline blocks」、ghci.md:389）と HALTED（「fully stopped execution」、:365）に反する見込み。
  2. **V1 の L2 の flush の要求が 1 cycle の pulse で、L2 が受けない時に黙って捨てられる**。その後 `l2_idle` で次に進むので、flush 無しで fence を書く。KC705 は DC（表示）が L2 の high の client で常に読むので、起きやすい見込み。
  3. **V8 で session の fault が CP を止めなくなった結果、FAULT_INFO が後の fault で上書きされる**（§3.5「first fault を latch」、ghci.md:415）。§3.3 の N05（fault した session の後の draw などを NOP にする、:375-379）は未実装。fault した session の tag の µop は SLOT_MAP が 0 になり slot 0（「unchecked work」、`atg_cp_prot.v:27,146`）で走る見込みで、保護の外で走る心配がある（確信は低い、確かめの依頼）。
  4. **依頼した試験の多くが足されていない**: V1 の dirty な data の照合、V3 の packet の途中の HALT、V4 の top の tb（3800cb30 は RTL だけ）、V6 の「IRQ_GLOBAL = 0 で irq = 0」（報告は足したと書くが diff に無い）、V7 の [4] の ring の試験。`SYNTH_KINTEX7` の経路を通す sim も無い（`sim/` に `SYNTH_KINTEX7` の定義が無い）。
- **報告の正確さ**: 最終報告の 3 register の表は、offset（0x004・0x100・0x104。仕様は DEVICE_CAPS 0x008、EXT_CAP_FMT0 0x080、EXT_CAP_FEATURE 0x084、ghci.md:236,247,302）と bit の意味（「[31] MS4=1, [16] ROP_STENCIL=1, [10] HAS_VF=1」は旧い CAPS の意味）が誤り。minimal を「CPU 無」として 0x40010480 とするが、KC705 の board top は `HAS_CPU(1)`（`boards/kc705/atg_kc705_top.v:328`）で、RTL の式（`atg_top.v:127`）では [4] = 1 → 0x40010490 の見込み。V1 の ROP の書き戻しを「`atg_rop_cache.v` を occ_clear・flush の pulse で」とするが、`atg_rop_cache` はどこからも instance されていない（ROP の dirty は `atg_rop_wb.v` の `ev` が busy に入り、CMD_FENCE の B_ALL の待ちで吐き出される、`atg_rop_wb.v:532,1083`、`atg_cp_seq_wait.v` の CMD_FENCE の pre）。

## V1〜V9

| # | 判定 | 根拠（fe6431b1） | 残り（所見の番号は下の表） |
| --- | --- | --- | --- |
| V1 CMD_FENCE の flags・HOST_FLUSH | 一部 | (a) `atg_cp_seq.v` の `fence_flags` の always（`:760`）から `flags` の書き込みを消した（`flags` は `:805` の always だけ）。(b) 合成の経路で dword 5 を取った時（`pay_take_b[0] && is_fence_cmd_r && p_is_4_r`、`:942-952`）に `inv_code_r`・`exec_done_st_r`（Q_FLUSH か Q_FREQ）を決め、`fence_flags` を decode で 0 にする（`:778`）。(c) Q_FLUSH を足し（`:177,1110-1112`）、`f_flush = (st == Q_FLUSH) && settle` を `l2_flush` として MC の L2 の `ctrl_inv_valid`（clean + invalidate）へ（`atg_cp.v:642`、`atg_mc.v:628`）、`l2_idle` を待って Q_FREQ。tb は seq_hi = 1・2・3 で上位の dword を照合（`atg_ghci_top_tb.v`）、seq の tb で flags 7 の inv と、次の flags 3 で inv が無い事（sim の経路） | A1（要求が捨てられる）、A2（Q_FLUSH の間の CP_STATE）、A3（試験）、A15（local の fence と L2） |
| V2 CMD_WAIT_FENCE 64 bit | 済み | `atg_cp_fence.v` の `f_64`・`f_wide_r`、8 byte に揃えて 2 word を読み `am_sub #(.W(64))`、`passed = f_wide_r ? !diff64[63] : !diff64[31]`。合成の経路は `mem_qword_r`。`atg_cp_fence_tb.v` の Case B・C と `atg_ghci_top_tb.v` の Case 1・2 は 32 bit の比較では結果が変わる値 | 小: `atg_ghci_top_tb.v` の Case 1 は RPTR の前進（fetch）だけを見る（待たずに通った事は Case 2 の結果から間接にしか分からない）。依頼に無い変更: 旧い OP_FENCE の書き込みが 4 byte（`be_word`）に、`f_aux` を CMD_FENCE・CMD_WAIT_FENCE・OP_RESULT 以外で 0 に（31c7039c、log に一部だけ記載） |
| V3 HALTING | 一部（回帰） | `atg_cp.v:938` の `s_seq_halt` から HALTING を外した。同時に `atg_cp_seq.v` の `hdr_take`（`:328`）、`n_rdy_idle`・`fw_rdy_idle`・`n_rdy_pay`・`fw_rdy_pay`（`:394-397`）、sim の経路の `n_ready`・`fw_ready`（`:448-449`）から `!halt` を全部消した。`halt` は port（`:72`）と comment（`:47`「halt is looked at only between packets」）にしか残らない | A4（seq が halt を見ない）、A14（未使用の信号と lint の主張）。依頼した tb（CMD_FENCE の途中で HALT）が無い |
| V4 FAULT_INFO | 一部 | (a) `atg_top` の `C_*` を `atg_cp` → `atg_cp_ctl` に渡し、`mc_client_to_unit` を parameter の連鎖で（`atg_cp_ctl.v:266-290`）。(b) `atg_su.v:1270-1283` の `warp_stage`（`init_valid` の時に `vl_go_sch` → VS、`fl_go_sch` → FS、他は CS）と `atg_su_sb.v` の `fault_warp`、`auto_su_stage` を撤去。(d) `x_fault_kind` 1、tag は `prot_unit_tag`（`atg_top.v:699-702`）。(e) PC は warp の開始で 0（`atg_su_sched.v:740-741`）なので入口からの相対（単位は B29） | A5（MC の経路の SU の STAGE が常に 0）。依頼した top の tb（FS・CS・VS と FS の同時・fence の AXI error・HAS_CPU = 0 の VF）が無い。小: mesh・task の warp は CS（2）になり MS（3）が出ない（今の preset は mesh 0、`config/minimal.mk:17`） |
| V5 U1 の水平展開 | 済み | grep（`GHCI_IRQ_RESULT` ほか 7 つと `HW_RESULT`）が rtl・sim/tb・sim/verilator・fw・include・tools で 0 件（自分で数えた）。`atg_kc705_pcie_tb.v` は DISP2 の DISP_IRQ_ENABLE.VSYNC と IRQ_STATUS[3] に、`atg_top_fw_tb.v` は RESULT の照合を外した。`.vh` の FLIP・DBG_DONE を撤去、`atg_cp_irq.v` の comment を直した。fw の hex を作り直した | A13（`atg_kc705_pcie_tb.v:37` の comment「IRQ_STATUS [9]」）。fw の結果の通知は「CMD_FENCE に一本化」と報告するが、実際は `HW_RESULT` の撤去だけ（host は ring の CMD_FENCE で完了を知る。仕様に合うので可） |
| V6 U2 の水平展開 | 一部 | `atg_top_tb.v`・`atg_top_fw_tb.v`・`atg_kc705_pcie_tb.v`・`harness.cpp`・`fw_harness.cpp` の CTRL の書き込みに IRQ_GLOBAL、RESET の後も立て直す | A6（「IRQ_GLOBAL = 0 で irq = 0」の試験が無い。報告は足したと書く） |
| V7 DEVICE_CAPS | 一部 | [3] = 0（`atg_top.v:126`）。[26] = `HAS_ROP_PACK && HAS_ROP`、[28:27] = `HAS_MESH`（`atg_cp_regs.v:696-698`）。`ATG_DEF_DEVICE_CAPS`・`ATG_DEF_CAPS`・`ATG_DEF_EXT_CAP_FMT0`・`ATG_DEF_EXT_CAP_FEATURE` を撤去 | A7（[4] が KC705 で 1、試験無し、報告の値の誤り）、A8（[28:27]・[25] の macro、死んだ define） |
| V8 SESSION_ACTIVE | 一部 | `atg_cp_ctl.v:541-543` の `is_session_fault`（HAS_PROT かつ FAULT_INFO の SESSION ≠ 0）で S_FAULTED に行かず（`:732`）、`fault_session_clear` で `atg_cp_prot.v:174-176` の `map_v[tag] <= 0`。`atg_top_prot_tb.v` で open(3) の後 0x9、fault の後 STATUS.FAULTED 0・RUNNING 1・bit 3 が 0 | A9（FAULT_INFO の上書き）、A10（N05 の NOP、slot 0 で走る心配）、A11（tb の照合の削除と説明の無い値） |
| V9 U9 の残り | 一部 | `ghci_regs.h` の `GHCI_LEGACY_CAPS_*`、`.vh` の `GHCI_ST_FAULTED` を撤去。grep（`GHCI_LEGACY_CAPS_[A-Z0-9_]*`・`GHCI_ST_FAULTED`）が 0 件（自分で数えた）。`harness.cpp` の照合を DEVICE_CAPS・EXT_CAP_FMT0・EXT_CAP_FEATURE・FMT の窓に | A12（新しい C の別名 2 つ、harness の期待値が構成から作られていない） |

## 所見（A: RTL・tb・報告の誤り、Gemini に先に確かめてもらう）

| # | 元 | 所見 | 根拠 | 確信 |
| --- | --- | --- | --- | --- |
| A1 | V1 | L2 の flush の要求が 1 cycle の pulse（`f_flush = (st == Q_FLUSH) && settle`、`atg_cp_seq.v:599,611`）。L2 は `S_IDLE` で、しかも high の要求が無い時だけ `ctrl_inv_valid` を受ける（`atg_mc_l2.v:465-485`、`s_req_ready` `:397`）。high は DC（`atg_mc.v:582`、`HAS_DC` の時）。pulse の cycle に L2 が他の要求を処理中か DC の要求が来ていれば捨てられ、Q_FLUSH は `!settle && l2_idle`（`:1110-1112`）で次へ進む → flush 無しで fence を書く。`ctrl_inv_done` は使われていない（`atg_mc.v:633` の `unused_l2`） | §7.3 HOST_FLUSH（ghci.md:972） | 中 |
| A2 | V1 | Q_FLUSH（seq_st 11）の間の CP_STATE が WAIT_MEM（5）にならない。`atg_cp_ctl.v:208-212` の対応表に 11 が無く、FETCH（1）か IDLE（0）を読む | §3.3 CP_STATE 5「Waiting on memory controller flush/quiesce」（ghci.md:355） | 高 |
| A3 | V1 | 依頼した試験が無い: (1) L2（か ROP）に dirty な data を作り flags 7 の fence の後に memory にある事、(2) 合成の経路を通す sim（`sim/` に `SYNTH_KINTEX7` を定義する所が無い）。flags 7 の試験は fence の値だけを照合する | 依頼 07 V1 の期待値 | 高 |
| A4 | V3 | seq が `halt` を見なくなった（上の表）。HALTED・FAULTED・RESETTING で、(1) ring が R_N で流している途中の packet の残りを seq が実行する（ring は `go` を R_IDLE でしか見ない、`atg_cp_ring.v:661,1623`。fault の後に draw などを kick しうる）、(2) firmware の経路（`fw_valid`、`atg_cp.v:1004`）の µop を HALTED・FAULTED でも取る。eeed36ed は FAULTED・RESETTING で payload も止めていた。正しい形は「HALTING は今の packet を終え次の header を取らない、HALTED は header を取らない、FAULTED・RESETTING は直ちに止める」の見込み | §3.3（ghci.md:363-366,389）、§5.2（:570） | 高 |
| A5 | V4 | MC の経路の SU の client（D$・I$）と ATOM の fault の STAGE が常に 0。`atg_cp_ctl.v:418,506` は `su_stage` を使うが、`su_stage` は SU の SB の fault と同じ cycle だけ値を持ち、他は 0（`atg_top.v:1576-1588` の `fault_stg = 4'd0`）。MC の拒否は SB の fault と別の時に来る | §3.5 SHADER_STAGE（ghci.md:439-444） | 高 |
| A6 | V6 | 「IRQ_GLOBAL = 0 で IRQ_STATUS & IRQ_ENABLE ≠ 0 でも irq = 0」の試験が無い。報告（V6）は「atg_top_tb に追加」と書くが、`git diff b58d4edd fe6431b1 -- sim/` に該当が無い | §3.4（ghci.md:393-394） | 高 |
| A7 | V7 | [4] CAP_SECONDARY_RING = `HAS_CPU && NSU > 0`（`atg_top.v:127`）のまま。KC705 は minimal でも `HAS_CPU(1)` なので 1。依頼 07 は「CMD_EXECUTE_COMMANDS で secondary の CMD_FENCE を実行する試験で示せたら 1、示せなければ 0」、依頼 02 の W4（ユーザーの承認）は「KC705・minimal の今の FW と HW では 0」。試験は無い（`sim/tb`・`sim/verilator`・`fw/cp/tests` に CMD_EXECUTE_COMMANDS を実行する試験が無い。`atg_ghci_tb.v:388` は decode だけ）。tb の期待値（`atg_ghci_top_tb.v:118`）は RTL の式の写し。報告の 3 register の表は上の「報告の正確さ」のとおり誤り | §3.2.1 [4]（ghci.md:254） | 高 |
| A8 | V7 | [28:27] は top の parameter `HAS_MESH`（`atg_top.v:121-125`、`CFG_ENABLE_MESH` の macro から）で、SU の mesh の回路（`atg_su` は mesh の port を常に持つ）とは結ばれていない。[25] と FMT0 [11:10] は `(HAS_FFILT != 0) \|\| ATG_DEF_FFILT`（`atg_cp_regs.v:699,718-719`）で、macro が 1 なら回路（`HAS_FFILT && HAS_TU`、`atg_top.v:621`）が無くても 1（b58d4edd から在った。前回の監査の見落とし）。`ATG_DEF_MESH`・`ATG_DEF_ROP_PACK`（`:59-61,183-185`）は使われない define | 依頼 07 の 0-2「値は実際に instance した回路から」 | 中 |
| A9 | V8 | session の fault で CP が止まらなくなったので、後の fault が FAULT_INFO を上書きする。`fault_info` は `take_fault_info` の度に `n_info` を書く（`atg_cp_ctl.v:599-613`）。前は FAULTED で `can_fault_r = 0` になり上書きが起きなかった。session の fault では `can_fault_r` を下げない（`:732` の分岐の外） | §3.5「latches the first fault. Subsequent faults while FAULT_INFO is uncleared are dropped」（ghci.md:415） | 高 |
| A10 | V8 | N05 の「fault した session の後の CMD_DRAW*・CMD_DISPATCH・CMD_COPY*・CMD_BLIT・CMD_EXECUTE_COMMANDS を stall も起動も無しに NOP」が無い（seq・prot に該当の変更が無い）。さらに `map_v[tag] = 0` の後、その tag の ring の µop は `map_u = 0`（`atg_cp_prot.v:146`）、つまり slot 0「unchecked work」（`:27`）で走る見込み。fault した session の後の仕事が保護の検査無しで走る心配がある（読んだだけ、確信は低い）。「fault の後の CMD_FENCE は普通に書く」も試験が無い | §3.3 N05（ghci.md:375-379） | 中（NOP が無い事は高、slot 0 で走る事は低） |
| A11 | V8 | tb の照合を説明無しに消した: `atg_top_prot_tb.v` の refuse の組の `check(mem.peek32(AREA_A) == DATA_A, "a is not written")`。拒否された copy の後に a が書かれるようになったのなら A10 の心配と結びつく。また open(A=3)・open(B=6)・open(C=7) の後、A の fault と close(B) の後に 0x00000001 を期待して PASS するが、C（7）の bit がなぜ 0 か説明が無い（3 つの open の後の値を照合していない） | 依頼 07 の 0-7・0-8 | 中 |
| A12 | V9 | `include/ghci_regs.h:259-260` に新しい C の別名 `GHCI_ST_FAULT`（→ `GHCI_CP_STATE_FAULT`）と `GHCI_STATUS_STATE(s)`（→ `GHCI_STATUS_CP_STATE(s)`）を足した（`fw_harness.cpp` を通すため）。`harness.cpp` の新しい期待値（TEX: FMT0[16] = 1・DEVICE_CAPS[25] = 1、CORE: [26] = 1・EXT_CAP_FEATURE[12] = 1）は構成から作られていない。minimal は TU_BILINEAR 0・FFILT 0・ROP_PACK 0・EARLY_Z 0（`config/minimal.mk:22,34,38,50`）。流したのは `sim/verilator/run.sh basic` だけで、tex・msaa・core の build は流していない | 依頼 07 の 0-3・0-7 | 高（別名）、中（期待値） |
| A13 | V5 | `sim/tb/atg_kc705_pcie_tb.v:37` の comment「(IRQ_STATUS [9], ...」（DISPLAY は [3]） | §3.4 | 高 |
| A14 | V3 | `atg_cp_seq.v` の input `halt` と `atg_cp.v:934` の `lc_halting` がどこからも使われない。`verilator -Wall` なら UNUSEDSIGNAL が出る見込みで、報告の「Warning 0 件」と合わない（A4 を直せば `halt` は使われる）。lint のコマンドと出力の全文が報告に無い | 依頼 07 の受け入れ 3 | 中 |
| A15 | V1 | fence_addr が local（[LOCAL_BASE, LOCAL_BASE + LOCAL_SIZE)）の時、CP の書き込みは L2 に入る（`atg_mc.v:566,597`: `nc = g_host`、local は cache する）。L2 は write-back（`dirty_mem`）。host の BAR2 の local の窓は GPU を通らず MIG の第 2 port へ行く（`boards/kc705/atg_kc705_top.v:16-18,234`）。よって local の fence の値は L2 から追い出されるまで host から見えない見込み（HOST_FLUSH は fence を書く前の flush で、fence 自身の書き込みは L2 に残る） | §7.3 CMD_FENCE「write ... to memory」、N12 の local の振り分け（ghci.md:961-972）、IRQ_FENCE（:400） | 中（host の読みの経路は読んだが、L2 の write-allocate の有無は未確認） |

数: A は 15（A1〜A15）。

## 仕様の問い（B、台帳に足した）

- **B33**: CAP_PROT = 1 で session i（≥ 1）の fault のうち、KIND 4（watchdog）と KIND 5（AXI_ERROR）も「session の fault」（FAULTED を立てない）か。RTL（fe6431b1）は KIND を見ず、FAULT_INFO の SESSION ≠ 0 だけで決める（`atg_cp_ctl.v:541`）。§3.3（ghci.md:388）は PROT の時の FAULTED を AXI_STUCK・ring の framing・session 0 の不正・「unrecoverable hardware pipeline hang」に限り、同じ文で「client session の fault は決して FAULTED を立てない」とも書く。watchdog の fault は `wdt_tag`（drain した session）を持つ。
- **B34**: FAULT_INFO が uncleared（前の session の fault を latch 中）の時に、別の session j の fault と whole-device の fault をどう扱うか。§3.5 は後の fault を「dropped」と書くが、SESSION_ACTIVE[j] を 0 にする事（§3.3）と、whole-device の fault で FAULTED にする事も止めるのかは書かない。
- **B35**: host の BAR の local の窓（MEM の WINDOW、KC705 は BAR2 → MIG）と GPU の L2 の一貫性。host が窓から書いた ring・vertex などを GPU が L2 の古い line で読む事と、GPU が local に書いた物（fence、render の結果）を host が窓から読めない事を、hardware が防ぐか（窓を L2 に通す、local を cache しない）、driver が何かの command で防ぐか。§6.3 は host memory の DMA の同期だけを書き、§11 の MEM の WINDOW は一貫性を書かない。A15 は CMD_FENCE の「write to memory」から A とした。

## 最小（KC705 minimal）の DEVICE_CAPS（自分で計算、RTL を読んだ見込み）

`atg_kc705_top.v:328` HAS_CPU 1、NSU 1、PHYS_LANES 4、HAS_DISP 1、他 0。`atg_top.v:126-127` で [3] 0、[4] = 1。→ [30]・[23:16] 1・[15:8] 4・[7] GSI1・[4] → **0x40010490**（報告は 0x40010480）。W4 と依頼 07 の V7 の期待は [4] = 0 で 0x40010480。EXT_CAP_FEATURE・EXT_CAP_FMT0 は範囲で式が変わらず 0x00110000・0x00001200（前回の監査の計算のまま）。

## 確かめていない事

- sim・lint・arith-lint・合成は走らせていない。agy の試験の PASS（`atg_top_prot` 536 s ほか）は報告の主張で、log は読んでいない。
- 全 tb（`sim/run.sh` の引数無し）は agy も流していない（報告は 7 本と fw の 7 本と verilator の basic だけ）。minimal と balanced のどちらで流したかも報告に無い。
- A1 の取りこぼしは読んだ見込みで、波形では確かめていない。A10 の slot 0 で走る事、A15 の L2 の write-allocate は推測を含む。
- 依頼 06 の U10〜U15 は判定していない（範囲外）。
- 2026-11-13 10:1x に amaterrace-gpu で `agy --remote-control` の process が 1 つ動いていた（12 時間超）。作業ツリーは clean（未 commit の変更なし）。
