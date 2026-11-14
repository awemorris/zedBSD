<!-- awesome-plan project=zedbsd record=ws052 -->

# WS052: 電源管理（S0i3、modern standby）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-27 q1615 P1: 保留を解く案。ユーザー「そもそもS0ixが動作した実績がないので、これに対応するWSを作成してやってほしいです。（上記WSでやってほしいです）」を Q1 がこの WS に割り当てた。下の「2026-10-27 の計画の直し」で 5330 で SLP_S0 の residency を出す計画に直した。Status の確定は Q1）
Primary Milestone: MG003
Related Milestones: MG004, MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし（q1615 は計画だけ）
Resume point: p014（5330 の NVMe から起動した zedBSD での基準の UAT）。前提は [WS300](../ws300/ws.md) p002（VMD の suspend・resume、今は `vmd_suspend` が常に EBUSY で S0 idle が必ず中止）。並べて p015（無人で起こす RTC の wake）・p016（residency の読み）は前提なしで進められる。全体の順は [pm-plan.md](../agents/P1/pm-plan.md)
Target: 段は無い（2026-10-27 ユーザー「ベータ3，ベータ4という概念はキャンセルされました。」）。旧 Target: ベータ3
<!-- awesome-plan-history-of-current:
旧 Status: incomplete・保留（ベータ3・4 の段は取り消し、何をやるかは Q1 とユーザーが整理中）（2026-10-27 P3 の精査 q1608、ユーザー「…masterとWSを修正して、completedやcanceledにしてください。」: 2026-10-27 ユーザー「なお、ベータ3，ベータ4という概念はキャンセルされました。…ベータ3でやること、ベータ4でやること、という内容は、今からQ1と私で整理していきます。」。元の段の出典: 2026-10-07 ユーザー「下記をベータ3に移動します。…Sleep」（ws052/ws.md Target））。旧: incomplete（2026-10-05: HAL の差分の案は専門家のレビューを受けて第 2 版 H1v2・H3v2・H4v2・H5（[proposed/README-v2.md](proposed/README-v2.md)）、承認待ち。p003・p004 cleared、p005・p009 は実機の UAT 待…。残り・制限: p001・p002 の HAL の差分の承認、p005〜p012 の実機・QEMU
旧 Resume point: p006（2026-10-05 P1 が HAL・kernel・UAPI を実装（910bc04c・ae2c61bf・58554e8b）。T1 の QEMU（S0IDLE の拒否・DEVICES の往復・拒否の試験）と 5330 の UAT（SLP_S0 の residency、電源ボタン・蓋での起床、counter と RTC）待ち）
旧 Target: **ベータ3**（2026-10-07 ユーザー「下記をベータ3に移動します。・左手デバイスOSK、ゲームパッドOSK, 写真の続き, カレンダーの続き, IMEの続き、POSIX, NVMe, make, RTL8822C, Sleep」）
-->
<!-- awesome-plan-current:end -->

- 2026-10-27 09:39 ユーザー（クリック）: residency を返す UAPI（KERN_SYSTEM_SLEEP_INFO）は可、MSR の counter を読む HAL の口は差分の案を見てから承認。5330 で S0ix を試すのはユーザーが居る時に、事前に Q1 が声をかける。S0ix は WS052 で行う（WS300 には移さない）。

## 目標

ACPI の S0i3（modern standby、S0 low power idle）に入り、wake の event で戻る。操作は `/dev/system` から行う。

## きっかけ

2026-09-24 ユーザー指示: 「電源管理の実装。ACPIのS0i3に対応します。/dev/systemで制御。モダンスタンバイのみで、S3,S4は対応不要。
S0i1, S0i2は必要に応じてサポートを検討するが、基本的にi3のみでよいと思います。」

## 範囲（ユーザーの指示）

- **S0i3 だけ**。S3（suspend to RAM）と S4（hibernate）は対応しない。S0i1・S0i2 は必要になったら検討する。
- 制御は **`/dev/system`**（既にある system の制御 device、`src/drivers/generic/system-device.c`）の ioctl を足す。

## 要るもの（p001 で確かめる）

- ACPI: FADT の `LOW_POWER_S0_IDLE_CAPABLE`、LPS0 の device（`_DSM`、Intel の UUID と Microsoft の UUID: display off/on、entry/exit の通知）、
  各 device の `_PS0`・`_PS3`・`_PRx`・`_DSW`、wake の GPE → **WS049 が前提**。
