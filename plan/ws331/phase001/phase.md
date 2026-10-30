<!-- awesome-plan project=zedbsd record=ws331-p001 -->

# ws331-p001: driver の HAL の利用の一覧と、kernel の置き換え先

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1713（P2）

## 目的

ユーザーの指示（ws.md の原文）の「違反利用された関数が、どの関数に置き換えられるか、一覧」と「ラッパーがない場合…その一覧」を作る。code は変えない。

## 成果（この phase.md に書く）

1. 違反の全数: `src/drivers/`・`include/drivers/` の HAL の header の include（file・行・header）と、`hal_*` の呼び出し（関数・file・行の数）。HAL の型・定数（`HAL_*`・`hal_*_t` の類）だけを使う所も分けて数える。試験の file（`tests/` の下）は分けて示す。
2. 表 A（置き換え先がある物）: `hal_*` の関数 → kernel の既存の関数（`include/kern/` の宣言の file と行）。意味・引数・失敗の値の違いがあれば書く（例: `hal_space_map_device` → `kern_device_map`、`hal_get_arch_handoff` → `kern_boot_handoff`）。
3. 表 B（wrapper が無い物）: kernel に足す関数の案（名前・宣言・意味・置く file・どの driver が使うか）。HAL の型・定数を driver が使っている所は、kernel の型・定数の案も。
4. 例外の候補: arch・board に固有の HAL の header（i386.h・bsp-pc98/display.h・bsp-rpi-common/cons.h）を使う所は、何を使っているかと、kernel の口にするか driver の中に移すかの案。
5. 書き換えの Phase の分け方の案（driver の群ごと、依存の順）。

## 確かめ

- 数は grep の出力から自分で数え、使った command を書く。

---

## 結果（2026-10-30、P2、worktree `agent/p2-ws331`、基点 main b5a703275）

### 0. 使った command

worktree の root（`/home/awe/zedBSD-worktrees/p2-ws331`）で流した。

```sh
# include（行と file の数）
grep -rnE '#\s*include\s*[<"]hal/' src/drivers include/drivers
grep -rnE '#\s*include\s*[<"]hal/' src/drivers include/drivers | wc -l      # 41
grep -rlE '#\s*include\s*[<"]hal/' src/drivers include/drivers | wc -l      # 38
# hal_ と HAL_ の識別子の全部（comment・文字列を含む）
grep -rnE '\bhal_[a-z0-9_]+\b|\bHAL_[A-Z0-9_]+\b' src/drivers include/drivers | grep -vE '#\s*include'
# kernel の口と、HAL を include する kernel の header
grep -nE 'hal' include/kern/*.h
grep -n 'kern_' include/kern/device-io.h include/kern/irq.h include/kern/pmem.h include/kern/platform.h include/kern/clock.h include/kern/panic.h
# 各 port の HAL の実装の有無
grep -rlE '^hal_irq_alloc_msi\(' src/hal     # attach・detach_msi_sync・free・cpu_current・putc も同じ形
```

