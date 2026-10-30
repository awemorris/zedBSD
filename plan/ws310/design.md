<!-- awesome-plan project=zedbsd record=ws310-design -->

# WS310 設計: ACPI の _OSC を呼び、システム全体の挙動を厳重に確かめる

- 版: 第 1 版（2026-10-31 P6、q1779-i01）。design-reviewer の敵対的レビュー（[design-review.md](design-review.md)）を反映した（§14）。
- 基点: worktree `p6-ws310`（branch `agent/p6-ws310`、main e2f816e14）。引用の file:行 は基点の物。code は書いていない。
- 読んだ物: `AGENTS.md`、`plan/agents/protocol.md`、[ws.md](ws.md)、`plan/guardrail.md`（範囲・HAL・driver は HAL を知らない・出典）、`plan/ws333/design.md`（§0・§1・§2・§4.4・§4.5・§4.16）、`plan/ws301/design.md`（§2.1 の `_CPC` の調べ）、`plan/ws303/design.md`（IETM の `_OSC`）、WS300 の branch `agent/p6-ws300` の design.md §4.3（Q300-6）、`plan/tools.md`（aml-host・hw5330）、source（§2）、5330 の DSDT・SSDT（`plan/tools/aml-host/latitude5330/`、iasl 20250404 で scratchpad に逆 assemble。行番号はその `.dsl` の物）。
- 取れなかった物: ACPI 6.5 の仕様（uefi.org の HTML と PDF が bot の確かめの page を返した。回避していない）、PCI Firmware Specification（PCI-SIG）、Intel の processor の vendor-specific な `_OSC`/`_PDC` の文書。bit の意味は 5330 の AML の使い方と、repo にある Linux の起動の log（QEMU q35）の bit の名前で裏付けた範囲だけを使い、各所に「仕様で未確認」と書く（§9、Q310-6）。

## 0. ユーザーの決定（要点）

| 日付 | 要点 | この設計での扱い |
| --- | --- | --- |
| 2026-10-29（WS301 Q301-3） | `_OSC` は最終的には呼ぶが、別の WS にして、システム全体の挙動に問題が無いかを厳重に確かめる。それまでは呼ばない（原文は [ws.md](ws.md)） | 既定は呼ばない（`acpi.osc=off` と同じ）まま実装し、確かめが通ってユーザーが承認してから既定を変える（§5.6、Q310-1）。確かめは host（5330 の表）・QEMU・5330 の実機の 3 段（§6） |
| 2026-10-31（Q1 の依頼） | 不在の間（昼頃まで）設計だけ進められる WS は設計を進める。WS301 は WS333 に統合され、WS333 の CPU の枠組みと熱が main に入ったので設計の時期になった | この文書。source は変えない |

## 1. 目標と範囲

- 目標（[ws.md](ws.md)）: zedBSD が ACPI の `_OSC` で OS の能力を firmware に申告し、firmware が出す情報（`_CPC` など）を使えるようにする。呼ぶ前と後で、電源・PCIe・熱・CPU の性能などシステム全体の挙動に問題が無いことを、実機（まず 5330）で厳重に確かめる。
- 含む: platform-wide（`\_SB._OSC`）・processor（ACPI0007 の `_OSC`）・PCI/PCIe の host bridge（PNP0A08・PNP0A03 の `_OSC`）の 3 種の評価、query と commit の手順、起動の switch、記録、host の harness の追加、5330 の前後の比べ。
- 含まない: USB4 の `_OSC`（UUID 23a0d13a-…、zedBSD に USB4 の connection manager が無い。§4.1）、Intel DTT（IETM）の `_OSC`（DTT の driver が無い。§4.4）、`_CPC` を使う CPU の制御（WS333。この WS は `_CPC` が namespace に現れ、HWP の MSR と同じ値を指す事を確かめるまで）、ASPM・LTR の設定そのもの（WS300。この WS は OS が制御を得た事を PCI の driver に伝える口まで）、S3（zedBSD は使わない）。
- 対象: 5330（i5-1245U、Dell BIOS 1.31.1 の表）。QEMU q35 は PCIe の `_OSC` だけを持つ（§6.2）。他の機械は `acpi.osc=off` が既定の間は振る舞いが変わらない。

## 2. 今の振る舞い（code の事実）

- `_OSC`・`_PDC` を評価する code は無い（`src/` を grep、0 件）。`\_OSI` は native の method で、Windows 2000〜"Windows 2022" と "Extended Address Space Descriptor" に真を返す（`src/drivers/acpi/aml-osi.c:27〜53`）。firmware は自分を Windows 2022 の上と見ている。
- ACPI の attach の順（`src/drivers/acpi/acpi-kern.c:165〜283`）: 表の発見 → SystemMemory・SystemIO・PCI_Config の handler（`:183〜196`）→ DSDT・SSDT の load（`:199`）→ `drv_acpi_initialize_objects()`（`:213`）→ ECDT の EC（`:218`）→ `_REG`（`:221`）→ `_STA`・`_INI`（`:226`）→ SCI と event の thread（`:229`）→ Global Lock → EC（`:240`）→ power・thermal・WMI・Dell・LPS0 → PCI の電源の口（`:270`）→ `/dev/acpi`（`:273`）。
- platform の順（`src/kern/platform/pcat.c`）: PCI の driver の登録 → `drv_pci_pcat_init()`（`:437`、host bridge の probe で登録済みの driver が bind、VMD とその下の NVMe もここ）→ `drv_acpi_attach()`（`:450`）→ 窓を待った function の割当（`:463`）→ UCSI → UEFI の変数 → `drv_cpu_x86_init()`（`:498`、WS333 の CPU の driver）。**PCI の function は ACPI より前に bind される**。
- PCI の driver が触る PCIe の機能（`src/drivers/pci/pci-power.c`）: PCI PM の PME_En（`:1033〜1071`、endpoint の PM の capability）、LnkCtl・LnkCtl2・LTR の latency・L1SS の保存と復元（`:212〜229`・`:318〜325`）。native の hotplug・AER・DPC・root port の PME の割り込みの処理は無い。ASPM・LTR を設定する code は main に無い（WS300 の branch に `pci.vmd.aspm=on` の時だけの道がある）。
- 起動の parameter: 固定の enum（`include/kern/boot.h:107〜129`）と名前の表・語の検査（`src/kern/boot.c`）。driver は `kern_boot_handoff("boot.command-line")` と `kern_boot_parameters_token_present()` で語を読む（前例 `src/drivers/platform/pcat/cpu-x86/cpu-x86.c:116〜153` の `cpuperf=off`）。
- 診断の口 `/dev/acpi`（`src/drivers/acpi/acpi-dev.c`）: 読むと namespace、path を書くとその評価の結果を `PATH = VALUE` で読める。
- Notify の受け手が無い時は `ACPI: Notify(PATH, 0xNN) has no handler` を log に出して捨てる（`src/drivers/acpi/aml-sync.c:242〜251`）。AML の `Debug` への store は `ACPI Debug: …` の行になる（`aml-eval.c:2038〜2050`）。
- MADT は HAL が読む（`src/hal/amd64/bsp-pcat/acpi.c`）。ACPI の driver は自分の表の写しから `drv_acpi_table_lookup("APIC", …)`（`acpi-kern.c:589〜611`）で読める。
- S3 は無い（`acpi-sleep.c` に `_WAK`・`_PTS` の S3 の道は無い。S0 idle だけ）。D3cold は `_PS3` と電源の資源を落とす道がある（`acpi-sleep.c:1009〜1058`・`:1261`）。

