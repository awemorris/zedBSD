<!-- awesome-plan project=zedbsd record=ws333-p027 -->

# ws333-p027: 5330 の残り（package の C-state 0% の原因、i915 の qs40、S0 idle）

Status: in-progress（2026-11-13 Q1、q2032-i01 P3。p028 の watchdog で人手なしに戻れる）
Parent: [WS333](../ws.md)

## 目的

[p014](../phase014/phase.md) の受け入れのうち、[p025](../phase025/phase.md) の後に残った物を確かめる。

1. **package の C-state 0%**: zedBSD は全 CPU が core C6 に約 90% でも PC2 0%（idle 5.4 W）、Debian（graphics の driver 無し）は PC2 99%（3.3 W）。Debian で 1 ms の tick を真似ても PC2 64.6% なので tick は主因でない。残る差は zedBSD が i915 を動かしている事（GT の RC6、display の DC state、`DMC program not loaded`）。i915 の GT RC6 の滞在・forcewake の数を読めるようにし、`i915.start=manual` との A/B で確かめる。原因が i915 なら i915 の WS へ渡す。
2. **i915 の qs40 × 600**（WS291 p022 の mview と model を 5330 の image に置く）。
3. **S0 idle** の回帰と S0 idle の後の要求（`sleepctl s0idle`、起こす事象が要る）。
- tick を止めるか（design.md §4.3.3 段 4、[p023] は未立て）は 1 の数の後に決める。Debian の真似で tick は約 1.1 W。

## 実機の危険

5330 は遠隔の電源が無い。`i915.start=manual`・S0 idle・新しい image は、起動や復帰に失敗すると Q1 も担当も戻せない。

## 結果（2026-11-13 P3、q2032-i01）

### 0. 読めるようにした物（commit 49e9eabc50）

- `hw.gputelemetry` を版 2 に（`include/uapi/sysctl.h`、entry 72 → 88 byte）: `sleep_ns`（GPU の電力を落とした idle の時間、i915 は GT の RC6）・`awake_holds`（driver が起こしたままにしている電力の domain の数、i915 は forcewake の domain）・`awake_wakes`（domain を起こした回数）、valid の bit `GPU_TELEMETRY_SLEEP`・`GPU_TELEMETRY_AWAKE`。`sysctl hw.gputelemetry` が表示、`docs/reference/gpu-sysctl.md` を合わせた。
- i915（`gt-power.c`・`device.c`・`gt.h`・`mmio.c`）: RC6 の滞在の counter（GT_GFX_RC6 138108h、出典は DG1 の PRM Vol. 13 の読める register の表）を、node が forcewake を持つ間だけ読み（GT の domain の中にあり、持たない時は正しく読めない）、32 bit の wrap を越えて 64 bit の ns に足す。park（GT を手放す）の間の時間は起きた時に足し、unpark の log に「parked N ms and in RC6 M ms」。D3hot（S0 idle の suspend）の後は counter が始めからになるので基準だけを置き直す。forcewake の domain を起こした回数を数える。1 count は 1.28 µs（実機で park 70547 ms に対し RC6 70553 ms、142129 ms に対し 142119 ms で合う）。
- 確かめ: kernel（x86_64、THERMAL・X86_POWER_CONTROL）rc 0・warning 0。host 試験 `sh plan/ws333/tests/rc6-host/run.sh build/p027/rc6-host`（i915 の RPS contract に RC6 の検査を足した: domain の無い時は読まない、最初は基準、781250 count = 1 秒、park の間は読まず起きた時に足す、wrap、電力を失った後の基準）ordinary・ASan/UBSan とも 81 checks PASS。`plan/tools/docs-check/check-gpu-uapi-doc.py` は基点の main でも `ValueError: '0660'` で落ちる（この変更の前から、未解決）。

### 1. package の C-state 0% の原因 → 3 つ。i915 の 2 つ（GT が RC6 に入らない）と 1 ms の tick

試験の道具（`plan/ws333/tests/hw5330/`）: `5330-p027.sh`（一回だけの起動、全部 `watchdog=600`、held → `hw.gpu.start=1` で park → parked → QUIET=1 なら Wi-Fi を落とし audiod を止めて quiet → unpark、次の起動で `i915.start=manual`、最後に p4 を戻して sha256 を照合）、試験だけの patch（tree に入れない）: `gt-park.patch`（何も保留していない時の `hw.gpu.start=1` で suspend の worker の park を使って GT を手放す、もう一度で戻す）、`rc6-hw.patch`（RC_CONTROL に HW_ENABLE bit 31・EI_MODE(1) bit 27 を足す）、`tick-100hz.patch`（amd64 の `HAL_TIMER_FREQUENCY` を 100、測りのためだけ。HAL の案ではない）。

5330 の結果（60 秒の窓、package の電力と PC2〜PC7、各 core の C6、GT の RC6、全部 watchdog=600 で ACPI timer は動いている）:

