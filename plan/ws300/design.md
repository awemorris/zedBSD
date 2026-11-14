<!-- awesome-plan project=zedbsd record=ws300-design -->

# WS300 設計: NVMe と VMD の低電力（S0 idle の VMD の suspend・NVMe の状態、APST・ASPM・LTR）

- 版: **第 3 版**（2026-11-15 P5、q2114-i01）。第 2 版（2026-10-29 P6、q1683-i01、merge されなかった branch `agent/p6-ws300` の上）を今の main（5202274642）に移し、
  次を直した（差は §16）: p002 は [BUG-424](../bugs/BUG-424.md) の実装（main の debec3781b・295eea4623）で置き換わった（§4.1）。build は top の Makefile と `mk/` になり、
  新しい .c は mk の変更（ユーザーの確認）が要るので、計算だけの部分は既存の header の inline（`pci-nvme-protocol.h` の形）に置く（§3.2）。p004 と UAPI は Q300-6・Q300-2 の決定待ち。
- 第 2 版の経緯: 第 1 版（P5 の下書き §0〜§3 に P6 が §4〜§14 を足した）を design-reviewer にかけ（[design-review.md](design-review.md)、重大 2・中 6・軽 15）、指摘を §15 のとおり反映した。
- 基点: worktree `p5-ws300`（main 5202274642）。**HAL は変えない**（§3.1）。
- 入力: [survey-5330.md](survey-5330.md)（5330 の Debian の読みと DSDT）、[survey-fable.md](survey-fable.md)（公開の資料）、zedBSD の source（§1）。
  web の取得はしていない。GPL の source は読んでいない（§12 の Linux の値は記憶と 5330 の Debian の読みの照合で、§12 に分けて書く）。

## 0. ユーザーの決定（要点）と解釈

| 日付 | 決定の要点 | 解釈 |
| --- | --- | --- |
| 2026-10-27 | NVMe と VMD が S0i3 などの低電力に対応する WS を立てる（まず計画、後で順に実装） | この WS の発端。VMD・NVMe が S0 idle（S0ix）を妨げないようにする |
| 2026-10-27 | S0ix が動いた実績が無いので、それにも取り組む（既存の WS で） | S0ix の全体は WS052（Q1 の割当）。この WS は storage の側 |
| 2026-10-27 09:39（クリック、ws.md） | 動作中の省電力（NVMe の APST、PCIe の ASPM L1.2・LTR）も入れる（docs/architecture/power-management.md の「runtime の省電力は対応外」を直す） | 範囲に APST・ASPM・LTR を含む。文書の範囲も変える（ユーザーの review） |
| 2026-10-29（Q1 経由、q1683） | 手の空いた担当は、試験なしで実装だけ残りの Phase を進める | 第 2 版の p002〜p005 は実装と build・host の試験まで（branch、merge されず）。判断の要る所は推しで進めて記録し Q1 に知らせる（§13） |
| 2026-11-15（Q1 経由、q2114） | WS300 を main の上で作り直して進める。新しい .c は mk の変更（ユーザーの承認）が要るので既存の file に収める形を先に選ぶ。Q300-2（UAPI）・Q300-6（`_OSC`）は判断待ち | p003 と p005 の APST（UAPI 無し）を main の上に。p004 と UAPI は手を付けない |

## 1. 今の振る舞い（code の事実、main 5202274642）

- VMD・PCI の共通（[BUG-424](../bugs/BUG-424.md) の debec3781b・295eea4623）: `pci.c` が root bus を出す function を bus に記録し（`drv_pci_bus_set_host_function`）、
  `drv_pci_device_upstream` で function の真上を返す。`pci-power.c` の深さは upstream で数える（NVMe 3、隠れた root port 2、VMD 1）ので、子が先に suspend、親が後、resume は逆。
  driver の無い function でも、上に driver の付いた function があれば config を保存・復元する（D3 にはしない）。保存に Slot Control（slot のある port）・Root Control を足し、
  Slot Control は今と違う時だけ書く。restore は LnkCtl を ASPM を外して書き、L1SS の後に元の値（第 2 版 §4.3.5 の直しはここで入った）。bus 番号を失った port は DLLLA を最大 1 s 待ち 100 ms。
  `vmd_suspend` は VMCONFIG（0x44）を保存し、PMCSR に No_Soft_Reset の無い VMD を `drv_pci_device_keep_d0` で D0 に残す。`vmd_resume` は VMCONFIG を違えば書き戻し、
  bus の始まり・窓・root port の Vendor ID を確かめ、だめなら EIO。
- 5330 の結果（BUG-424 の b424d、2026-11-13）: `sleepctl devices` の往復は成功（NVMe → … → VMD の順、戻りで「vmd: ffff: resumed (configuration fe01, kept)」、root port の reset は無し）。
  S0 idle に入って戻る事は未達（xHCI の port 3 の USB の NIC が戻らない [BUG-428](../bugs/BUG-428.md)、TCO の watchdog [BUG-429](../bugs/BUG-429.md)）。5330 の VMD は `NoSoftRst+`。
- NVMe（`pci-nvme.c`、main 5202274642 で読み直した）: `nvme_suspend`（2265〜）は要求を止めて待ち（`nvme_suspend_wait_idle`）、`nvme_suspend_stop`（2413〜）で割り込みを mask し、CC.SHN の
  正規の shutdown → disable → bus master を切る → 割り込みの drain → I/O の quiesce。D3hot は PCI の共通。
  `nvme_resume`（2303〜）は `nvme_resume_steps`（2480〜）で disable → queue の memory を作り直し → enable → 割り込みの復元 → I/O queue の作成 → **HMB を Memory Return つきで渡し直す**
  （`nvme_hmb_reenable`、断られたら新しく渡す）。**suspend の前に HMB を止めていない**（§4.2）。admin の完了は割り込みだけで待つ（`nvme_admin_execute_mode`）。
  APST（Feature 0Ch）・Feature 02h の定義、Identify の NPSS・APSTA・PSD の読みは無い。HMB の Set Features の組み立て（`drv_nvme_set_hmb_command`）は disable（EHM = 0）も作れる。
