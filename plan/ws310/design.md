<!-- awesome-plan project=zedbsd record=ws310-design -->

# WS310 設計: ACPI の _OSC を呼び、システム全体の挙動を厳重に確かめる

- 版: **第 2 版**（2026-10-31 P6、q1779-i01）。第 1 版（41a915514）に design-reviewer の敵対的レビュー（[design-review.md](design-review.md)、blocker 0・major 12・minor 9）を反映した。反映の表は design-review.md。
- 基点: worktree `p6-ws310`（branch `agent/p6-ws310`、main e2f816e14）。引用の file:行 は基点の物。code は書いていない。
- 読んだ物: `AGENTS.md`、`plan/agents/protocol.md`、[ws.md](ws.md)、`plan/guardrail.md`（範囲・HAL・driver は HAL を知らない・出典）、`plan/ws333/design.md`（§0・§1・§2・§4.4・§4.5・§4.16）、`plan/ws301/design.md`（§2.1 の `_CPC` の調べ）、`plan/ws303/design.md`（IETM の `_OSC`）、WS300 の branch `agent/p6-ws300` の design.md §4.3（Q300-6）、`plan/tools.md`（aml-host・hw5330）、source（§2）、5330 の DSDT・SSDT（`plan/tools/aml-host/latitude5330/`、iasl 20250404 で scratchpad に逆 assemble。`dsdt.dsl:N` などの行番号はその `.dsl` の物）、Linux v6.12.5 の tree（`/home/awe/visionfive-linux`、読むだけ。`include/linux/acpi.h` の `_OSC` の定数、`include/acpi/proc_cap_intel.h`、`drivers/acpi/bus.c` と `init/main.c` の順。扱いは §9）。
- 取れなかった物: ACPI 6.5 の仕様（uefi.org が bot の確かめの page を返した。回避していない）、PCI Firmware Specification（PCI-SIG）、Intel の processor の vendor-specific な `_OSC` の文書。bit の値は Linux の header の定数（GPL の file の定数は出典の明示で使える、guardrail）と 5330 の AML の使い方の両方で裏付けた（§4.2、§9、Q310-6）。

## 0. ユーザーの決定（要点）

| 日付 | 要点 | この設計での扱い |
| --- | --- | --- |
| 2026-10-29（WS301 Q301-3） | `_OSC` は最終的には呼ぶが、別の WS にして、システム全体の挙動に問題が無いかを厳重に確かめる。それまでは呼ばない（記録は [ws.md](ws.md)） | 既定は呼ばない（`acpi.osc=off` と同じ）まま実装し、確かめが通ってユーザーが承認してから既定を変える（§5.6、Q310-1）。確かめは host（5330 の表）・QEMU・5330 の実機の 3 段（§6） |
| 2026-10-31（Q1 の依頼） | 不在の間（昼頃まで）設計だけ進められる WS は設計を進める。WS301 は WS333 に統合され、WS333 の CPU の枠組みと熱が main に入ったので設計の時期になった | この文書。source は変えない |
| 2026-10-31（Q1） | ACPI 6.5 を手で置く件は cockpit の 01（WS333 の同じ件）にまとめて聞く | Q310-6 は「01 と同じ件」 |

## 1. 目標と範囲

- 目標（[ws.md](ws.md)）: zedBSD が ACPI の `_OSC` で OS の能力を firmware に申告し、firmware が出す情報（`_CPC` など）を使えるようにする。呼ぶ前と後で、電源・PCIe・熱・CPU の性能などシステム全体の挙動に問題が無いことを、実機（まず 5330）で厳重に確かめる。
- 含む: platform-wide（`\_SB._OSC`）・processor（ACPI0007 の `_OSC`）・PCI/PCIe の host bridge（PNP0A08・PNP0A03 の `_OSC`）の 3 種の評価、query と commit の手順、起動の switch、記録、host の harness の追加、5330 の前後の比べ。
- 含まない: USB4 の `_OSC`（UUID 23a0d13a-…、zedBSD に USB4 の connection manager が無い）、Intel DTT（IETM）の `_OSC`（DTT の driver が無い）、`_CPC` を使う CPU の制御（WS333。この WS は `_CPC` が namespace に現れ、HWP の MSR と同じ register を指す事を確かめるまで）、ASPM・LTR の設定そのもの（WS300。この WS は OS が制御を得た事を PCI の driver に伝える口と規則まで）、S3（zedBSD は使わない）。
- 対象: 5330（i5-1245U、Dell BIOS 1.31.1 の表）。QEMU q35 は PCIe の `_OSC` だけを持つ（§6.2）。`acpi.osc=off` が既定の間は、どの機械も振る舞いが変わらない。

## 2. 今の振る舞い（code の事実）

- `_OSC`・`_PDC` を評価する code は無い（`src/` を grep、0 件）。`\_OSI` は native の method で、Windows 2000〜"Windows 2022" と "Extended Address Space Descriptor" に真を返す（`src/drivers/acpi/aml-osi.c:27〜53`）。5330 の `\_SB.PC00._INI` はこの答えで `OSYS` を決める（`dsdt.dsl:28091〜28139`）。
- ACPI の attach の順（`src/drivers/acpi/acpi-kern.c:165〜283`）: 表の発見 → SystemMemory・SystemIO・PCI_Config の handler（`:190〜203`）→ DSDT・SSDT の load（`:205`）→ `drv_acpi_initialize_objects()`（`:213`）→ ECDT の EC（`:218`）→ `_REG`（`:221`）→ `_STA`・`_INI`（`:226`）→ `start_events()`（`:229`。この中の `drv_acpi_events_init()` が ACPI_ENABLE を SMI_CMD に書いて **ACPI mode に入る**、`acpi-event.c:366`・`:1237〜1259`）→ FACS の Global Lock（`:233`）→ EC（`:240`）→ power・thermal・WMI・Dell・LPS0 → PCI の電源の口（`:270`）→ `/dev/acpi`（`:273`）。Global Lock の共有の前は `\_GL_` は OS の中の mutex に過ぎない（`aml-sync.c:45〜49`）。5330 の GNVS・CPU NVS の Field は `Lock`（`dsdt.dsl:1236`、`ssdt2.dsl:192`）。
- platform の順（`src/kern/platform/pcat.c`）: PCI の driver の登録 → `drv_pci_pcat_init()`（`:437`、host bridge の probe で登録済みの driver が bind。VMD・NVMe・xHCI・i915 などはここで動き始める）→ `drv_acpi_attach()`（`:450`）→ 窓を待った function の割当（`:463`）→ UCSI → UEFI の変数 → `drv_cpu_x86_init()`（`:498`、WS333 の CPU の driver）。**PCI の function は ACPI より前に bind される**。
- PCIe の link の register を書く既存の code:
  - `src/drivers/pci/pci-power.c`: PCI PM の PME_En（`:1033〜1071`）、LnkCtl・LnkCtl2・LTR の latency・L1SS の保存と復元（`:212〜229`・`:318〜325`）。
  - `src/drivers/ethernet/rtl8125.c:2526〜2556`: LnkCtl の ASPM と Clock PM の field を**落とす**（`disable_link_power_management`）。
  - ASPM・LTR を**有効にする** code は main に無い（WS300 の branch に `pci.vmd.aspm=on` の時だけの道がある）。native の hotplug・AER・DPC・root port の PME の割り込みの処理は無い。
- VMD の下の bus は合成の segment を持つ（`src/drivers/pci/pci-vmd.c:108`・`:310`）。
- 起動の parameter: 固定の enum（`include/kern/boot.h:107〜129`）と名前の表・語の検査（`src/kern/boot.c`）。語の検査が EINVAL なら `kern_boot_parameters_initialize()` が失敗し、kernel は idle に入って起動を止める（`src/kern/main.c:187〜195`）。driver は `kern_boot_handoff("boot.command-line")` と `kern_boot_parameters_token_present()`（token を丸ごと比べる）で語を読む（前例 `src/drivers/platform/pcat/cpu-x86/cpu-x86.c:116〜158` の `cpuperf=off`・`cpuperf.hwp=off`）。
- 診断の口 `/dev/acpi`（`src/drivers/acpi/acpi-dev.c`）: 読むと namespace、path を書くとその**評価**の結果を `PATH = VALUE` で読める（Method を書けば実行される）。
- Notify の受け手が無い時は `ACPI: Notify(PATH, 0xNN) has no handler` を log に出して捨てる（`src/drivers/acpi/aml-sync.c:242〜251`）。AML の `Debug` への store は `ACPI Debug: …` の行になる（`aml-eval.c:2038〜2050`）。While の timeout は `aml-eval.c:1367〜1407`。
- MADT は HAL が読む（`src/hal/amd64/bsp-pcat/acpi.c`）。ACPI の driver は自分の表の写しから `drv_acpi_table_lookup("APIC", …)`（`acpi-kern.c:589〜611`）で読める。
- S3 は無い（`_WAK`・`_PTS` の S3 の道が無い。S0 idle だけ）。D3cold の関数は `acpi-sleep.c` にあるが、`DRV_ACPI_D3_COLD` を渡す呼び手は `acpi-sleep.c` の外に無い（PCI の電源の道は D0 と D3hot だけ、`acpi-pci-power.c:102〜104`）。