| run | kernel | 状態 | package | PC2 | core C6 | GT RC6 |
| --- | --- | --- | --- | --- | --- | --- |
| p027a | tree＋gt-park（d2df85268869a583） | held（i915 起動のまま） | 5.30 W | 0.0 % | 86.0〜90.4 % | 0.0 %（forcewake 5 domain 握ったまま） |
| p027a | 同 | parked（forcewake 0） | 4.34 W | 0.0 % | 87.9〜92.8 % | park 70661 ms のうち 0 ms |
| p027a | 同 | `i915.start=manual`（GT は firmware のまま） | 3.86 W | 0.0 % | 88.4〜93.7 % | （i915 未起動で読めない） |
| p027b | ＋rc6-hw（8e8dd23484abb4c9） | held | 5.53 W | 0.0 % | 85.5〜90.2 % | 0.0 % |
| p027b | 同 | parked | 4.41 W | 0.0 % | 87.8〜92.9 % | park 70547 ms のうち 70553 ms |
| p027d | 同 | parked / quiet（Wi-Fi 落とし・audiod 停止） | 4.41 / 4.38 W | 0.0 / 0.1 % | 〜93 % | park 141987 ms のうち 141998 ms |
| p027e | ＋rc6-hw＋tick-100hz（073847c6a9e227f6） | held | 5.11 W | 0.0 % | 91.4〜96.5 % | 0.0 % |
| p027e | 同 | parked | 4.06 W | **89.6 %** | 94.0〜98.7 % | park 142129 ms のうち 142119 ms |
| p027e | 同 | quiet | 4.05 W | **95.2 %** | 98.2〜98.9 % | （同じ park） |

Debian の対照（p027c、`build/hw5330/ws333/p027c/`）: 既定（iGPU は vfio-pci で **D3hot**）PC2 98.9 %・3.35 W。ACPI timer を動かしても（`ACPI_TMR_CTL` 0x2 → 0、後で 0x2 に戻した）PC2 99.3 %・3.26 W。iGPU を Linux の i915 に bind（D0、DMC adlp 2.20 loaded、GuC の RC6、DC3→DC5・DC5→DC6 の回数 0）でも PC2 98.7 %・3.22 W。iGPU は vfio-pci（D3hot）に戻した。

原因と根拠:

1. **i915 が forcewake を node の公開の間ずっと握る**: `src/drivers/gpu/i915/device.c` `i915_start_publish()`（「Holds every domain for as long as the node is published」）が全 domain（RENDER・GT・VDBOX0・VDBOX2・VEBOX0）を取り、離すのは suspend の park（`worker.c` `i915_worker_park_here()` → `drv_i915_device_park_gt()`）と stop だけ。forcewake は GT を C6 に入れない（KBL PRM Vol. 2c、IHD-OS-KBL-Vol 2c-1.17、RING_BUFFER_CTL の注記「SW must set the Force Wakeup bit to prevent GT from entering C6 … Force Wakeup bit should be reset to enable C6 entry」）。実機: held は全 run で RC6 0.0 %。
2. **RC_CONTROL が RC6_ENABLE だけ**: `gt-power.c` `drv_i915_rc6_enable()` は `ctl_enable = RC6_ENABLE`（0x40000）を書き、comment は「Without the GuC's RC6 the driver itself enables RC6」。この値は GuC が RC6 を制御する時の物で、GuC の無い driver は HW_ENABLE（bit 31）| EI_MODE(1)（bit 27）| RC6_ENABLE を書く（Linux v6.8 `drivers/gpu/drm/i915/gt/intel_rc6.c` `gen11_rc6_enable()`、tree の `intel/` の register の値と同じ出典の流儀）。RC6_ENABLE だけだと hardware の timer は RC6 に入らない（Linux は park で RC_STATE に target を書いて直に入れる）。実機: forcewake を離しても RC6 0 ms（p027a parked）、HW_ENABLE を足すと ≈100 %（p027b・d・e parked）。
3. **1 ms の tick**: GT が RC6 ≈100 % でも 1000 Hz の tick では PC2 0.0〜0.1 %（p027b・d）、100 Hz にすると 89.6〜95.2 %（p027e）。12th Gen datasheet vol. 1（655258 rev. 011）§4.2.5 の表: PKG C2 は「All processor IA cores in C6 or deeper + Processor Graphic cores in RC6」。core の C6 は 1000 Hz で 86〜93 %、100 Hz で 94〜99 %。tick の位相は p012 で揃えてあるが、それでも 1 ms ごとの起きで package は PC2 に入らない（理由の細部、core の C6 の出入りの時間と package の入りの判定は未解明）。

