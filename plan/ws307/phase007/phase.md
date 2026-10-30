<!-- awesome-plan project=zedbsd record=ws307-p007 -->

# ws307-p007 UEFI の runtime services と Boot####

- 親: [WS307](../ws.md)。仕様: [spec.md](../spec.md) §5.8（U5・U15）。Queue: q1665 / q1665-i01（担当 P4、2026-10-29）。
- Status: in-progress（改訂 5 の案 [proposed/uefi-runtime-window.md](../proposed/uefi-runtime-window.md) でユーザーの判断待ち（2026-10-30 q1711-i01。改訂 4 はユーザーの「ドライバから見て、HALは使えません。…」で作り直し）。code はまだ無い）
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
