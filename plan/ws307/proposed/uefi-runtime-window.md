<!-- awesome-plan project=zedbsd record=ws307-proposed-uefi-runtime-window -->

# 案: UEFI の firmware の変数の driver（ws307-p007、改訂 5）

- 親: [WS307](../ws.md)・[p007](../phase007/phase.md)。書いた人: P6（q1711-i01、2026-10-30）。状態: 2026-10-30 のユーザーの決定（G2 は HAL の API の変更でなく amd64 の実装の補完）で確定し、q1718-i01（P6）で実装した。実装と試験は [phase007/phase.md](../phase007/phase.md)、形の本体は [spec.md](../spec.md) §5.8。
- 改訂 5 の理由（ユーザーの原文、2026-10-30、Q1 の中継）:「HALは関係なくないですか？」「ドライバから見て、HALは使えません。存在を知りません。カーネルの機能を使ってマッピングするのみです。」→ driver は `hal_*` を一切呼ばず、kernel の口（`kern_*`）だけを使う形に直した。改訂 4（driver が `hal_get_arch_handoff`・`hal_irq_disable` を呼ぶ形）は取り下げ。
- `include/hal/hal.h` は変えない。

## 1. driver が要る物と、kernel の既存の口

driver（`src/drivers/platform/pcat/uefi-variables.c`）が firmware の GetVariable・SetVariable・GetNextVariableName を呼ぶのに要る物を、kernel の既存の口と一つずつ照らした。

| 要る物 | kernel の既存の口 | 足りるか |
| --- | --- | --- |
| 呼び出しの直列 | `mutex_init`・`mutex_lock`・`mutex_unlock`（`include/kern/lock.h:113-130`） | 足りる |
| 呼び出しの間の割り込みの禁止 | `kern_irq_disable`・`kern_irq_enable`（`src/kern/irq.c:202-216`。前の状態を bool で返す） | 足りる |
| FP の状態の退避と UEFI の求める MXCSR 0x1F80・x87 0x037F | kernel の口は無い。kernel は `-mgeneral-regs-only` で build し（`platform/amd64/vmunix.mk:79`）、user の FP の状態は register に置いたまま（eager、`src/hal/amd64/task.c:286`・`:475` の fxsave64） | **driver の中で足りる**: 割り込みを止めた間（context switch が挟まらない）に driver の 16 byte 境界の領域へ `fxsave64`、呼んだ後 `fxrstor64`。pcat の driver が CPU の命令を inline asm で使う前例（`src/drivers/platform/pcat/serial-mirror.c:81`・`:91` の `outb`・`inb`）。HAL も kernel の新しい関数も要らない |
| firmware の data を読む（system table の signature・CRC32 の確かめ） | `kern_device_map(address, size, KERN_DEVICE_UNCACHED, &mapped)`（`src/kern/pmem.c:145-163`）。runtime の範囲は loader が `ZBL6_MEMORY_RESERVED` にし（`bootloader/uefi/memory-map-v6.c:133-135`）、HAL の RAM の map の外（`src/hal/amd64/space.c:2640`）なので device として map できる。ACPI の driver が firmware の table を同じ形で読む（`src/drivers/acpi/acpi-kern.c:922`） | 読むだけなら足りる |
| system table の物理 address | `kern_boot_handoff(name)`（`src/kern/pmem.c:217-222`）は kernel の口で、ACPI の driver が `kern_boot_handoff("acpi.rsdp")` で RSDP を受ける前例（`src/drivers/acpi/acpi-kern.c:168`）。ただし system table を運ぶ名前が無い（`src/hal/amd64/bsp-pcat/boot.c:412-468` の名前は `boot.command-line`・`boot.selector`・`pcat.boot-font`・framebuffer の値・`acpi.rsdp` だけ）。loader の handoff にも field が無い（`bootloader/include/amd64-handoff.h:312` の v7 が最後） | **足りない（G1）** |
| runtime の範囲の一覧（どこが code・data・MMIO か） | 同じく名前が無い。kernel の `bsp_mem_range` の一覧は型を `RESERVED` に潰している（`memory-map-v6.c:133-135`） | **足りない（G1）** |
| firmware の code を ring 0 で実行できる kernel の map | `kern_pmem_to_kernel` は管理された RAM だけ（`include/kern/pmem.h:58-63`、direct map は NX）。`kern_device_map` は読み書きだけで、下の `hal_space_map_device` が `HAL_SPACE_EXEC` を拒む（`src/hal/amd64/space.c:372-377`）。kernel に実行できる kernel の map を作る関数は無い（`src/kern` で `HAL_SPACE_EXEC` を使うのは user の空間の `elf.c`・`vm/vmspace-map.c` だけ） | **足りない（G2）** |
| firmware の内部の pointer を map の仮想 address に合わせる | — | **足りない（G3）** |