- ASPM・L1SS・LTR を設定する code は無い（save と restore だけ）。

## 2. 調べた事実（要約、詳細は survey の 2 つ）

- BG5: PS0〜PS2 操作（4.0・2.5・1.3 W、ENLAT/EXLAT 1 µs）、PS3 0.05 W（800 µs・1000 µs）、PS4 0.003 W（3000 µs・44900 µs）、APSTA 1、RTD3R 100 ms・RTD3E 500 ms、HMB を求める（DRAM 無し）。
- 隠れた root port 10000:e0:06.0 と NVMe: ASPM L1 と L1SS の能力が有り、Debian でも ASPM・L1SS は全部無効。LTR は両端で有効。
  root port の L1SS の cap は PCI-PM L1.2・L1.1、ASPM L1.2・L1.1、CMRT 110 µs、T_POWER_ON 500 µs。NVMe は PCI-PM L1.2・L1.1、ASPM L1.2（ASPM L1.1 無し）、CMRT 60 µs、T_POWER_ON 150 µs。
  両端の CTL の値は T_CommonMode 110 µs（root port）、T_PwrOn 500 µs、LTR_L1.2_THRESHOLD 616448 ns（§4.3 の計算の値と同じ。Linux が書いた値と見る）。
  NVMe の LTR の max snoop は 3145728 ns（同じく Linux の VMD の quirk の値）。
- PMC: Debian でも動作中は Package C2 まで、S0ix の residency は 0（他の妨げもありうる、未確認）。
- DSDT: `\_SB.PC00.VMD0` に `StorageD3Enable`（`VDSD == 1` の時）。`VMD0.PEG0` に D3cold の method `VPOF`・`VPON`（呼び手は DSDT に無い）。


## 3. 設計の方針

### 3.1 HAL は変えない（理由）

この WS が触る物は全部 PCI の config 空間・NVMe の admin command・ACPI の device の電源で、どれも driver の層に既に口がある:
config の読み書き（`drv_pci_device_config_*`、VMD の下は VMD の bus の ops）、D-state（`drv_pci_device_set_power_state`）、save と restore（`drv_pci_device_save_state`）、
ACPI の `_PSx`（`drv_pci_platform_power`）、時間の待ち（`sched_sleep`・`kern_usleep_range`）。CPU の idle・割り込みの mask・timer は WS052 の既存の HAL の口のまま。
HAL の宣言・契約を変える必要は無い。

### 3.2 全体の形

| Phase | 何を | どこで |
| --- | --- | --- |
| p002 | VMD の suspend・resume、suspend の順の明示 | **BUG-424 で main に入った**（§4.1）。この WS では実装しない |
| p003 | S0 idle の NVMe: SHN＋D3hot を保ち、shutdown の前に HMB を止める。suspend・resume の時間を log に | `pci-nvme.c` |
| p004 | VMD の下の link の ASPM L1・L1 substates・LTR（attach の後に設定、resume で同じ値に戻る）。restore の順の直しは BUG-424 で済み | 計算は header の inline（案: `include/drivers/pci/pci-link-plan.h`、.c を足さない）、書きは `pci-vmd.c`。Q300-6 の決定の後 |
| p005 | NVMe の APST（PSD の読み、表の組み立て、Set Features 0Ch、resume・recovery の後に設定し直す）、power state の log | 計算は `include/drivers/pci/pci-nvme-protocol.h` の inline（既存の HMB の計算と同じ置き方）、送りは `pci-nvme.c` |
| p006 | 5330 の受け入れ（1 本の script） | `plan/ws300/tests/` |
| p007 | 規約の全文の見直し | 上の file |

- 計算だけの部分（L1SS の値、LTR の閾値の符号化、ASPM の可否、APST の表）は kernel の header に依らない形にし、host で試す（§9）。
- 第 2 版は計算を新しい .c（`pci-link-plan.c`・`pci-nvme-power.c`）に置き `platform/*/vmunix.mk` に足したが、今の build（top の Makefile と `mk/`）では新しい .c に mk の変更が要る
  （AGENTS.md §7、ユーザーの確認）。header の inline なら mk を変えずに済み、host の試験も header だけを include すれば良い（`plan/ws299/tests/host/nvme-hmb-test.c` と同じ形）。

## 4. 各 Phase の設計

### 4.1 p002: VMD の suspend・resume（BUG-424 で置き換わった）

- [BUG-424](../bugs/BUG-424.md)（2026-11-13 P2）が、第 2 版の p002（VMD の driver が隠れた root port を自分で集めて save・D3hot・restore、`pci-power.c` に root bus の「持ち主」の固定 8 の表）
  を読み、「config の保存・復元と順は PCI の core の責務、bus と function の繋がりは topology なので bus の欄に置く」形で main に入れた（§1）。第 2 版の p002 の code は merge しない。
- BUG-424 が第 2 版から取り入れた物: No_Soft_Reset の無い VMD を D0 に残す（R4）、resume で root port の Vendor ID を確かめ EIO（R4・R6）、Slot Control は違う時だけ書く（R20）、LnkCtl の ASPM を L1SS の後に（p004 の一部）。
- 取り入れていない物（この WS の残りの候補、要否は p006 の 5330 の測りの後）: 隠れた root port の D3hot（No_Soft_Reset の立つ port だけ、下の function が全部 D3hot の時だけ）、
  診断の boot の語 `pci.vmd.d3=force`。S0 idle の PC の滞在を見てから決める（ws.md の Resume point）。
- 第 2 版の §4.1.1〜§4.1.4（持ち主の表、port の一覧、suspend・resume の手順）は削った（git の履歴の branch `agent/p6-ws300` と `plan/backup-20261114/agent_p6-ws300.diff` にある）。

### 4.2 p003: S0 idle の NVMe の状態

