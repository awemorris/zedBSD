<!-- awesome-plan project=zedbsd record=ws052p002 -->

# ws052-p002: 詳細の調査と HAL の差分の案

Phase ID: `ws052-p002`
Parent: [WS052](../ws.md)
Status: in-progress（2026-10-05 P1 generation17 / q727。HAL の差分の案を置き、5330 の FACP・LPIT・動的な SSDT を読んだ。HAL の差分の承認を待つ。残りは承認の後の p006 の実装だけ）
Phase disposition: normal
Queue: q727（Q1 の投入、ベータ2 の準備）

## 範囲

- HAL の API の差分の案（design §6: MWAIT の idle、tick の停止と再開、AP の停止、wake の割り込みの mask）を `plan/ws052/proposed/` に差分ごとに置く。
  `include/hal/hal.h` は変えない。承認は差分ごと。
- §10 の決定（入れない device は中止して理由を返す、i915・NVMe・xHCI は必須で HDA・Wi-Fi は後（止めれば入れる）、契機は蓋・電源ボタンの短押し・
  無操作の時間）に合わせて p002 以降の Phase の設計を更新する。
- code は書かない（`src/hal` の実装の準備も、承認の後）。

## 結果（2026-10-05）

- [proposed/README.md](../proposed/README.md) と 4 つの差分（どれも `include/hal/hal.h` だけ、今の tree に `patch --dry-run` で当たることを確かめた）:

| # | 差分 | 内容 |
| --- | --- | --- |
| H1 | `hal-cpu-idle-deep.diff` | `int hal_cpu_idle_deep(uint32_t hint)`: 最も深い C-state の idle（hint は `_CST` の FFixedHW の MWAIT の hint、0 で HAL が CPUID leaf 5 から選ぶ）。深い state が無ければ `HAL_ERR_UNSUPPORTED` |
| H2 | `hal-timer-stop.diff` | `int hal_timer_stop(void)`・`int hal_timer_resume(void)`: 今の CPU の periodic の tick の停止と再開。`hal_rtc_read_counter` は数え続ける（無理なら `HAL_ERR_UNSUPPORTED`） |
| H3 | `hal-cpu-notify-wake.diff` | `hal_cpu_notify()` の契約の明記（API の追加なし）: H1 の深い idle・tick の停止中・H4 の mask 中も必ず起こす。AP の停止は新しい API にせず、kernel が各 CPU の idle で H2・H1 を使い notify で起こす |
| H4 | `hal-irq-suspend.diff` | `int hal_irq_suspend(const int *wake_irqs, unsigned count)`・`int hal_irq_resume(void)`: wake の源以外（HAL の内部の LAPIC の LVT、登録の無い I/O APIC の pin を含む）を止め、元の mask を HAL の中に保って戻す |

- **第 2 版（2026-10-05 P4、専門家のレビューの後）**: [proposed/README-v2.md](../proposed/README-v2.md)。H1 → `hal_cpu_idle_suspend_supported()`・`hal_cpu_idle_suspend()`
  （raw の hint を消し、tick の停止と per-CPU の LVT を HAL の内側に。`hal-cpu-idle-suspend.diff`）、H2 は H1v2 に吸収（削除）、H3 → H3v2（契約の文を確定、
  `hal-cpu-notify-wake-v2.diff`）、H4 → `hal_irq_set_wake()`・`hal_irq_suspend()`・`hal_irq_resume()`（wake の IRQ は handler を呼ぶ、latch しない。`hal-irq-wake.diff`）、
  新規 H5 `hal_rtc_read_counter()` の契約の強化（`hal-rtc-counter-idle.diff`）。承認を求めるのはこの 4 つ。第 1 版の file は履歴として残す。
- 当初の案（design §6）の「AP の停止」は、新しい API の代わりに H3 の契約の明記にした（Linux の suspend-to-idle も CPU を offline にせず、各 CPU を
  深い idle に置く）。HAL の API の追加が 3 つ（H1・H2・H4）と契約の明記が 1 つ（H3）になる。
- [design.md](../design.md) の §6 に差分の案、§9 の Phase を §10 の決定に合わせて p002〜p008 に改訂（p004 は必須の 3 device と中止の規則、
  p005 は HDA・Wi-Fi などの「止めて入る」経路、p006 は承認された HAL の差分と入口・出口と `/dev/system`、p007 は Keiland の 3 つの契機）。
  [ws.md](../ws.md) の Phase の表も合わせた。

## 5330 の table の読み（2026-10-05、FACP・LPIT・動的な SSDT の取り出しの後）

