<!-- awesome-plan project=zedbsd record=ws307-p007 -->

# ws307-p007 UEFI の runtime services と Boot####

- 親: [WS307](../ws.md)。仕様: [spec.md](../spec.md) §5.8（U5・U15）。Queue: q1665 / q1665-i01（担当 P4、2026-10-29）。
- Status: in-progress（q1718-i01、P6、2026-10-30: 改訂 5 で実装し QEMU（OVMF）の試験が PASS。Q1 の判定待ち。下の「q1718-i01」）
- 出典: 2026-10-28 U5「…Boot####の作成もWSに入れる。」、U15「末尾に足します。また、KeiではなくzedBSDです。…」。

## 最初の段（2026-10-29、P4）

- HAL の差分の案: [proposed/hal-uefi-variables.md](../proposed/hal-uefi-variables.md)（loader の handoff v8、`hal_get_arch_handoff("uefi.runtime")`、`hal.h` に UEFI の変数の 4 関数、amd64 の実装の方針、判断 H1〜H3 と推し）。AGENTS.md §7 により承認まで実装しない。
- 承認までの間: installer（p009・p010）は loader を ESP に置き、S11 で「firmware の boot menu で選ぶ」を出すだけ（spec §5.8）。

## 改訂 2（2026-10-29、P4）

- ユーザーの回答（原文、Q1 の中継）:「UEFIに限らないファームウェア環境変数という概念で作り直してみてください。もう一度レビューして承認します。」
- 案を [proposed/hal-fw-variables.md](../proposed/hal-fw-variables.md)（firmware の環境変数: 名前空間（UEFI・Open Firmware・U-Boot・Pi の EEPROM）＋vendor＋UTF-8 の名前の key、`hal_fw_variable_space_info`・`get`・`set`・`next`）に作り直した。前の案 [proposed/hal-uefi-variables.md](../proposed/hal-uefi-variables.md) は置き換え（file の削除は Q1）。

## 残り

- ユーザーの承認（改訂 2 の F1〜F3、Q1 経由）。
- 承認の後: loader の v8、HAL の実装、`/dev/system` の UAPI（形・版・上限を code の前に決める）、`efiboot`、QEMU の OVMF で Boot#### を書いて読み戻す（案 §4）。

## 改訂 3（2026-10-30、Q1 の中継、q1698-i01）

- ユーザーの回答（原文、ws.md）:「WS307ですが、ファームウェアの環境変数は文字列でいいのではないかと思ったのと、HALでアクセスする必要性が感じられませんでした。UEFIの起動エントリーは、UEFIランタイムサービスを経由すると思うのですが、それはカーネルのドライバで実装するものではないでしょうか。カーネルにファームウェア変数書き込みのドライバを作るのがいいと思います。」「ご確認の、バイト列にするのはOKです。」
- **取り下げ**: [proposed/hal-uefi-variables.md](../proposed/hal-uefi-variables.md)（最初の案）と [proposed/hal-fw-variables.md](../proposed/hal-fw-variables.md)（改訂 2）は取り下げた。`hal.h` は変えない。file の削除は Q1（履歴は git に残る）。
- Q1 の指示の形（2026-10-30）: loader が UEFI の system table（runtime services の表）と memory map を渡し、kernel は既存の `hal_get_arch_handoff` で新しい名前（`uefi.system-table`・`uefi.memory-map` など）として受け取る。名前の引き当ては `src/hal/amd64` の実装（承認不要）。kernel の firmware 変数の driver が runtime services の領域を kernel に map し、呼び出しを直列にし、amd64 では MS x64 ABI で GetVariable・SetVariable・GetNextVariableName を呼ぶ。`/dev` の口は名前が文字列（GUID と名前）、値が byte 列。userland の `efiboot` が Boot####（EFI_LOAD_OPTION）と BootOrder を作り・読む。QEMU の OVMF で Boot#### を書いて読み戻す。
- 順番: q1698-i01 の他の項目（p015・p002・p004・p008・p014）の後。

### 設計の下書き（実装の前に詰める、P4）