関数ごと・file ごとの数は、C の comment（`/* */`・`//`）と文字列を空にしてから数える短い Python（scratchpad の `calls.py`・`halsyms.py`・`comments.py`、git に入れない）で出した。
呼び出しは `\bhal_[a-z0-9_]+\s*\(`（行の先頭から始まる物は関数の定義として別に数える）。型・定数は `\b(hal_[a-z0-9_]+_t|paddr_t|HAL_[A-Z0-9_]+|hal_pc98_[a-z0-9_]+)\b`（`#if` の行も含む）。`halsyms.py` は HAL の header（hal.h・types.h・arch.h・atomic.h・arch/*.h）が宣言する名前を file ごとに数え、include だけで何も使わない file を見つける。

試験の file は `src/drivers/gpu/i915/tests/` の下だけ（他の driver の `tests/` に HAL の利用は無い）。

### 1. 違反の全数

#### 1.1 include（41 行・38 file。`include/drivers/` は 0）

| 区分 | file | 行 | header | その file が使う HAL の名前 |
| --- | --- | --- | --- | --- |
| 本体 | src/drivers/acpi/acpi-kern.c | 23 | hal/hal.h | space_map/unmap_device・io_inp/outp 8/16/32・mmio_read/write 8〜64・HAL_OK・HAL_SPACE_*・hal_physaddr_t |
| 本体 | src/drivers/typec/typec-kern.c | 18 | hal/hal.h | space_map/unmap_device・mmio_read8/write8・HAL_OK・HAL_SPACE_*・hal_physaddr_t |
| 本体 | src/drivers/serial/ns16550.c | 45 | hal/hal.h | irq_disable・irq_enable |
| 本体 | src/drivers/generic/serial-console.c | 26 | hal/hal.h | putc |
| 本体 | src/drivers/gpu/gpu.c | 37 | hal/hal.h | rtc_read_counter |
| 本体 | src/drivers/gpu/powervr/render-queue.c | 47 | hal/hal.h | rtc_read_counter |
| 本体 | src/drivers/gpu/powervr/display/dc8200.c | 40 | hal/hal.h | get_arch_handoff・cpu_current |
| 本体 | src/drivers/gpu/i915/i915.c | 36 | hal/hal.h | get_arch_handoff |
| 本体 | src/drivers/gpu/i915/irq.c | 33 | hal/hal.h | irq_attach_msi・irq_detach_msi_sync・irq_send_eoi・hal_irq_ack_t・HAL_OK |
| 本体 | src/drivers/gpu/i915/pci.c | 22 | hal/hal.h | irq_alloc_msi・irq_free_msi・paddr_t・HAL_OK |
| 本体 | src/drivers/gpu/i915/worker.c | 44 | hal/hal.h | cpu_current |
| 本体 | src/drivers/gpu/i915/device.c | 39 | hal/hal.h | space_map/unmap_device（WC）・HAL_OK |
| 本体 | src/drivers/gpu/i915/display/bootfb.c | 66 | hal/hal.h | space_map/unmap_device（WC）・HAL_OK |
| 本体 | src/drivers/gpu/i915/display/vbt-shell.c | 28 | hal/hal.h | space_map/unmap_device（READ だけ）・HAL_OK |
| 本体 | src/drivers/gpu/i915/display/xd-display.c | 21 | hal/hal.h | space_map/unmap_device（READ だけ）・HAL_OK |
| 本体 | src/drivers/gpu/i915/display/xd9-firmware.c | 24 | hal/hal.h | space_map/unmap_device（READ だけ）・HAL_OK |
| 本体 | src/drivers/gpu/i915/display/display.c | 66 | hal/hal.h | （無し。include だけ） |
| 本体 | src/drivers/platform/pcat/graphics/backend.c | 18 | hal/hal.h | （無し） |
| 本体 | src/drivers/platform/pcat/graphics/text.c | 20 | hal/hal.h | cpu_current |
| 本体 | src/drivers/platform/pcat/graphics/splash.c | 39 | hal/hal.h | rtc_read_counter |
| 本体 | src/drivers/platform/pc98/pc98-lgy98.c | 19 | hal/hal.h | （無し） |
| 本体 | src/drivers/platform/pc98/pc98-lgy98.c | 21 | "hal/i386/i386.h" | asm_inb・asm_outb・asm_inw・asm_outw（§4） |
| 本体 | src/drivers/platform/pc98/pc98-busmouse.c | 26 | hal/hal.h | （無し） |
| 本体 | src/drivers/platform/pc98/pc98-ide.c | 24 | hal/hal.h | （無し） |
| 本体 | src/drivers/platform/pc98/graphics/pc98-graphics.c | 23 | hal/hal.h | （無し） |
| 本体 | src/drivers/platform/pc98/graphics/backend.c | 16 | "hal/i386/bsp-pc98/display.h" | pc98_display_graphics_start・_stop（§4） |
| 本体 | src/drivers/platform/pc98/graphics/backend.c | 19 | hal/hal.h | （無し） |
| 本体 | src/drivers/platform/pc98/graphics/text.c | 23 | hal/hal.h | （無し。HAL の表は自分の extern で参照、§1.3） |
| 本体 | src/drivers/platform/rpi4/rpi4-console.c | 32 | hal/hal.h | putc |
| 本体 | src/drivers/platform/rpi4/rpi4-console.c | 34 | "hal/arm64/bsp-rpi-common/cons.h" | rpi_cons_suspend_output・rpi_cons_resume_output（§4） |
| 本体 | src/drivers/platform/x68k/x68k-console.c | 26 | hal/hal.h | putc |
| 本体 | src/drivers/platform/sun4u/sun4u-console.c | 36 | hal/hal.h | get_arch_handoff・putc・irq_disable・irq_enable |
| 本体 | src/drivers/platform/sun4u/sun4u-firmware-fb.c | 33 | hal/hal.h | get_arch_handoff |
| 本体 | src/drivers/platform/zybo/zynq-devcfg.c | 52 | hal/hal.h | get_arch_handoff・HAL_FATAL |
| 本体 | src/drivers/platform/zybo/zynq-ethernet.c | 33 | hal/hal.h | get_arch_handoff・hal_physaddr_t |
| 本体 | src/drivers/platform/s905x5m/s905x5m-soc.c | 24 | hal/hal.h | hal_physaddr_t だけ（pmem.h からも来る） |
| 本体 | src/drivers/platform/s905x5m/s905x5m-mmc.c | 28 | hal/hal.h | （無し） |
| 試験 | src/drivers/gpu/i915/tests/execution/ktest-sync.c | 34 | hal/hal.h | irq_*_msi・space_map/unmap_device・cpu_current・HAL_ERR_*・HAL_SPACE_*・paddr_t |
| 試験 | src/drivers/gpu/i915/tests/execution/ktest-gt.c | 54 | hal/hal.h | （無し） |
| 試験 | src/drivers/gpu/i915/tests/contracts/host_unreached.c | 25 | hal/hal.h | hal_irq_alloc_msi・hal_irq_free_msi の host の代役の定義・paddr_t |
| 試験 | src/drivers/gpu/i915/tests/display/host-kernel.c | 45 | hal/hal.h | hal_space_map/unmap_device の host の代役の定義 |

- 本体 37 行・34 file、試験 4 行・4 file。
- arch・board に固有の header は 3 行・3 file（i386.h・bsp-pc98/display.h・bsp-rpi-common/cons.h）。
- **include だけで HAL の名前を何も使わない file**（include を消すだけで済む見込み。他の header に頼っていないかは build で確かめる）: i915/display/display.c、pcat/graphics/backend.c、pc98-busmouse.c、pc98-ide.c、pc98/graphics/pc98-graphics.c、pc98/graphics/text.c、s905x5m-mmc.c、試験の ktest-gt.c の 8 file。pc98-lgy98.c・pc98/graphics/backend.c の `hal/hal.h` も使っていない（固有の header だけ使う）。

#### 1.2 `hal_*` の呼び出し（comment・文字列を除く）

合計 **105 回**（本体 80・試験 25）、関数は 27 種。これとは別に、試験の host の代役が `hal_*` の関数を **4 個定義**している（host_unreached.c:261 `hal_irq_alloc_msi`、:280 `hal_irq_free_msi`、host-kernel.c:1739 `hal_space_map_device`、:1761 `hal_space_unmap_device`）。

| 関数 | 本体 | 試験 | 本体の file:行 |
| --- | --- | --- | --- |
| hal_space_map_device | 9 | 4 | acpi-kern.c:731,922,1111、i915/device.c:1551、i915/display/bootfb.c:579、vbt-shell.c:525、xd-display.c:389、xd9-firmware.c:118、typec-kern.c:210 |
| hal_space_unmap_device | 10 | 1 | acpi-kern.c:750,757,930,1117、i915/device.c:2082、bootfb.c:389、vbt-shell.c:561、xd-display.c:410、xd9-firmware.c:127、typec-kern.c:241 |
| hal_get_arch_handoff | 10 | 0 | i915/i915.c:514、powervr/display/dc8200.c:653,1546、sun4u-console.c:220,264,535、sun4u-firmware-fb.c:492、zynq-devcfg.c:276、zynq-ethernet.c:467,515 |
| hal_rtc_read_counter | 7 | 0 | gpu/gpu.c:1836,1846、powervr/render-queue.c:650,1603,3342,3353、pcat/graphics/splash.c:289 |
| hal_putc | 4 | 0 | generic/serial-console.c:136、rpi4-console.c:214、sun4u-console.c:309、x68k-console.c:97 |
| hal_cpu_current | 3 | 3 | i915/worker.c:418、dc8200.c:1174、pcat/graphics/text.c:987 |
| hal_irq_alloc_msi | 1 | 5 | i915/pci.c:788 |
| hal_irq_free_msi | 1 | 7 | i915/pci.c:818 |
| hal_irq_attach_msi | 1 | 2 | i915/irq.c:310 |
| hal_irq_detach_msi_sync | 1 | 3 | i915/irq.c:371 |
| hal_irq_send_eoi | 3 | 0 | i915/irq.c:1313,1325,1334 |
| hal_irq_disable | 2 | 0 | sun4u-console.c:662、serial/ns16550.c:298 |
| hal_irq_enable | 2 | 0 | sun4u-console.c:686、ns16550.c:334 |
| hal_io_inp8 / 16 / 32 | 2 / 2 / 4 | 0 | acpi-kern.c:470,1156 / 473,1165 / 476,1174,1184,1185 |
| hal_io_outp8 / 16 / 32 | 2 / 2 / 4 | 0 | acpi-kern.c:503,1154 / 506,1163 / 509,1172,1181,1182 |
| hal_mmio_read8 / write8 | 2 / 2 | 0 | acpi-kern.c:1042 / 1040、typec-kern.c:256 / 271 |
| hal_mmio_read16・32・64 / write16・32・64 | 各 1 | 0 | acpi-kern.c:1051,1060,1069 / 1049,1058,1067 |

試験の呼び出しは全部 `src/drivers/gpu/i915/tests/execution/ktest-sync.c`（MSI 602〜689、WC の map 702〜808、cpu_current 333,557,1315）。

#### 1.3 HAL の型・定数・data（呼び出しとは別に数える）

| 名前 | 本体 | 試験 | 場所（本体） | 備考 |
| --- | --- | --- | --- | --- |
| `HAL_OK` | 14 | 15 | acpi-kern.c 3、typec-kern.c 2、i915 の device 1・bootfb 2・vbt-shell 1・xd-display 1・xd9-firmware 1・irq.c 2・pci.c 1 | map と MSI の戻り値の比較 |
| `HAL_ERR_INVALID`・`HAL_ERR_STATE` | 0 | 6・1 | — | ktest-sync.c の HAL の契約の試験 |
| `HAL_SPACE_READ`・`WRITE`・`NOCACHE`・`WC`・`DEVICE` | 9・5・2・2・0 | 4・0・1・4・1 | map の属性（§2 の kern_device_map の行） | |
| `HAL_FATAL` | 1 | 0 | zynq-devcfg.c:384 | |
| `hal_irq_ack_t` | 4 | 2 | i915/irq.c:244,245,1294,1341 | handler の引数の型 |
| `paddr_t`（`include/hal/types.h:14`、= `hal_physaddr_t`） | 1 | 5 | i915/pci.c:768 | MSI の message address |
| `hal_physaddr_t` | 26（23 file） | 4（3 file） | 多くは `kern_pmem_to_kernel((hal_physaddr_t)x)` の cast（rpi4-bt/pcie/wifi、rpi5-bt/pcie/wifi・rp1、s905x5m-soc、zynq-ethernet、bcm2711 attach/attach71、mali-attach、powervr/firmware-boot 2、i915/ppgtt、vf2-dmatest）、`struct kern_pmem` の `paddr` への代入（camera/cam.c:1115）、hal_space_map_device の引数（acpi 3、typec 1、i915 5） | **HAL の header を include しない file でも使える**: `include/kern/pmem.h:13` が `<hal/hal.h>` を include し、`kern_pmem_to_kernel(hal_physaddr_t)`（pmem.h:65）・`struct kern_pmem.paddr`（pmem.h:24）が HAL の型を持つため。試験の 4 は host の代役の `kern_pmem_to_kernel` の定義の引数ほか |
| `hal_pc98_jisx0208_to_ucs`（data の表） | 4（2 file） | 0 | pc98/graphics/display-glyph.c:17（extern）,54、pc98/graphics/text.c:68（extern）,808 | 定義は `src/hal/i386/bsp-pc98/jisx0208.c`（platform/pc98/vmunix.mk:31,774 で build）。HAL の中に他の利用者は無い（`grep -rn hal_pc98_jisx0208_to_ucs src/hal` は定義の file だけ） |
| build の macro `HAL_ARCH_AMD64`・`HAL_BOARD_RPI4`・`HAL_BOARD_RPI5`・`HAL_BOARD_ODROID_C5`（`#if`） | 6 file・16 行 | 0 | overlayfs.c:159,444、amdrdna/node.c:808、nvrtx/node.c:983、pci-xhci.c:1440,1450,1479,1493,1504,1546（RPI4 と RPI5 を同じ行で）、rpi4-console.c:47,61,77,79,86、rpi4-sdhci.c:60 | HAL の header の名前ではなく `-D` の build の macro（platform/*/vmunix.mk・Makefile:1072）。§6 の未決 |