## 3. 5330 の firmware の `_OSC`（表の読み）

`\_SB._OSC`・`\_SB.PRxx._OSC`・`\_SB.PC00._OSC`・`\_SB.IETM._OSC`（DPTF）の 4 つがある（他の `_OSC` は無い、`grep -c _OSC` で DSDT 9 行・SSDT2 64 行・SSDT3 1 行）。AML の block は写さず、振る舞いを自分の言葉で書く。

### 3.1 platform-wide `\_SB._OSC`（DSDT `dsdt.dsl:28366〜28447`）

- UUID 0811b06e-4a27-44f9-8d60-3cbbc22e7b48 の時:
  1. **どの呼び（query でも）でも最初に `DDMS()`**（`:80647〜`）: `\DPIW` が 0 でなく `\_SB.PC00.GFX0.IPCO()`（`PCON` の bit 7・8 が両方 1、`ssdt6.dsl:772〜`）が真なら `\_SB.SGOV(DPG1, 1)`（GPIO の出力を 1）。**hardware への書き**。DPIW・PCON・DPG1 の値は未読（p003）。
  2. revision 1 なら `OSCP` ← DWORD2（query でも commit でも書く。namespace の Name）。bit 2（`_PR3`）を求めると `OSCO` ← 4、`HGMD & 0xF != 2` かつ `RTD3 == 0` なら bit 2 を落として「masked」を返す。
  3. `U4FN`（USB4、`:28296〜28358`）: `U4SE == 0` なら `OSCM` ← 0 で終わり。`U4SE != 0` で `U4CM & 0x70 != 0` かつ **commit（query の bit が 0）なら `DOCM()`**（`:28226〜28293`）: iTBT の host router ごとに `STCM(OSU4, U4CM)`（`ssdt7.dsl:1989〜2026`）。pre-boot の CM の mode（`U4CM & 7`）が OS の望み（bit 18 を求めなければ 0 = firmware CM）と違えば `CHCM()`（`ssdt7.dsl:2141〜`）で **iTBT の mailbox に CM の切り替えを送る**（force power、`Sleep` の待ちを含む）。その後 `U4CM & 0x70 == 0x20` なら **`WCMP(OSCM)` が CMOS の bank（port 0x72・0x73、index 0xF2）に書く**（`:28217〜28223`）。CMOS は電源を切っても残る。
  4. bit 18（native USB4）を求めたのに platform が SW CM を出せない時は bit 18 を落とす。
- UUID 23a0d13a-…（USB4）は `OSCM == 1` の時だけ処理する。他の UUID は「unrecognized UUID」を返す。
- `OSCP` を読む所（`grep \bOSCP\b`）: processor の `GCAP`（bit 5・6、`ssdt2.dsl:449〜466`・`:515〜520` ほか）、`_PSS` と `_PDL` の表の選び（bit 10、`dyn-ssdt17.dsl:57`・`ssdt3.dsl:1375`）。`OSCO` を読む所は無い。

### 3.2 processor `\_SB.PRxx._OSC`（SSDT2 "CpuSsdt" `ssdt2.dsl:344〜`、PR00〜PR63）

- `\_SB.PR00`〜`PR63` は ACPI0007（`dsdt.dsl:30494〜`、`_STA` 無し。在るかは MADT）。各 `_OSC` は UUID 4077a616-290c-47be-9ebd-d87058713953（Intel の processor の UUID、文書は未取得）を確かめ（違えば status 6）、revision 1 でなければ status 0xA、query なら DWORD2 を 0x3BFF で masked して返すだけ。commit なら `GCAP`:
  - `PFxx` ← `(PFxx & 0x7FFFFFFF) | DWORD2`（初期値 0x80000000 は「まだ呼ばれていない」の印）。PR01 以降は `PF00` にも同じ値を写す（`:530`）。
  - PR00: `CFGD & 0x72` かつ `CFGD & 0x200` かつ `PF & 0x18` なら `_CST` の表を Load、`PF & 9 == 9` かつ `CFGD & 1` なら `_PSS`・`_PCT`・`_PPC` の表（IST）を Load、`CFGD & 1` か `CFGD` bit 22 なら `_PSD` の表を Load、`OSYS >= 2015` かつ `CFGD` bit 22 かつ **`\_SB.OSCP & 0x40` なら `_CPC` の表（Cpu0Hwp）を Load し、CPU NVS の `HWPV` ← 2**（`ssdt2.dsl:234` の field。SystemMemory 0x61511000 の `PNVS`。AML の中に読み手は無く、SMM が読むかは不明）。
  - PR01〜: `PF & 9 == 9` で IST、`\_SB.OSCP & 0x20` で `_CPC`（ApHwp）、どちらかで `_PSD`、`PF & 0x18` で `_CST` の AP の表を Load。
  - Load の済みは `\SDTL` の bit で覚え、二度目は Load しない。表の番地と長さは SSDT2 の `\SSDT` の package（`ssdt2.dsl:93〜120`、`plan/tools/aml-host/latitude5330/dynamic/` の 8 つの file の長さと一致を確かめた）。
- `PF00` を読む所: `SPNT`（AC の抜き差しで `_PPC` が有る時、processor に Notify 0x80・0x81、`ssdt14.dsl:577〜`）、`_PTC`（DPTF の participant、`ssdt3.dsl:1256`）、動的な `_PSD` の HW の協調（`dyn-ssdt18.dsl:83`）、`_CST` の FFH の選び（`dyn-ssdt16.dsl:140〜150`）。
- `_CPC`（`dyn-ssdt19.dsl:425`）は FFH の register で、MSR 0xCE・0x771（HWP_CAPABILITIES）・0x774（HWP_REQUEST）・0xE7 などを指す。WS333 の driver が直に読む MSR と同じ物。

### 3.3 PCIe の host bridge `\_SB.PC00._OSC`（DSDT `dsdt.dsl:5406〜5462`）