1. **loader**（`bootloader/uefi`）: ExitBootServices の直前の最後の GetMemoryMap の生の descriptor の列（型・物理・仮想・page 数・attribute）を、boot allocation（lifetime KEEP）の領域に写して渡す。handoff に版を足し（v8）、system table の物理 address、map の物理 address・byte 数・descriptor の大きさ・版を置く。
2. **HAL の実装**（`src/hal/amd64/bsp-pcat/boot.c`、API は変えない）: 既存の framebuffer の値と同じく、名前ごとに 1 つの `uint64_t` を返す: `uefi.system-table`・`uefi.memory-map`・`uefi.memory-map-size`・`uefi.memory-descriptor-size`・`uefi.memory-descriptor-version`。無ければ NULL（BIOS・古い loader）。
3. **driver**（amd64、`src/drivers/firmware/` の新しい file）: system table の signature・revision・CRC32 を確かめ、RuntimeServices の表を得る。
   - **map の方式（詰める点）**: (a) runtime の領域を 1:1（VA＝PA）で map した専用の page table に呼び出しの間だけ切り替える（SetVirtualAddressMap を呼ばない）、または (b) 領域を kernel の VA に map し SetVirtualAddressMap を 1 回呼ぶ。どちらも「ring 0 から実行できる低い VA の 1:1 の map」が少なくとも 1 度要る（(b) でも SetVirtualAddressMap 自体は物理の address で呼ぶ）。既存の口: `hal_space_map(HAL_SPACE_SYS, …, HAL_SPACE_EXEC…)` は kernel の半分にしか置けず、`hal_space_create` の空間は user の page（SMEP・SMAP で ring 0 から実行・参照できない見込み）。loader の `bootstrap_cr3`（identity の map）は paging の後に回収される。まず HAL を変えずに済む形（kernel の半分に runtime の領域を map して (b) の SetVirtualAddressMap を HAL の起動の早い段（identity の map がまだある時）に HAL の実装の中で済ませる、など）を調べ、既存の口で足りない時は理由を付けて Q1 に返す（AGENTS.md §7）。
   - **割り込み**: 呼び出しの間は割り込みを止め、mutex で直列にする（firmware は再入できない。flash への書き込みで数 ms 止まり得る）。
   - **FP の状態**: amd64 の HAL は user の FP の状態を register に置いたまま kernel を走らせ（`task.c` の context switch で fxsave/fxrstor）、firmware は SSE を使ってよい。呼び出しの前に driver が自分の 16 byte 境界の領域に fxsave64 し、UEFI の求める MXCSR 0x1F80・x87 の制御語 0x037F にして呼び、後で fxrstor64 する（割り込みを止めた間なので context switch は挟まらない）。
   - **ABI**: 関数の型に `__attribute__((ms_abi))`（clang が shadow space と callee-saved の XMM6〜15 を扱う）。stack は今の kernel の stack（16 byte 境界）。
4. **/dev の口**: `/dev/fwvar`（root だけ、0600）の ioctl: GET・SET・NEXT。名前は GUID の文字列（36 文字）と UTF-8 の名前（kernel が UCS-2 に変える）、値は byte 列と attribute。上限（名前の長さ・値の大きさ）を code の前に決める。
5. **efiboot**（userland）: `list`（BootOrder と各 Boot####）、`create`（説明「zedBSD」、HD(GPT の partition 番号・署名・開始・大きさ)/File(`\EFI\ZEDBSD\BOOTX64.EFI`)、BootOrder の末尾に足す、U15）、`delete`。installer の S11 から使う所は spec §5.8。
6. **試験**: QEMU の OVMF（書ける VARS の複写）で zedBSD を起動し、`efiboot create` → `efiboot list` で読み戻し、再起動の後も残ること、OVMF がその entry で起動すること（BootNext か BootOrder）。

### 引き継ぎ（2026-10-30 09:13、q1698-i01 のラップアップ）

- 記録だけ（3c14ef8f4、branch `agent/p4-ws307o`）。code は無い。`proposed/` の 2 file は取り下げ、削除は Q1。
- 次の 1 手: 設計の下書きの 3 の「map の方式」を詰める（`hal_space_map(HAL_SPACE_SYS, …)` で EXEC の kernel の map が作れるか、SMEP・SMAP の下で 1:1 の map を HAL の実装の中だけで作れるか、SetVirtualAddressMap を HAL の起動の早い段で呼ぶ形か）。既存の口で足りなければ理由を付けて Q1 に返す。決めた後 loader v8 → HAL の名前 → driver → /dev → efiboot → OVMF の試験の順。

