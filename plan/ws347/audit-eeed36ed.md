<!-- awesome-plan project=zedbsd record=ws347-audit-eeed36ed -->

# 監査: amaterrace-gpu eeed36ed（依頼 04 まとめ版の R1〜R14）（2026-11-12）

- 監査の担当（Opus、読むだけ、sim・lint・Vivado は走らせていない）。RTL を読んだ見込み。Q1 が STATUS（ghci.md:340-348 と `atg_cp_ctl.v:769`）と FAULT_INFO（ghci.md:417〜 と `atg_cp_ctl.v:440-446`）を自分でも確かめた。
- 範囲 8ebb2e2f..eeed36ed（17 commit、message は全部 `WIP`、docs/ の変更なし）。

## 結論

- R1（RING_SIZE を byte で）・R2（LOCKED_REG）・R9（前処理）・R11（GCLK_HZ）・R12（template）は済み。R6 は並びだけ済み。R3・R4・R5・R7・R8・R10・R13・R14 は一部。
- minimal（KC705）の 3 register は 0x40010480・0x00110000・0x00001200 で依頼どおり（自分で計算）。balanced（KC705 の board）は 0xE70108E0・0xF044BF02・0x00074600。
- **受け入れ 3（timing）未達**: route まで行っていない。log は place の Phase 2.4（05:14）で途切れ、Vivado の process は無い。05:20 の report は合成の後（Synthesized、LUT 200,427 = 98.34%）。bitstream は無い。
- **受け入れ 2 未達の見込み**: 既存の tb の少なくとも 11 file と harness が、消えた名前（`GHCI_CTRL`・`GHCI_RING_BASE`・`GHCI_CAPS` など）を使い compile できない。`atg_kc705_top_tb.v` も。
- **新しい A**: STATUS と FAULT_INFO の bit の並びが仕様と違い、新しい tb はその誤った位置で R2・R3 を照合して PASS している。
- メモ 2（AXI の ID）: RTL の変更ではなく tb の model の直し。KC705 の board の経路（am_axi_split ID_W 1、host_dn・host_cc の ID 4）と合っており、正当。

## A（RTL の誤り）