- Intel の PMC（Alder Lake）: SLP_S0 の residency の確認、PMC の LTR の無視の設定、IP の電源の状態の確認（S0i3 に入れない原因の調べ方）。
- device ごとの suspend/resume: xHCI（USB）、NVMe（APST・D3）、i915（display の電源、DC6/DC9）、HDA、Wi-Fi（RTL8822B など）、有線 LAN、SATA/AHCI の LPM。
- CPU: 全 core の idle を深い C-state（MWAIT の C10 など）にし、timer と割り込みを止める（HAL の idle と timer の口が要るなら承認が要る）。
- wake の源: 電源 button、lid、キーボード（EC の `_Qxx` / GPE）、USB。
- 利用者の操作: `/dev/system` の ioctl（入る、状態、wake の理由）と、それを呼ぶ command。

## 受け入れ

- 対象機で S0i3 に入り（PMC の SLP_S0 の residency が増えることを確かめる）、電源 button か lid で戻り、login した session と network が続く。
- 規約の全文、build、boot test。実機の証拠が中心（QEMU の S0ix は無い）。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| [ws052-p001](phase001/phase.md) | 調査と設計: 対象機の FADT・LPS0 の `_DSM`・device の電源の method、PMC の register、driver ごとの suspend/resume の要否と順序、CPU の idle と timer（HAL の口の要否）、`/dev/system` の ioctl の案 | in-progress（2026-10-04。[design.md](design.md) 第 1 版、レビュー待ち） | WS049 の p001 | 設計文書 |
| [ws052-p002](phase002/phase.md) | 詳細の調査と HAL の差分の案（第 1 版 H1〜H4 → 専門家のレビュー → 第 2 版 H1v2 suspend idle＋probe、H3v2 notify の契約、H4v2 wake の arm と IRQ の suspend、H5 counter の契約）、FACP・LPIT、wake の GPE、`_CST`、PMC | in-progress（2026-10-05: 第 2 版 [README-v2](proposed/README-v2.md) を置いた、承認待ち） | p001 | `proposed/` |
| [ws052-p003](phase003/phase.md) | ACPI の側（LPS0 の `_DSM`、wake の GPE、`_PSx`・`_PRx`・`_DSW`）、`_OSI` と電源ボタンの経路（HIDD は不要） | cleared（2026-10-05 Q1、T1-153） | p001、ws049-p007 | `src/drivers/acpi/acpi-sleep.c`、`acpi-event.c` |
| [ws052-p004](phase004/phase.md) | device の suspend・resume の口、必須の NVMe・xHCI（失敗で中止と理由）、`KERN_SYSTEM_SLEEP` の devices だけの mode。i915 は p009 へ | cleared（2026-10-05 Q1） | p003 | `src/drivers/pci/pci-power.c`、`pci-nvme.c`、`pci-xhci.c` |
| [ws052-p005](phase005/phase.md) | HDA・Wi-Fi・LPSS-I2C の「止めて入る」経路 | in-progress（2026-10-05: 3 つの driver に suspend・resume、T1 の QEMU 待ち） | p004 | `pci-hda.c`、`intel-ax211.c`、`lpss-i2c.c` |
| [ws052-p006](phase006/phase.md) | CPU の idle・tick・割り込み（承認された H1v2・H3v2・H4v2・H5 の amd64 の実装と他 arch の stub）、S0i3 の入口・出口（[README-v2 §7](proposed/README-v2.md) の流れ、spurious wake の再突入、CPU 0 の時刻の補正）、user の停止、`/dev/system` の ioctl と事象 | in-progress（2026-10-05 P1 q742: HAL の実装 910bc04c、kernel の流れ ae2c61bf、UAPI 58554e8b。vmunix の link・sleepctl の build（warning 0）まで。T1 の QEMU と 5330 の UAT 待ち） | p002 の承認（済）、p004、p005、p009、WS132 | |
| [ws052-p007](phase007/phase.md) | Keiland の契機（蓋・電源ボタンの短押し・無操作の時間）と中止の理由の表示、networkd が sleep の前に radio を切る流れ | planning（2026-10-05: 設計の第 1 版、Q1 のレビュー待ち。code は p006 の口の後） | p006、WS132 p008、WS089 | |
| ws052-p008 | 実機の確認（20 回、session と network が続く、residency が増える）と規約の全文 | planned | p007、p014、p018（2026-10-27 の直し） | |
| [ws052-p009](phase009/phase.md) | i915 の suspend・resume（display・DC9・GT の RC6・GGTT・display の core の再初期化・出力の設定の再適用）。設計から、検証は 5330 の UAT | in-progress（2026-10-05: 段 (a)(b)(c) を実装、5330 の UAT 待ち） | p004 | `src/drivers/gpu/i915/park.c`、`worker.c` |
| [ws052-p010](phase010/phase.md) | networkd の SLEEP_PREPARE（opcode 80）・SLEEP_END（81）（p007 の設計 第 4 版 §10） | test-wait（2026-10-07 P2: 実装、build warning 0、host 20/0。QEMU・実機は p011 の後） | p007 の設計 | |
| ws052-p011 | sessiond の sleep の子 process と backend の `POWER suspend`、kernel の CAN_SLEEP の bit（N1 = A1） | test-wait（2026-10-07 P2: kernel の CAN_SLEEP の flag・sessiond の sleep.c・backend、host 13/0・21/0、build warning 0。QEMU・実機は p012 の後） | p010、N1〜N3 | |
| [ws052-p012](phase012/phase.md) | compositor の sleep.c（契機・lock の 2 frame・中止の理由の表示）。電源ボタンは眠らせない（WS182 の口）、外部の出力への切り替えは p004a・p011a の後 | test-wait（2026-10-07 T1-356 QEMU PASS、実機 5330 の UAT） | p011、N5〜N9 | |
| [ws052-p013](phase013/phase.md) | Settings の Power の頁（sleep・画面の時間） | cleared（2026-10-07 T1-358） | p012、N4 | |
| ws052-p014 | 5330 の基準の UAT（NVMe から起動、1 本の script）: `sleepctl devices` の往復（Gears を動かして、p009 の 4.1〜4.4）、`sleepctl s0idle` の入口と出口（起こすのは p015 の RTC、人の操作の蓋・電源ボタンはユーザーの試験の一覧へ）、p005・p006・p009〜p012 の実機の印をまとめて確かめる | planned（2026-10-27 q1615 の案） | WS300 p002（2026-11-15 Q1: BUG-424 の debec3781b・295eea4623 で置き換わり、main にある）、p015 | 実機 5330 |
| ws052-p015 | 無人で起こす wake: ACPI の時刻の wake（AWAC の `_TIV`・`_TIP`、GPE 0x72）か、HAL・kernel の timer の wake、`sleepctl s0idle --wake-after N`（試験の口は QEMU だけの hook にしない、N10） | planned（案） | p006 | `src/drivers/acpi/`、`userland/tests/sleepctl` |
| ws052-p016 | residency の読み: LPIT の 2 つの counter（5330 は state 0 = MSR 0x632 の PKG C10、state 1 = MMIO 0xFE00193C の SLP_S0、8197 Hz）を sleep の前後で読み、`KERN_SYSTEM_SLEEP_INFO`（design §7、UAPI の決定が要る）か sleep の結果の欄で返す。MSR の counter は HAL の口が要るかもしれない（差分の案と承認） | planned（案） | p006 | HAL（案だけ）、`src/kern/sleep.c`、`system-device.c` |
| ws052-p017 | Intel PMC（ADL-P）の診断: S0ix の要件の表と生の状態（どの IP が SLP_S0 を止めているか）、LTR の無視の設定、PCH の device の電源の状態の一覧。読むだけの道具（`sleepctl pmc` など）から | planned（案） | p016 | `src/drivers/platform/`（新）、`userland/tests/sleepctl` |
| ws052-p018 | S0ix の阻害の解消（p017 の表で見つけた物を 1 つずつ、上限付き）: driver の無い PCH の device（Thunderbolt・TCSS、ISH、IPU、GNA、CSME/HECI、Serial IO など）の D3・LTR、i915 の DC9、AX211・HDA の止め方、[WS300](../ws300/ws.md) の NVMe・link の状態 | planned（案、上限 12 LW、超えたら uncleared で理由と再開の条件） | p014、p017 | |