## q1711-i01（2026-10-30、P6、worktree p6-ws307、branch agent/p6-ws307、基点 main 5aca73738）: map の方式の案（承認待ち）

- 調べた事（HAL の API を変えない形を探した）:
  - `hal_space_map` は user の半分の VA しか受けない（`space.c` の `valid_user_range`）。`HAL_SPACE_SYS`（NULL）は拒む。管理された RAM でない頁は `HAL_SPACE_DEVICE` だけで、それは実行を拒む。`hal_space_map_device` も EXEC を拒み、RAM を拒む。よって driver から既存の口で「実行できる kernel の map」を作ることはできない。
  - driver から CR3 を切り替える口も無い（1:1 の private な page table に移る (a) は driver からはできない）。
  - 一方、HAL の実装（`src/hal/amd64`、承認不要）は system の page table を自分で組み、framebuffer・ACPI・ECAM・device の窓を top の PML4 の PDPT の slot 507〜511 に置いている。slot 0〜506 は空き。top の PDPT は全ての user の空間が共有する（`hal_space_create` が `system_pml4` の上半分を写す）。
  - loader は ExitBootServices の後も firmware の identity の map の上で走る。UEFI の規則で SetVirtualAddressMap は ExitBootServices の後、物理の address の mode で 1 度だけ呼べる。
- **案の形（(b) の変形、`hal.h` は変えない）**。2026-10-30 Q1 の判断: 新しい名前・HAL の窓の map・SetVirtualAddressMap の 3 つはユーザーの承認が要る（guardrail の前例）。案は [proposed/uefi-runtime-window.md](../proposed/uefi-runtime-window.md)（改訂 4 f1ba6c9b3。改訂 5 で driver は kern_* だけを使う形に作り直し、足りない点 G1〜G3 を挙げた）、Q1 がユーザーに聞く。承認まで p007 の code は書かない:
  1. loader（`bootloader/uefi/bootx64.c`）: ExitBootServices の直後、最後の map の `EFI_MEMORY_RUNTIME` の記述子に、決まった kernel の窓 `ZBL6_UEFI_RUNTIME_VIRTUAL_BASE`（PDPT の slot 506 = `0xfffffffe80000000`）から詰めて VirtualStart を付け、SetVirtualAddressMap を呼ぶ。窓（64 MiB）に入らない・呼び出しが失敗した時は runtime を渡さない（起動は続ける）。記述子（物理・仮想・page 数・型・attribute）を low block の handoff の後ろの決まった場所に写し、handoff v8 に system table の物理 address・記述子の数・SetVirtualAddressMap の結果の flag を足す。
  2. HAL の実装（`src/hal/amd64`）: handoff v8 を読み、記述子を自分の static な領域に写す（low block は init の後に再利用される）。system の page table を組む時に、slot 506 の窓に各記述子を VirtualStart で 4 KiB の頁で map する（RuntimeServicesCode は実行可・書き込み可（PE の .data を含む）、他は NX、MMIO と WB の無い物は uncached）。`hal_get_arch_handoff("uefi.system-table")` は system table の kernel の VA（uint64_t）を返す（無い時 NULL）。framebuffer の値と同じ「名前ごとに 1 つの uint64_t」。
  3. driver（`src/drivers/platform/pcat/`）: system table と RuntimeServices の signature・revision・CRC32 を確かめ、mutex で直列にし、割り込みを止め、FP の状態を fxsave64 で退避して MXCSR 0x1F80・x87 0x037F にし、`__attribute__((ms_abi))` の関数 pointer で GetVariable・SetVariable・GetNextVariableName を呼び、戻す。
  4. UAPI: spec §5.8 の 4 のとおり `/dev/system` の ioctl（root だけ）。名前は GUID の文字列と UTF-8 の名前、値は byte 列（2026-10-30 ユーザー「ご確認の、バイト列にするのはOKです。」）。kernel の中では firmware に依らない口（provider の登録）にし、provider が無い所は ENOTSUP。