#### 1.4 comment・文字列の中の `hal_*`（code の変更に合わせて直す物）

14 か所（comment 9・文字列 5）: serial-console.c:11、rpi4-console.c:12、sun4u-console.c:22、x68k-console.c:11（hal_putc）、render-queue.c:156,1591（hal_rtc_read_counter）、intel-gpio.c:34（hal_irq_set_mode。code は既に kern_irq_set_mode）、i915/irq.c:316（log の文字列）、試験の host-kernel.c:1736,1751,1758,1769・host_unreached.c:273,286。
語の `HAL` は driver の 37 file・83 行にある（`grep -rnwE 'HAL' src/drivers include/drivers`）。pc98 の graphics には `drv_pc98_*_make_hal`・`backend_hal`・`gdc_hal` など `hal` を含む driver の中の名前が 9 file にある（HAL の API ではない）。書き換えの Phase で関係の所を直し、残りは規約の見直しの Phase で見る。

### 2. 表 A: kernel に既に置き換え先がある物

宣言の行は `include/kern/`、実装は `src/kern/`。「中身」は実装を読んで確かめた。

| HAL（driver が使う物） | kernel の置き換え先（宣言） | 中身 | 意味・引数・失敗の値の違い |
| --- | --- | --- | --- |
| `hal_space_map_device(paddr, size, attr, &va)` | `kern_device_map(uint64_t address, size_t size, unsigned attributes, void **mapped)`（pmem.h:74、実装 pmem.c:145） | wrapper で attr を作り直す: 常に `HAL_SPACE_READ\|WRITE`、`KERN_DEVICE_WRITETHROUGH` なら `WRITETHRU`、他は `NOCACHE` | (1) 戻り値は errno（0・EINVAL・ENOMEM・EBUSY・ENOTSUP・他は EIO、pmem.c:34 `pmem_error`）。`!= HAL_OK` は `!= 0` に。(2) 属性は `KERN_DEVICE_UNCACHED`（0）・`KERN_DEVICE_WRITETHROUGH`（1）だけ（pmem.h:40-41）で、**WC と読み出しだけの map を表せない**（表 B の B1）。(3) address は `uint64_t`（cast が要らない）。(4) acpi-kern.c:731（FACS、`READ\|WRITE` で cache の指定無し）は UNCACHED になる。amd64 の HAL は WC 以外の device の map を全部 cache 無しにする（src/hal/amd64/space.c:372 の comment）ので、amd64 では動きが同じ。ACPI・typec は amd64 だけで build（platform/amd64/vmunix.mk:361,366）。 |
| `hal_space_unmap_device(va, size)` | `kern_device_unmap(void *mapped, size_t size)`（pmem.h:76、pmem.c:169） | 単なる wrapper（errno に直すだけ） | 戻り値が errno |
| `hal_get_arch_handoff(name)` | `kern_boot_handoff(const char *name)`（platform.h:79、pmem.c:217） | 単なる wrapper | 無し（無い名前は NULL）。driver で既に 13 か所が使う |
| `hal_rtc_read_counter(&c, &f)` | `kern_rtc_read_counter(uint64_t *counter, uint64_t *frequency_hz)`（clock.h:194、pmem.c:308） | 単なる wrapper | 無し |
| `hal_irq_disable()` | `kern_irq_disable(void)`（irq.h:97、irq.c:202） | 単なる wrapper | 無し（bool、前の状態） |
| `hal_irq_enable()` | `kern_irq_enable(void)`（irq.h:98、irq.c:213） | 単なる wrapper | 無し |
| `hal_irq_send_eoi(ack)` | `kern_irq_send_eoi(kern_irq_ack_t)`（irq.h:89、irq.c:192） | wrapper（型の cast だけ） | 型が `kern_irq_ack_t` |
| `hal_irq_ack_t`（handler の引数の型） | `kern_irq_ack_t`（irq.h:25）、handler の型 `kern_irq_handler_t`（irq.h:35） | 同じ表現（uintptr_t、irq.c の先頭の comment） | 無し |
| `hal_io_inp8/16/32`・`hal_io_outp8/16/32` | `kern_io_in8/16/32`・`kern_io_out8/16/32`（device-io.h:29-34、device-io.c:24-70） | 単なる wrapper | 名前だけ（`inp`→`in`、`outp`→`out`） |
| `hal_mmio_read8/16/32/64`・`hal_mmio_write8/16/32/64` | `kern_mmio_read8/16/32/64`・`kern_mmio_write8/16/32/64`（device-io.h:42-49、device-io.c:75-138） | 単なる wrapper | 無し |
| `HAL_FATAL(msg)`（`hal_fatal`） | `KERN_FATAL(message)`（panic.h:24。`kern_fatal` panic.h:21、panic.c:88） | wrapper ではない: 割り込みを止め、log の ring に記録し、console を出してから `hal_fatal` | 止まる前に記録と表示が増える |
| `HAL_OK` | `0`（kernel の口は errno） | — | — |
| `kern_pmem_to_kernel((hal_physaddr_t)x)` の cast | `kern_pmem_to_kernel`（pmem.h:65、pmem.c:134、単なる wrapper） | — | 引数の型が `hal_physaddr_t` のまま。型は表 B の B6 |