- DMC・DC state は PC2 の原因ではない（Linux の i915 は DC5/6 の回数 0 でも PC2 98.7 %）。DC state は PC3 より深い状態と display の電力の話で、firmware の license の同意の後に別に測る（下の残り）。
- ACPI timer（watchdog）も PC2 に効かない（Debian の A/B）。
- Wi-Fi の radio と audiod の HDA の stream（`hda: suspended (playback running 1)`、idle でも再生の DMA が走っている）は 1000 Hz で 0.0 → 0.1 %、100 Hz で 89.6 → 95.2 %（PC2 を少し削る）。
- held（forcewake を握ったまま）は 100 Hz でも PC2 0 %。直しの順は 1・2（i915）が先で、3（tick）は 1・2 の後でないと効きが見えない。
- `i915.start=manual`（GT は firmware のまま、D0）は PC2 0 %。Debian の既定は iGPU が D3hot なので、zedBSD の manual とは比べられない（Linux の i915 の D0 との比べが上）。

i915 の WS へ渡す物（i915 の直しはこの WS の範囲の外）: (1) node の公開の間の forcewake の常時の握りをやめ、register に触る間だけ持つ（idle で GT を手放す、park の仕組みは既にある）。(2) GuC を使わない時の RC_CONTROL を HW_ENABLE | EI_MODE(1) | RC6_ENABLE に。直しの効きは `hw.gputelemetry` の `sleep_ns`（RC6 の時間）と `awake_holds` で見える。

### 2. tick を止めるか（p023 の数）

- GT が RC6 の時、1000 Hz → 100 Hz で PC2 0.0 → 89.6 %（Wi-Fi・audiod 込み）、0.1 → 95.2 %（quiet）。package 4.41 → 4.06 W（parked）、4.38 → 4.05 W（quiet）。tick は PC2 の可否を決める（GT を直しても 1 ms の tick のままでは PC2 に入らない）。tickless の idle（design.md §4.3.3 段 4）か tick の間隔を伸ばす事が要る、が数の答え。100 Hz は測りの手段で、HAL の `HAL_TIMER_FREQUENCY` の変更の案ではない（Vulkan の desktop は 1 ms の tick を望む、amd64.h の comment）。
- 4.05 W は Debian の 3.2〜3.35 W より約 0.7〜0.8 W 多い。PC3 以上は zedBSD・Debian（Linux の i915 も）とも 0 %。残りの差は未解明（audiod の stream、USB・NIC の poll、tick の 100 Hz 分など）。

### 3. S0 idle

- 道具: `5330-p027-s0.sh`（試験だけの `s0idle-wake.patch`: 次の S0 idle を CMOS の RTC の alarm（秒が今＋40、分と時は don't care、ACPI の RTC の fixed event）で起こし、TCO の watchdog は眠る間も数える（`watchdog=120`）ので、眠ったまま戻らなければ Debian に戻る）。`sleepctl s0idle` は sessiond を通さず kernel に直に頼む。
- 結果（p027s0b〜d）: **5330 は S0 idle に入らない**。(a) intel-ax211（00:14.3）が `EBUSY`: radio が on の時は設計どおり拒む（`intel-ax211-pci.c` `ax211_pci_suspend()`、sessiond の sleep が networkd に radio を落とさせてから眠る形、`sleepctl` はその道を通らない）。(b) radio を落とす（tree の static の `wifi wlan0 down` と `ifconfig wlan0 down`）と次に **VMD（00:0e.0）が `EBUSY`**: `src/drivers/pci/pci-vmd.c` `vmd_suspend()` は常に EBUSY（「The root ports behind the VMD have no driver to save and restore their bus numbers and windows across D3」）。5330 の NVMe（root の disk）は VMD の裏にある。NVMe・HDA・xHCI は suspend と resume をして戻った（`nvme: resumed epoch=2`、`xhci: resumed`）。
- 眠りに入らないので、RTC の alarm で起きる事・resume の後の要求（GPU の要求、session）は未確認。tick を止める前の S0 idle は VMD の suspend（root port の保存と復元）が先に要る（VMD の WS へ）。

### 4. qs40 × 600

（実行中、下に追記）

### 実機の状態

- 5330 は毎回 Debian（Linux 6.19.13+deb13-amd64）で始まり Debian に戻った。各 run の最後に p4 の vmunix 4c98d617279bf96b・zedbsd.cfg 1b33a395b0ff4bd7 に戻して sha256 を照合した（p027a・b・d・e・s0b・s0c・s0d）。p027s0（最初の S0 の run）は script の誤り（`sudo sh -c` の中の `$HOME` が /root になり、cfg の段で /mnt/zb を mount したまま止まった）で zedBSD を起動せずに失敗し、手で umount して p4 を戻し同じ sha256 を照合した。
- 元の写し: Debian の `~/ws245/vmunix-p027-orig`・`zedbsd.cfg-p027-orig`。試験の kernel の写し `~/ws245/vmunix-p027-*`（消すのは Q1）。
