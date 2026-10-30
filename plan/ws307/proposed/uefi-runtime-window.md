<!-- awesome-plan project=zedbsd record=ws307-proposed-uefi-runtime-window -->

# 案: UEFI の runtime services の窓（ws307-p007、改訂 4）

- 親: [WS307](../ws.md)・[p007](../phase007/phase.md)。書いた人: P6（q1711-i01、2026-10-30）。状態: **ユーザーの承認待ち**（Q1 が聞く）。承認までこの案の 3 つの点に当たる code は書かない。
- `include/hal/hal.h` の宣言は変えない。この案が求める承認は、HAL の責務の追加 2 つと spec §5.8 の変更 1 つ（下の A1〜A3）。

## 1. 求める承認

| # | 何 | 種類 |
| --- | --- | --- |
| A1 | amd64 の HAL が `hal_get_arch_handoff("uefi.system-table")` を返す。値は 64 bit の word 1 つへの pointer（framebuffer の値の名前と同じ形）で、中身は UEFI の system table の **kernel の仮想 address**。loader が runtime を渡さなかった時（BIOS の loader、古い loader、下の A3 が失敗した時）は NULL | HAL の責務の追加（新しい名前。前例 `acpi.rsdp`） |
| A2 | amd64 の HAL が起動の時、loader が渡した runtime の記述子（`EFI_MEMORY_RUNTIME` の範囲）を、system の page table の決まった窓（top の PDPT の slot 506、`0xfffffffe80000000` から 64 MiB）に map する。RuntimeServicesCode は実行可・書き込み可、他は NX。MMIO と WB を持たない範囲は uncached。窓は全ての空間が共有する system の半分にある | HAL の責務の追加（kernel の半分に firmware の実行できる code を置く） |
| A3 | UEFI の loader（`bootloader/uefi/bootx64.c`）が ExitBootServices の直後に `SetVirtualAddressMap` を 1 回呼び、runtime の記述子に A2 の窓の仮想 address を詰めて付ける。窓に入らない時・呼び出しが失敗した時は runtime を渡さずに起動を続ける。handoff を v8 にし、system table の物理 address・記述子の列（物理・仮想・page 数・型・attribute、最大 64）・成否の flag を足す | spec §5.8 の 3 の「`SetVirtualAddressMap` は呼ばない」の変更。loader → HAL の構造体は `bootloader/include/amd64-handoff.h`（guardrail「HAL と kernel・boot loader と HAL の受け渡し」の置き場所） |

A1〜A3 の後は HAL に触れない: driver（`src/drivers/platform/pcat/uefi-variables.c`）が A1 の値から system table と RuntimeServices の signature・CRC32 を確かめ、mutex で直列にし、割り込みを止め（`hal_irq_disable`）、FP の状態を fxsave64 で退避して MXCSR 0x1F80・x87 0x037F にし、`__attribute__((ms_abi))` の関数 pointer で GetVariable・SetVariable・GetNextVariableName を呼ぶ。UAPI は `/dev/system` の ioctl（root だけ、名前は文字列・値は byte の列）、userland の `efiboot` が Boot####・BootOrder を作る。

## 2. HAL を変えずに済まない理由

firmware の runtime の関数を kernel から呼ぶには、(i) firmware に渡す system table の場所を kernel が知ること、(ii) firmware の code が ring 0 で実行できる仮想 address に map されていること、の 2 つが要る。

### (i) system table の場所 → A1 が要る

- kernel・driver は `bootloader/` の header を include できない（guardrail、2026-10-18 ユーザー「bootloader/のヘッダを見ていいのは、HALだけにします。HALからカーネルへは、名前ごとの値でアクセスします」）。よって loader の handoff の構造体を kernel が直に読む道は規則で閉じている。
- HAL → kernel の汎用の構造体 `struct kern_boot_handoff`（`include/kern/boot.h`）には firmware の table の field が無い。field を足すのはこの汎用の構造体（HAL と kernel の契約）の変更で、名前を 1 つ足すより大きい。
- 既存の名前（`boot.command-line`・`boot.selector`・`pcat.boot-font`・framebuffer の値・`acpi.rsdp`）のどれも system table を運ばない。`acpi.rsdp` は ACPI の table の物理 address で、UEFI の system table への道は無い（RSDP から system table は辿れない）。
- 物理 memory を kernel が走査して system table の signature を探す形は、firmware の data を推測で読むことになり、偽の一致を拒めない（EFI の仕様に探索の手順は無い）。

### (ii) 実行できる map → A2 と A3 が要る

既存の HAL の口を一つずつ当てた:

| 口 | 使えない理由（`src/hal/amd64/space.c`） |
| --- | --- |
| `hal_space_map(HAL_SPACE_SYS, …)` | `space == NULL` を `HAL_ERR_INVALID` で拒む。system の半分に map する口ではない |
| `hal_space_map(user の空間, …)` | `valid_user_range` で下の半分の VA だけ。頁は `user_page_allowed` で「管理された RAM」か `HAL_SPACE_DEVICE` に限る。runtime の範囲は loader が `ZBL6_MEMORY_RESERVED` にする（`memory-map-v6.c:134`）ので管理された RAM ではなく、`HAL_SPACE_DEVICE` は `HAL_SPACE_EXEC` を拒む。さらに user の頁（U=1）を ring 0 で実行するのは SMEP の機械で fault し、設計としても誤り |
| `hal_space_map_device` | `HAL_SPACE_EXEC` を拒み、RAM と重なる範囲を拒む（runtime の code は RAM） |
| `hal_pmem_to_kernel` | 管理された RAM だけを返し、direct map は NX（`AMD64_PTE_NX`）。runtime の範囲は管理外で NULL |
| `hal_pmem_map_uncached` | 管理された RAM だけ、amd64 は実装無し、実行は不可 |
| CR3 の切り替え（firmware の 1:1 の map で物理の mode のまま呼ぶ形） | `hal_space_switch` は `hal_space_create` の空間（上と同じ制約の user の頁）しか選べない。driver が CR3 を直に書くのは HAL の迂回で規則に反する |