i386.h の `asm_inb/outb/inw/outw`（pc98-lgy98.c の 10 回: 80,167,181,194,210,223,235,236,240,241）も `kern_io_in8/out8/in16/out16` に置き換わる（§4）。

### 3. 表 B: kernel に足す物（ユーザーが決める一覧）

**関数 6 個（B2〜B7）、型 1 個（B8）、定数 2 個（B1）**。HAL の API（`include/hal/hal.h`）は変えない。どれも既存の `hal_*` を呼ぶ kernel の wrapper で、呼ぶ HAL の関数は全部の port に実装がある（`grep -rlE '^hal_irq_alloc_msi\(' src/hal` などで、MSI の 4 関数は 12 の irq.c、hal_cpu_current は 11、hal_putc は 13 の file を確かめた）。

| # | 足す物（名前・宣言） | 意味 | 置く file（案） | 使う driver |
| --- | --- | --- | --- | --- |
| B1 | **定数** `KERN_DEVICE_WRITECOMBINE`（2U、cache の方針の値）と flag `KERN_DEVICE_READONLY`（0x100U）。`kern_device_map` の宣言は変えず、実装で `HAL_SPACE_WC` と READ だけ（WRITE を付けない）に直す | write-combining の map と、読み出しだけの map | include/kern/pmem.h（40-41 行の後）、src/kern/pmem.c の kern_device_map | WC: i915/device.c:1551（GMADR の aperture）、i915/display/bootfb.c:579。READ だけ: acpi-kern.c:922、vbt-shell.c:525、xd-display.c:389、xd9-firmware.c:118 |
| B2 | `int kern_irq_alloc_msi(const char *source, int *mapped_irq, uint64_t *mapped_address, uint32_t *mapped_event);` | handler を付けずに MSI の vector・routing・message を確保（hal_irq_alloc_msi と同じ契約。address は kern_irq_register_msi と同じく uint64_t） | include/kern/irq.h（56 行の後）、src/kern/irq.c | i915/pci.c:788（試験 ktest-sync.c） |
| B3 | `int kern_irq_attach_msi(int mapped_irq, kern_irq_handler_t handler, void *argument);` | B2 の vector に handler を付ける | 同上 | i915/irq.c:310 |
| B4 | `int kern_irq_detach_msi_sync(int mapped_irq, kern_irq_handler_t handler, void *argument);` | handler を外し、走っている handler の終わりを待つ（sleep できる文脈で） | 同上 | i915/irq.c:371 |
| B5 | `int kern_irq_free_msi(int mapped_irq);` | B2 の vector を返す（handler を外し、device の送信を止めた後） | 同上 | i915/pci.c:818 |
| B6 | `unsigned kern_cpu_current(void);` | 呼んだ文脈の CPU の番号（hal_cpu_current。hal_cpu_id_t は uint32_t） | include/kern/sched.h、src/kern/sched.c（新しい `include/kern/cpu.h` でもよい） | pcat/graphics/text.c:987、dc8200.c:1174（比べる `struct spinlock.owner_cpu` は unsigned、lock.h:74）、i915/worker.c:418（試験 ktest-sync.c 3） |
| B7 | `void kern_console_early_putc(int character);` | HAL の early console（firmware・serial の線）へ 1 文字（hal_putc と同じ。kernel が `kernel_putc` を出した後は HAL がそちらへ回す、hal.h:81-85） | include/kern/text-display.h（kern_text_* の近く）、src/kern/text-display.c | generic/serial-console.c:136、rpi4-console.c:214、sun4u-console.c:309、x68k-console.c:97 |
| B8 | **型** `typedef uintptr_t kern_physaddr_t;`（hal_physaddr_t と同じ表現）。`kern_pmem_to_kernel(kern_physaddr_t)`（pmem.h:65）と `struct kern_pmem.paddr`（pmem.h:24）をこの型に | driver が物理 address を HAL の名前なしで書く | include/kern/pmem.h、src/kern/pmem.c | §1.3 の hal_physaddr_t の 23 file（cast を `kern_physaddr_t` に）。MSI の `paddr_t`（i915/pci.c:768）は B2 で `uint64_t` に |