## 3. 5330 の firmware の `_OSC`（表の読み）

`\_SB._OSC`・`\_SB.PRxx._OSC`・`\_SB.PC00._OSC`・`\_SB.IETM._OSC`（DPTF）の 4 つがある（他の `_OSC` は無い。`grep -c _OSC` で DSDT 9 行・SSDT2 64 行・SSDT3 1 行）。AML の block は写さず、振る舞いを自分の言葉で書く。

### 3.1 platform-wide `\_SB._OSC`（DSDT `dsdt.dsl:28366〜28447`）

UUID 0811b06e-4a27-44f9-8d60-3cbbc22e7b48 の時:

1. **どの呼び（query でも）でも最初に `DDMS()`**（`:80647〜`）: `\DPIW` が 0 でなく（論理の AND）`\_SB.PC00.GFX0.IPCO()`（`PCON` の bit 7・8 が両方 1、`ssdt6.dsl:772〜`）が真なら `\_SB.SGOV(DPG1, 1)`（GPIO の出力を 1）。**hardware への書き**。DPIW・PCON・DPG1 の値は未読（p003）。
2. revision 1 なら、**query でも commit でも、まず `OSCP` ← DWORD2**（namespace の Name。mask より前に書くので、firmware が bit を落としても `OSCP` は求めた値のまま。review の harness で確認）。bit 2（`_PR3`）を求めると `OSCO` ← 4、`HGMD & 0xF != 2` かつ `RTD3 == 0` なら bit 2 を落とす。
3. `U4FN`（USB4、`:28296〜28358`）: `U4SE == 0` なら `OSCM` ← 0 で終わり。`U4SE != 0` かつ `U4CM & 0x70 != 0` で、**commit（query の bit が 0）なら `DOCM()`**（`:28226〜28293`）:
   - `CMSK` の host router ごとに `\_SB.PC00.TDMx.STCM(OSU4, U4CM)`（`ssdt7.dsl:1989〜2026`）。`U4CM` の valid の bit（0x80）が立っていれば、**切り替えの要否に関わらず `SCME`（`\_SB.PC00.TDMx.SCME`）を書く**（`:2003`）。pre-boot の CM の mode（`U4CM & 7`）が OS の望み（bit 18 を求めなければ 0 = firmware CM）と違えば `CHCM()`（`:2141〜`）で **TBT の Force Power（`DFPE` ← 1、PCI config の field、`:1543`）を立て、`P2TB` の mailbox に CM の切り替えを送り**（`Sleep` の待ちを含む）、成功なら `SCME` ← 望み。`DFPE` を戻す code は見当たらない。
   - `SCME` を読む所: TBT の電源の資源 `TBT0`・`TBT1` の `_ON`・`_OFF`（0 の時だけ `TBON`・`TBOF`）、TDM の `_PS0`・`_PS3`・`_PR0`・`_PR3`・`D3CE`（`ssdt7.dsl:741〜777`・`:1560`・`:1595`・`:1622`・`:1666`・`:1721`）。つまり **CM の mode は TBT の RTD3 の振る舞い（S0 idle の電力）を変える**。
   - CHCM が失敗しても U4FN は OSU4 = 0 なら成功を返す（firmware の内部の失敗が `_OSC` の戻りに出ない）。
   - 最後に `U4CM & 0x70 == 0x20` なら **`WCMP(OSCM)` が CMOS の bank（port 0x72・0x73、index 0xF2）に書く**（`:28217〜28223`、`U4SE == 1` の時だけ定義）。CMOS は電源を切っても残る。
4. bit 18（native USB4）を求めたのに platform が SW CM を出せない時は bit 18 を落とす。

他: UUID 23a0d13a-…（USB4）は `OSCM == 1` の時だけ処理する。他の UUID は「unrecognized UUID」。`OSCP` を読む所: processor の `GCAP`（bit 5・6）、`_PSS` と `_PDL` の表の選び（bit 10、`dyn-ssdt17.dsl:57`・`ssdt3.dsl:1375`）。`OSCO` を読む所は無い。

### 3.2 processor `\_SB.PRxx._OSC`（SSDT2 "CpuSsdt" `ssdt2.dsl:344〜`、PR00〜PR63）

- `\_SB.PR00`〜`PR63` は ACPI0007（`dsdt.dsl:30494〜`、`_STA` 無し。在るかは MADT）。各 `_OSC` は UUID 4077a616-290c-47be-9ebd-d87058713953 を確かめ（違えば status 6）、revision 1 でなければ status 0xA、query なら DWORD2 を 0x3BFF で masked して返すだけ。commit なら `GCAP`:
  - `PFxx` ← `(PFxx & 0x7FFFFFFF) | DWORD2`（初期値 0x80000000 は「まだ呼ばれていない」の印）。PR01 以降は `PF00` にも写す（`:530`）。
  - PR00: `CFGD & 0x72`・`CFGD & 0x200`・`PF & 0x18` で `_CST` の表、`PF & 9 == 9` と `CFGD & 1` で IST（`_PSS`・`_PCT`・`_PPC`）、`CFGD & 1` か `CFGD` bit 22 で `_PSD` の表、**`OSYS >= 2015`（`_INI` が `_OSI` で決めた値）・`CFGD` bit 22・`\_SB.OSCP & 0x40` で `_CPC` の表（Cpu0Hwp）を Load し、CPU NVS の `HWPV` ← 2**（`ssdt2.dsl:445〜466`。`HWPV` は 0x61511000 の `PNVS` の field。AML の中に読み手は無く、SMM が読むかは不明）。
  - PR01〜: `PF & 9 == 9` で IST、`\_SB.OSCP & 0x20` で `_CPC`（ApHwp、`OSYS` と `CFGD` bit 22 も）、どちらかで `_PSD`、`PF & 0x18` で `_CST` の AP の表。
  - Load の済みは `\SDTL` の bit で覚える。表の番地と長さは SSDT2 の `\SSDT` の package（`ssdt2.dsl:93〜120`。`dynamic/` の 8 つの file の長さと一致を確かめた）。
- `PF00` を読む所: `SPNT`（AC の抜き差しで `_PPC` が有る時に呼ばれ、`TCNT > 1` かつ `PFxx & 0x08` の CPU に Notify 0x80、`& 0x18` で 0x81、`ssdt14.dsl:577〜`）、`_PTC`（`ssdt3.dsl:1256〜`、`PF00 != 0x80000000` で分かれ、DWORD2 = 0 の commit の後は FFH でない SystemIO の形を返す。読み手は無い）、動的な `_PSD` の HW の協調（`dyn-ssdt18.dsl:83`）、`_CST` の FFH の選び（`dyn-ssdt16.dsl:140〜150`）。
- `_CPC`（`dyn-ssdt19.dsl:385〜428`）: `GCPC` が `CPOC` か `CPC3` を返す（`CFGD`・`ITBM`・`LMPS` で選ぶ）。`CPC3` は **revision 3・23 要素**（`:209〜211`）。Highest・Guaranteed などは FFH の register の記述子で MSR 0xCE・0x771（HWP_CAPABILITIES）・0x774（HWP_REQUEST）・0xE7 などを指す（値ではない）。Nominal Perf（index 3）・Reference Perf（index 20）・Nominal Freq（index 22）は `GCPC` が CPU NVS の `SFBC`/`SFSC`・`RFBC`/`RFSC`・`NMFQ` を P コア・E コア別（`CORE` の bit）に書き込む整数。AP の `_CPC` は `\_SB.PR00.GCPC` を呼ぶので、PR00 の `_CPC` の表が Load されていないと AP の `_CPC` は評価の error になる（`ssdt2.dsl:445〜466`・`:565〜573`）。