- UUID 33db4d5b-1ff7-401c-9657-7441c03dd766 の時: `SUPP` ← DWORD2、`CTRL` ← DWORD3。`NEXP == 0` なら CTRL の bit 0〜2 を落とす、`ITSP` が真なら bit 3 を落とす。
- **query でも状態を変える**: query の判定が `If (~(CDW1 & One))`（bit の否定。値は 0 にならないので常に真）なので、query でも `OSCC` ← CTRL（GNVS の field、`dsdt.dsl:1290`、SMM から見える）と `SWAK(0xFFFF, 0)` が走る。`SWAK`（`ssdt14.dsl:1263〜`）は `NEXP` が真なら、`OSCC & 1` で `NHPG()`（全 root port の hotplug の SCI を止め、status を clear）、`OSCC & 4` で `NPME()`（全 root port の PME の SCI を止める）。つまり **CTRL の bit 0 か 2 を求めると、query の時点で firmware の hotplug・PME の処理が止まる**。
- `SUPP` を読む所は無い。`OSCC` を読むのは `SWAK` だけ（`_OSC` と `_WAK` から、`dsdt.dsl:27725`）。
- harness の予備の評価（2026-10-31、§6.1 の今の harness、memory は 0 なので `NEXP == 0`）: query・revision 1・DWORD2 0x1F・DWORD3 0x1D で、戻りは DWORD1 0x11（query と masked）・DWORD3 0x18、評価の後の `\OSCC` は 0 → 0x18。**query で GNVS が書かれる事を zedBSD の interpreter で再現した**。

### 3.4 IETM `\_SB.IETM._OSC`（SSDT3 "DptfTabl" `ssdt3.dsl:363〜`）

- DTT の UUID で commit すると EC に `DPST(1)`（DTT の driver が有る）を伝える（WS303 design §2 の調べと同じ）。zedBSD は DTT を持たないので**呼ばない**。

### 3.5 Linux との比べ（参考）

repo にある 5330 の Linux の `_OSC` の log は無い（`plan/ws031/display-ref/dmesg.txt:212〜222` は QEMU q35 の Linux 6.8: processor の `_OSC` が無く `_PDC` に落ちる、PCIe の `_OSC` で OS が ExtendedConfig・ASPM・ClockPM・Segments・MSI・EDR・HPX-Type3 を申告し、SHPCHotplug・PME・AER・PCIeCapability の制御を得る）。5330 の Debian の log は p003 で読む。

## 4. 方針（何を何の bit で呼ぶか）

原則: **zedBSD が実際に行う事だけを申告し、zedBSD が処理する機能の制御だけを求める**。firmware の処理を止める bit（hotplug・PME・AER・DPC）は、zedBSD に代わりの処理が無い間は求めない。

### 4.1 呼ぶ物と呼ばない物

| `_OSC` | 呼ぶか | 理由 |
| --- | --- | --- |
| PCIe の host bridge（PNP0A08、PNP0A03 は PCI の bit だけ） | 呼ぶ（段 1） | WS300 の ASPM・LTR が OS の制御を要る（Q300-6）。5330 で求める bit（§4.2）は firmware の SCI の処理を止めない |
| platform-wide `\_SB._OSC` | 呼ぶ（段 2） | processor の `_CPC` の Load の条件（`OSCP` bit 5・6）。firmware は Windows の上と見ており、`_OSC` を呼ばない OS は firmware の試された道の外 |
| processor（ACPI0007・`Processor`） | 呼ぶ（段 3） | `_CPC`・`_PSD` が namespace に現れる。MADT で有効な CPU だけ |
| USB4（23a0d13a-…） | 呼ばない | USB4 の SW CM が無い。platform-wide の bit 18 も求めない |
| IETM・他の device の `_OSC` | 呼ばない | 対応する driver が無い。device の `_OSC` はその device の driver が持つ物で、この WS の枠ではない |

### 4.2 bit（DWORD）

bit の意味は 5330 の AML の使い方（§3）と QEMU q35 の Linux の log の名前で裏付けた範囲。**仕様の表では未確認**（§9、Q310-6）。

**PCIe の host bridge**（UUID 33db4d5b-…、revision 1、3 DWORD）:

| DWORD | bit | 名前（裏付け） | 申告・要求 | 理由 |
| --- | --- | --- | --- | --- |
| 2（support） | 0 | Extended Config（Linux log の 1 番目） | する | ECAM を使う（`pci-pcat.c:113`） |
| 2 | 1 | ASPM | WS300 の ASPM の道が kernel に入った時だけ | 今の main は ASPM を設定しない |
| 2 | 2 | Clock PM | しない | 設定しない |
| 2 | 3 | Segments | p005 で決める | config の読み書きは segment 0 の外を ECAM で扱う（`pci-pcat.c:200〜`）。列挙（`drv_pci_scan_all`）が MCFG の全ての segment を走るかを p005 で読み、走るなら申告する。5330 の firmware は DWORD2 を読まないので 5330 の振る舞いは変わらない |
| 2 | 4 | MSI | する | MSI・MSI-X を使う（`pci.c:82〜84`） |
| 2 | 7・8 | EDR・HPX-Type3 | しない | 無い |
| 3（control） | 0・1・2・3 | PCIe hotplug・SHPC hotplug・PME・AER（5330 の `NHPG`・`NPME`・`ITSP` の扱い） | 求めない | zedBSD に代わりの処理が無く、5330 では求めた時点で firmware の hotplug・PME の SCI が止まる（§3.3） |
| 3 | 4 | PCIe Capability Structure | 求める | WS300 が LnkCtl（ASPM）を書き、pci-power が LnkCtl・LnkCtl2 を復元する |
| 3 | 5 | LTR | 求める | WS300 が LTR を有効にし、pci-power が LTR の latency を復元する |
| 3 | 7 | DPC | 求めない | 無い |

5330 の firmware は DWORD2 を読まず、DWORD3 の bit 4・5 を落とさない（§3.3）。DWORD3 0x30 なら `OSCC` ← 0x30、`SWAK` は何もしない（bit 0・2 が 0）。

**platform-wide**（UUID 0811b06e-…、revision 1、2 DWORD）: DWORD2 = 0x64。

| bit | 名前（裏付け） | 申告 | 理由 |
| --- | --- | --- | --- |
| 1・3 | `_PPC`・hotplug の `_OST` | しない | `_OST` を評価しない |
| 2 | `_PR3`（5330 の `OSCO`・`RTD3` の扱い） | する | D3cold で `_PR3` の資源を落とす道がある（`acpi-sleep.c:1009〜1058`） |
| 4 | APEI | しない | 無い |
| 5 | CPPC（5330 は AP の `_CPC` を Load） | する | `_CPC` を namespace に出す（目標）。zedBSD は読むだけで、CPU の制御は MSR の直（WS333） |
| 6 | CPPC v2（5330 は PR00 の `_CPC` を Load、`HWPV` ← 2） | する | 同上。5330 の `_CPC` は v2 の形（`CPC3`、`dyn-ssdt19.dsl:209`） |
| 7・8 | LPI | しない | `_LPI` を読まない（idle は HAL が CPUID で選ぶ） |
| 10 | 16 を超える P-state（5330 は `TPSS`/`LPSS` の選び） | しない | `_PSS` を使わない |
| 18 | native USB4 | しない | SW CM が無い |
| 21・22 | PRM・FFH の OpRegion | しない | 無い |