## 人間の判断が要る点

- （済）HAL（CPU の idle、timer の停止、割り込みの wake）の差分の承認: 2026-10-05 ユーザー「hal.hの追加をレビューしました。OKです。…HAL実装に進むことを承認します。」（[p006](phase006/phase.md):13、hal.h に 29f954f5 で適用）。
  p001・p002 の Status の行と Master の再開点の「p001・p002 の HAL の差分の承認」は古い（Q1 が照合して p002 の clearance を判定する）。
- 2026-10-27 の計画の直しで要る判断（下の節の D1〜D5）。

## 2026-10-04 予定（Q1）

ユーザー「次の新規実装項目は、USB-C の DisplayPort Alternate Modeの実現を目標にします。その次が電源管理です。これらは併走できると思います。共通のpredecessorがAMLですね。」→ [queue.md](../queue.md) の q679（WS050 p001）・q680（WS051 p001）・q681（WS052 p001）。設計は WS049 の q677（BUG-165、DSDT）と並走、実装は q677・q678（ws049-p007）の後。

- 2026-10-05 ユーザー「/dev/system から S0 idle に入るための UAPIの変更を承認します。docs/に電源管理のドキュメントを作成して、記録しておいてください。あとでレビュー対象にできるようにです。」→ P1 の案（`KERN_SYSTEM_SLEEP_S0IDLE`、`wake` の欄、`KERN_SYSTEM_WAKE_*`）を承認。Q1 が `docs/architecture/power-management.md` に設計と UAPI を書いた（ユーザーの review 待ち）。p006 の実装はこの文書に従う。

