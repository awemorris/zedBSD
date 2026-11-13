<!-- awesome-plan project=zedbsd record=ws347-ghci-rtl-questions -->

# GHCI の仕様と Amaterrace GPU の RTL の台帳

- 役割分担（2026-11-11 ユーザーの決定、AGENTS.md §7）: Claude は仕様と RTL を読んで監査し、driver を書く。Gemini は Claude の指揮の下で RTL を実装する。
  `docs/ghci.md` が正。食い違いは A（仕様の文が明確で RTL が違う → Gemini への修正依頼）と B（仕様が黙っている・表せない・設計の意図が要る → ユーザーの判断）に分ける。迷ったら B。
- この file が唯一の台帳。決定が変わったら行を書き換える（訂正を書き足さない）。担当は会話でなくこの file を読む。
- Gemini への依頼の文: [gemini-request-01.md](gemini-request-01.md)（A1〜A11 を T1〜T4 に。2026-11-11 作成、ユーザーが渡す）。
- Gemini への質問の文: [gemini-questions-01.md](gemini-questions-01.md)（B4〜B10 の RTL の意図と事実。2026-11-11 作成、ユーザーが渡す）。1 次回答と Q1 の照合: [gemini-answers-01.md](gemini-answers-01.md)。2026-11-11 ユーザーが修正方針を承認し、依頼の文 [gemini-request-02.md](gemini-request-02.md)（W1〜W7）を作った。
- Gemini の報告 02（ad23fe01）の監査: [audit-ad23fe01.md](audit-ad23fe01.md)。直しの依頼: [gemini-request-03.md](gemini-request-03.md)（C1〜C11、ring の回帰が最優先）。
- C1・C2 だけの直し（8ebb2e2f）の監査: [audit-8ebb2e2f.md](audit-8ebb2e2f.md)（C1 ほぼ完了、C2 完了）。依頼 04（まとめ版、R1〜R14）: [gemini-request-04.md](gemini-request-04.md)。その結果（eeed36ed）の監査: [audit-eeed36ed.md](audit-eeed36ed.md)。依頼 05（まとめ版、S1〜S11）: [gemini-request-05.md](gemini-request-05.md)。その結果（28a8d920）の監査: [audit-28a8d920.md](audit-28a8d920.md)。依頼 06（まとめ版、U1〜U15）: [gemini-request-06.md](gemini-request-06.md)。その結果（b58d4edd、U1〜U9 だけを判定）の監査: [audit-b58d4edd.md](audit-b58d4edd.md)。依頼 07（まとめ版、V1〜V9）: [gemini-request-07.md](gemini-request-07.md)。その結果（fe6431b1）の監査: [audit-fe6431b1.md](audit-fe6431b1.md)（V2・V5 済み、他は一部、V3 の直しで seq が halt を見なくなった回帰）。**次の依頼は 08（まとめ版、01〜07 を置き換え、V の残りを X1〜X9 に、U10〜U15 はまた次に回す、下書き。Gemini がまず 1 次回答）: [gemini-request-08.md](gemini-request-08.md)。**
- 読んだ版: amaterrace-gpu main 50f13348（2026-11-11 15:28、Gemini の修正 5fb4ad16・1e80cfb6・50f13348 の後）。ghci.md は bace7d66 から VA_BITS_ENC の 2 行だけ違う。RTL は読んだだけで、sim・実機では確かめていない。監査の全文は [audit-50f13348.md](audit-50f13348.md)。

## A: RTL の修正依頼（Gemini へ）