**processor**（UUID 4077a616-…、revision 1、2 DWORD）: DWORD2 = 0（何も申告しない）。理由: zedBSD は `_CST`・`_PSS`・`_PPC`・`_PTC` を使わず、idle と P-state は HAL と WS333 の driver が CPUID・MSR で直に行う。0 の commit でも 5330 は `PFxx` を 0 にし、`OSCP` に従って `_PSD` と `_CPC` を Load する（§3.2）。IST（`_PSS`・`_PPC`）を Load しないので、AC の抜き差しでの processor の Notify（`SPNT`）も起きない。Intel の文書の bit を申告する案は §10・Q310-3。

### 4.3 呼ぶ時期と順

ACPI の attach の中、`_STA`・`_INI`（`acpi-kern.c:226`）の後、SCI と event の thread（`:229`）の前に、次の順で呼ぶ:

1. **platform-wide**（段 2）: query → commit。
2. **processor**（段 3）: MADT の有効な processor の UID の順。各 UID の ACPI0007（`_UID` で照合）か `Processor`（ProcessorID で照合）の `_OSC` に query → commit。`_OSC` の無い processor は飛ばす（`_PDC` には落ちない。§10）。
3. **PCIe の host bridge**（段 1）: namespace の PNP0A08・PNP0A03 ごとに query → commit。結果を（segment、bus）ごとに覚える。

理由:
- platform を processor より先にする: 5330 の `GCAP` は `\_SB.OSCP` を読んで `_CPC` の Load を決める（§3.2）。逆にすると `_CPC` が出ない。
- `_INI` の後: `_OSC` の method が module level の code（`_INI` の中や表の Load の時）で作られる名前に頼る firmware がある。5330 の `_OSC` は `_INI` の結果に頼らないが、一般の順として後にする。
- event の前: SCI の handler（`_Lxx`・`_Qxx`）が `OSCC`・`PFxx`・`OSCP` を読む時に、最初から確定した値を見せる。interpreter の lock（`acpi-kern.c:103`）が評価を直列にするので、event の thread を先に始めても壊れはしないが、確かめの対象を減らす。
- PCI の bind の後になる事: `drv_pci_pcat_init()`（`pcat.c:437`）は ACPI より前。`_OSC` は列挙（config の読み）を妨げない。制御を求めた機能（LnkCtl・LTR）を書く code は、制御を得た後に書く（§5.5 の口。WS300 の ASPM の書きは ACPI の後の段に移す。§5.12）。

## 5. 設計

### 5.1 責務と境界

```
 kern/platform/pcat.c: drv_pci_pcat_init → drv_acpi_attach → … → drv_cpu_x86_init（順は変えない）
 src/drivers/acpi/acpi-kern.c  drv_acpi_attach: … _INI → drv_acpi_osc_negotiate() → start_events …
 src/drivers/acpi/acpi-osc.c   （新）3 種の _OSC の順・対象の列挙・log・結果の保持、起動の語の読み
 src/drivers/acpi/acpi-osc-codec.c/.h（新）純粋な関数: DWORD の組み立て、戻りの検査、query → commit の決め
 src/drivers/acpi/acpi-pci-root.c  host bridge の列挙（既存の root_visitor の判定を使う）
 include/drivers/acpi/acpi.h   drv_acpi_pcie_osc_control()（PCI の driver が引く口）
 src/drivers/pci/…             （WS300 の側）LnkCtl・LTR を書く前に口を引く
 HAL: 変えない（§5.13）
```

- `acpi-osc-codec`: kernel の header を使わず host で試せる（`acpi-text.c` と同じ置き方）。UUID の byte 列（ToUUID の並び）、DWORD の little endian の組み立てと読み、status の bit の判定、「得た control ⊆ 求めた control」の切り詰め、commit で求める値の決め。
- `acpi-osc.c`: interpreter（`drv_acpi_evaluate`・`drv_acpi_object_buffer_new`・`drv_acpi_walk`・`drv_acpi_table_lookup`）だけを使う。driver は HAL を include しない（guardrail 2026-10-30）。

### 5.2 内部の interface（案）

```c
/* src/drivers/acpi/acpi-osc-codec.h（案） */
enum drv_acpi_osc_outcome {
	DRV_ACPI_OSC_ABSENT,		/* no _OSC method */
	DRV_ACPI_OSC_GRANTED,		/* commit returned without an error bit */
	DRV_ACPI_OSC_MASKED,		/* commit returned with fewer bits than asked */
	DRV_ACPI_OSC_REFUSED,		/* _OSC failure, unknown UUID or revision */
	DRV_ACPI_OSC_BROKEN		/* not a buffer, too short, or evaluation error */
};

struct drv_acpi_osc_request {
	const uint8_t *uuid;		/* 16 bytes in ToUUID order */
	uint32_t revision;
	uint32_t count;			/* DWORDs, 2 or 3 */
	uint32_t support;		/* DWORD 2 */
	uint32_t control;		/* DWORD 3 when count == 3 */
};

struct drv_acpi_osc_result {
	enum drv_acpi_osc_outcome outcome;
	uint32_t status;		/* DWORD 1 of the commit */
	uint32_t support;		/* DWORD 2 returned, cut to the asked bits */
	uint32_t control;		/* DWORD 3 returned, cut to the asked bits */
};

size_t drv_acpi_osc_encode(const struct drv_acpi_osc_request *request, bool query, uint8_t *buffer, size_t size);
int drv_acpi_osc_decode(const struct drv_acpi_osc_request *request, const uint8_t *buffer, size_t length, struct drv_acpi_osc_result *result);
void drv_acpi_osc_commit_request(const struct drv_acpi_osc_request *asked, const struct drv_acpi_osc_result *query, struct drv_acpi_osc_request *commit);
```

```c
/* include/drivers/acpi/acpi.h（足す案） */
/*
 * The PCI Express features the firmware let the operating system control
 * for the host bridge of a segment and bus (the _OSC control DWORD,
 * cut to what was asked).  Zero when _OSC was not run, refused or absent.
 */
uint32_t drv_acpi_pcie_osc_control(uint16_t segment, uint8_t bus);
int drv_acpi_osc_negotiate(void);	/* called once by drv_acpi_attach */
```

- 結果の表: host bridge ごとに 1 行（5330・QEMU q35 は 1 つ。上限は固定の小さな数にし、超えた host bridge は呼ばずに log 1 行）。processor は数えた物の要約（呼んだ数・GRANTED・REFUSED・BROKEN の数）だけを持つ。表は `drv_acpi_osc_negotiate()` が一度だけ書き、その後は読むだけなので lock は要らない（`firmware` の表と同じ扱い、`acpi-kern.c:105〜111`）。PCI の driver が読むのは `drv_acpi_attach()` の後（§5.12）。
- 名前の接頭辞は `drv_acpi_osc_`。仕様の名前の写しではない。

### 5.3 手順（1 つの `_OSC`）