### 3.3 PCIe の host bridge `\_SB.PC00._OSC`（DSDT `dsdt.dsl:5406〜5462`）

- UUID 33db4d5b-1ff7-401c-9657-7441c03dd766 の時: `SUPP` ← DWORD2、`CTRL` ← DWORD3。`NEXP == 0` なら CTRL の bit 0〜2 を落とす、`ITSP` が真なら bit 3 を落とす。
- **query でも状態を変える**: query の判定が `If (~(CDW1 & One))`（bit の否定。値は 0 にならないので常に真）なので、query でも `OSCC` ← CTRL（GNVS の 8 bit の field、`dsdt.dsl:1290`、SMM から見える）と `SWAK(0xFFFF, 0)` が走る。method の最後でも無条件に `OSCC` ← CTRL（`:5454`）。`SWAK`（`ssdt14.dsl:1263〜`）は `NEXP` が真なら、`OSCC & 1` で `NHPG()`（全 root port の hotplug の SCI を止め、status を clear）、`OSCC & 4` で `NPME()`（全 root port の PME の SCI を止める）。つまり **CTRL の bit 0 か 2 を求めると、query の時点で firmware の hotplug・PME の処理が止まる**。
- `SUPP` を読む所は無い。`OSCC` を読むのは `SWAK` だけ（`_OSC` と `_WAK`、`dsdt.dsl:27725`）。
- harness の評価（2026-10-31、今の harness、memory は 0 なので `NEXP == 0`）: query・DWORD2 0x1F・DWORD3 0x1D で戻りは DWORD1 0x11・DWORD3 0x18、評価の後の `\OSCC` は 0 → 0x18（**query で GNVS が書かれる事を zedBSD の interpreter で再現**）。commit で DWORD3 0x30 なら `\OSCC` = 0x30（review の評価）。

### 3.4 IETM `\_SB.IETM._OSC`（SSDT3 "DptfTabl" `ssdt3.dsl:363〜`）

DTT の UUID で commit すると EC に `DPST(1)`（DTT の driver が有る）を伝える（WS303 design §2 と同じ）。zedBSD は DTT を持たないので**呼ばない**。

### 3.5 他の OS の順（参考）

- Linux v6.12.5: ACPI mode に入る（`init/main.c:1100` の `acpi_subsystem_init()`、`drivers/acpi/bus.c:1313`）のが先で、その後の `acpi_bus_init()` が表の load・`_INI`（`:1367`）の後に platform-wide（`:1377`）→ USB4（`:1378`）→ processor（`:1388`）の順に `_OSC` を呼ぶ。PCIe の `_OSC` は host bridge の追加（PCI の走査の前）。repo の QEMU q35 の Linux 6.8 の log（`plan/ws031/display-ref/dmesg.txt:212〜222`）も PCIe の `_OSC` が PCI の走査より前。
- 5330 の Debian の log は p003 で読む。

## 4. 方針（何を何の bit で呼ぶか）

原則: **zedBSD が実際に行う事だけを申告し、zedBSD が処理する機能の制御だけを、それを使う code が kernel に入った時に求める**。firmware の処理を止める bit（hotplug・PME・AER・DPC）は、zedBSD に代わりの処理が無い間は求めない。

### 4.1 呼ぶ物と呼ばない物

| `_OSC` | 呼ぶか | 理由 |
| --- | --- | --- |
| PCIe の host bridge（PNP0A08、PNP0A03 は PCI の bit だけ） | 呼ぶ（段 `pcie`） | support の申告。control は WS300 の有効にする code が入ってから（§4.2） |
| platform-wide `\_SB._OSC` | 呼ぶ（段 `platform`） | processor の `_CPC` の Load の条件（`OSCP` bit 5・6）。firmware は自分を Windows の上と見ており、`_OSC` を呼ばない OS は firmware の試された道の外 |
| processor（ACPI0007・`Processor`） | 呼ぶ（段 `cpu`） | `_CPC`・`_PSD` が namespace に現れる。MADT で有効な CPU だけ |
| USB4（23a0d13a-…） | 呼ばない | USB4 の SW CM が無い。platform-wide の bit 18 も求めない |
| IETM・他の device の `_OSC` | 呼ばない | 対応する driver が無い。device の `_OSC` はその device の driver の物で、この WS の枠ではない |

### 4.2 bit（DWORD）

値の出典: Linux v6.12.5 `include/linux/acpi.h`（`:562〜620`）と `include/acpi/proc_cap_intel.h`（`:11〜22`）。5330 の AML の使い方（§3）と QEMU の log の名前の並びが同じ値を示す。仕様の表では未確認（§9）。

**PCIe の host bridge**（UUID 33db4d5b-…、revision 1、3 DWORD）:

| DWORD | bit | 意味 | 申告・要求 | 理由 |
| --- | --- | --- | --- | --- |
| 2（support） | 0 | 拡張の config 空間 | する | ECAM を使う（`pci-pcat.c:113`・`:200〜`） |
| 2 | 1 | ASPM | WS300 の ASPM を有効にする code が kernel に入った時だけ | 今の main は ASPM を落とすだけ（rtl8125） |
| 2 | 2 | Clock PM | しない | 有効にしない |
| 2 | 3 | segment の群 | p005 で決める | config の読み書きは segment 0 の外を ECAM で扱う（`pci-pcat.c:200〜`）。列挙（`drv_pci_scan_all`）が MCFG の全ての segment を走るかを p005 で読み、走るなら申告。5330 の firmware は DWORD2 を読まない |
| 2 | 4 | MSI | する | MSI・MSI-X を使う（`pci.c:82〜84`） |
| 2 | 7・8 | EDR・HPX type 3 | しない | 無い |
| 3（control） | 0・1・2・3 | PCIe hotplug・SHPC hotplug・PME・AER | 求めない | 代わりの処理が無く、5330 では求めた時点で（query でも）firmware の hotplug・PME の SCI が止まる（§3.3） |
| 3 | 4 | PCIe Capability Structure | WS300 の有効にする code が入った時だけ（Q310-4） | 有効にする書きの前提（§5.5）。今の main に使う code は無い |
| 3 | 5 | LTR | 同上 | 同上 |
| 3 | 7 | DPC | 求めない | 無い |

既定（WS300 の code の前）: DWORD2 = 0x11（bit 0・4、segment の bit は p005）、DWORD3 = 0。WS300 の code が入った後: DWORD2 に bit 1、DWORD3 = 0x30。切り替えは `acpi-osc.c` の 1 か所の定数で、WS300 の Phase が変える（§5.12）。5330 の firmware は DWORD2 を読まず、DWORD3 0 なら `OSCC` ← 0（§3.3）。

**platform-wide**（UUID 0811b06e-…、revision 1、2 DWORD）: DWORD2 = 0x60（bit 5・6）。ただし bit 5・6 は CPU の性能を zedBSD が HWP で扱う時だけ（`CONFIG_CPU_PERF=y` で `cpuperf=off`・`cpuperf.hwp=off` が無い時）。そうでなければ DWORD2 = 0（commit はする。§5.3）。

| bit | 意味 | 申告 | 理由 |
| --- | --- | --- | --- |
| 0 | Processor Aggregator Device | しない | 無い |
| 1・3 | `_PPC` の `_OST`・hotplug の `_OST` | しない | `_OST` を評価しない |
| 2 | `_PR3` | しない | D3cold を使う呼び手が無い（§2）。D3cold を使う WS が足す |
| 4 | APEI | しない | 無い |
| 5 | CPPC | 上の条件で | `_CPC` を namespace に出す（目標）。zedBSD の CPU の制御は `_CPC` が指す同じ MSR を直に扱う（WS333） |
| 6 | CPPC v2 | 上の条件で | 5330 の PR00 の `_CPC` の Load の条件（§3.2）。5330 は `HWPV` ← 2 |
| 7・8 | LPI（platform 協調・OS 主導） | しない | `_LPI` を読まない（idle は HAL が CPUID で選ぶ） |
| 9 | 速い熱の標本化 | しない | `_TSP` の速い値を使わない |
| 10 | 16 を超える P-state | しない | `_PSS` を使わない |
| 11 | GED | しない | GED の driver が無い |
| 12・14 | CPPC の多様な highest・柔軟な番地空間 | しない | `_CPC` を読むだけで、その解釈を持たない |
| 13 | IRQ の ResourceSource | しない | 無い |
| 17 | Generic Initiator | しない | 無い |
| 18 | native USB4 | しない | SW CM が無い |
| 19 | 充電の上限 | しない | 無い |
| 21・22 | PRM・FFH の OpRegion | しない | 無い |