B2〜B5 の errno: `irq.c` の `irq_error`（irq.c:27）は `HAL_ERR_STATE` を既定の EIO にする。`hal_irq_free_msi` は handler が付いたままだと `HAL_ERR_STATE`（ktest-sync.c:642 が確かめる）なので、案では `irq_error` に STATE → EBUSY の 1 行を足す。

**MSI の別の案**（関数を足さない）: i915 の確保と handler の取り付けを、既存の `kern_irq_register_msi`（irq.h:53、確保と取り付けを一度に）・`kern_irq_unregister_msi`（irq.h:56）に寄せる。i915 は pci.c で確保して message を device に書き、後で irq.c で handler を付ける 2 段の作りで、handler の引数（irq の構造体）は確保の時にまだ無いので、driver の作りを変えることになる。P2 の勧めは B2〜B5 を足す方。

**B7 の注意**: serial-console・x68k・sun4u・rpi4 の console の driver は、自分の putc を HAL の early console へ回す作り。B7 は今の動きを kernel の名前で包むだけで、動きは変えない。console を driver が自分で持つ作りに変えるかは §4 の rpi4 の件と合わせて別に決める。

### 4. 例外の候補（arch・board に固有の HAL の header）

| file | header | 使う物 | 案 |
| --- | --- | --- | --- |
| src/drivers/platform/pc98/pc98-lgy98.c:21 | `hal/i386/i386.h`（src/hal/i386/i386.h） | `asm_inb`・`asm_outb`・`asm_inw`・`asm_outw`（10 回） | **kernel の既存の口にする**: `kern_io_in8/out8/in16/out16`（device-io.h:29-33）。関数を足さない。 |
| src/drivers/platform/pc98/graphics/backend.c:16 | `hal/i386/bsp-pc98/display.h` | `pc98_display_graphics_start`（:550）・`pc98_display_graphics_stop`（:566） | **driver の中に移す**: 中身は graphics の GDC（port 0xa0・0xa2）へ START（0x0d）・STOP（0x0c）を FIFO が空くのを待って送るだけ（src/hal/i386/bsp-pc98/display.c:29-59）で、driver で `kern_io_in8/out8` を使って書ける。`pc98_display_*` の 3 関数（text_restore を含む）を呼ぶのは backend.c だけ（`grep -rn pc98_display_ src include` で display.c・display.h の外は backend.c:550,566 だけ）なので、display.c ごと driver に移せる見込み。HAL から消すのは `git mv` で、Q1 の OK を得てから。 |
| src/drivers/platform/rpi4/rpi4-console.c:34 | `hal/arm64/bsp-rpi-common/cons.h` | `rpi_cons_suspend_output`（:357,381,412）・`rpi_cons_resume_output`（:376） | **driver にも kernel にも移せない**: 止める物は HAL の early console の framebuffer の描画で、状態（`output_suspended`）と lock が HAL の中にあり（src/hal/arm64/bsp-rpi-common/cons.c:184-245）、HAL の stall watch（src/hal/arm64/watch.c:1091）も読む。kernel から呼んでも BSP の header を include することになる。案は 3 つで、**ユーザーの判断が要る**: (a) 当面の例外として残す（guardrail に例外として書く）。(b) 汎用の HAL の口（例 `hal_console_suspend_output`・`hal_console_resume_output`）を足し、kernel の wrapper を通す。HAL の API の変更で事前承認が要る。既存の口で足りない理由: hal.h に early console の描画を止める口は無く（`grep -nE 'cons\|display\|early' include/hal/hal.h` で該当の宣言なし）、`kernel_putc` を出す handover は、この board では console の driver 自体が hal_putc を使うので使えない。(c) rpi4 の console の driver が framebuffer の text を自分で描く作りにし、HAL は handover の後に描かない。大きい。P2 の勧めは (a) で WS331 を閉じ、(b)・(c) は別の WS で決める。 |