- **推し: 今の SHN＋D3hot を保つ。** 理由: (1) 5330 の DSDT は VMD0 に `StorageD3Enable` を持ち、`VDSD == 1` なら platform が D3 を求めている（`VDSD` の値は未読、§14-3、R12）。
  (2) D3hot の NVMe の link は PCI-PM L1 に入り、p004 で PCI-PM L1.2 を有効にすれば L1.2 に入れる（S0ix の link の条件）。
  (3) 「最深の非操作の PS＋D0」（Linux の既定の形）は platform の D3 の要求と逆で、resume が速い代わりに VMD・root port を D0 に残す。
- 足す事: **shutdown の前に HMB を止める**（Set Features 0Dh、EHM = 0。NVMe Base 2.0e §8.9）。置き場所は `nvme_suspend_stop` の最初、割り込みの mask の前
  （admin の完了は割り込みだけで待つので、mask の後では毎回 timeout になる、R10）。timeout の後の `admin_fault` は戻しの `nvme_admin_queue_memory_reset` が消すので、戻しの resume で回復する。止めの失敗（timeout 以外）は log だけで続ける
  （SHN は続ける。resume の Memory Return つきの渡し直しは今のまま、断られたら新しく渡す）。timeout は今の suspend の失敗と同じに扱う（中止）。
- suspend と resume の時間（ms）を今の log の行の後ろに足す（`nvme: suspended in N ms`、`nvme: resumed epoch=E in N ms`。照合する試験・道具は無い、§9）。
- `StorageD3Enable` は読まない（どちらの値でも D3hot にするので、読んでも振る舞いが変わらない）。D3cold（`VPOF`・`VPON`）は p006 の測りで要ると分かった時に別の WS（ws.md の範囲の外）。
- 第 3 版の実装（main の上、`pci-nvme.c` だけ）: `nvme_hmb_disable` は HMB の状態が ENABLED の時だけ送る（READY・NONE・UNKNOWN は送らない）。成功で READY（memory は持ったまま）、
  timeout で UNKNOWN にして ETIMEDOUT（suspend は中止し、`nvme_resume_start` の disable と admin の queue の作り直しで戻る）、拒否は log の 1 行で SHN を続ける。
  command は既存の `drv_nvme_set_hmb_command(enable = 0)`（`plan/ws299/tests/host/nvme-hmb-test.c` の「disable command」が組み立てを確かめている）。
- 範囲の外の気づき（未確認、変えていない）: system の shutdown（`nvme_shutdown`）も HMB を止めずに SHN する。電源を切るなら memory は失われるだけだが、warm reboot では次の起動の
  firmware・OS が memory を使い始める前に controller が reset される保証を確かめていない。要れば別の Phase か Bug（Q1 の判断）。

### 4.3 p004: ASPM L1・L1 substates・LTR

#### 4.3.1 何をどこで

- VMD の attach の scan の後に、各 port と、その secondary bus の slot 0 function 0 の function（endpoint）の link を設定する。VMD の下だけ（他の PCIe の
  device の一般化は ws.md の範囲の外）。設定の値は両端の save_state に入るので、S0 idle の往復で同じ値に戻る（4.1.4、core の NVMe の復元）。
- 計算（第 3 版の案: `include/drivers/pci/pci-link-plan.h` の inline。第 2 版は `src/drivers/pci/pci-link-plan.c` で、今の build では mk の変更が要る）: 両端の LnkCap・DevCap・DevCap2・L1SS の cap から、
  有効にする物（ASPM L1、L1SS の 4 つの enable、LTR）と書く値（CMRT、T_POWER_ON、LTR_L1.2_THRESHOLD）を決める。kernel の header を使わず host で試す。
- **第 3 版（2026-11-15）では p004 に手を付けない**（Q300-6 の決定待ち、Q1 の指示）。第 2 版の code（branch の `pci-link-plan.c`・`vmd_link_power`・`drv_acpi_pcie_aspm_forbidden`）は
  決定の後に main の上で作り直す時の参考。
- 書き（`pci-vmd.c`）: 下の手順で config を書く。**既定では何もしない**。boot の語 `pci.vmd.aspm=on` の時だけ設定する（R2: 5330 で試すまで、毎回の起動の振る舞いを変えない。
  p006 の PASS の後に既定を on にする）。FADT の IAPC_BOOT_ARCH の bit 4（PCIe ASPM Controls、ACPI 6.5 Table 5.11）が立っていれば `on` でも何もしない（R1、`drv_acpi_pcie_aspm_forbidden`）。
  `_OSC` は評価しない（zedBSD は `_OSC` を持たない。platform の契約の判断なので Q300-6 として Q1 に上げた）。

#### 4.3.2 register（PCI Express Base の版 4.0 以降の配置。§12）

| register | bit |
| --- | --- |
| LnkCap（cap + 0x0C） | 11:10 ASPM の対応（bit 11 = L1）、17:15 L1 の exit latency の符号（0: < 1 µs … 6: 32〜64 µs、7: > 64 µs）、20 DLLLA の報告 |
| LnkCtl（cap + 0x10） | 1:0 ASPM の control（bit 1 = L1）、6 Common Clock |
| LnkSta（cap + 0x12） | 13 Data Link Layer Link Active |
| DevCap（cap + 0x04） | 11:9 endpoint の L1 の許容 latency（0: < 1 µs … 6: < 64 µs、7: 制限なし） |
| DevCap2（cap + 0x24）・DevCtl2（cap + 0x28） | DevCap2 bit 11 LTR の対応、DevCtl2 bit 10 LTR の有効 |
| L1SS の cap（ext 0x1E、+ 0x04） | 0 PCI-PM L1.2、1 PCI-PM L1.1、2 ASPM L1.2、3 ASPM L1.1、4 L1SS、15:8 CMRT（µs）、17:16 T_POWER_ON の scale（2・10・100 µs）、23:19 その値 |
| L1SS の CTL1（+ 0x08） | 3:0 上と同じ並びの enable、15:8 CMRT、25:16 LTR_L1.2_THRESHOLD の値、31:29 その scale（1・32・1024・32768・1048576・33554432 ns） |
| L1SS の CTL2（+ 0x0C） | 1:0 T_POWER_ON の scale、7:3 その値 |
| LTR（ext 0x18、+ 0x04・+ 0x06） | max snoop・max no-snoop の latency（9:0 値、12:10 scale、scale の段は 32 倍） |