1. STATUS（§3.3、ghci.md:340-348）: 仕様は [0] RUNNING・[1] HALTED・[2] FAULTED・[8] MC_IDLE・[11] RING_EMPTY・[12] MEM_READY・[13] AXI_STUCK・[14] QUIESCE。RTL（`atg_cp_ctl.v:769`）は [0] CP の busy・[7:1] unit の busy・[8] mc_idle・[9] faulted・[10] halted・[11] ring_empty・[12] mem_ready・[13] axi_stuck・[14] halt_int・[18:16] cp_state。tb は [9] を見る。
2. FAULT_INFO（§3.5、ghci.md:417〜）: 仕様は [3:0] KIND・[7:4] UNIT・[15:8] SESSION・[19:16] STAGE・[31:20] SYNDROME。RTL（`atg_cp_ctl.v:440-460`）は kind を [15:12]・block [23:16]・client [7:0]・tag [31:24]。tb は [15:12] を見る。
3. SESSION_ACTIVE が PROT=1 でも 1 固定（§3.3・§11.6）。
4. CPFW の register（IBASE〜MBOX、0x1A0〜0x1BC）が §4.1 の CPFW の窓 0x160〜0x17F の外（11 個の register は 16 byte に入らない、B と対）。SA（id 7）の指す先が 0x140（FMT と同じ、SA は list に無いので実害なし）。
5. FMT_OPTIMAL・LINEAR: FFILT=0 でも FP16 の format が FILTER_LINEAR、ROP_PACK=0 でも UINT・SINT が COLOR_ATTACHMENT（§4.1 の除外）。
6. DEVICE_CAPS: [7] GSI1 を NSU≠0 で作る（仕様は Fixed 1）。[25] TEX_FFILT に SU の FP16 を掛けている（仕様は CONFIG_TU_ENABLE_FFILT だけ）。[3]・[4] は parameter の既定 0 で実装から作っていない（seq・fw に DRAW_INDIRECT・EXECUTE_COMMANDS の処理があり、0 が誤りの可能性、未確認）。[31] は MS4 だけを見る。
7. EXT_CAP_FMT0: [9] SRGB 1 固定、[11] FFILT_FP32 0 固定、[10] が SU の FP16 に依る（依頼は FMT の FP16）。[7] ETC2: atg_top が読む `CFG_TU_ENABLE_ETC2` を Makefile が作らない（Makefile は `CFG_ENABLE_ETC2`）。ETC の decoder は FFILT の時に atg_tu_ffilt の中にあり、balanced で decoder が在るのに [7] が 0。`CFG_SU_ENABLE_SUBGROUP` も Makefile が作らない名前。
8. CAPS を外から注入する parameter が残る（`atg_cp_regs.v:758`）。VEND の SUBSYSTEM の既定が汎用 module で KC705 の値（:304、ZCU106 も KC705 の ID を答える）。
9. include: `.vh` に新しい map と重なる別名（`GHCI_PERF0` = 0x080 は EXT_CAP_FMT0 と重なる、:242）、`GHCI_KICK = GHCI_LEGACY_KICK` を宣言の前に使う（:268、Vivado Synth 8-6901）。`.h` の FENCE_LAST・PIPE_FSM・DBG_* に接頭辞が無い。tb・harness が追従していない。
10. 死んだ論理: ring の queue 1・2・DMA・preempt・sema を 0 を入れて残し unused_ok に。dbg の本体、`kick_prot_ok`、ctl の comment の KICK・aperture（:10,12,33,51,52,82）、`I_PERF_CTRL`・`A_LOCAL_*`、ring の `sz_clamp_c`（:1276）、unused_ok の古い幅（:1889）。合成の版だけが RING_SIZE の書き込みに `wmask[0]` を要る。
11. `atg_cp_regs.md` に RTL に無い物（PERF_CTRL 0x160、PERF_SRC 0x168..、CPFW_DOORBELL 0x1C0・RESET 0x1C4、WINDOW_OFFSET 0x1F4（RW）。0x1F4 は ghci.md で MEM_CACHE_CFG（RO））。VF の comment「4,8,16」（仕様 2,4,8）。

## tb（R13）の残り

- preset ごとに compile する仕組みが無い（`sim/run.sh` に無い）。
- DUT は atg_top を直に置き、KC705 の board と parameter が違う（S_ID_W 12、SU_GRP_W・SU_NREGS・SU_NWARPS・HAS_L2・L2_*・PROT_GRANTS を渡さない、minimal の GRP_W が 2 のまま（正しくは 3）、SU_SUBGROUP を Makefile が作らない macro から取り balanced で 0、board は 1）。
- 16 進の定数の期待値が残る（RESIDENT_WARPS 0x00200004、FMT 0xEF・0xED、EXTCAP の指す先）。CAPS の式が RTL の macro の写しの所が多い。
- EXTCAP は指す先だけを照合し、PERF_SRC・CPFW・DISP2・MEM_CACHE_CFG・DBG を照合しない。
- RESET を IDLE で書いて ring が動かない事は試していない。TIMESTAMP_HI は内部の counter を force して試す。
- R2・R3 は誤った bit の位置（上の A1・A2）で照合。

## B（仕様が黙っている）

- RING_SIZE のリセット値（RTL は 4 KiB）、size を変えた時の WPTR・RPTR の扱い、RING_BASE_HI が fetch に効かない（ADDR_W 32）。
- TIMESTAMP の LO と HI の読みの原子性。
- VEND の VENDOR_ID が PCI の 0x10EE か "AMAT" か。PERF0〜3 の counter の置き場所が GHCI 1.0 の map に無い。
- MEM_CACHE_CFG: [27:24] は 4 bit で TU 16 KB が入らない、minimal の値（L2 4 KB・2 way・TU 2 KB）が列挙の外。
- FMT_BUFFER の bit の並び。
- §5 の「リセットで SESSION_ACTIVE を消す」と §6 の「session 0 は常に 1」の関係。
- CPFW の 11 個の register が §4.1 の CPFW の窓（PERF と共用の 0x160〜0x17F）に入らない。