## 2. 足りない点と、何で埋めるか

### G1. system table と runtime の範囲の場所

- kernel の中の新しい関数では埋まらない: kernel が firmware の情報を受ける口は `kern_boot_handoff(name)` だけで、その中身は HAL の実装が名前ごとに返す（`src/kern/pmem.c:217-222` → `hal_get_arch_handoff`）。kernel・driver は loader の header を見ない規則（guardrail「HAL と kernel・boot loader と HAL の受け渡し」、2026-10-18 ユーザー）。
- **要る物（HAL の実装の補完、hal.h 不変）**: amd64 の HAL の実装が名前を返す。案: `uefi.system-table`（system table の物理 address）、`uefi.runtime-map`・`uefi.runtime-map-count`・`uefi.runtime-map-entry-size`（HAL が low block から自分の領域に写した runtime の記述子の列（物理・仮想・page 数・型・attribute）の物理 address と数と大きさ）。driver は `kern_boot_handoff` で受ける。無い時（BIOS・古い loader）は NULL で driver は ENODEV。
- **loader**（HAL の外）: handoff を v8 にして system table の物理 address と記述子の列を渡す（loader → HAL の固有の構造体、`amd64-handoff.h`）。

### G2. 実行できる kernel の map

- **kernel の中の新しい関数**: `kern_firmware_map(uint64_t paddr, void *vaddr, size_t size, unsigned prot, unsigned cache)`（`src/kern/pmem.c`）。kernel の決まった窓（G3 の理由で仮想 address は呼び手が決める）に firmware の範囲を map する。driver はこれだけを呼ぶ。
- **その下で HAL の実装の補完が要る（hal.h 不変）**: 今の amd64 の `hal_space_map` は `HAL_SPACE_SYS`（NULL）を拒み（`src/hal/amd64/space.c:1119`）、user の VA だけを受ける（`:1121` の `valid_user_range`）。`hal_space_map_device` は EXEC を拒み、仮想 address を HAL が選ぶ（`:372-377`）。案: amd64 の `hal_space_map(HAL_SPACE_SYS, va, pa, size, attr)` を、firmware の窓（top の PDPT の slot 506、`0xfffffffe80000000` から 64 MiB、今は空き。使っているのは slot 507〜511、`space.c:686-695`）の中で、HAL の RAM の map の外の頁に限って受ける。`HAL_SPACE_EXEC` を許し、`HAL_SPACE_NOCACHE` で uncached。窓は全ての空間が共有する system の PDPT にあるので、どの空間からの呼び出しでも見える。
  - 注意（判断を求める点）: hal.h の `hal_space_map` の文（`include/hal/hal.h:748-762`）は `HAL_SPACE_SYS` を禁じていないが、今は全ての port が `HAL_SPACE_SYS` を拒む（例 `src/hal/riscv64/space.c` の `if (handle == HAL_SPACE_SYS)`、armv7・mips・ppc も同じ）。amd64 だけが受けるのを「実装の補完」と見るか「契約の変更」と見るかはユーザーの判断。補完と見ない時の代わりは、amd64 の HAL が起動の時に窓を自分で map し（改訂 4 の A2）、kernel の関数は要らない形。