## 2026-10-27 の計画の直し（q1615、P1）

ユーザー（2026-10-27）「そもそもS0ixが動作した実績がないので、これに対応するWSを作成してやってほしいです。（上記WSでやってほしいです）」。Q1 がこの WS に割り当て、保留を解く（Q1 の確定）。
全体の依存は [pm-plan.md](../agents/P1/pm-plan.md)。

### 今の事実（2026-10-27 の調べ）

- 実装は source にある: HAL（`include/hal/hal.h` の `hal_cpu_idle_suspend*`・`hal_irq_set_wake`/`suspend`/`resume`、`src/hal/amd64/bsp-pcat/idle-suspend.c`、LPIT は `bsp-pcat/acpi.c:800`）、
  kernel（`src/kern/sleep.c` 554 行、`freeze.c`、`clock.c` の補正）、ACPI（`acpi-sleep.c` の LPS0 の `_DSM`・`_PSx`・`_DSW`、`acpi-event.c` の wake の GPE）、
  PCI（`pci-power.c` の suspend・resume の全体）、UAPI（`include/uapi/system.h` の `KERN_SYSTEM_SLEEP`・`SLEEP_DEVICES`/`S0IDLE`・`WAKE_*`・`CAN_SLEEP`）、userland（sessiond・networkd・compositor・Settings・`sleepctl`）。
- **5330 の実機の UAT は一度も記録が無い**（p005・p006・p009〜p012）。QEMU は T1-178（拒否と devices の往復）・T1-356 で PASS。
- 新しい阻害: WS298（2026-10-27）で 5330 の NVMe は VMD の下になり、`vmd_suspend` が常に EBUSY（`src/drivers/pci/pci-vmd.c:422-435`）。NVMe から起動した 5330 では S0 idle は必ず中止 → [WS300](../ws300/ws.md) p002 が前提。
- 受け入れの「SLP_S0 の residency が増える」を確かめる code が無い（0xFE00193C・MSR 0x632 を読む所が無い、`KERN_SYSTEM_SLEEP_INFO` も無い）→ p016。
- 無人で起こす口が無い（RTC・timer の wake は未実装、phase006:485）。5330 には遠隔の電源が無く、起きないと人が要る → p015 を先に。
- `drv_pci_device_set_wake` を呼ぶ所が無く、PME・USB の remote wake は arm されない（キーボードでの起床は未対応、受け入れは電源ボタン・蓋）。
- 5330 の USB の stick は 2026-10-27 に挿さっていない（USB から起動して VMD を避ける回り道も今は使えない。USB の root で xHCI を suspend するのも危うい）。

### 直した順（案）

1. p015（無人の wake）・p016（residency の読み）を先に。どちらも 5330 を使わず host・QEMU で進められる（QEMU の S0 idle は EOPNOTSUPP なので、wake の timer は devices の往復で確かめる）。
2. [WS300](../ws300/ws.md) p002（VMD の suspend・resume）の後に p014（5330 の基準の UAT）。ここで初めて 5330 の s0idle に入る。起きない時は Q1 → ユーザー（電源ボタン）。
3. p017（PMC の診断）で residency が 0 の理由を出し、p018 で 1 つずつ外す。WS300 の p003〜p005（NVMe の状態・ASPM・APST）が p018 の候補の一部。
4. p007（Keiland の契機）は設計が済み、p010〜p012 の実装は済み（test-wait）。p014 で実機の印をまとめて付ける。p008（20 回と規約の全文）が最後。

### 判断が要る点

- D1: `KERN_SYSTEM_SLEEP_INFO`（residency の読み）の UAPI。2026-10-05 の承認は `SLEEP_S0IDLE`・`wake`・`WAKE_*` で、SLEEP_INFO は含まれていない（P1 の読み）。ユーザーの明示の決定が要る。
- D2: MSR の counter（PKG C10、LPIT の FFH）を読む HAL の口（hal.h の差分、承認）。MMIO の SLP_S0 の counter だけなら driver の側で足りる。
- D3: 試験のための RTC の wake を ACPI の AWAC（5330 の GPE 0x72）でやるか、HAL の timer でやるか（p015 の設計）。
- D4: p018 の上限（12 LW）と、5330 で起きなくなった時の扱い（ユーザーに電源ボタンを頼む頻度）。
- D5: `docs/architecture/power-management.md`（ユーザーの review 待ち、2026-10-05）を p016・p017 と WS300 に合わせて直す時期。

### 見積もり（LW、±50%）

p014 3、p015 4、p016 4、p017 5、p018 12（上限）、p008 3。既存の test-wait の Phase の実機の印は p014 に含める。合計 約 31 LW（WS300 p002 の 6 LW は WS300 の側）。