1. `_OSC` を `drv_acpi_lookup()` で探す。無ければ ABSENT（log は 1 行、processor は要約だけ）。
2. **query**: DWORD1 = 1（query の bit）、DWORD2・3 = 求める値で評価する。
3. 戻りを検査: Buffer でない・長さが `count × 4` 未満 → BROKEN。DWORD1 の bit 1（failure）・2（UUID）・3（revision）→ REFUSED。bit 4（masked）は続ける。
4. **commit の値**: support は query で返った物 ∩ 求めた物。control は query で返った物 ∩ 求めた物。0 になっても commit する（firmware に OS が `_OSC` を知る事を伝える。5330 の platform の `DOCM` は commit でしか走らないので、§6 の確かめで commit の有無を比べられるように段を分ける）。
5. **commit**: DWORD1 = 0 で評価し、3 と同じ検査。GRANTED・MASKED の時、得た control は「返った物 ∩ commit で求めた物」。firmware が求めない bit を立てても使わない。
6. log 1 行（§5.7）。

query の後の commit で firmware がさらに落とした場合（2 回目の masked）は、その値で終える（3 回目は呼ばない。ACPI の手順は query と commit の 2 回で足りる）。

### 5.4 状態遷移

```
 OFF（acpi.osc=off、既定）──────────────────────────────── 何も呼ばない
 OFF 以外: NOT_RUN ─drv_acpi_osc_negotiate()→ 段ごとに RUNNING ─→ DONE（段ごとの outcome を保持）
 DONE は boot の間変わらない（S0 idle の往復で呼び直さない。namespace と GNVS は保たれる）
```

- 呼び直さない理由: S0 idle は firmware の状態を保つ（S3 と違い BIOS の resume の道を通らない）。5330 の `_WAK` → `SWAK` は S3・S4 の道で、zedBSD は通らない。
- hotplug で新しい host bridge・processor が現れる事は扱わない（zedBSD に ACPI の hotplug の処理が無い）。

### 5.5 PCI の driver への口

- `drv_acpi_pcie_osc_control(segment, bus)`: その host bridge で OS が得た control の bit。ACPI の無い platform・`acpi.osc=off`・`pcie` の段を外した時は 0。
- VMD の下の domain（ACPI の node が無い）は VMD の function が居る host bridge（5330 は segment 0・bus 0 の `PC00`）の値を使う。
- 規則: LnkCtl の ASPM の field・DevCtl2 の LTR の enable・L1SS の control を**変える**書きは、bit 4（LnkCtl・L1SS）・bit 5（LTR）を得た時だけ。firmware が設定した値の**復元**（pci-power の resume）は今のまま得ていなくても行う（firmware の値をそのまま戻すだけで、制御を奪わない）。

### 5.6 起動の switch

- 語: `acpi.osc=`（`include/kern/boot.h` の enum に `KERN_BOOT_PARAMETER_ACPI_OSC`、`boot.c` に名前と語の検査）。kernel の変更で HAL ではない。
  - `off`: 何も呼ばない（今の振る舞いと同じ）。
  - `on`: 3 段とも。
  - `pcie`・`platform`・`cpu` を `,` で並べる（例 `acpi.osc=pcie,platform`）: その段だけ。`cpu` だけで `platform` が無い時は、processor の `_OSC` は呼ぶが 5330 では `_CPC` が出ない（§4.3）。log にその旨を 1 行。
  - 不正な語は boot.c が EINVAL（既存の規則）。driver は不正な語を `off` と扱う（安全側）。
- 既定: 実装の Phase では `off`（語が無い時も `off`）。p007（§8）で受け入れが通り、ユーザーが承認してから既定を `on` に変える（Q310-1）。既定の変更は code の 1 行と docs。
- `off` に戻した時に元へ戻らない物: 5330 の CMOS の 0xF2（`WCMP`、§3.1 の 3）と、Debian との交互の起動での iTBT の CM の mode。namespace・GNVS・`OSCP`・`PFxx`・CPU NVS は次の起動で firmware が作り直す（BIOS の初期化。p006 で `off` の再起動の後の値で確かめる）。

### 5.7 記録と観察

- kernel の log（段ごと 1 行、processor は要約 1 行）:
  - `acpi: _OSC \_SB_ platform: asked 0x64, got 0x60 (masked)`
  - `acpi: _OSC processors: 12 called, 12 granted, 0 refused, 0 broken, 52 not enabled`
  - `acpi: _OSC \_SB_.PC00 PCIe: support 0x11, control asked 0x30, got 0x30`
  - `acpi: _OSC is off (acpi.osc=off)`
- firmware の値は `/dev/acpi` に path を書いて読む（`\_SB.OSCP`、`\OSCC`、`\SDTL`、`\PF00`、`\_SB.PR00._CPC`、QEMU は `\_SB.PCI0.CTRL`）。UAPI は足さない（§10）。
- AML の `Debug` の行（5330 の `ADBG`）は既存の log の道で出る。

### 5.8 資源・寿命

- 引数の object（UUID の Buffer、revision・count の Integer、DWORD の Buffer）は評価ごとに作り、評価の後に release。戻りの object も検査の後に release。
- 結果の表は static（§5.2）。MADT は driver の表の写しを読むだけ（`drv_acpi_table_lookup`、写しは attach の後に変わらない）。

### 5.9 並行性と lock

- `drv_acpi_osc_negotiate()` は `drv_acpi_attach()` の中、起動の thread だけが呼ぶ。SCI の thread はまだ無い（§4.3）。評価は `drv_acpi_evaluate()` が interpreter の lock を取る。
- `drv_acpi_pcie_osc_control()` は negotiate の後に読むだけ。negotiate の前に呼ばれたら 0（「得ていない」）を返す（安全側）。
- 5330 の `CHCM` の `Sleep` の待ち（最大 `ITFP` ms）は interpreter が lock を外して寝る既存の道。起動の thread が待つだけ。

### 5.10 割り込みと DMA

- 新しい割り込み・DMA は無い。`_OSC` の評価の中の hardware の操作（GPIO、iTBT の mailbox、CMOS の port、GNVS・CPU NVS の memory）は firmware の AML が既存の region の handler（SystemMemory・SystemIO・PCI_Config）で行う。
- PME・hotplug の SCI は firmware に残す（§4.2 の bit 0・2 を求めない）。

### 5.11 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| `_OSC` が無い | ABSENT。何も得ていない。起動を続ける |
| 評価が error（AML の例外、stack の予算、While の timeout） | BROKEN、log 1 行、何も得ていない。次の段へ進む。namespace は firmware の途中の状態のまま（Load の途中なら表は一部）。`acpi.osc=off` で避ける |
| Buffer でない・短い | BROKEN、同上 |
| status の failure・UUID・revision | REFUSED、何も得ていない |
| masked | 返った bit だけ得た |
| firmware が求めない bit を返す | 求めた bit で切る |
| platform が REFUSED・BROKEN | processor の段はそのまま呼ぶ（5330 では `_CPC` が出ないだけ）。log に 1 行 |
| commit が hang する（AML の無限の待ち） | interpreter の While の timeout（`aml-eval.c:1367〜1406`）で BROKEN。Sleep の繰り返しで長い時は起動が遅れる。p006 で起動の時間を `off` と比べる |
| `_OSC` の後に機能が壊れる | `acpi.osc=off`（全部）か段を外す語で起動し直す |