- よって「ring 0 で実行できる、firmware の code の map」は今の HAL の口では作れない。作れるのは system の page table を組む HAL の実装だけで、それは HAL の責務の追加になる（A2）。
- A2 で kernel の半分の固定の窓に map するなら、firmware の内部の pointer をその仮想 address に合わせる `SetVirtualAddressMap` が要る（A3）。UEFI の規則で、この呼び出しは ExitBootServices の後、物理の address の mode で 1 回だけ。loader はちょうどその状態（firmware の identity の map の上）にいるので、loader で呼ぶのが一番小さい。HAL の起動の早い段で呼ぶ形（Q1 の (b)）も考えたが、HAL は loader の identity の map を paging の後に回収し、自分の page table には低い 1:1 を AP の trampoline の 1 頁しか持たないので、HAL の中で物理の mode の呼び出しを作るとさらに大きな変更になる。
- SetVirtualAddressMap を呼ばない形（spec §5.8 の元の前提）は、低い 1:1 の実行できる supervisor の map と呼び出しの間の CR3 の切り替えが要り、HAL の関数（前の案 `hal_uefi_runtime_call`、取り下げ済み）無しにはできない。

## 3. HAL を変えない案（検討した。どれも採らない）

| 案 | だめな理由 |
| --- | --- |
| C1. loader の handoff の既存の構造体で system table を渡し、kernel がそれを読む | kernel・driver は `bootloader/` の header を見られない（規則）。kernel が見られる汎用の `kern_boot_handoff` には field が無く、足すのは HAL と kernel の契約の変更 |
| C2. kernel の既存の map の口で runtime の範囲を map する | 上の表のとおり、実行できる kernel の map の口が無い |
| C3. user の空間に 1:1 で map し、`hal_space_switch` で切り替えて呼ぶ | 管理された RAM でない頁は EXEC 付きで map できない。U=1 の頁を ring 0 で実行することになり SMEP の機械で fault する |
| C4. **loader が Boot#### を作る**（boot services の間なら runtime の呼び出しは firmware の map のまま安全に使える）: installer が導入先の ESP に要求の file（例 `\EFI\ZEDBSD\BOOTENT.REQ`）を置き、導入先の loader が次の起動の ExitBootServices の前に SetVariable で Boot#### と BootOrder の末尾を書き、file を消す | HAL も kernel も変えずに済む唯一の形。ただし (1) entry ができるのは導入先から初めて起動した後なので、最初の起動は firmware の boot menu で disk（または `\EFI\ZEDBSD\BOOTX64.EFI`）を選ぶ必要がある（spec §5.8 の「導入の直後に boot menu に『zedBSD』が出る」を満たさない）。(2) 動いている system から Boot#### の一覧・削除（`efiboot list`・`delete`、spec §5.8 の 4）ができない。(3) 名前が「zedBSD」の古い entry の掃除（spec §5.8）を loader が行うことになり、loader が大きくなる。ユーザーが A1〜A3 を採らない時の代わりの案として残す |

## 4. 承認された時の実装の順と試験

1. loader v8（A3）: 記述子の列を low block の `0xa200` から（`BOOT_ALLOCATIONS_OFFSET` の page の後ろ、最大 64 × 40 byte）、`ZBL6_HANDOFF_FLAG_UEFI_RUNTIME`、`ZBL6_UEFI_RUNTIME_VIRTUAL_BASE`・`_SIZE` を `amd64-handoff.h` に。
2. HAL（A2・A1）: `boot.c` が v8 を読み記述子を static な領域に写す、`space.c` が slot 506 に static な PD と 32 枚の PT（64 MiB）で map、`hal_get_arch_handoff("uefi.system-table")`。
3. driver・UAPI・`efiboot`（HAL の外）。
4. 試験: QEMU の OVMF（書ける VARS の複写）で起動し、`efiboot list` が OVMF の既存の Boot#### を読む、`efiboot create` → 読み戻し、再起動の後も残る、その entry で OVMF が起動する（BootNext）。BIOS の起動と古い v7 の loader の起動で `uefi.system-table` が NULL のまま起動すること（回帰）。

## 5. 危険と制限

- 窓の code の頁は W+X（PE の image の .data が同じ RuntimeServicesCode の範囲にあるため。EFI_MEMORY_ATTRIBUTES_TABLE を読んで分ける形は後の改善）。
- firmware の不具合（runtime の呼び出しが runtime でない memory に触る）は kernel の page fault になる。OVMF では起きない見込み、実機は release の UAT で確かめる（p012 は canceled）。
- SetVirtualAddressMap は起動ごとに 1 回で取り消せない。kernel が古く窓を map しなくても、runtime を呼ばないだけで起動には影響しない。