**processor**（UUID 4077a616-…、revision 1、2 DWORD）: DWORD2 = 0（何も申告しない、Q310-3）。理由: zedBSD は `_CST`・`_PSS`・`_PPC`・`_PTC` を使わず、idle と P-state は HAL と WS333 の driver が CPUID・MSR で直に行う。0 の commit でも 5330 は `PFxx` を 0 にし、`OSCP` に従って `_PSD` と `_CPC` を Load する（§3.2）。`PFxx & 0x08` が 0 なので `SPNT` の processor の Notify も出ない。

### 4.3 呼ぶ時期と順

**位置**: ACPI の attach の中、`start_events()`（ACPI mode に入る）と FACS の Global Lock の共有（`acpi-kern.c:229〜236`）の後、`drv_acpi_ec_attach()`（`:240`）の前。順:

1. **platform-wide**（段 `platform`）: query → commit。
2. **processor**（段 `cpu`）: platform が GRANTED か MASKED の時だけ（§5.11）。MADT の有効な processor の UID の順に、UID の一致する ACPI0007（`_UID`）か `Processor`（ProcessorID）の `_OSC` へ query → commit。`_OSC` の無い processor は飛ばす。
3. **PCIe の host bridge**（段 `pcie`）: namespace の PNP0A08・PNP0A03 ごとに query → commit。結果を（segment、bus）ごとに覚える。

理由:
- **ACPI mode の後**（review M1）: 5330 の `_OSC` は GNVS・CPU NVS の `Lock` の field（`OSCC`・`HWPV`）を書き、SMM と共有する。Global Lock の共有の前は `\_GL_` が firmware と排他にならない。ACPI_ENABLE の SMI の handler が `_OSC` の結果を初期化し直すかは分からない（firmware の source が無い）。Linux は ACPI mode に入ってから呼ぶ（§3.5）。firmware の試された道に合わせる。
- **`_INI` の後**: 5330 の `GCAP` は `OSYS >= 2015` を見て `_CPC` を Load し、`OSYS` は `\_SB.PC00._INI` が `_OSI` で決める（§2）。`_INI` より前だと `_CPC` が出ない。
- **platform を processor より先**: 5330 の `GCAP` は `\_SB.OSCP` を読む（§3.2）。
- **EC の attach の前**: `_OSC` の道（§3）は EC の region を使わない（`DDMS`・`DOCM` は GPIO・PCI config・CMOS の port）。EC の query の event より前に firmware の状態を確定させる。SCI の thread は既に動くが、評価は interpreter の lock（`acpi-kern.c:103`）で直列になる。
- **PCI の bind の後になる事**（review M2）: `drv_pci_pcat_init()`（`pcat.c:437`）が ACPI より前なので、`_OSC` の時には driver が既に hardware を動かしている。5330 で `_OSC` の commit の中の firmware が触る device と、その driver の扱い:

| firmware の操作 | 触る device | 起動の時に動いている driver | 扱い |
| --- | --- | --- | --- |
| `CHCM`（CM の切り替え、Force Power、mailbox）・`SCME` | TBT の DMA（0:0d.2・0:0d.3）、その先の TCSS の xHCI（0:0d.0）の USB-C の port | xHCI（`pci-xhci.c`）、UCSI（ACPI の後） | p003 で切り替えが起きるかを値で知る（Q310-2）。起きる構成では p006 で「USB-C に device を挿したまま起動」を必須にし、xHCI が port の切断・再接続を正しく扱うか（device の消失・再列挙）を見る |
| `DDMS` の GPIO（`DPG1`） | 不明（名前から display か panel の電源と推定） | i915 | p003 で値、p006 で画面と backlight を比べる |
| `NHPG`・`NPME` | root port の SCI | — | bit 0・2 を求めないので起きない |
| `OSCC`・`HWPV`・CMOS | firmware の記憶 | — | p006 の記録 |

  bind の前に呼ぶ（ACPI の attach を PCI の前へ動かす、platform だけ先に呼ぶ）案は §10。

### 4.4 段と戻しの単位

段（`pcie`・`platform`・`cpu`）は起動の語で個別に外せる（§5.6）。`cpu` は `platform` に依る（`_CPC` の条件、§5.11）。

## 5. 設計

### 5.1 責務と境界

```
 kern/platform/pcat.c: drv_pci_pcat_init → drv_acpi_attach → … → drv_cpu_x86_init（順は変えない）
 src/drivers/acpi/acpi-kern.c  drv_acpi_attach: … _INI → start_events → Global Lock → drv_acpi_osc_negotiate() → EC …
 src/drivers/acpi/acpi-osc.c   （新）3 種の _OSC の順・対象の列挙・log・結果の保持、起動の語の読み
 src/drivers/acpi/acpi-osc-codec.c/.h（新）純粋な関数: DWORD の組み立て、戻りの検査、query → commit の決め
 src/drivers/acpi/acpi-pci-root.c  host bridge の列挙（既存の root_visitor の判定を使う）
 include/drivers/acpi/acpi.h   drv_acpi_pcie_osc_control()（PCI の driver が引く口）
 src/drivers/acpi/ の内部の header  drv_acpi_osc_negotiate()（attach の内部の口）
 src/drivers/pci/…             （WS300 の側）有効にする書きの前に口を引く
 HAL: 変えない（§5.13）
```

- `acpi-osc-codec`: kernel の header を使わず host で試せる（`acpi-text.c` と同じ置き方）。UUID の byte 列（ToUUID の並び）、DWORD の little endian の組み立てと読み、status の bit の判定、「得た control ⊆ 求めた control」の切り詰め、commit で求める値の決め。
- `acpi-osc.c`: interpreter の口（`drv_acpi_evaluate`・`drv_acpi_object_buffer_new`・`drv_acpi_walk`・`drv_acpi_lookup`・`drv_acpi_table_lookup`）だけを使う。driver は HAL を include しない（guardrail 2026-10-30）。CPU の性能の条件（§4.2 の platform の bit 5・6）は `CONFIG_CPU_PERF` と起動の語（`cpuperf=off`・`cpuperf.hwp=off`）を同じ `kern_boot_parameters_token_present()` で読む。

### 5.2 interface（案）

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
 * for the host bridge that decodes the given segment and bus (the _OSC
 * control DWORD, cut to what was asked).  A function behind a VMD is
 * asked about by the VMD function's own address.  Zero when _OSC was not
 * run, was refused or is absent, or before the ACPI attachment ran it.
 */
uint32_t drv_acpi_pcie_osc_control(uint16_t segment, uint8_t bus);

/* a private header of src/drivers/acpi/（案）: run once by drv_acpi_attach() */
int drv_acpi_osc_negotiate(void);
```

- 結果の表: host bridge ごとに 1 行（5330・QEMU q35 は 1 つ。上限は固定の小さな数にし、超えた host bridge は呼ばずに log 1 行）。processor は要約（呼んだ数・GRANTED・REFUSED・BROKEN の数）だけ。表は `drv_acpi_osc_negotiate()` が一度だけ書き、その後は読むだけ（`firmware` の表と同じ扱い、`acpi-kern.c:105〜111`）。
- 名前の接頭辞は `drv_acpi_osc_`。Linux の symbol の名前（`OSC_SB_*` など）は写さない（guardrail）。

### 5.3 手順（1 つの `_OSC`）

1. `_OSC` を `drv_acpi_lookup()` で探す（上の scope を探さない、`aml-namespace.c:73〜78`）。無ければ ABSENT。
2. **query**: DWORD1 = 1（query の bit）、DWORD2・3 = 求める値で評価する。
3. 戻りを検査: Buffer でない・長さが `count × 4` 未満 → BROKEN。DWORD1 の bit 1（failure）・2（UUID）・3（revision）→ REFUSED。bit 4（masked）は続ける。
4. **commit の値**: support・control とも「query で返った物 ∩ 求めた物」。0 になっても commit する（OS が `_OSC` を知る事を伝える。5330 の platform は commit で初めて `DOCM` が走るので、段の比べで commit の有無を分けられる）。
5. **commit**: DWORD1 = 0 で評価し、3 と同じ検査。得た control は「返った物 ∩ commit で求めた物」。firmware が求めない bit を立てても使わない。
6. log 1 行（§5.7）。

commit で firmware がさらに落とした場合は、その値で終える（3 回目は呼ばない）。

### 5.4 状態遷移

```
 OFF（acpi.osc=off、既定）──────────────────────────────── 何も呼ばない
 OFF 以外: NOT_RUN ─drv_acpi_osc_negotiate()→ 段ごとに RUNNING ─→ DONE（段ごとの outcome を保持）
 DONE は boot の間変わらない（S0 idle の往復で呼び直さない）