### G3. 仮想 address への付け替え

- firmware の code は自分の内部の pointer を持つ。kernel の窓で実行するには、UEFI の `SetVirtualAddressMap` で firmware にその仮想 address を教える必要がある。UEFI の規則で、この呼び出しは ExitBootServices の後、物理の address の mode（runtime の範囲の 1:1 の map の上）で 1 回だけ。
- kernel の中では埋まらない: kernel の page table には低い 1:1 の実行できる map が無く（HAL は AP の trampoline の 1 頁だけを残す、`space.c` の `system_identity_pt`）、それを作る口も無い。
- **loader で行う**（HAL の外）: loader は ExitBootServices の直後、firmware の identity の map の上にいる。runtime の記述子に G2 の窓の仮想 address を詰めて付け、`SetVirtualAddressMap` を呼ぶ。窓に入らない・失敗した時は G1 の名前を渡さない（起動は続ける）。spec §5.8 の 3 の「`SetVirtualAddressMap` は呼ばない」の変更になる。
- 窓の仮想 address は loader と HAL の実装が同じ定数（`amd64-handoff.h` の `ZBL6_UEFI_RUNTIME_VIRTUAL_BASE`・`_SIZE`）で知り、kernel と driver は記述子の「仮想」の field で知る（定数を見ない）。

## 3. 形の全体（承認された時）

1. loader（`bootloader/uefi/bootx64.c`、`amd64-handoff.h`）: G3 の `SetVirtualAddressMap`、handoff v8（system table の物理 address・記述子の列・成否の flag）。
2. HAL の実装（`src/hal/amd64`、hal.h 不変）: G1 の名前、G2 の `hal_space_map(HAL_SPACE_SYS, …)` の firmware の窓。
3. kernel（`src/kern/pmem.c`、`include/kern/pmem.h`）: `kern_firmware_map`。
4. driver（`src/drivers/platform/pcat/uefi-variables.c`、`hal_*` を呼ばない）: `kern_boot_handoff` で名前を受け、記述子ごとに `kern_firmware_map`（RuntimeServicesCode は実行可・書き込み可、他は NX、MMIO と WB の無い範囲は uncached）、system table と RuntimeServices の signature・CRC32 を確かめ、`mutex` で直列、`kern_irq_disable`、fxsave64・MXCSR・x87、`__attribute__((ms_abi))` の関数 pointer で呼ぶ、fxrstor64、`kern_irq_enable`。
5. UAPI: `/dev/system` の ioctl（root だけ、名前は文字列・値は byte の列）、kernel の中は firmware に依らない provider の口。userland の `efiboot` が Boot####・BootOrder を作る。
6. 試験: QEMU の OVMF で `efiboot list`・`create` → 読み戻し、再起動の後も残る、その entry で起動。BIOS の起動・v7 の loader で driver が ENODEV のまま起動する回帰。

## 4. HAL に触れない案（参考、改訂 4 の C4）

- loader が Boot#### を作る: installer が導入先の ESP に要求の file を置き、導入先の loader が次の起動の ExitBootServices の前（boot services の間は runtime の呼び出しを firmware の map のまま使える）に SetVariable で Boot#### と BootOrder の末尾を書く。HAL・kernel・driver は変えない。
- 制限: entry は導入先から初めて起動した後にできる（導入の直後の boot menu に「zedBSD」が出ない、spec §5.8 を満たさない）。動いている system から一覧・削除（`efiboot list`・`delete`）ができない。

## 5. 危険と制限

- firmware の code の頁は W+X（PE の image の .data が同じ RuntimeServicesCode の範囲にあるため）。
- firmware が runtime でない memory に触る不具合は kernel の page fault になる。OVMF では起きない見込み。実機は release の UAT。
- `SetVirtualAddressMap` は起動ごとに 1 回で取り消せない。kernel が窓を map しなくても runtime を呼ばないだけで起動には影響しない。