#### 4.3.3 決め方

- ASPM L1: 両端の LnkCap の L1 が有り、link の L1 の exit latency（両端の大きい方の符号の上限）が endpoint の許容（符号 7 は制限なし）以下の時だけ。L0s は使わない。
  両端の Common Clock（LnkCtl bit 6）が立っていない link は、L1 の exit latency の報告が当てにならないので ASPM も L1SS も有効にしない（retrain はしない）。
- LTR: 両端の DevCap2 の LTR が有る時だけ有効に。endpoint の LTR の max snoop と max no-snoop が両方 0 なら、両方に 0x1003（3 × 1048576 ns = 3145728 ns）を書く
  （0 のままだと device の報告が 0 に縛られ、platform が深い状態に入らない。値は 5330 の Debian の読みと同じ、§12）。
- L1SS: 両端の cap の L1SS（bit 4）が有る時、enable = 両端の cap の bit 3:0 の共通の部分。ASPM L1.2・L1.1（bit 2・3）は ASPM L1 を有効にする時だけ、ASPM L1.2 は LTR を有効にした時だけ。
  - T_POWER_ON（CTL2、両端）= 両端の cap の T_POWER_ON の µs の大きい方（その端の scale と値のまま）。
  - CMRT（CTL1、上流の port だけ）= 両端の cap の CMRT の大きい方。
  - LTR_L1.2_THRESHOLD（CTL1、両端）= CMRT + T_POWER_ON + 6 µs を、値が 1023 以下になる最小の scale で切り上げて表す。5330 では 110 + 500 + 6 = 616 µs → scale 2（1024 ns）、値 602 = 616448 ns（Debian の値と一致）。
- 5330 の見込み: ASPM L1、PCI-PM L1.2・L1.1、ASPM L1.2（ASPM L1.1 は NVMe に無い）、LTR は既に有効、NVMe の LTR の max は 0 なら 0x1003。

#### 4.3.4 手順（有効にする時）

0. 読んだ値を確かめる: 両端の LnkCap、endpoint の DevCap、両端の L1SS の cap のどれかが 0xFFFFFFFF なら（VMD の下の読みは失敗を返さず、応えない端は全 1 を読む）その link を触らない。
   T_POWER_ON の scale が予約（3）か時間が 0 の端があれば L1SS を有効にしない（R7）。
1. ASPM を両端で切る（LnkCtl の 1:0 を 0、下流の endpoint から、次に上流の port）。control は 16 bit で読み書きする（隣の status の RW1C を書かない）。
2. LTR: endpoint の LTR の max が 0 なら先に書き（R19）、DevCtl2 の LTR を上流、次に下流で有効に。
3. L1SS: CTL1 の enable（3:0）を下流、次に上流で 0 に。CTL2 を両端に書く。CTL1 の CMRT（上流）と閾値（両端）を enable 0 のまま書く。enable を上流、次に下流で書く。
4. ASPM L1 を上流、次に下流で有効に。
- 読みの失敗（手順 0 の前）は何も書かずに log。書きは VMD の CFGBAR の MMIO で失敗を返さないので、書きの途中の失敗の後始末は持たない（R7 の後始末の案は、検出の手段が無いので採らない）。
- 結果は 1 行の log（`vmd: link ffff:e0:06.0 aspm L1 l1ss pcipm-l1.2 pcipm-l1.1 aspm-l1.2 ltr threshold 616448 ns`）。

#### 4.3.5 restore の順の直し（`pci-power.c`、BUG-424 で main に入った）

第 3 版の注: 下の順の直しは BUG-424（debec3781b）が `drv_pci_device_restore_state` に入れた（LnkCtl を ASPM を外して書き、L1SS の後に保存した LnkCtl）。p004 では足さない。

今の `drv_pci_device_restore_state` は LnkCtl（ASPM の enable を含む）を L1SS の CTL より先に書く。L1SS の enable は ASPM L1 が切れている時に変えるのが規則なので、
順を「DevCtl、LnkCtl の ASPM の bit を 0 にした値、DevCtl2、LnkCtl2、LTR、L1SS の CTL2・CTL1、最後に保存した LnkCtl」に変える。
link の両端は VMD の resume（上流の port）→ core の NVMe の復元（下流）の順に復元されるので、L1SS と ASPM の enable は上流が先になる。
ASPM L1 の入りは link の Upstream Port（endpoint）が始めるので、上流の port を先に戻し endpoint を後に戻す順なら、endpoint が自分の L1SS を書く間（自分の ASPM は 0）link は ASPM L1 に入らない。
driver の無い上流の port（PCH の root port など、他の機械）は restore されず、firmware の値のまま（R9。この理屈は記憶に依る、§12）。
他の機械の function も同じ restore を通るが、書く値は保存した値のままで、変わるのは順だけ。

### 4.4 p005: APST と電源の状態の log

- Identify Controller（既に読んでいる）から NPSS（byte 263、0 起点で状態の数 − 1）、APSTA（byte 265 bit 0）、PSD（byte 2048 から 32 byte × 32）の
  MP・MXPS・NOPS・ENLAT・EXLAT を読み、controller に覚える（第 3 版: `pci-nvme-protocol.h` の inline の parse、host で試す。第 2 版は新しい `pci-nvme-power.c`）。
- 表の組み立て（256 byte、32 entry × 8 byte、bit 7:3 ITPS、31:8 ITPT の ms）:
  - 使える目標 = 非操作（NOPS）の状態で、ENLAT + EXLAT が許容（既定 100000 µs）以下の物。ENLAT・EXLAT が 0（未報告）の状態は使わない。
  - 各状態 i の entry: i より番号の大きい、使える目標で、最大の電力（MP）が i 以下の最初の状態 j を指す。ITPT は 2 段（R11）: ENLAT_j + EXLAT_j が 15000 µs 以下なら 100 ms、
    それより遅ければ 2000 ms（5330 の Debian の BG5 の表と Linux の parameter の値）。j が無ければ 0（遷移しない）。
    idle の電力（IDLP）は比べない（BG5 は報告しない見込み、未確認。MP の比較で代える）。
  - 5330 の BG5: PS0〜PS2 → PS3 を 100 ms（1.8 ms）、PS3 → PS4 を 2000 ms（47.9 ms）。Debian の表と同じ。