| # | 食い違い | 仕様 | RTL | 期待 | 状態 |
| --- | --- | --- | --- | --- | --- |
| A1 | KC705 の board top が atg_top に `SLIM_MMIO` を渡さず既定 0（旧の map）。GHCI 1.0 の page 0（MAGIC・DEVICE_CAPS・EXT_CAP_*）が host から見えない見込み | §3 の page 0 の表 | `boards/kc705/atg_kc705_top.v`、`rtl/gpu/top/atg_top.v:97`、`atg_cp_regs.v` の非 SLIM の分岐 | KC705 で Slim MMIO の page 0 | 直った（ad23fe01） |
| A2 | `DEVICE_CAPS` の並び: [24] が SA、[25] が TRILINEAR、[26]・[30] が 1 固定 | §3.2.1: [24] PROT、[25] TEX_FFILT、[26] ROP_PACK、[30] DISP | `rtl/gpu/cp/atg_cp_regs.v` の `ATG_DEF_DEVICE_CAPS` | 構成から仕様の bit | 一部（ad23fe01）→ 依頼 04 の R7・R8 |
| A3 | `EXT_CAP_FEATURE` が古い並び（[0] LOGICOP・[1] EARLY_Z・[2] CURSOR・[3] 1・[7:4] MRT 数）。include の `GHCI_EXT_FEAT_*` も古い並び | §3.2.2: [3:0] VA_BITS_ENC（VA − 30）・[7:4] PA_BITS_ENC・[8] DUAL・[9] COARSE・[10] FAST_CLEAR・[11] LOGIC_OP・[12] EARLY_Z・[13] HW_CURSOR・[14] COLOR_LUT・[15] MRT_BLEND・[19:16] MAX_MRT・[27:20] LDS_KB・[28] FP16・[29] SUBGROUP・[30] CLIP_POLY・[31] BLT_SCALE | `ATG_DEF_EXT_CAP_FEATURE`、`include/ghci_regs.h`・`.vh` | 仕様の並び（minimal は VA_BITS_ENC 0x0） | 一部（ad23fe01）→ 依頼 04 の R7・R8 |
| A4 | `EXT_CAP_FMT0[9] CAP_SRGB` が 0 固定（minimal は sRGB を有効に構成） | §3.2.2 の EXT_CAP_FMT0 | `ATG_DEF_EXT_CAP_FMT0` | 構成の sRGB | 一部（ad23fe01）→ 依頼 04 の R7・R8 |
| A5 | `config.mk.template` に `CONFIG_TU_L1_CACHE_KB`・`CONFIG_MC_L2_WAYS`・`CONFIG_VF_CACHE_LINES` が無く、生成した `atg_config.vh` の macro が空で `atg_tu` などが構文 error（P1 の読み） | — | `config.mk.template`、Makefile の `include/atg_config.vh` の規則 | 既定の構成で elaborate が通る | 未着手 → 依頼 04 の R12 |
| A6 | extended capability の窓への書き込みが全部捨てられる（`hcr_we` は page 0 の 11 個だけ）。DISP2・MEM の WINDOW_CTRL・CPFW・FMT_INDEX・PROT に host が書けない | §3.1 の 0x100..0x3FF は RO/RW、§4.1 | `atg_cp_regs.v:287-301`（5fb4ad16 で表に出た） | 窓の register に書ける | 一部 → 依頼 04 の R4・R5 |
| A7 | 答えの無い register: RESIDENT_WARPS 0x04C・GCLK_HZ 0x064・NATIVE_ISA_ID 0x068・MEM 0x1E0〜0x1F3（全部 0 に読める見込み）・PERF_SRC・VEND | §3.1、§3.2.3、§4.1 | `atg_cp_regs.v:560-578` の own の case | 仕様の値（MEM は LOCAL_BASE 0x80000000・LOCAL_SIZE 0x40000000・WINDOW_SIZE 0x04000000・HOST_LIMIT 0x80000000） | 一部 → 依頼 04 の R3・R5 |
| A8 | 読みの index の流用で別の register と混ざる: SCRATCH←WDT_CYCLES、SESSION_ACTIVE←RING_WPTR、TIMESTAMP_LO←RING_RPTR、TIMESTAMP_HI←APERTURE_BASE、GCLK_HZ←APERTURE_LIMIT、NATIVE_ISA_ID←FAULT_ADDR、0x06C・0x070←FAULT_INFO・FENCE_LAST。atg_ghci_tb は単独の試験なので見つからない | §3.1 | `atg_cp_regs.v:412-431` の `hcr_reg_ridx` の default | 各 offset が自分の値だけ | 未着手・悪化 → 済み（8ebb2e2f） |
| A9 | FAULT_INFO 0x018 の W1C が効かない（`hcr_we_ok` に無い）。TIMESTAMP_HI が 0 固定で counter が 32 bit | §3.1 | `atg_cp_regs.v`、`:571` | W1C、64 bit の timestamp | 未着手 → 依頼 04 の R3・R5 |
| A10 | EXTCAP の配置: 見出しの 3 つ目以降を 0x180 から置き仕様の VEND と重なる。FMT 0x160（仕様 0x140）、PERF 0x110、CPFW 0x120、VEND 0xC0（仕様 0x180）。MEM 0x1E0・DISP2 0x200 は一致。NATIVE_ISA_ID の定数が .h・.vh に無い | §4.1 | `atg_cp_regs.v:507-523`、`atg_cp.v:481` | 仕様の配置 | 一部 → 依頼 04 の R4・R5 |
| A11 | `` `elsif defined(...) ``（Verilog の前処理として不正）。macro が無い時に CLIP_POLY・HAS_BLT・TU_BILINEAR が「有り」に倒れる（Makefile を通らない sim・lint で誤る） | — | `atg_cp_regs.v:50,94,136-164` | 正しい前処理、無い時の既定は「無し」か必須の定義 | 未着手・悪化 → 依頼 04 の R9 |
| A12 | `include/ghci_regs.h:193` の comment が VA_BITS を「(32..40)」のまま（仕様は VA_BITS_ENC = VA − 30、VA は 30 bit 以上） | §3.2.2 | `include/ghci_regs.h:193` | comment だけ | 未依頼（2026-11-13 P4 の p003 で見つけた） |

## B: 仕様の問い（ユーザーへ）

| # | 問い | 根拠 | 状態 |
| --- | --- | --- | --- |
| B1 | `CAP_BLT_SCALE=0` の時、copy・1:1 の blit の src と dst の address に何 byte の揃えが要るか（RTL は `atg_blt_align` が外れ、line の中の位置がずれた copy は壊れる見込み） | §3.2.2 [31]、`atg_blt_2d.v`（P2 の読み） | 判断待ち（minimal は Blitter 無しで実機に影響しない） |
| B2 | flat（`CAP_HW_WALKER=0`）の時、VA_BITS は GPU の出す address を縛るか。Q1 は「縛らない、host への到達は MEM の HOST_LIMIT と PA_BITS」と仮に P2 に伝えた（仕様は変えていない） | §11.6、§11 の MEM の表、KC705 の host の窓 [0, 0x8000_0000) | 判断待ち（Q1 の仮の判断の確認） |
| B3 | minimal の LDS は 1 KiB、§3.2.2 の文は「KC705 は 4 KiB を報告」 | `config/minimal.mk`、§3.2.2 [27:20] | 判断待ち（文を直すか） |
| B4 | CP_HEAP_BASE・LIMIT（0x050・0x054）は RTL で旧い APERTURE_BASE・LIMIT に行く。atg_cp_ctl には別に I_HEAP_BASE・LIMIT（index 0x2C・0x2D）がある。§11.2 の heap はどちらか | `atg_cp_regs.v` の hcr_reg_widx・ridx、`atg_cp_ctl.v:126-127` | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W1 で依頼 |
| B5 | EXT_CAP_FMT0[15:12]: 仕様は「8 を報告」と「EXT_CAP_FEATURE[19:16] を写す」の両方を書く。minimal（MRT 1）は 1 か 8 か | §3.2.2 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W2 で依頼 |
| B6 | KC705 に FMT capability が無い（SU_VARY_WORDS 16 で CORE1=0）。仕様は FMT を 0x140 に挙げるが必須とは書かない。minimal に要るか | `atg_top.v:222-223`、kc705_top:352 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W3 で依頼 |
| B7 | KC705 は HAS_CPU=1（RV32 の CP）だが minimal は `CONFIG_CP_USE_RV32_CORE=0` で DEVICE_CAPS[3]・[4]（INDIRECT・SECONDARY）は 0。報告すべきか | §3.2.1 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W4 で依頼 |
| B8 | EXT_CAP_FEATURE[14] COLOR_LUT・[15] MRT_BLEND に対応する構成の値が仕様に無い | §3.2.2 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W5 で依頼 |
| B9 | GCLK_HZ: 仕様 §3.2.3 は「KC705 は 100 MHz」、board top は 200 MHz | kc705_top:109-110 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W6 で依頼 |
| B10 | `spec/gpu/cp/atg_cp.md` が CTRL 0x010・STATUS 0x014・SCRATCH 0x048 と書き ghci.md §3.1 と違う。`atg_cp_regs.md` は「GHCI 2.0」のまま | — | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W7 で依頼 |
| B11 | DBG（EXTCAP の id 2）の指す先と register の窓が §4.1 に無い。KC705 は DBG を list に載せ、指す先は 0 | §4.1、`atg_cp_regs.v` の見出し | 判断待ち |
| B12 | RING_BASE の整列: 仕様は 64 byte の整列を求めるが、hardware が下位 bit を mask するか fault にするかを書いていない（RTL は 32 byte で mask） | ghci.md §3.1 0x024、`atg_cp_ring.v` | 判断待ち |
| B13 | CPFW の 11 個の register が §4.1 の CPFW の窓（PERF と共用の 0x160〜0x17F、32 byte）に入らない。RTL は 0x170 と 0x1A0〜0x1BC に置く | §4.1、`ghci_regs.vh` の CPFW_* | 判断待ち |
| B14 | ring: RING_SIZE のリセット値（RTL は 4 KiB）、size を変えた時の WPTR・RPTR の扱い、RING_BASE_HI が fetch に効かない（RTL の ADDR_W 32）、RING_SIZE の不正な値（RTL は前の値を保って無視） | §3.1、§6 | 判断待ち |
| B15 | TIMESTAMP の LO と HI の読みの原子性（LO を読んだ時に HI を latch するか） | §3.1 | 判断待ち |
| B16 | VEND の VENDOR_ID が PCI の 0x10EE か "AMAT" か。PERF0〜3 の counter の置き場所が GHCI 1.0 の map に無い | §3.2.3、§4.1 | 判断待ち |
| B17 | MEM_CACHE_CFG: [27:24] は 4 bit で TU 16 KB が入らない、minimal の値（L2 4 KB・2 way・TU 2 KB）が列挙の外 | §11 の MEM の表 | 判断待ち |
| B18 | FMT_BUFFER の bit の並び（§4.1 は OPTIMAL・LINEAR の並びだけを書く） | §4.1 | 判断待ち |
| B19 | §5 の「リセットで SESSION_ACTIVE を消す」と §6 の「session 0 は常に 1」の関係 | §5、§6 | 判断待ち |
| B20 | `CONFIG_FMT_ENABLE_FP16`・`FP32`・`SRGB` はどの回路も切り替えない（`atg_cp_regs.v` だけが読む）。EXT_CAP_FMT0 [9]〜[11] と FMT の窓を、この key に従わせて key で回路・format を外すか、回路の実際（常に在る）に従わせて key を消すか。依頼 05 の S4 の「[9] = `CONFIG_FMT_ENABLE_SRGB`」は key が回路を切り替える前提だった | §3.2.2 の EXT_CAP_FMT0、`config/minimal.mk`、28a8d920 の `atg_cp_regs.v:740-752` | 判断待ち |
| B21 | §3.1 の page 0 に無い offset（0x06C、0x070、0x07C、0x088〜0x0FC）は 0 を読み書きを捨てるべきか。RTL は 0x070 に旧い FENCE_LAST（host RW）を戻した。DISP2 には「other offsets read 0」があるが page 0 には無い | §3.1、ghci.md:2697、28a8d920 の `atg_cp_regs.v:415,432,587` | 判断待ち |
| B22 | CAP_PROT = 1 の時、走行中の locked register への書き込み（kind 7）で STATUS.FAULTED を立てるか。§3.3 は PROT の時の FAULTED を AXI_STUCK・ring の framing・session 0 の不正・hang に限る（ONLY）が、host の MMIO の誤りは挙げていない。RTL と tb は両方の preset で FAULTED | §3.3 | 判断待ち |
| B23 | ROP_PACK = 0 の時、Path 2 の format の BLEND と BLIT_DST も 0 にするか。§4.1 は COLOR_ATTACHMENT だけを書く。HAS_BLT = 1 なら blitter が書ける。RTL は両方 0 | §4.1、`atg_cp_fmt.v:63-65` | 判断待ち |
| B24 | DISP2 の FLIP の不正（plane・base・record）の FAULT_INFO の KIND と UNIT。§3.5 の UNIT に display が無い（display_id の不正だけ kind 1 と書く）。RTL は kind 3・UNIT 0。MC の client の SA・DC の UNIT も同じ問い（b58d4edd の RTL は SA → 2（SU）、DC → 0（CP）、`atg_cp_ctl.v:254-272`） | §3.5、§7.3 CMD_FLIP、`atg_cp_disp.v:206` | 判断待ち |
| B25 | MC の拒否と ring の読みの error の KIND: session の保護の拒否は 6（PROT_FAULT）か 10（PERM_FAULT）か、flat で HOST_LIMIT・local の外は 9（PAGE_FAULT）か、ring の読みの応答の error は 2（RING_OOB）か 5（AXI_ERROR）か。RTL（b58d4edd）は 10・9・5（ring の読みの error は 28a8d920 の 2 から依頼の外で 5 に変わった） | §3.5、`atg_cp_ctl.v:399-401,485-487`、`atg_cp_ring.v:1776,1787` | 判断待ち |
| B27 | CP の firmware の core（RV32）の例外（PC の範囲外、不正な命令、不整列の jump）の FAULT_INFO の KIND と UNIT。§3.5 の KIND・UNIT は firmware の core の fault を挙げない。RTL は KIND 1（INVALID_OP）・UNIT 0（28a8d920 は KIND 4） | §3.5、`atg_cpu_core.v:373-380`、`atg_cp.v:612` | 判断待ち |
| B28 | CMD_WAIT_FENCE の `fence_addr` の揃え。§7.3 は CMD_FENCE の `fence_addr` だけに「8-byte aligned」と書き、CMD_WAIT_FENCE には書かない。RTL は 4 byte に揃えて 32 bit を比べる。仕様の 64 bit の比較（依頼 07 の V2）で 8 byte を読むなら揃えが要る（V2 は仮に 8 byte） | §7.3 ghci.md:960-991、`atg_cp_fence.v` | 判断待ち |
| B26 | §4.1 の PERF_SRC は「STATUS の busy の bit の番号（0 CP〜6 SU）」を数えるが、§3.3 の STATUS に busy の bit はもう無い。PERF_SRC の番号を何で定義するか | §4.1 PERF、§3.3 | 判断待ち |
| B29 | SU の FAULT_ADDR の単位: §3.5（ghci.md:450）の「stage の入口からの PC」は命令の番号（8 byte 単位、今の `sb_fault_pc`）か byte の offset か | ghci.md:450、`atg_su_sb.v:775` | 判断待ち |
| B30 | §11 の page table の walk は VA 32 bit と 40 bit だけを書く。30〜31・33〜39 bit の時の段数と root の大きさ。driver は VA ≤ 32 を 2 段、VA > 32 を 3 段とし、VA の中だけの address を渡す（どちらの答えでも安全） | §11、§3.2.2 | 判断待ち（Q1 の推し: VA ≤ 32 は 32 bit の walk の上位を 0、33〜40 は 40 bit の walk の上位を 0、と 1 文足す） |
| B31 | FMT0[11] FFILT_FP32 = 0 で CAP_TEX_FFILT = 1 の時の意味（FP32 の format の linear の filter が無い、で良いか）。driver は「FP32 の linear の filter を報告しない」と読む（機能を減らす側だけ） | §3.2.2 の EXT_CAP_FMT0 | 判断待ち（Q1 の推し: driver の読みで良い。仕様に 1 文足す） |
| B32 | flat（`CAP_HW_WALKER=0`）で discrete（`CAP_UMA=0`）の時、shader の load・store、descriptor、PSO、`CMD_COPY_*` の address が local memory と host のどちらへ行くか。§11.6 は「All addresses are flat physical」とだけ書き、local と host の振り分けは §7.3 の `CMD_FENCE` の `fence_addr`（N12: [LOCAL_BASE, LOCAL_BASE + LOCAL_SIZE) は local、他は host）にしか無い。driver は今、walker の無い GPU では local memory を使わない（KC705 の minimal は frame も Vulkan の memory も host memory に置き、表示の controller は host を PCIe 越しに読む） | §11.6、§7.3 CMD_FENCE（N12）、§13.2 の FB_BASE | 判断待ち（P4 の推し: flat でも全ての address を `CMD_FENCE` と同じ規則で振り分ける、と §11.6 に 1 文足す。決まれば driver は KC705 の minimal でも frame を local に置け、表示の読みと shader の写しの書きが PCIe から外れる） |
| B33 | CAP_PROT = 1 で session i（≥ 1）の fault のうち、KIND 4（watchdog）と KIND 5（AXI_ERROR）も「session の fault」（FAULTED を立てず CP は走り続ける）か。§3.3 の同じ文が、PROT の時の FAULTED を AXI_STUCK・ring の framing・session 0 の不正・「unrecoverable hardware pipeline hang」に限り、かつ「client session の fault は決して FAULTED を立てない」とも書く。RTL（fe6431b1）は KIND を見ず FAULT_INFO の SESSION ≠ 0 だけで session の fault にする。watchdog の fault は drain した session の tag（`wdt_tag`）を持つ | §3.3（ghci.md:388）、`atg_cp_ctl.v:541`、`atg_cp_prot.v` の `wdt_tag` | 決定（2026-11-13 ユーザー: session の fault。今の RTL どおり、SESSION ≠ 0 なら KIND 4・5 も session だけを止め CP は走り続ける） |
| B34 | FAULT_INFO が uncleared（前の fault を latch 中）の時に、別の session j の fault や whole-device の fault が来たらどうするか。§3.5 は後の fault を「dropped」と書くが、それが FAULT_INFO の記録だけを捨てるのか、SESSION_ACTIVE[j] を 0 にする事（§3.3）や FAULTED への遷移も捨てるのかを書かない | §3.5（ghci.md:415）、§3.3 | 決定（2026-11-13 ユーザー: 捨てるのは FAULT_INFO・FAULT_ADDR の記録だけ。SESSION_ACTIVE[j] の 0・FAULTED への遷移・IRQ_FAULT は行う） |
| B35 | host の BAR の local の窓（MEM の WINDOW、KC705 は BAR2 → MIG の第 2 port で GPU を通らない）と GPU の L2（write-back）の一貫性。host が窓から書いた ring・vertex を GPU が L2 の古い line で読む事、GPU が local に書いた物（fence、描画の結果）を host が窓から読めない事を、hardware が防ぐか（窓を L2 に通す、local を cache しない、doorbell で L2 を無効化する）、driver が command で防ぐか。§6.3 は host memory の DMA の同期だけ、§11 の MEM の WINDOW は一貫性を書かない | §6.3（ghci.md:682-687）、§11 の MEM、`atg_mc.v:566,597`、`boards/kc705/atg_kc705_top.v:16-18,234` | 決定（2026-11-13 ユーザー、同日に訂正: host が窓から GPU の local memory に書いてよいのは GOP の VRAM（firmware の boot の framebuffer）だけ。ring・vertex・fence など host と GPU が分け合う物は全部 host memory に置き GPU が DMA で読み書きする。だから窓と L2 の一貫性の問題は起きず、driver がこの使い方を守る。RTL は X3 で変えない。仕様の §6.3・§11 に「窓は GOP の VRAM のため」と書く） 追加（2026-11-13 ユーザー）: discrete でも scan-out は host memory を読める。GOP の framebuffer が host memory か板の VRAM かは、host（または板の UEFI）の firmware による。§13.2・§11.3 の比べの表の「discrete の scan-out は local」は直す。 |

## 決定済み

| # | 件 | 決定（2026-11-11 ユーザー） |
| --- | --- | --- |
| D1 | SFU（rcp・rsq・exp2・log2・sin・cos・fsqrt.s） | 仕様上必須。minimal の RTL はひとまず SFU 無しのまま。疎通の試験は compiler が SFU を使わない範囲で |
| D2 | VA の幅 | 30 bit 以上。`VA_BITS_ENC` = VA − 30（6949b248）。page table の説明は変えない |
| D3 | 命令の規則 | CAPS に無い命令は使ってはいけない |

## 情報（P1 の読み）

- TU の BC decoder（`atg_tu_decp`）はどこからも instance されていない。GPU の top に `atg_mmu` が無く、VA_BITS は使われていない。KC705 は minimal でも HAS_CPU=1。