取り出しは user の許可（2026-10-04 17 時、queue.md の決定 (2): 5330 の ACPI の table の読み取り専用の取り出しと commit）の範囲で、Q1 の指示により
10.0.30.3 から `sudo cat /sys/firmware/acpi/tables/FACP`・`LPIT`・`dynamic/SSDT16`〜`23`。置き場と sha256 は
[plan/tools/aml-host/latitude5330/README.md](../../tools/aml-host/latitude5330/README.md)。host の設定は変えていない。

| 事柄 | 5330 の値 | 意味 |
| --- | --- | --- |
| FADT の flags | 0x0020C4F5、**Low Power S0 Idle (bit 21) = 1**、Hardware Reduced = 0、SCI 9 | firmware は S0ix を申告している（S3 の代わり）。§3 の未確認が埋まった |
| LPIT の state 0 | entry trigger FFH address **0x60**（MWAIT の hint 0x60 = C10）、residency 30 ms、latency 3 ms、counter FFH **0x632**（MSR `PKG_C10_RESIDENCY`） | H1 の hint は 0x60。CPU 側の深さは package の C10 の residency の MSR で確かめる |
| LPIT の state 1 | 同じ trigger、counter **SystemMemory 0xFE00193C**（32 bit、周波数 0x2005 = 8197 Hz、約 122 µs ごと） | **SLP_S0 の residency の counter**: PMC の PWRM（DSDT の `PWRM`、0xFE000000 の 128 KiB の窓）の offset 0x193C。§8 の確かめ方で読む |
| LPIT の state 2 | disabled | 使わない |
| `_CST`（`Cpu0Cst`、動的） | C1（FFH）、C6/C7（FFH の時は hint `C6MW`/`C7MW`）、最深 `CDTM`（FFH の時は hint `CDMW`、latency `CDLT`）。hint は CPU NVS（`CDMW` など）から実行時に入る | 静的な値ではない。p006 では LPIT の 0x60 を使い、`_CST` は確かめ用（評価には `_PDC`・`_OSC` と動的な Load が要る、WS049） |
| wake の GPE（`_PRW`） | 0x6D: XHCI・TXHC・XDCI・HDAS・CNVW（Wi-Fi）・GLAN・TDM0/1・TXDC、0x69: PCIe の root port（RP01〜・PEG0〜3）とその先、0x72: AWAC（RTC の alarm）、LID0・PBTN: `PPRW`（GPE は Dell の SMI の呼び出し `EEAC (3, 0)` で実行時に決まる、sleep state 3） | p003 の「wake の GPE だけを有効にする」の対象。EC の `_GPE` は 0x6E（eSPI の時） |
| 電源ボタン | `\_SB.PBTN` の `_STA` は `OSYS >= 0x07DF`（`_OSI ("Windows 2015")`）かつ `\_SB.HIDD.BTLD` の時に 0 を返す。その時の電源ボタンは `\_SB.HIDD`（`INTC1070`、Intel の HID event filter）の Notify で来る | **WS132 の電源ボタンの事象に関わる**: zedBSD が Windows 2015 以降の `_OSI` に真を返すと PBTN が消え、HIDD の扱いが要る。p003 で `_OSI` の答えと HIDD の要否を決める（Q1 に報告）→ [p003](../phase003/phase.md) で確かめた: `HIDD.BTLD` を 1 にするのは OS が呼ぶ `HIDD.BTNL` だけで zedBSD は呼ばないので PBTN は present のまま、HIDD の扱いは要らない |

## 人間の判断が要る点

- ~~H1〜H4 の承認（差分ごと）~~ → 第 2 版 H1v2・H3v2・H4v2・H5 の承認（差分ごと、README-v2 §1 の SHA256）。README-v2 §12 の残る判断（`hal_irq_resume` の返り値、HAL が LPIT を読む責務、freeze の置き場）。
- ~~design §10-4: 5330 の FACP・LPIT の取り出しの許可~~ → 2026-10-04 の許可の範囲で取り出した（上）。

## 残り（p002 の中）

- ~~FACP・LPIT の読み、wake の GPE の一覧、`_CST` の hint、SLP_S0 の residency の register~~ → 済み（上）。
- 承認の後: p006 で H1〜H4 の amd64 の実装（他の architecture は `HAL_ERR_UNSUPPORTED`）。

## 確認

- 差分は `include/hal/hal.h` に `patch -p1 --dry-run` で当たる（4 つとも、2026-10-05 の main で）。build は無し（code を書いていない）。