- Set Features 0Ch（CDW11 bit 0 APSTE = 1、PRP1 = 表）を、attach（I/O queue の作成・HMB の後）、resume の後（`nvme_resume_steps` の中。suspend の失敗の戻しも同じ道）、
  recovery の後（`recovery_command` 1。0 では recovery の間 ENXIO になる、R10）に送る。
  **既定では送らない**（firmware の設定のまま）。boot の語 `nvme.apst=on` の時だけ送る（R2。p006 の PASS の後に既定を on に）。on で APSTA が 0 なら送らず、使える目標が無ければ APSTE = 0 を送る。
  失敗は log だけで続ける（APST は省電力で、I/O の正しさには関わらない）。
- 表は controller ごとの 4 KiB の DMA の buffer（attach で取り、detach で返す）。admin の command は今の `nvme_admin_execute_mode` で 1 つずつ。
- log: attach の時（`nvme.apst=on` で送った時）に、状態の数と on/off の 1 行、非操作の状態ごとに電力と latency の 1 行、移りごとに 1 行（`nvme: ps3 to ps4 after 2000 ms idle`）。
- 状態を見る口（今の PS・APST の表・link の状態）の UAPI はユーザーの決定の後（§8、Q300-2）。p005 では log だけ。
- 第 3 版の実装（main の上、1f22d22af8）: `pci-nvme-protocol.h` に `drv_nvme_power_parse`（PSD の MP を µW に、MXPS で 0.0001 W と 0.01 W）・`drv_nvme_apst_target`（上の「最初の使える状態」）・
  `drv_nvme_apst_build`（表と、状態ごとの行き先と idle の ms の要約）・`drv_nvme_apst_command`（PRP1 の表は 1 page の中、dword 揃え）。`pci-nvme.c` は probe の Identify Controller の検査の直後に parse、
  HMB の setup の後に `nvme_apst_setup`（`nvme.apst=on` の時だけ: APSTA が 0 なら log「not supported」で何もしない、page を 1 つ取り、非操作の状態ごとに 1 行、`nvme_apst_apply` で送り、
  「autonomous power state transitions on (N states, M transitions)」と移りごとに「psI to psJ after T ms idle」）。resume（`nvme_resume_steps` の HMB の後）と recovery（HMB の後、`recovery_command` 1）で
  `nvme_apst_apply` が送り直す（page を取っていなければ何もしない）。失敗は log だけ。timeout は admin の queue を faulted に残す（HMB と同じ扱い。次の reset で戻る）。

## 5. 状態遷移と順（S0 idle の DEVICES の往復、5330）

```
suspend: NVMe(深さ 3): 要求を止める → HMB を止める → SHN → disable → (core) save → D3hot      … link は PCI-PM L1（L1.2）
         root port(深さ 2、driver 無し): (core) save だけ（D0 のまま）
         VMD(深さ 1): VMCONFIG を保存（No_Soft_Reset が無ければ D0 に残す） → (core) VMD の save → D3hot → _PS3
resume:  VMD: (core) _PS0 → D0 → 復元 → VMCONFIG・bus の始まり・窓・root port の Vendor ID を確かめる
         root port: (core) 復元（bus 番号を失っていれば link を待つ）
         NVMe: (core) D0 → 復元（L1SS・ASPM・LTR を含む） → enable → queue → HMB(MR=1) → APST（`nvme.apst=on` の時）
```

- 他の driver（i915・xHCI など）の位置は今と同じ。VMD は segment 0 の深さ 1 の function として他の深さ 1 の function と並ぶ（bus の順）。

## 6. lock と並行

- VMCONFIG の保存は S0 idle の thread（suspend・resume）だけが触る（BUG-424）。
- config の読み書きは今の `config_lock` の下（`vmd_config_read`・`write`）。待ち（10 ms、link の 1 ms、100 ms）は lock の外の `sched_sleep`・`kern_usleep_range`。
- NVMe の HMB の停止・APST の送りは suspend・resume・probe・recovery の中で、admin の queue を 1 つの流れが使う時（今の HMB と同じ所）。
- root bus を出す function（`drv_pci_bus_set_host_function`、BUG-424）は boot の attach だけが書き、S0 idle の thread が読む。
- ASPM の設定（`pci.vmd.aspm=on` の時）は scan の後に NVMe の config を read-modify-write する。NVMe の driver は LnkCtl・DevCtl2・L1SS・LTR を触らないので衝突しない。
  NVMe の driver がこれらを触るようになる時は、この設定と順を決め直す（R21）。

## 7. 失敗と回復

| どこ | 失敗 | 振る舞い |
| --- | --- | --- |
| VMD の suspend・resume | （BUG-424 の形） | resume で VMCONFIG・bus の始まり・root port が戻らなければ EIO と log。NVMe の resume が失敗すれば disk は quarantine（今の NVMe の振る舞い） |
| NVMe の suspend | HMB の停止の timeout | 今の suspend の失敗と同じ（controller を戻して中止） |
| NVMe の suspend | HMB の停止の拒否 | log、SHN を続ける |
| ASPM の設定 | 読み書きの失敗 | その link の ASPM を切ったまま、log。boot は続ける |
| APST | Set Features の失敗 | log、APST なしで続ける |

## 8. UAPI（状態を見る口）

- この WS の p002〜p005 では UAPI を足さない（ws.md の「UAPI は別の明示の決定」）。観察は log の行（§4.3.4、§4.4、§4.2）。
- 推し（Q300-2）: read-only の sysctl `hw.nvme.<n>.power`（今の PS は Get Features 02h、APST の有効と表、PS ごとの ENLAT・EXLAT）と、
  `hw.pci.link.<addr>`（LnkCtl・L1SS の CTL1・LnkSta）。`/dev/system` は sleep の制御の口で、device の状態を並べる場所ではないため。WS301 の決定があれば揃える。