- spec §5.8 の 3 の「SetVirtualAddressMap は呼ばない」は HAL の関数を足す前の案の前提だった。HAL の API を変えずに driver から呼ぶには kernel の半分の VA で実行できる map が要り、それを HAL の実装が起動の時に作るには SetVirtualAddressMap が要る、という理由でこの形に変えた（Q1 の指示の (b) の形）。

## q1718-i01（2026-10-30、P6、worktree p6-ws307、branch agent/p6-ws307、基点 main f69189c7e）: 改訂 5 の実装と試験

- 決定（2026-10-30 のユーザーの決定の要点、Q1 の中継）: firmware の変数は kernel の driver が書く。driver は HAL を知らず kernel の機能だけで map する。runtime services の address は handoff で渡り、kernel・driver が kernel の機能で map する。amd64 の `hal_space_map` が `HAL_SPACE_SYS`（firmware の窓）と実行可を受けるようにするのは API の変更でなく実装の補完（hal.h は変えない）。
- 形の本体は [spec.md](../spec.md) §5.8（この Queue で書き直した）。

### 実装（commit c4bedbcf5・166aaba80・178c8517b）

| 段 | file | 要点 |
| --- | --- | --- |
| G3 loader | `bootloader/uefi/bootx64.c`・`include/uefi.h`・`bootloader/include/amd64-handoff.h` | handoff v8（v7＋32 byte の runtime の部分、flag bit 7）。ExitBootServices の後に `give_runtime_addresses()` が runtime の記述子を low block の 0xb000 に写し、窓 `0xfffffffe80000000`（64 MiB）に map の順で詰めた VirtualStart で `SetVirtualAddressMap` を呼ぶ。入らない・拒まれた時は渡さず起動を続ける |
| G1 HAL | `src/hal/amd64/bsp-pcat/boot.c`・`handoff-validation.c/.h` | v8 の分類、runtime の部分の検査（low block の boot allocation の中、記述子の大きさ・数、窓の中で昇順、system table を含む）、HAL の領域へ写して名前 `uefi.system-table`・`uefi.runtime-map`・`-count`・`-entry-size`・`-version` で返す |
| G2 HAL | `src/hal/amd64/space.c` | PDPT の slot 506 に `system_firmware_pd` を常に繋ぐ。`hal_space_map(HAL_SPACE_SYS, …)` は `firmware_window_map()` へ: 窓の中・頁境界・RAM の map の外・未 map の頁だけ、READ/WRITE/EXEC/NOCACHE だけ。leaf table は初めて要る時に確保し kernel の生涯残す。not-present → present なので TLB の shootdown は要らない |
| G2 kernel | `include/kern/pmem.h`・`src/kern/pmem.c` | `kern_firmware_map(物理, 仮想, 大きさ, KERN_PROT_*, KERN_FIRMWARE_CACHED/UNCACHED)`。他の port は HAL が `HAL_SPACE_SYS` を拒むので EINVAL |
| driver | `src/drivers/platform/pcat/uefi-variables.c/.h`、`src/kern/platform/pcat.c`（weak で呼ぶ、i386 の pcat は link しない）、`platform/amd64/vmunix.mk` | `kern_boot_handoff` で名前を受け、各範囲を `kern_firmware_map`、表の signature と header の大きさと関数の pointer が code の範囲かを確かめる。mutex・`kern_irq_disable`・fxsave64・fninit・MXCSR 0x1F80・`ms_abi` の呼び出し・fxrstor64。`hal_*`・HAL の header は使わない |
| UAPI | `include/uapi/system.h`、`src/drivers/generic/system-device.c` | ioctl 22〜24（GET・SET・NEXT）、`struct system_firmware_variable`（576 byte、ILP32 と LP64 で同じ）、root だけ、provider は weak（無い port は EOPNOTSUPP）、値は kernel の写しを通す（上限 64 KiB） |
| efiboot | `userland/base/efiboot/`（diskpart の `table.c` で GPT を読む）、`config/ci/config-amd64.mk` | `list`・`create [-l label] [-n] disk partition loader`・`delete`・`order`・`next` |
| installer | `userland/desktop/installer/admin/session.c`、`installer.h`・`flow.c`・`screens.c`、`userland/desktop/locale/ja/installer.tr` | spec §5.8 の「installer の使い方」: 導入の最後（ESP を外した後）に `efiboot create -l zedBSD /dev/<disk> <ESP の slot> /<loader の場所>` を 60 秒の上限で走らせ `STATE boot-entry XXXX|none`。S11 は entry がある時「起動順の最後に追加した」の文（ja 訳あり）、無い時は今までの文 |