`hal_pc98_jisx0208_to_ucs`（HAL の data の表を driver が extern で参照、§1.3）も例外の候補: **driver の中に移す**案（表の file を `src/drivers/platform/pc98/graphics/` へ移し、名前から `hal_` を外す。HAL の中に他の利用者は無い）。`git mv` で、Q1 の OK を得てから。

### 5. 書き換えの Phase の分け方（案）

依存の順。各 Phase は build（warning 0）と、その driver を含む port の起動の確認（boot-test.sh）まで。

| 案の Phase | 内容 | 依存 | 確かめ |
| --- | --- | --- | --- |
| p002 | kernel に表 B を足す（B1 の定数と kern_device_map の実装、B2〜B5 の MSI と irq_error、B6、B7、B8 の型と pmem.h・pmem.c）。driver は変えない | ユーザーの表 B の決定 | 全 port の vmunix の build（warning 0） |
| p003 | 置き換え先がある所の機械的な書き換え（表 A）と include だけの file: rtc_read_counter・get_arch_handoff・irq_disable/enable・io/mmio・HAL_FATAL・putc（B7）・cpu_current（B6）・hal_physaddr_t の cast（B8）、acpi-kern・typec の map/unmap（UNCACHED・READONLY） | p002 | amd64・arm64（rpi4/5、odroid-c5、s905x5m）・armv7（zybo）・riscv64（vf2）・m68k（x68k）・i386（pc98）・sparcv9 の build と QEMU の起動 |
| p004 | i915 の map（WC・READ だけ）と MSI（B2〜B5）、irq の型（kern_irq_ack_t）、host の試験の代役（host-kernel.c・host_unreached.c・host_render.c）の名前と型、ktest-sync.c の HAL の契約の試験の扱い（kernel の口の試験に直すか、HAL の試験に移すか。AGENTS.md §9 の試験の整理で Q1 と決める） | p002 | amd64 の build、i915 の host の試験。実機（5330）の i915 の起動は Q1 の割当で |
| p005 | 例外の候補（§4）: pc98-lgy98 を kern_io へ、pc98 の graphics の start/stop を driver へ、JIS の表を driver へ、rpi4-console はユーザーの決定（§4 の a〜c）に従う | p003、ユーザーの決定 | pc98・arm64 の build と QEMU の起動 |
| p006 | 規約の全文の見直し（変えた C の file、comment の `hal_*`・`HAL` の語、§1.4） | p003〜p005 | grep で include・`hal_*` が 0（build の macro は §6 の 3 の決定に従う） |