## 9. 試験

| 段 | 何を | どこで |
| --- | --- | --- |
| host（p003） | HMB の disable の command の組み立て | `plan/ws299/tests/host/nvme-hmb-test.c`（既存） |
| host（p004） | L1SS の値の決め方（5330 の両端の cap → enable・CMRT・T_POWER_ON・閾値 616448 ns）、閾値の符号化の境（1023 と 1024）、ASPM の可否（exit と許容の符号）、L1SS の無い端・LTR の無い端 | （p004 の着手の時に `plan/ws300/tests/host/link-plan-test.c`。第 2 版の branch にある） |
| host（p005） | PSD の parse（MXPS・NOPS・ENLAT・EXLAT）、5330 の BG5 の表（PS0〜PS2 → PS3 100 ms、PS3 → PS4 2000 ms）、15000 µs の境、許容を超える状態・未報告の状態・APSTA 0 | `plan/ws300/tests/host/nvme-apst-test.c`（第 3 版で header の API に合わせて書き直した） |
| host（VMD・PCI の共通） | BUG-424 の `plan/bugs/BUG-424/run.sh`、WS052 の `make -C plan/ws052/tests pci-power` | 既存 |
| build | `config/config.x86_64.mk`（`build/x86_64/vmunix`）と `config/config.vf2.mk`（riscv64、NVMe を持つ）の vmunix が warning 0。arm64 の rpi4・rpi5 は NVMe を持たない（`mk/platform-rpi5/defaults.mk`） | 各 config |
| QEMU | `plan/ws052/tests/p004-guest.sh roundtrip`（QEMU の nvme・xHCI の `sleepctl devices` の往復）。image は `plan/ws300/tests/config-amd64-sleep.mk`（WS052 の config の include を絶対 path にした写し） | `plan/tools/guest/test-image.sh` |
| 5330（p006、Q1 の割当） | `sleepctl devices` の往復 20 回と root の UFS の読み書き、`sleepctl s0idle` が VMD で中止されない、`nvme: suspended in N ms`、`pci.vmd.aspm=on`・`nvme.apst=on` の時の APST・ASPM の log、WS299 の速さの回帰、`5330-run.sh`。BUG-428・BUG-429 の後 | 1 本の script |

- QEMU: QEMU に VMD は無く、QEMU の nvme の APST は Set Features を受けるだけ。p006 で余れば QEMU の nvme に APST の Set Features が通る事だけを見る（受け入れには入れない）。
- 照合する log の文字列: `plan/ws052/tests/p004-guest.sh` と ws052 phase009 が `nvme: suspended`・`nvme: resumed` を部分一致で照合する（R14）。後ろに「in N ms」を足すだけなので追従は要らない。
- QEMU の回帰（R8）: HMB の停止の位置は全部の機械の S0 idle の NVMe を通る。`plan/ws052/tests/p004-guest.sh roundtrip` を p003 の後に流す（第 3 版、結果は phase003）。
  QEMU の nvme は HMB を求めない見込み（HMPRE 0）で、HMB の停止の「使っていない」道と APST を送らない道（`nvme.apst=on` 無し）を通る。
- VMD の port の判断の host 試験（config の模型）は BUG-424 の `host-vmd-power.c` が持つ。

## 10. Phase と受け入れ

| Phase | 内容 | 依存 | 受け入れ |
| --- | --- | --- | --- |
| p001 | この設計と design-reviewer | — | review の指摘を反映 |
| p002 | VMD の suspend・resume、suspend の順 | p001 | **BUG-424 で置き換わった**（main の debec3781b・295eea4623、5330 の devices の往復は BUG-424 の b424d で成功） |
| p003 | NVMe の HMB の停止、時間の log | BUG-424 | build warning 0、QEMU の `p004-guest.sh roundtrip`（p006 で 5330） |
| p004 | ASPM・L1SS・LTR（restore の順は BUG-424 で済み） | BUG-424、Q300-6 | host 試験 PASS、build warning 0（p006 で 5330） |
| p005 | APST、power state の log（UAPI は Q300-2 の後） | p003 | host 試験 PASS、build warning 0（p006 で 5330） |
| p006 | 5330 の受け入れ（ws.md の受け入れ 1〜3） | p004、p005、WS052 p014 | ws.md の受け入れ |
| p007 | 規約の全文の見直し | p006 | 規約の全文、build warning 0、`5330-run.sh` PASS |

第 2 版の p002〜p005 は「実装済み・試験未実施」で branch に残り、merge されなかった。第 3 版は main の上で作り直す（p003・p005、2026-11-15）。
- WS299 との順（R13）: ws.md の「WS299 p003・p004 の後に WS300 の p003 以降」は、HMB の code（WS299）が main に入った後なので、p003（HMB の停止）・p005（APST）はその上に書いた。
  WS299 p005〜p007（UFS の先読み・HMB・乱読み）が `pci-nvme.c` を同時に変えるなら merge の時に衝突しうる（触る関数は suspend・resume・probe の末尾・recovery の末尾）。
- 5330 の観察（p006、WS052 p014 の script と一緒に、R23）: 「vmd: ffff: sleeps in D3hot/D0」「resumed (configuration …, kept)」の行、root port の link の待ちの有無、`nvme: suspended in N ms`・`resumed … in N ms`。
- 既定の切り替え（R2）: p006 で `pci.vmd.aspm=on`・`nvme.apst=on` の 5330 の PASS を確かめた後、既定を on にする（opt-out の語に変える）差分を p006 か p007 に入れる。

## 11. 文書

docs/architecture/power-management.md の「runtime power saving of idle devices」を対応外から外し、NVMe の行（「deepest power state」→ 正規の shutdown と D3hot、
HMB の停止）、VMD の行、APST と ASPM の節を足す。文書の変更はユーザーの review が要る（ws.md）ので、p006 の後に案を置く（この版では変えない）。
新しい boot の語（`pci.vmd.aspm=on`・`nvme.apst=on`。第 2 版の `pci.vmd.d3=force` は BUG-424 の形に無い）は `docs/reference/kernel-boot-parameters.md` に載せる（R17。同じく p006 の後、Q1 が review の要否を決める）。