### 5.12 既存の framework・他の WS との関係

- **WS300（ASPM・LTR）**: Q300-6（`_OSC` 無しで ASPM を設定してよいか）の答えの一つがこの WS。WS300 の `pci.vmd.aspm=on` の書きは今 VMD の attach（ACPI の前）にある。この WS の `pcie` の段を使うには、WS300 の書きを ACPI の後（`drv_pci_pcat_assign_deferred()` の段、`pcat.c:463` の近く）に移し、`drv_acpi_pcie_osc_control()` の bit 4・5 を確かめる。これは WS300 の Phase の作業で、この WS は口と規則（§5.5）を出す。Q1 が両 WS の順を決める（§11）。
- **WS333（CPU・熱）**: `_CPC` を使う code は足さない。p006 で `_CPC` の値（Highest・Nominal・Lowest など）を WS333 の driver が読む HWP_CAPABILITIES と比べ、食い違いを WS333 に渡す。`HWPV` ← 2 の後に HWP の MSR（PM_ENABLE・REQUEST）が firmware に変えられないかを、WS333 の strategy の要求の前後で読む。
- **WS052（S0 idle）**: `_OSC` の後の S0 idle の往復（今の 5330 で行ける所まで）を比べる。`NPME` を呼ばないので root port の PME の SCI（wake）は firmware に残る。
- **UCSI（typec）**: `_DSM` だけで `_OSC` を使わない。USB4 の bit を求めないので firmware CM のまま。USB-C の充電・画面・hub を p006 で比べる。
- **i915**: `DDMS` の GPIO（§3.1 の 1）が display の電源に関わる可能性がある（名前からの推定）。p006 で画面と backlight を比べる。
- **Dell の WMI・thermal（WS333 p017・p018）**: `_OSC` は DDV・SMM の道に触れない（AML を grep、`OSCP`・`OSCC`・`PFxx` は Dell の WMI の method から読まれない）。fan・温度・thermal mode を p006 で比べる。
- **cdev・`/dev/acpi`**: 変えない。
- **docs**: `docs/architecture/power-management.md`（ACPI の firmware との取り決め）に `_OSC` の段と switch を p007 で足す（docs は目標の設計。未実装の時は書かない）。

### 5.13 HAL を変えない理由

- `_OSC` の評価は ACPI の driver の interpreter の中で閉じる。必要な物（表・namespace・評価・region の handler・起動の語）は既に driver と kernel の口（`kern_boot_handoff`・`kern_boot_parameters_token_present`）にある。
- processor の有効・無効は MADT から driver が自分で読める（`drv_acpi_table_lookup("APIC")`）。HAL の MADT の解析（`bsp-pcat/acpi.c`）に口を足す必要は無い。HAL の CPU の番号と ACPI の UID の対応は `_OSC` には要らない（呼ぶ順と数だけ）。
- idle（HAL の MWAIT）は `_CST` に依らないので、processor の `_OSC` の後も HAL の振る舞いは変わらない。
- よって `include/hal/hal.h` の差分の案は無い。

## 6. 前後の差を確かめる方法

### 6.1 host（5330 の表を aml-host で評価）

今の harness（`plan/tools/aml-host/`）は memory を 0 で模すので、GNVS（`dsdt.dsl:1236` の 0x6150C000）・CPU NVS（`ssdt2.dsl:192` の 0x61511000）の値が実機と違い、動的な表の Load（番地の memory から）も失敗する。p002 で足す option（tools の変更。所有は Q1 が Queue で割り当てる）:

| option | 働き |
| --- | --- |
| `--set 'PATH i:N'` | 表の load と `_INI` の後、呼びの前に名前（Name・field unit）へ値を store（実機から読んだ GNVS・CPU NVS の値を入れる） |
| `--memory-file A=FILE` | 物理番地 A に file の byte を置く（`\SSDT` の package の番地に動的な表を置き、`Load(OpRegion)` を通す） |
| `--trace` | 呼びの間の region の書き（space・番地・幅・値）、Load・Unload、Notify、Sleep・Stall の合計を 1 行ずつ出す |
| `--diff` | 呼びの前と後で namespace の全ての名前と Integer・String・Buffer の値を取り、増えた・消えた・変わった物を出す |
| `--call-every-method` | 呼びの後に、引数の無い全ての method を評価して結果を出す（check-latitude5330.sh の 4 と同じ。前後の比べに使う） |

手順（p003）:
1. 5330 の zedBSD（main、`_OSC` 無し）で `/dev/acpi` から入力の値を読む（読むだけ。field unit と Name だけで method は評価しない）: `NEXP`・`ITSP`・`OSCC`・`CFGD`・`OSYS`・`SDTL`・`PF00`〜`PF11`・`HWPV`・`HWPE`・`INFS`・`ITBM`・`LMPS`・`TCNT`・`U4SE`・`U4CM`・`CMSK`・`EPTU`・`RTD3`・`HGMD`・`DPIW`・`DPG1`・`PCON`・`PSCP`・`DPTF`・`IN34`・`DDDR` と、`\_SB.PC00.TDM0`・`TDM1` の `_STA` の値（Name の物だけ）。一覧は p003 の script に持つ。
2. 同じ 5330 の Debian で読み取り専用で `dmesg` の `_OSC`・thunderbolt・USB4・intel_pstate の行を取る（Linux が何を申告し何を得たか、SW CM か）。
3. harness で `off`（何も呼ばない）と段ごと（`pcie`・`platform`・`cpu`・全部）を、1 の値と動的な表で走らせ、`--trace`・`--diff`・`--call-every-method` を比べる。
4. 出す物: `plan/ws310/osc-effects-5330.md`（段ごとの hardware への書き（GPIO・PCI config・SystemIO・GNVS・CPU NVS）・Load された表・増えた method・値の変わる method（`_PSS`・`_PDL`・`_PTC`・`_CPC`・`_PSD` など）・Notify・Sleep の合計の表）。**CMOS の port 0x72・0x73 と iTBT の mailbox への書きの有無**（§3.1 の 3）を最初の行に書く。
5. 判定: 段ごとに「書きの一覧が §3 の読みと一致」「意図しない書きが無い」。CMOS・iTBT の書きが有れば Q310-2 の答えに従う。

### 6.2 QEMU