sun4u-console.c・sun4u-firmware-fb.c は sparcv9 で、sparcv9 はユーザーが main の checkout で手を入れている。p003 の中で別の commit にし、Q1 が main の sparcv9 の変更と重ならないか確かめてから merge する。

### 6. 未決（ユーザーの判断が要る物）

1. 表 B の採否（B1〜B8）。MSI は B2〜B5 を足すか、i915 を `kern_irq_register_msi` に寄せるか（§3）。
2. rpi4-console の `rpi_cons_*`（§4 の a・b・c）。
3. build の macro `HAL_ARCH_*`・`HAL_BOARD_*` の `#if`（6 file・16 行、§1.3）: HAL の header の include でも `hal_*` の呼び出しでもないが、HAL の名前を driver が知る。そのままにするか、kernel の名前の macro（例 `KERN_ARCH_AMD64`・`KERN_BOARD_RPI5`）を build で併せて定義して driver をそちらにするか。
4. kernel の header が HAL の header を include している件（driver は間接に HAL の名前を見られる）: `include/kern/` の pmem.h・sched.h・thread.h・atomic.h・buf.h・input-device.h・input-keymap.h・io-scratch.h・random.h・sleep.h・vm-object.h・vmspace.h が `<hal/hal.h>`、clock.h が `<hal/arch.h>` を include する（`grep -n '#include <hal/' include/kern/*.h`）。driver の file でこれらを include する数は clock 202・sched 131・pmem 98・thread 67・atomic 23・input-device 8・input-keymap 4・random 3・sleep 3・buf 1・vm-object 1・vmspace 1（`grep -rlE '#include [<"]kern/<名前>.h' src/drivers include/drivers | wc -l`）。WS331 の目標（driver の include と呼び出しを 0）の外だが、「存在を知らない」を厳密にするなら kernel の header の整理（`hal_cpu_id_t`・`hal_task_t`・`hal_space_t` を持つ構造体）が要り、大きい。別の WS にするかを決める。

### 7. 未実施

- code の変更・build・試験は無し（範囲の外）。§1.1 の「include だけ」の file が他の header に頼っていないかは build で確かめていない。
- B1〜B8 の宣言は案で、compile して確かめていない。