```

- 呼び直さない理由: S0 idle は firmware の状態を保つ（BIOS の resume の道を通らない）。5330 の `_WAK` → `SWAK` は S3・S4 の道で、zedBSD は通らない。呼び直すと 5330 の `DOCM` が TBT に再び触れる。
- 新しい host bridge・processor の hotplug は扱わない（ACPI の hotplug の処理が無い）。

### 5.5 PCI の driver への口と書きの規則

- `drv_acpi_pcie_osc_control(segment, bus)`: その host bridge で OS が得た control の bit。ACPI の無い platform・`acpi.osc=off`・`pcie` の段を外した時・negotiate の前は 0。
- VMD の下の function（合成の segment、ACPI の node 無し）は、呼び手が VMD の function 自身の（segment、bus）で引く（5330 は segment 0・bus 0 の `PC00`）。
- 規則（review M6）:
  - **落とす書き**（ASPM・Clock PM・LTR・L1SS を無効にする）: 制御を得ていなくても許す。理由: firmware が有効にした省電力を driver が device の不具合のために止めるのは、link を壊さず firmware の管理とも衝突しない（今の `rtl8125.c:2526〜2556`）。
  - **復元**（pci-power の resume が firmware の値か driver が前に書いた値を戻す）: 今のまま許す。
  - **有効にする書き**（ASPM・L1SS・LTR を 0 から 1 に）: bit 4（LnkCtl・L1SS）・bit 5（LTR）を得た時だけ。WS300 の書きはこの規則で口を引き、ACPI の attach の後の段（`pcat.c:463` の近く）で行う。
  - 今の main で「有効にする書き」は 0 件（§2）。p005 の受け入れに grep を入れる。

### 5.6 起動の switch

- 語（4 つ、`include/kern/boot.h` の enum と `boot.c` の名前・語の検査に足す。HAL ではない kernel の変更。`cpuperf`・`cpuperf.hwp` と同じ on/off の形）:
  - `acpi.osc=on|off`: 全体。**既定 off**（語が無い時も off）。
  - `acpi.osc.pcie=on|off`・`acpi.osc.platform=on|off`・`acpi.osc.cpu=on|off`: 段ごと。既定 on（`acpi.osc=on` の時だけ効く）。`cpu=on` で `platform=off` なら processor の段は飛ばす（§5.11）と log 1 行。
  - 語の値が on・off 以外なら boot.c の検査が EINVAL を返し、kernel は起動を止める（既存の規則、§2）。誤記は起動の失敗として見える（安全側にはならない）。
- 既定の変更: p008（§8）で受け入れが通り、ユーザーが承認してから既定を `on` にする（Q310-1）。code の 1 行と docs。
- `off` に戻しても元へ戻らない物: 5330 の CMOS の 0xF2（`WCMP`、§3.1 の 3）と、TBT の pre-boot の CM の mode（CMOS から BIOS が読むなら）。namespace・GNVS・`OSCP`・`PFxx`・CPU NVS・`SCME`・`DFPE` は次の起動で firmware と platform の reset が作り直す見込み（p006 で `off` の再起動の後の値で確かめる）。

### 5.7 記録と観察

- kernel の log（段ごと 1 行、processor は要約 1 行）:
  - `acpi: _OSC \_SB_ platform: asked 0x60, got 0x60`
  - `acpi: _OSC processors: 12 called, 12 granted, 0 refused, 0 broken, 52 not enabled`
  - `acpi: _OSC \_SB_.PC00 PCIe: support 0x11, control asked 0x0, got 0x0`
  - `acpi: _OSC is off`
- firmware の値は `/dev/acpi` に Name・field unit の path を書いて読む（`\_SB.OSCP`、`\OSCC`、`\SDTL`、`\PF00`、`\_SB.PC00.TDM0.SCME`。QEMU は `\_SB.PCI0.CTRL`）。Method の path は書かない（評価が走る）。`_CPC` は Method なので、p006 で「副作用が無い事を host で確かめた Method」の一覧に限って評価する。UAPI は足さない（§10）。
- AML の `Debug` の行（5330 の `ADBG`）は既存の log の道で出る。

### 5.8 資源・寿命

- 引数の object（UUID の Buffer、revision・count の Integer、DWORD の Buffer）は評価ごとに作り、評価の後に release。戻りの object も検査の後に release。
- 結果の表は static（§5.2）。MADT は driver の表の写しを読むだけ（写しは attach の後に変わらない）。

### 5.9 並行性と lock

- `drv_acpi_osc_negotiate()` は `drv_acpi_attach()` の中、起動の thread だけが呼ぶ。SCI の event の thread は既に動いている（§4.3）が、`_OSC` の評価も GPE・EC の method の評価も interpreter の lock（`acpi-kern.c:103`）で直列になる。AML の `Sleep` の間は lock が外れ、その間に event の method が走り得る（他の OS と同じ形）。
- `drv_acpi_pcie_osc_control()` は negotiate の後に読むだけ。negotiate の前に呼ばれたら 0（安全側）。結果の表の書きは negotiate の 1 回だけで、PCI の呼び手は attach の後（`pcat.c:463` 以降）にしか引かない（規則として p005 の comment と WS300 に書く）。
- 5330 の `CHCM` の `Sleep` の待ち（最大 `ITFP` ms と `WTBS` の待ち）は起動の thread が待つだけ。

### 5.10 割り込みと DMA

- 新しい割り込み・DMA は無い。`_OSC` の評価の中の hardware の操作（GPIO、TBT の mailbox と Force Power、CMOS の port、GNVS・CPU NVS）は firmware の AML が既存の region の handler（SystemMemory・SystemIO・PCI_Config）で行う。
- PME・hotplug の SCI は firmware に残す（§4.2 の bit 0・2 を求めない）。

### 5.11 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| `_OSC` が無い | ABSENT。何も得ていない |
| 評価が error（AML の例外、stack の予算、While の timeout 30 s） | BROKEN、log 1 行、何も得ていない。次の段へ進む（下の platform の行を除く）。namespace は firmware の途中の状態のまま。`acpi.osc=off` か段の語で避ける |
| Buffer でない・短い | BROKEN、同上 |
| status の failure・UUID・revision | REFUSED、何も得ていない |
| masked | 返った bit だけ得た |
| firmware が求めない bit を返す | 求めた bit で切る |
| platform が GRANTED・MASKED でない（ABSENT・REFUSED・BROKEN） | processor の段を飛ばし、log 1 行（review M9: 5330 は query で既に `OSCP` を書くので、commit が失敗しても processor を呼べば `_CPC` が Load される。失敗した firmware の上で `_CPC` を出さない） |
| commit は成功だが firmware の内部で失敗（5330 の `CHCM` の失敗は `_OSC` の戻りに出ない、§3.1） | `_OSC` からは分からない。p003・p006 で `SCME` と `U4CM & 7` を比べて検出する。検出した構成は Q310-2 で扱う |
| commit が長く待つ（AML の `Sleep` の繰り返し） | 起動が遅れる。p006 で起動の時間を比べ、`CHCM` が走る構成は別に記録する |
| `_OSC` の後に機能が壊れる | `acpi.osc=off` か段の語で起動し直す。5330 は電源の入れ直しで Debian に戻る（BootNext は 1 回限り、§6.3） |

### 5.12 既存の framework・他の WS との関係

- **WS300（ASPM・LTR）**: Q300-6（`_OSC` 無しで ASPM を設定してよいか）の答えの一つがこの WS。WS300 は (1) 有効にする書きを ACPI の後の段へ移し、(2) `drv_acpi_pcie_osc_control()` の bit 4・5 を確かめ、(3) `acpi-osc.c` の PCIe の要求の定数（§4.2）を DWORD2 bit 1・DWORD3 0x30 に変える。(1)〜(3) は WS300 の Phase の作業で、この WS は口と規則（§5.5）を出す。WS300 の merge の後に、この WS の p007 で PCIe の control つきの段を 5330 で流し直す（§8）。
- **WS333（CPU・熱）**: `_CPC` を使う code は足さない。p006 で `_CPC` の要素を §6.3 の判定で確かめ、食い違いを WS333 に渡す。`HWPV` ← 2 の後に HWP の MSR（PM_ENABLE・REQUEST）が firmware に変えられないかを、WS333 の strategy の要求の前後で読む。platform の bit 5・6 は WS333 の起動の語に従う（§4.2）。
- **WS052（S0 idle）**: `_OSC` の後の S0 idle の往復（今の 5330 で行ける所まで）を比べる。`NPME` を呼ばないので root port の PME の SCI（wake）は firmware に残る。TBT の `SCME` が RTD3 を変える（§3.1）ので、TBT の電源の資源の状態を比べる。
- **USB（xHCI）・UCSI**: §4.3 の表。USB4 の bit を求めないので firmware CM のまま。
- **i915**: `DDMS` の GPIO（§4.3 の表）。
- **Dell の WMI・thermal（WS333 p017・p018）**: `OSCP`・`OSCC`・`PFxx` は Dell の WMI の method から読まれない（AML を grep）。fan・温度・thermal mode を p006 で比べる。
- **`/dev/acpi`**: 変えない。
- **docs**: `docs/architecture/power-management.md` に `_OSC` の段と switch を p008 で足す（docs は目標の設計。未実装の時は書かない）。

### 5.13 HAL を変えない理由

- `_OSC` の評価は ACPI の driver の interpreter の中で閉じる。要る物（表・namespace・評価・region の handler・起動の語）は既に driver と kernel の口（`kern_boot_handoff`・`kern_boot_parameters_token_present`）にある。
- processor の有効・無効は MADT から driver が自分で読める（`drv_acpi_table_lookup("APIC")`）。HAL の MADT の解析に口を足す必要は無い。HAL の CPU の番号と ACPI の UID の対応は `_OSC` には要らない（呼ぶ順と数だけ）。
- idle（HAL の MWAIT）は `_CST` に依らないので、processor の `_OSC` の後も HAL の振る舞いは変わらない。
- ACPI mode に入る時（`start_events()`）は driver の中で、HAL に関わらない。
- よって `include/hal/hal.h` の差分の案は無い。

## 6. 前後の差を確かめる方法

### 6.1 host（5330 の表を aml-host で評価）

今の harness（`plan/tools/aml-host/`）は memory を 0 で模すので、GNVS（`dsdt.dsl:1236` の 0x6150C000）・CPU NVS（`ssdt2.dsl:192` の 0x61511000）の値が実機と違う。`CFGD == 0` なので動的な表の Load は試されもせず、`U4SE == 0` なので `DOCM`・`CHCM` の道も通らない。p002 で足す option（tools の変更。所有は Q1 が Queue で割り当てる）:

| option | 働き |
| --- | --- |
| `--set 'PATH i:N'` | 表の load と `_INI` の後、呼びの前に名前（Name・field unit）へ値を store（実機から読んだ値を入れる） |
| `--memory-file A=FILE` | 物理番地 A に file の byte を置く（`\SSDT` の package の番地に動的な表を置き、`Load(OpRegion)` を通す） |
| `--trace` | 呼びの間の region の書き（space・番地・幅・値。SystemIO 0x72・0x73、PCI config を含む）、Load・Unload、Notify、Sleep・Stall の合計を 1 行ずつ出す |
| `--diff` | 呼びの前と後で namespace の全ての名前と Integer・String・Buffer の値を取り、増えた・消えた・変わった物を出す |
| `--call-every-method` | 呼びの後に、引数の無い全ての method を評価して結果を出す（`check-latitude5330.sh` の 4 と同じ。前後の比べに使う） |
| `--acpi-mode` | 呼びの前に PM1 の SCI_EN を立てた状態にする（ACPI mode の前と後の比べ、review M1） |

手順（p003）:
1. 5330 の zedBSD（main、`_OSC` 無し）で `/dev/acpi` から入力の値を読む。**Name と field unit だけを書く**（Method は書かない。`/dev/acpi` は書いた path を評価する、§2）: `NEXP`・`ITSP`・`OSCC`・`CFGD`・`OSYS`・`SDTL`・`PF00`〜`PF11`・`HWPV`・`HWPE`・`INFS`・`ITBM`・`LMPS`・`CORE`・`NMFQ`・`SFBC`・`SFSC`・`RFBC`・`RFSC`・`TCNT`・`U4SE`・`U4CM`・`CMSK`・`EPTU`・`RTD3`・`HGMD`・`DPIW`・`DPG1`・`PCON`・`PSCP`・`DPTF`・`IN34`・`DDDR`・`\_SB.PC00.TDM0.SCME`・`TDM1.SCME`・`ITFP`。一覧は p003 の script に持ち、各名前が Name・field unit である事を host の namespace で先に確かめる。
2. 同じ 5330 の Debian で読み取り専用で `dmesg` の `_OSC`・thunderbolt・USB4・intel_pstate の行を取る（Linux が何を申告し何を得たか、SW CM か）。
3. CMOS の 0xF2: Debian で `/dev/port` の 0x72 に index を書き 0x73 を読む（index の port の書きだけで、値は書かない）。**host の操作なので Q1 の許可の後**（§11）。
4. harness で `off` と段ごと（`pcie`・`platform`・`cpu`・全部）を、1 の値と動的な表で、`--acpi-mode` の有り無しで走らせ、`--trace`・`--diff`・`--call-every-method` を比べる。
5. 出す物: `plan/ws310/osc-effects-5330.md`。最初の行に **CMOS（SystemIO 0x72・0x73）・TBT の mailbox と Force Power（PCI config の `DFPE`・`P2TB`）・GPIO（`SGOV`）への書きの有無**。その後に段ごとの hardware への書き・Load された表・増えた method・値の変わる method（`_PSS`・`_PDL`・`_PTC`・`_CPC`・`_PSD`・TBT の `_PS0`・`_PS3`・`_PR0`・`_PR3` の結果）・Notify・Sleep の合計の表。
6. 判定（p003 の受け入れ）: (a) 段ごとの書きの一覧が §3 の読みと一致（違えば §3 を直して理由を書く）、(b) 一覧の外の書きが無い、(c) CMOS・TBT・GPIO の書きの有無が yes/no で決まり、Q310-2 の A の条件（切り替えも CMOS の書きも無い）を満たすかが書かれている。

### 6.2 QEMU

- q35 の DSDT は `\_SB.PCI0._OSC` を持ち、platform・processor の `_OSC` を持たない（`plan/ws031/display-ref/dmesg.txt:212〜222`。QEMU 10.0 の表は p005 で `/dev/acpi` で確かめる）。q35 は LTR と PCIe hotplug を落とす（同じ log の `:221`）。今の要求（DWORD3 = 0）では GRANTED・control 0 が期待。
- 試験の SSDT（自分で書く ASL、`plan/ws310/tests/`、`-acpitable file=…` で足す）: q35 に無い `\_SB._OSC` と、q35 の CPU の device の scope に `_OSC` を足す。型: well-behaved（呼ばれた回数と DWORD を Name に残す）、bit を落とす、failure・unknown UUID・unknown revision、Integer を返す、短い Buffer、query で状態を変える（5330 の型）、AML の例外で止まる、`Sleep` を長く繰り返す。
- PCIe の失敗の型は host の harness の ASL の試験（p002）で行う。q35 の `\_SB.PCI0._OSC` は既にあるので SSDT で置き換えられず、偽の host bridge を足すと `acpi-pci-root.c` の root_visitor と `_CRS` の窓の割当に影響するため、QEMU では足さない。
- 判定は guest の serial（`plan/tools/guest/serial.py`）で `/dev/acpi` に記録の Name を書いて読む（console の log は読まない）。`acpi.osc=off`・`on`・段の語の組で、呼ばれた回数・DWORD が設計の通りか。q35 の `\_SB.PCI0.CTRL` が求めた値と q35 の mask の積。boot-test.sh で login prompt。

### 6.3 実機（5330）の前後の比べ

同じ kernel を語だけ変えて起動する（`plan/tools/hw5330/5330-run.sh --nvme-kernel VMUNIX NAME` に `CFG_ADD='acpi.osc=…'`）。**5330-run.sh は Debian から BootNext で zedBSD を 1 回起動し、終わると Debian に戻る**（`5330-run.sh:2〜8`）ので、全ての run が Debian と交互になる（review M3）。そのため:

- 毎回の起動で、`_OSC` の前の値（`U4SE`・`U4CM`・`CMSK`・`SCME`）を zedBSD の `/dev/acpi` で、CMOS の 0xF2 を前後の Debian で（§6.1 の 3、Q1 の許可の後）読み、要約の先頭に書く。
- 比べは同じ pre-boot の状態（`U4CM & 7` と CMOS 0xF2 が同じ）どうしに限る。
- 1 本の script `plan/ws310/tests/osc-5330.sh`（配置 → 起動 → 実行 → 回収 → 要約、p006 で作る）。各構成 3 回。

段の順: `acpi.osc=off` → `on`＋`platform=off`＋`cpu=off`（pcie だけ）→ `on`＋`cpu=off`（pcie・platform）→ `on`（3 段）→ `on`＋`cpuperf.hwp=off`（platform の bit 5・6 が 0 になる組、review m9）。各段の前に host の予測（§6.1）を持つ。段で FAIL が出たら次の段へ進まず切り分ける。

**script で自動に行う物（受け入れの必須）**:

| 項目 | 方法 | 判定 |
| --- | --- | --- |
| 起動 | 5330-run.sh の survey と PNG、login | `off` と同じく login まで。起動の時間（kernel の log の時刻）の差 ≤ 0.5 s。ただし `CHCM` が走る構成（p003 で分かる）は差を記録するだけ |
| kernel の log | `5330-survey-diff.py` で `off` と比べる（実機の log。QEMU の console log ではない） | 増えた行が §5.7 の行・`ACPI Debug` の行だけ。新しい error・warning・`Notify … has no handler` が無い |
| firmware の値 | `/dev/acpi` で §6.1 の 1 の Name・field unit | §6.1 の host の予測と一致 |
| namespace | `/dev/acpi` の全体の前後の diff | 増えた node が host の予測（`_CPC`・`_PSD` の表）と一致 |
| `_CPC` | host で副作用が無いと確かめた後に `\_SB.PR00._CPC` と AP の 1 つを評価 | FFH の要素: MSR の番地と bit の位置が WS333 が読む field（0x771、0x774、0xE7・0xE8）と一致。整数の要素（Nominal Perf・Reference Perf・Nominal Freq）: 値を記録し、P コアと E コアで `CORE` の bit に従って違う事。HWP_CAPABILITIES との値の一致は求めない（E コアの scale された Nominal は一致しないのが正常の見込み） |
| CPU の性能 | WS333 の `/sbin/thermal` の CPU の表示、4 strategy で 1 CPU の spin の MHz（WS333 p014 の手順）、PM_ENABLE・HWP_REQUEST の読み | `off` と同じ。HWP の MSR が firmware に書き換えられていない |
| 電力 | idle の package の電力（WS333 の RAPL の meter）、5 分 × 3 | `off` との差が揺れ（3 回の幅）の内か、下がる |
| 熱・fan | `/sbin/thermal`（温度・fan・Dell の thermal mode の get/set） | `off` と同じ |
| S0 idle | WS052 の今の手順で往復 3 回（行ける所まで）。wake は script で起こせる源 | `off` と同じ所まで行き、戻る。TBT の電源の資源の状態が host の予測と一致 |
| PCIe・storage | NVMe（VMD）の読み書き 1 GiB、PCI の一覧、LnkCtl・DevCtl2・LTR の値 | `off` と同じ |
| USB-C に device を挿したまま起動 | USB の stick を USB-C の port（TCSS）に挿したまま起動し、読み書き | `off` と同じく認識し続ける（`CHCM` が走る構成で必須、review M2）。stick を挿すのは Q1 の機材の準備 |
| network | AX211 の接続と ping（RTL8156B は有る時の任意） | `off` と同じ |
| 画面 | backlight の 0・100、画面の PNG | `off` と同じ |
| 戻し | `acpi.osc=off` で再起動 | firmware の値が `off` の基準に戻る（CMOS 0xF2 を除く、§5.6） |

**ユーザーが拾える試験の一覧に置く物（記録だけ、受け入れの必須ではない）**（AGENTS §3: ユーザーに試験を頼まない）: AC の抜き差し、lid の開閉、電源 button、USB-C の充電器・hub・画面の抜き差し、TBT の dock、音の再生、touchpad。p006 は一覧の項目・手順・`off` の時の期待を Q1 に渡す。

**止まった時**: 5330 に遠隔の電源は無い。BootNext は 1 回限りなので、電源の入れ直しで Debian に戻る。入れ直しは Q1 が扱う（AGENTS §9）。script は起動の待ちに上限を付け、来なければ FAIL として要約に書く。

**耐久**（Q310-5 で承認された時）: 一晩、`on` で idle と S0 idle の往復を繰り返す。hang・reboot・log の error が無い。

## 7. WS の受け入れ

1. host: harness の `on` の評価で、5330 の表の書きの一覧が設計（§3・§4）と一致し、CMOS・TBT・GPIO の書きの扱いが Q310-2 の答えの通り。codec の host 試験が PASS（ASan・UBSan）。
2. QEMU: 試験の SSDT の全ての型で、段ごとの呼びの回数・DWORD・outcome が設計の通り。`acpi.osc=off` で 1 回も呼ばれない。boot-test.sh で login prompt。
3. 5330: §6.3 の「自動」の表の全ての項目が、全ての段で PASS（同じ pre-boot の状態どうしの比べ）。`_CPC` が namespace にあり §6.3 の判定を満たす。`acpi.osc=off` で前と同じ振る舞いに戻る（CMOS 0xF2 を除き、その値を記録）。
4. WS300 の有効にする code の merge の後: PCIe の control つきの段で 3 を 1 回通す（p007）。WS300 が未 merge のまま WS を閉じる時は、この項を WS300 へ移管と書く。
5. 耐久（Q310-5 で承認された時）: 一晩で hang・reboot・新しい error が無い。
6. 既定の変更（Q310-1 で承認された時）: 既定を `on` に変えた kernel で 3 を 1 回通す。docs を直す。

## 8. Phase

| Phase | 目的 | 依存 | 受け入れ条件 | 担当の型 | 規模 |
| --- | --- | --- | --- | --- | --- |
| p001 | 調べと設計（この文書）、review | — | design-reviewer の review を反映、問いを [user-questions.md](user-questions.md) に | 設計 | — |
| p002 | aml-host の option（§6.1 の 6 つ）と ASL の試験（`_OSC` の型 8 つと PCIe の失敗の型、acpiexec と比べる） | p001、tools の所有の割当 | `make -C plan/tools/aml-host`・`run-asl.py`・`check-latitude5330.sh` が PASS、新しい ASL の試験が acpiexec と一致、tools.md の行の更新を Q1 に頼む | phase-runner | 2 LW |
| p003 | 5330 の入力の読み（`/dev/acpi`、Debian の dmesg と CMOS、読むだけ）と host の評価、`osc-effects-5330.md` | p002、5330 の割当、CMOS の読みの許可 | §6.1 の 6 の (a)〜(c) | phase-runner-high | 2 LW |
| p004 | `acpi-osc-codec`（純粋な関数）と host 試験 | p001 | 正常・masked・failure・UUID・revision・短い Buffer・Integer・余計な bit・count 2 と 3 の試験が PASS（ASan・UBSan）、わざと壊す 3 通りを捕える、UUID と bit の値の block に `Values referenced:`（§9） | phase-runner-high | 1 LW |
| p005 | `acpi-osc.c` の統合（3 段・ACPI mode の後の位置・MADT・host bridge・結果の表・PCI の口・log）、4 つの起動の語（既定 off）、QEMU の試験の SSDT と script | p004、Q310-3・Q310-4 の答え | amd64 の build（warning 0）、QEMU の §6.2 が PASS、`acpi.osc=off` で呼ばれない、有効にする PCIe の書きが 0 件（grep）、boot-test.sh で login | phase-runner-high | 3 LW |
| p006 | 5330 の前後の比べ（§6.3、1 本の script）、段ごと | p003、p005、Q310-2 の答え、5330 の割当 | §6.3 の「自動」の表の全ての項目が全ての段で PASS。FAIL の段は止めて切り分け、原因と戻しを記録して uncleared。ユーザーが拾える一覧を Q1 に渡す | phase-runner-high | 3 LW（実機の時間を含む） |
| p007 | PCIe の control つきの段の 5330 の比べ | p006、WS300 の有効にする code の merge | §7 の 4 | phase-runner-high | 1 LW |
| p008 | 既定の変更（Q310-1 の承認の後）・docs・耐久（Q310-5 の承認の後） | p006（p007 は WS300 次第） | §7 の 5・6 | phase-runner-high | 1 LW＋一晩 |
| p009 | 規約の全文の見直し（変えた C の code） | p008 | coding-style の全文と照らし、書き方だけを直す | conformance-reviewer | 0.5 LW |

- p002 と p004 は並べてよい（file が重ならない）。
- 標準廉価実装手順（`docs/agent/spec-module-delegation.md`）は p004 に当てない: 仕様が手元に無く（§9）、header の仕様を一次の出典から書けない。仕様が手に入れば p004 を手順に乗せ直してよい。

## 9. 出典と引用

- ACPI 6.5 §6.2.11（`_OSC`）と platform-wide の DWORD の表: **未取得**（uefi.org が bot の確かめの page を返した、2026-10-31）。cockpit の 01 と同じ件（Q310-6）。
- PCI Firmware Specification 3.3（PCI の host bridge の `_OSC`）: 未取得（PCI-SIG の会員向け）。
- Intel の processor の `_OSC` の bit: 未取得。processor は DWORD2 = 0 にするので bit の意味に頼らない。UUID は 5330 の AML と Linux の両方に現れる値。
- **値の出典**（guardrail「MIT・GPL の出典のデータの引用」: GPL の file の定数・表はデータとして block ごとに出典を示して使ってよい。code・comment・構造・symbol の名前は写さない）: UUID と bit の値は Linux v6.12.5 `include/linux/acpi.h`（`:551〜620`）から、block の直前の複数行の comment に `Values referenced: Linux v6.12.5 include/linux/acpi.h (the _OSC DWORD bits)` の形で書く。仕様が手に入れば、仕様の版・節・表を出典に足すか置き換える（merge を仕様の入手に縛らない。review M11 の 2 案のうち guardrail の道を採った）。file の先頭には書かない。
- **手順の出典**: 呼ぶ順（ACPI mode の後、`_INI` の後、platform → processor）は Linux v6.12.5 `init/main.c`・`drivers/acpi/bus.c` の `acpi_subsystem_init()`・`acpi_bus_init()` を参考にした。code に書く時は該当の block の直前に `Procedure referenced:` で。
- 5330 の DSDT・SSDT: Dell の著作物（`plan/tools/aml-host/latitude5330/README.md`）。この文書は振る舞いを自分の言葉で書き、AML の block を写していない。名前（`OSCC`・`NEXP` など）と番地は事実として書いた。
- QEMU q35 の Linux の log（`plan/ws031/display-ref/dmesg.txt`）: 出力の文字列で code ではない。

## 10. 選ばなかった案

| 案 | 選ばなかった理由 |
| --- | --- |
| Linux と同じ bit を申告・要求する（hotplug・PME・AER の制御も） | zedBSD に代わりの処理が無い。5330 では query の時点で PME・hotplug の SCI が止まり、wake と hotplug が壊れる（§3.3） |
| PCIe の control 0x30 を今すぐ求める（第 1 版） | 使う code が main に無く、確かめが空振りになる（review M7）。WS300 の code と一緒に入れ、p007 で確かめる |
| platform の bit 2（`_PR3`）を申告する（第 1 版） | D3cold を使う呼び手が無い（review M5） |
| processor に Intel の bit（C-state の FFH・`_PSS` など）を申告する | zedBSD は `_CST`・`_PSS`・`_PPC` を使わない。5330 は IST を Load して `SPNT` の Notify と `_PPC` の上限を出す（受け手が無い）。Q310-3 |
| `_OSC` の無い processor に `_PDC` を呼ぶ | 古い口。5330 は `_OSC` を持つ。QEMU は両方無い |
| platform-wide を呼ばない | 5330 は `_CPC` を出さない。Q310-2 の答えで外す道は語（`acpi.osc.platform=off`）で残る |
| `_INI` の後・ACPI mode の前に呼ぶ（第 1 版） | ACPI mode と Global Lock の前に SMM と共有の記憶を書く。firmware の試された道の外（review M1） |
| ACPI の attach を PCI の bind より前に動かす／platform の `_OSC` だけ PCI の前に呼ぶ | PCI_Config の region の handler が PCI の driver を使い、`_CRS` の窓の割当も PCI の後の段にある。ACPI mode に入るのも PCI の後になる。順を変える影響が大きい。bind の後に firmware が触る device を列挙し（§4.3）、実機の比べで確かめる |
| 結果を新しい sysctl・UAPI で出す | `/dev/acpi` と log で足りる |
| `acpi.osc=pcie,platform` のような列の語（第 1 版） | `boot.c` の語の検査は固定の語が前提で、列の順・重複の文法に向かない。`cpuperf.hwp` と同じ on/off の 4 つの語にした（review m2） |
| 呼びを kernel の build の CONFIG で切る | 1 つの kernel で `off` と `on` を比べたい（§6.3） |
| S0 idle の後に `_OSC` を呼び直す | firmware の状態は保たれる。呼び直すと 5330 の `DOCM` が TBT に再び触れる |
| 標準廉価実装手順で codec を Sonnet に委ねる | 仕様が手元に無い（§8） |
| merge を仕様の入手（Q310-6）に縛る | guardrail が GPL の定数の出典つきの使用を許す。仕様は手に入り次第、出典に足す |

## 11. 問いと Q1 への確認

ユーザーの判断が要る点は [user-questions.md](user-questions.md) の Q310-1〜6。Q1 への確認:

1. p002 は `plan/tools/aml-host/` を変える（tools の共有の file）。Queue で所有を割り当ててほしい。
2. WS300 との順（§5.12）: WS300 の有効にする書きを ACPI の後へ移す作業と、`acpi-osc.c` の PCIe の定数の変更は WS300 の Phase。この WS の p005 の口が先に main に入る必要がある。
3. p003・p006 の 5330 の割当。p003 の CMOS の読み（Debian で `/dev/port` の 0x72 に index を書き 0x73 を読む）は host の hardware の操作なので、許可の要否を決めてほしい。
4. `include/drivers/acpi/acpi.h` と `include/kern/boot.h`・`src/kern/boot.c` の変更（HAL ではない kernel の変更）を p005 の範囲に入れる。
5. [ws.md](ws.md) の 2026-10-29 の決定は原文のまま残した（Q1 の指示）。AGENTS §8（2026-10-30、原文を載せない）との扱いは Q1 が決める（review M12）。
6. Linux v6.12.5 の tree は `/home/awe/visionfive-linux` を読んだ（読むだけ）。実装の Phase でも同じ tree を値の照合に使ってよいか。

## 12. 未確認の事

1. 5330 の GNVS・CPU NVS の値（`NEXP`・`U4SE`・`U4CM`・`CFGD`・`DPIW`・`PCON` など）と CMOS 0xF2。p003。
2. `HWPV`・`OSCC`・CMOS 0xF2 を SMM・BIOS が読むか、ACPI_ENABLE の SMI が `_OSC` の結果を書き換えるか。firmware の source が無い。p006 で値と HWP の MSR を比べる。
3. `DDMS` の GPIO（`DPG1`）が何の電源か。p003 で値、p006 で画面を比べる。
4. CMOS の 0xF2 を BIOS が次の起動で読むか。p006 の毎回の記録で。
5. platform・PCIe の bit の意味の仕様での確認（§9）。
6. QEMU 10.0 の q35 の `_OSC` の mask（p005 で `/dev/acpi`）。
7. 5330 の BIOS の版（今の表は BIOS 1.31.1）。p003 で実機の版を確かめ、違えば表を取り直す（Q1 に頼む）。
8. TCSS の xHCI が CM の切り替えの時の port の切断・再接続を正しく扱うか。p006。

## 13. 履歴

- 2026-10-31 第 1 版（P6、q1779-i01、41a915514）。
- 2026-10-31 第 2 版（P6、同じ Queue）: review を反映（design-review.md の反映の表）。