## 12. 出典

| 値・手順 | 出典 | 確かさ |
| --- | --- | --- |
| NVMe の Identify・PSD・Feature 0Ch・02h・0Dh、§8.9 HMB と shutdown | NVM Express Base Specification 2.0e（survey-fable §1、P5 が読んだ） | 確か |
| PCIe の LnkCap・LnkCtl・DevCap・DevCap2・DevCtl2・LTR・L1SS の bit の位置 | PCI Express Base Specification 4.0 §7.5.3・§7.8.2・§7.8.3（会員だけの文書で、この WS では読めていない）。FreeBSD 15.0 `sys/dev/pci/pcireg.h`（BSD、survey-fable §3 が確かめた分） | L1SS の CTL1 の閾値の位置（25:16・31:29）と CTL2 は記憶。5330 の Debian の CTL1 の値（616448 ns）と cap の値の読みで照合した（§4.3.3） |
| L1SS の enable の順（上流が先）、ASPM を切ってから L1SS を変える、ASPM L1 の enable は上流が先 | PCI Express Base Specification 4.0 §5.5.4（記憶）、Linux の ASPM の手順の散文（survey-fable §3） | 推定（p006 で 5330 の読みで確かめる） |
| 閾値 = CMRT + T_POWER_ON + 6 µs | Linux 6.12 `drivers/pci/pcie/aspm.c`（GPL、値だけ、記憶。版・関数の名前は p007 の監査で 1 つの版を読んで確かめる、R15） | 5330 の Debian の値と一致 |
| FADT の IAPC_BOOT_ARCH（offset 109）の bit 4 PCIe ASPM Controls | ACPI Specification 6.5 Table 5.9・5.11（記憶） | 推定 |
| No_Soft_Reset（PMCSR bit 3）、CRS の Vendor 0001h、reset の後 1.0 s | PCI Bus PM 1.2 §3.2.4、PCIe Base 4.0 §2.3.1・§7.5.3.13（記憶） | 推定 |
| LTR の max の 0x1003 | Linux 6.x の VMD の quirk（GPL、値だけ、survey-fable §4 の散文） | 5330 の Debian の値と一致 |
| APST の ITPT の 2 段（15000 µs 以下 100 ms、それ以外 2000 ms）、許容 100000 µs | 5330 の Debian の `/sys/module/nvme_core/parameters`（survey-5330 §1、Linux 6.19 `drivers/nvme/host/core.c` の parameter） | 値だけ、5330 の表と一致 |
| suspend で HMB を止めてから SHN | FreeBSD の nvme の suspend（BSD、survey-fable §2 の散文）、NVMe Base 2.0e §8.9 | 確か（散文） |
| PCIe の link の訓練の後 100 ms | PCI Express Base Specification §6.6.1（記憶） | 推定 |

code の comment には block ごとに「Value(s) referenced:」「Procedure referenced:」で書く（guardrail の「出典」）。GPL の code・comment・構造は写さない。

## 13. 推しで決めた事（ユーザー不在、Q1 に報告。クリックの問いにしない）

| ID | 問い | 推し（実装した形） |
| --- | --- | --- |
| Q300-1 | S0 idle の NVMe: SHN＋D3hot のままか、最深の非操作の PS＋D0 か、D3cold か | SHN＋D3hot のまま＋HMB の停止（§4.2）。p006 の測りで residency が出なければ再検討 |
| Q300-2 | 状態を見る口の UAPI | 足さない（log だけ）。推しは read-only の sysctl（§8）。ユーザーの決定の後に別の Phase |
| Q300-3 | 隠れた root port・VMD を D3hot にするか | 第 3 版: BUG-424 の形（VMD は No_Soft_Reset が無ければ D0、root port は save・restore だけで D0）。root port の D3hot は p006 の測りの後に要否。第 2 版の推し: No_Soft_Reset が立つ物だけ D3hot、他は D0（R4）、`pci.vmd.d3=force` で D3hot に。Linux の s2idle は bridge を D3 にする（記憶）。Windows の RST の振る舞いは公開の資料が無い（推測を外した、R12） |
| Q300-4 | ASPM・APST を既定で有効にするか | p006 の 5330 の PASS までは既定 off、`pci.vmd.aspm=on`・`nvme.apst=on` で入れる（R2）。PASS の後に既定 on（ユーザーの 2026-10-27 の決定「動作中の省電力も入れる」の実現） |
| Q300-6 | `_OSC` を評価せずに ASPM を OS が設定してよいか | zedBSD は `_OSC` を持たない。FADT の bit だけを見る。Q1 → ユーザーの判断（R1）。既定 off なので判断の前に振る舞いは変わらない |
| Q300-5 | docs の範囲の変更 | p006 の後に案（§11） |

## 14. 未確認

1. VMD・root port の D3hot で隠れた root port の config が失われるか。第 3 版の注: BUG-424 の b424d（5330）で VMD の D3hot の往復は「configuration fe01, kept」、root port の reset は無かった。root port 自身の D3hot は試していない（BUG-424 の形は root port を D0 に置く）。
2. zedBSD の起動の時の隠れた root port の L1SS・LTR・NVMe の LTR の max の値（firmware が設定しないので 0 と見る。p004 の log で分かる）。
3. GNVS の `VDSD`・`STD3`・`VMS0` の値（読まない設計にした、§4.2）。
4. L1SS の CTL1・CTL2 の bit の位置（§12、5330 の値で照合済み、仕様の本文は未読）。
5. Get Features 02h が APST の今の状態を返すか（Q300-2 の口の為、p005 では使わない）。
6. Debian（Linux 6.19）が VMD の quirk（LTR の 0x1003）と L1SS の閾値の計算を走らせたのに ASPM・L1SS の enable を入れなかった理由（R1）: FADT の ASPM の bit、`_OSC`、NVMe の DevCap の L1 の許容、起動の parameter のどれか。
   Debian で読むもの（Q1 の割当、読みだけ）: `dmesg | grep -i aspm`、NVMe の `lspci -vvv` の DevCap の「L1 <x」と PM の「NoSoftRst±」（root port・VMD も）、`acpidump -n FACP` の IAPC_BOOT_ARCH。