- 判断（担当）: driver は system table と runtime services の表の CRC32 を見ない（firmware により SetVirtualAddressMap の後の値が揃わない事があり、Linux も見ない。signature・header の大きさ・関数の pointer の範囲で確かめる）。efiboot は amd64 の config にだけ入れた（他の port の userland を変えない）。名前の古い entry を消す（spec §5.8 の「指す partition がどの disk にも無い zedBSD の entry は消す」）は未実装（残り）。

### 確かめ

- build（warning 0）: `make ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p009.mk BUILD=build/p007 build/p007/vmunix build/p007/uefi/BOOTX64.EFI build/p007/bin/efiboot` rc 0、`amd64 vmunix check: PASS`、`BOOTX64.EFI check: PASS`。installer の `screens.o`・`flow.o`・install-admin の `session.o`（`ZEDBSD_IMAGE_PROFILE=installer BUILD=build/p007i`）rc 0、warning 0。`tools/i18n/tr.py check` 242 entries 0 problems。
- host: handoff の v8 の分類と runtime の検査の 13 の場合を scratchpad の C の試験で PASS（v7 の flag に runtime が立つと INVALID、記述子が allocation の外・小さすぎる・多すぎる、窓の外・逆順・頁境界でない・system table を含まない、を拒む）。
- QEMU（OVMF 4M の VARS の写し、KVM、q35、NVMe、SSH だけで判定）: `plan/ws307/tests/p007-efiboot.sh build/p007/native.img <f69189c7e の BOOTX64.EFI>` PASS（`build/ws307-test/p007-efiboot.r0b3Hw`、image は c4bedbcf5 の source、`config-amd64-p007.mk`）:
  - create: driver は 5 つの範囲で起動（kernel の log の 1 行を SSH の dmesg で見た）。`efiboot create -n nvme0n1 1 '\EFI\BOOT\BOOTX64.EFI'` → Boot0003、`list` で `Boot0003* zedBSD HD(1,GPT,…,0x800,0x20000)/File(\EFI\BOOT\BOOTX64.EFI)`、BootOrder の末尾、BootNext 0003、同じ path でもう一度 → 同じ Boot0003。
  - after（同じ VARS で起動し直し）: BootCurrent 0003（その entry で起動）、entry は残り、BootNext は使われて消えた。delete → 一覧と BootOrder から消えた。
  - gone（もう一度起動）: 消えたまま。
  - nouefi（ESP の loader を f69189c7e の v7 の loader に替えた image）: kernel は起動し SSH が答え、`efiboot list` は `Operation not supported`（名前が NULL）。
- QEMU の 2 回目（全部、最後の source 178c8517b の image）: `ZEDBSD_VARIANT=hybrid` の image（`plan/tools/guest/test-image.sh plan/ws307/tests/config-amd64-p007.mk build/amd64 ZEDBSD_VARIANT=hybrid`）で `sh plan/ws307/tests/p007-efiboot.sh build/p007/hybrid2.img <f69189c7e の BOOTX64.EFI> build/p007/hybrid2.img` PASS（`build/ws307-test/p007-efiboot.j32eXu`）: 上の 4 つの起動に加え、SeaBIOS（`-machine pc`、IDE。SeaBIOS は NVMe からこの image を起動しない）で起動し SSH が答え、`efiboot list` は `Operation not supported`。
- 他の port（warning 0）: `make -j16 ZEDBSD_CONFIG=config/ci/config-<port>.mk BUILD=build/p007-ports/<port> …/vmunix` が pcat・pc98・intelmac・armv7-virt・zybo・m68k-virt・ppc32-mac99・ppc64-pseries・riscv32-virt・riscv64-virt・visionfive2・rpi4・rpi5・odroid-c5 の 14 で rc 0、warning 0（`pmem.c`・`system-device.c` を含む）。CI の config の無い sparcv9・x68k・mips32/64-malta は build していない。
- 未実施: install-admin の Boot#### の導入の QEMU（installer-image の build が要る）、実機（release の UAT）。