- q35 の DSDT は `\_SB.PCI0._OSC` を持ち、platform・processor の `_OSC` を持たない（`plan/ws031/display-ref/dmesg.txt:212〜222` の Linux の log。QEMU 10.0 の表は p005 で `/dev/acpi` で確かめる）。
- 試験の SSDT（自分で書く ASL、`plan/ws310/tests/`、`-acpitable file=…` で足す）: well-behaved な platform・processor の `_OSC`（呼ばれた回数・DWORD を Name に残す）、bit を落とす物、failure・unknown UUID・unknown revision を返す物、Integer を返す物、短い Buffer を返す物、query で状態を変える物（5330 の型）、AML の例外で止まる物、`Sleep` を長く繰り返す物。
- 判定は guest の serial（`plan/tools/guest/serial.py`）で `/dev/acpi` に path を書いて記録の Name を読む（console の log は読まない）。`acpi.osc=off`・`on`・各段で、呼ばれた回数と DWORD が設計の通りか。q35 の PCIe は `\_SB.PCI0.CTRL` が求めた 0x30 と firmware の mask の積。boot-test.sh で login prompt。

### 6.3 実機（5330）の前後の比べ

同じ kernel を語だけ変えて起動する（`plan/tools/hw5330/5330-run.sh --nvme-kernel VMUNIX NAME` に `CFG_ADD='acpi.osc=…'`）。各構成 3 回の起動。1 本の script（p006 で作る `plan/ws310/tests/osc-5330.sh`、配置 → 起動 → 実行 → 回収 → 要約）。

| 項目 | 方法 | 判定 |
| --- | --- | --- |
| 起動 | 5330-run.sh の survey と PNG、login | `off` と同じく login まで。起動の時間（kernel の log の時刻）の差 ≤ 0.5 s |
| kernel の log | 5330-survey-diff.py で `off` と比べる | 増えた行が §5.7 の行・`ACPI Debug` の行・既知の Notify の行だけ。新しい error・warning が無い |
| firmware の値 | `/dev/acpi` で §6.1 の 1 の名前を読む | §6.1 の host の予測と一致 |
| namespace | `/dev/acpi` の全体の前後の diff | 増えた node が host の予測（`_CPC`・`_PSD` の表）と一致 |
| `_CPC` | `\_SB.PR00._CPC` などを読む | Highest・Nominal・Lowest・Guaranteed が WS333 の driver の HWP_CAPABILITIES と一致（不一致は WS333 に渡す） |
| CPU の性能 | WS333 の `/sbin/thermal` の CPU の表示、4 strategy で 1 CPU の spin の MHz（WS333 p014 の手順） | `off` と同じ。HWP の MSR が firmware に書き換えられていない |
| 電力 | idle の package の電力（WS333 の RAPL の meter、無ければ p002 の器具）、5 分 × 3 | `off` との差が揺れ（3 回の幅）の内か、下がる |
| C-state の滞在 | WS333 の滞在の統計（有れば） | package の深い状態が減らない |
| 熱・fan | `/sbin/thermal`（温度・fan・Dell の thermal mode の get/set） | `off` と同じ |
| 電源の event | AC の抜き差し 3 回、lid の開閉、電源 button の短押し（S0 idle の入口まで）、battery の残量 | `off` と同じ event が届く。processor の Notify の新しい行が無い（IST を Load しないので） |
| S0 idle | WS052 の今の手順で往復 3 回（行ける所まで） | `off` と同じ所まで行き、戻る。wake の源（lid・key・電源 button）が効く |
| PCIe・storage | NVMe（VMD）の読み書き 1 GiB、`hw.` の PCI の一覧 | `off` と同じ。LnkCtl・LTR・DevCtl2 の値が `off` と同じ（制御を得ても今の main は書かない） |
| USB・USB-C・TBT | USB の stick・keyboard、USB-C の充電器・hub・画面（有れば）、TBT の dock（有れば） | `off` と同じ。iTBT の CM の mode が変わらない（host の予測と一致） |
| 画面 | backlight の 0・100、画面の PNG | `off` と同じ（`DDMS` の GPIO の影響が無い） |
| network・音・入力 | AX211 の接続と ping、RTL8156B（有れば）、音の再生、touchpad | `off` と同じ |
| Debian との交互 | zedBSD（`on`）→ Debian → zedBSD（`off`）→ Debian の起動、Debian の dmesg の thunderbolt・`_OSC` の行 | Debian の振る舞いが変わらない。CMOS の 0xF2 が変わるなら Q310-2 |
| 耐久 | 一晩（idle と S0 idle の往復の繰り返し） | hang・reboot・log の error が無い。**夜間の実施はユーザーに確かめてから**（AGENTS §9、Q310-5） |

段の順（p006）: `pcie` → `pcie,platform` → `on`（3 段とも）。各段の前に host の予測（§6.1）を持つ。段で FAIL が出たら次の段へ進まず、§5.11 の戻し（語）で原因を切り分ける。

## 7. WS の受け入れ

1. host: harness の `on` の評価で、5330 の表の書きの一覧が設計（§3・§4）と一致し、CMOS・iTBT・GPIO の書きの扱いが Q310-2 の答えの通り。codec の host 試験が PASS（ASan・UBSan）。
2. QEMU: 試験の SSDT の全ての型で、段ごとの呼びの回数・DWORD・outcome が設計の通り。`acpi.osc=off` で 1 回も呼ばれない。boot-test.sh で login prompt。
3. 5330: §6.3 の全ての項目が `on` で `off` と同じか良い（電力）。`_CPC` が namespace にあり HWP_CAPABILITIES と一致（不一致は記録して WS333 へ）。`acpi.osc=off` で前と同じ振る舞いに戻る。
4. 耐久（Q310-5 で承認された時）: 一晩で hang・reboot・新しい error が無い。
5. 既定の変更（Q310-1 で承認された時）: 既定を `on` に変えた kernel で 3 を 1 回通す。docs を直す。

## 8. Phase

| Phase | 目的 | 依存 | 受け入れ条件 | 担当の型 | 規模 |
| --- | --- | --- | --- | --- | --- |
| p001 | 調べと設計（この文書）、review | — | design-reviewer の review を反映、問いを [user-questions.md](user-questions.md) に | 設計 | — |
| p002 | aml-host の option（§6.1 の 5 つ）と ASL の試験（`_OSC` の型 8 つ、acpiexec と比べる） | p001 | `make -C plan/tools/aml-host`・`run-asl.py`・`check-latitude5330.sh` が PASS、新しい ASL の試験が acpiexec と一致、tools.md の行の更新を Q1 に頼む | phase-runner | 2 LW |
| p003 | 5330 の入力の読み（`/dev/acpi`、Debian の dmesg、読むだけ）と host の評価、`osc-effects-5330.md` | p002、5330 の割当 | 段ごとの書きの一覧、CMOS・iTBT・GPIO の書きの有無、Q310-2 の判断の材料 | phase-runner-high | 2 LW |
| p004 | `acpi-osc-codec`（純粋な関数）と host 試験 | p001 | 正常・masked・failure・UUID・revision・短い Buffer・Integer・余計な bit・count 2 と 3 の試験が PASS（ASan・UBSan）、わざと壊す 3 通りを捕える | phase-runner-high | 1 LW |
| p005 | `acpi-osc.c` の統合（3 段・MADT・host bridge・結果の表・PCI の口・log）と `acpi.osc=` の語（boot.h・boot.c）、既定 `off`、QEMU の試験の SSDT と script | p004 | amd64 の build（warning 0）、QEMU の §6.2 が PASS、`acpi.osc=off` で呼ばれない、boot-test.sh で login | phase-runner-high | 3 LW |
| p006 | 5330 の前後の比べ（§6.3、1 本の script）、段ごと | p003、p005、Q310-2 の答え、5330 の割当 | §6.3 の表の全項目の判定を記録、FAIL は段で止めて切り分け | phase-runner-high | 3 LW（実機の時間を含む） |
| p007 | 既定の変更（Q310-1 の承認の後）・docs・耐久（Q310-5 の承認の後） | p006 | §7 の 4・5 | phase-runner-high | 1 LW＋一晩 |
| p008 | 規約の全文の見直し（変えた C の code） | p007 | coding-style の全文と照らし、書き方だけを直す | conformance-reviewer | 0.5 LW |