7. NVMe の DevCap の L1 の許容 latency（R16）。32 µs 未満なら 5330 では ASPM L1・ASPM L1.2 は入らず PCI-PM L1.x だけになる（S0 idle の D3hot の L1.2 には効く）。
8. 隠れた root port が MSI-X を持つか（R18。持てば save が失敗し D0 で眠る）。
9. 隠れた root port・VMD の No_Soft_Reset（R4）。第 3 版の注: 5330 の VMD（00:0e.0）は `NoSoftRst+`（BUG-424、Debian の lspci）。隠れた root port の値は未確認。

## 15. review の反映（第 1 版 → 第 2 版、2026-10-29 P6）

第 3 版の注: 下の code の列の SHA は merge されなかった branch `agent/p6-ws300` の物（main には無い）。R3〜R6・R18・R20・R22 の VMD の部分の扱いは BUG-424 の形に置き換わった（§4.1）。

| 指摘 | 重さ | 反映 | code |
| --- | --- | --- | --- |
| R1 ASPM の前提 | 重大 | FADT の PCIe ASPM Controls を見る（§4.3.1）。`_OSC` は Q300-6。Debian の読みを §14-6 に（Q1 の割当待ち） | d503c39ea（`drv_acpi_pcie_aspm_forbidden`） |
| R2 未試験の振る舞いを既定 on | 重大 | ASPM・APST を既定 off、`pci.vmd.aspm=on`・`nvme.apst=on` の opt-in。p006 の後に既定 on（§10） | d503c39ea |
| R3 reset の判定 | 中 | 保存した secondary と比べる（実装は第 1 版から）、文を直した（§4.1.4） | d86fe0c5d |
| R4 No_Soft_Reset | 中 | No_Soft_Reset の無い port・VMD は D0（`keep_d0`）。`pci.vmd.d3=force`。port の Vendor ID 0xFFFF は EIO。attach で port ごとに log | d503c39ea |
| R5 下の function の状態 | 中 | 下の全部の function が D3hot の時だけ port を D3hot | d503c39ea |
| R6 link と子の準備 | 中 | 下に function のある port は DLLLA を毎回見る。reset か link が落ちていれば link を待ち、子の Vendor ID（0xFFFF・0x0001 でない）を最大 1000 ms 待つ。VMCONFIG 0xFFFF は直ちに EIO | d503c39ea |
| R7 ASPM の失敗の形 | 中 | 全 1 の読みの端は触らない、T_POWER_ON の予約の scale は L1SS を有効にしない（host 試験に足した）。書きの途中の後始末は採らない（検出の手段が無い） | d503c39ea |
| R8 試験の計画 | 中 | QEMU の `p004-guest.sh roundtrip` を p006 の前に（Q1 の割当）。VMD の port の判断の host 試験は残り（§9） | — |
| R9 restore の順の理由 | 軽 | §4.3.5 に書いた | — |
| R10 HMB・APST の置き場所 | 軽 | §4.2・§4.4 に書いた（実装は第 1 版からそう） | — |
| R11 ITPT の式と出典 | 軽 | 2 段（100 ms・2000 ms、15000 µs）にして 5330 の Debian の表と一致させた | d503c39ea |
| R12 理由の確かさ | 軽 | `VDSD` の条件、Windows の部分を外した（§4.2・Q300-3）。HMB の停止の時間は suspend の ms で p006 に見る | — |
| R13 WS299 と ws.md | 軽 | §10 に順と衝突の範囲。ws.md の「今の事実」の古い行に注を足した | — |
| R14 照合する試験 | 軽 | §9 を直した | — |
| R15 Linux の値の出典 | 軽 | §12 に版・path。1 つの版で読んで確かめるのは p007 の出典の監査で（残り） | — |
| R16 DevCap の許容 | 軽 | §14-7 | — |
| R17 boot の語の文書と名前 | 軽 | 名前を `pci.vmd.*` に。kernel-boot-parameters.md は §11（p006 の後） | d503c39ea |
| R18 port の MSI-X | 軽 | save の失敗は D0 で眠る（S0 idle を止めない）。§14-8 | d503c39ea |
| R19 LTR の max の順 | 軽 | max を書いてから有効に | d503c39ea |
| R20 Slot Control | 軽 | 違う時だけ、bit 10 は今の値のまま | d503c39ea |
| R21 NVMe の driver との RMW | 軽 | §6 に書いた | — |
| R22 driver の有無 | 軽 | suspend の時に確かめ直す | d503c39ea |
| R23 WS052 p014 | 軽 | §10 に観察の項目 | — |

## 16. 第 2 版 → 第 3 版（2026-11-15 P5、q2114-i01）

| 所 | 変えた事 | 理由 |
| --- | --- | --- |
| 版・§0 | 基点を main 5202274642 に。§0 はユーザーの発言の原文をやめ要点に（AGENTS.md §8） | 記録の規則 |
| §1 | BUG-424 の後の PCI の共通・VMD の事実、5330 の b424d の結果、`pci-nvme.c` の今の行 | main が変わった |
| §3.2 | p002 は BUG-424。計算は新しい .c でなく header の inline | 新しい .c は mk の変更（ユーザーの確認）が要る |
| §4.1 | 第 2 版の p002 の設計を削り、BUG-424 との差（root port の D3hot・`pci.vmd.d3=force` は入っていない）だけを残した | p002 は BUG-424 で置き換わった |
| §4.2 | 実装の要点（状態の扱い）、system の shutdown の HMB の気づき | 第 3 版の実装 |
| §4.3 | p004 は手を付けない（Q300-6）、restore の順は BUG-424 で済み | Q1 の指示、main の事実 |
| §4.4 | 実装の要点と log の形 | 第 3 版の実装 |
| §5〜§7・§9〜§11・§13 | VMD の行を BUG-424 の形に。試験の表（host・build の config・QEMU の image） | main の事実 |