- p002 と p004 は並べてよい（file が重ならない）。p003 は 5330 の時間の割当が要る（読むだけ、`off` の kernel）。
- 標準廉価実装手順（`docs/agent/spec-module-delegation.md`）は p004 に当てない: ACPI 6.5 の仕様が手元に無く（§9）、header の仕様を出典から書けない。仕様が手に入れば p004 を手順に乗せ直してよい。
- WS300 の ASPM の書きを ACPI の後へ移す作業は WS300 の Phase（§5.12）。

## 9. 出典と引用

- ACPI 6.5 §6.2.11（`_OSC`）・platform-wide の DWORD の表: **未取得**（uefi.org が bot の確かめの page を返した、2026-10-31）。cockpit の 01（WS333 の ACPI 6.5 を手で置く件）と同じ件として Q310-6。
- PCI Firmware Specification 3.3（PCI の host bridge の `_OSC` の DWORD）: 未取得（PCI-SIG の会員向け）。
- Intel の processor の vendor-specific な `_OSC`/`_PDC` の bit: 未取得。processor は DWORD2 = 0 にするので、bit の意味に頼らない（§4.2）。
- 5330 の DSDT・SSDT: Dell の著作物（`plan/tools/aml-host/latitude5330/README.md`）。この文書は振る舞いを自分の言葉で書き、AML の block を写していない。名前（`OSCC`・`NEXP` など）と番地は事実として書いた。
- QEMU q35 の Linux の log（`plan/ws031/display-ref/dmesg.txt`）: 出力の文字列で、code ではない。bit の名前の並びの裏付けにだけ使った。
- 実装の code の出典の書き方（guardrail「MIT・GPL の出典のデータの引用」）: UUID・DWORD の bit の値を書く block の直前に `Values referenced:` を複数行の comment で、仕様の版・節・表の名前で書く（仕様が手に入るまでは「the firmware's own use」を書かず、値を仕様で確かめてから merge する。p004 の受け入れに入れる）。file の先頭には書かない。Linux の code は参照しない。

## 10. 選ばなかった案

| 案 | 選ばなかった理由 |
| --- | --- |
| Linux と同じ bit を申告・要求する（hotplug・PME・AER の制御も） | zedBSD に代わりの処理が無い。5330 では PME・hotplug の SCI が止まり、wake と hotplug が壊れる（§3.3） |
| processor に Intel の文書の bit（C-state FFH・`_PSS` など）を申告する | 文書が無い。zedBSD は `_CST`・`_PSS`・`_PPC` を使わないのに使うと申告する事になり、5330 は IST を Load して AC の抜き差しで processor の Notify と `_PPC` の上限を出す（受け手が無い）。Q310-3 |
| `_OSC` が無い processor に `_PDC` を呼ぶ | `_PDC` は古い口。5330 は `_OSC` を持つ。QEMU は両方無い |
| platform-wide を呼ばない（PCIe と processor だけ） | 5330 は `_CPC` を出さない（目標に届かない）。Q310-2 の答えで platform を外す道は語（`acpi.osc=pcie,cpu`）で残る |
| 結果を新しい sysctl・UAPI で出す | `/dev/acpi` と log で足りる。UAPI を増やすと review が要る |
| ACPI の attach を PCI の bind より前に動かす | PCI_Config の region の handler が PCI の driver を使い、`_CRS` の窓の割当も PCI の後の段にある。順を変える影響が大きい。制御を得た機能を書く code の側を後へ寄せれば足りる（§5.5） |
| `_OSC` の後に event の thread を始めない（今のまま後にする） | 採った（§4.3） |
| 呼びを kernel の build の CONFIG で切る | 1 つの kernel で `off` と `on` を比べたい（§6.3）。起動の語の方が戻しが速い |
| S0 idle の後に `_OSC` を呼び直す | firmware の状態は保たれる。呼び直すと 5330 の `DOCM` が iTBT に再び触れる |
| 標準廉価実装手順で codec を Sonnet に委ねる | 仕様が手元に無い（§8） |

## 11. 問いと Q1 への確認

ユーザーの判断が要る点は [user-questions.md](user-questions.md) の Q310-1〜6。Q1 への確認:

1. p002 は `plan/tools/aml-host/` を変える（tools の共有の file）。Queue で所有を割り当ててほしい。
2. WS300 の ASPM の書きを ACPI の後へ移す作業の順（WS300 の branch `agent/p6-ws300` は未 merge）。この WS の p005 の口が先に main に入る必要がある。
3. p003・p006 の 5330 の割当（p003 は読むだけ、`off` の kernel）。
4. `include/drivers/acpi/acpi.h` と `include/kern/boot.h`・`src/kern/boot.c` の変更（HAL ではない kernel の変更）を p005 の範囲に入れる。

## 12. 未確認の事

1. 5330 の GNVS・CPU NVS の値（`NEXP`・`U4SE`・`U4CM`・`CFGD`・`DPIW`・`PCON` など）。p003。
2. `HWPV` を SMM が読むか、読んで何をするか（AML の中に読み手は無い）。p006 で HWP の MSR を比べる。
3. `DDMS` の GPIO（`DPG1`）が何の電源か。p003 で値、p006 で画面を比べる。
4. CMOS の 0xF2 を BIOS が次の起動で読むか。p006 の Debian との交互の起動で。
5. platform の bit 5・6・10・18 と PCIe の bit の名前（仕様で未確認、§9）。
6. QEMU 10.0 の q35 の `_OSC` の mask（p005 で `/dev/acpi`）。
7. firmware の BIOS の版が変わると表が変わる（今の表は BIOS 1.31.1）。p003 で実機の版を確かめる。

## 13. 履歴

- 2026-10-31 第 1 版（P6、q1779-i01）。

## 14. review の反映

[design-review.md](design-review.md) を参照（反映の表はそこに）。
