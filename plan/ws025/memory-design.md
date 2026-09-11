# amd64 の全 RAM 利用: 詳細設計

日付: 2026-09-07。現行 HEAD `dd4f31c` を静的確認した設計。実装結果ではない。

Parent: [WS025](ws.md)。所有 Phase: p002–p005、p016、p023。

## 1. 確認できた現状

| 箇所 | 現在の動作 | 方針 |
| --- | --- | --- |
| [defs.h](/home/awe/zedBSD/src/hal/amd64/defs.h:19) | image と direct map が `0xffffffff80000000` を共有し、`AMD64_DIRECT_LIMIT=0x40000000` | image base と RAM map base を分離 |
| [BSP boot](/home/awe/zedBSD/src/hal/amd64/bsp-pcat/boot.c:166) | BIOS の mem_upper と UEFI の usable 最高終端を 1 GiB で切る | 正規化した range と明確な集計へ移行 |
| [UEFI normalizer](/home/awe/zedBSD/bootloader/uefi/memory-map.c:89) | base/size は 64 bit。1 GiB 切捨てなし。BootServices/Loader memory は RESERVED に統合 | 高位情報を保持。再利用時期を表す型を追加 |
| [UEFI bootstrap](/home/awe/zedBSD/bootloader/uefi/bootx64.c:877) | 低位 transition と image 用の初期 1 GiB map | 全 RAM の上限ではない初期 map として維持 |
| [BIOS Stage 2](/home/awe/zedBSD/bootloader/pcat/bootzbsd.S:98) | INT 15h AH=88h の AX を mem_upper に渡す。E820 の range 引渡しはない | E820 の 64-bit range を渡す。mem_upper を全 RAM の代用にしない |
| [page allocator](/home/awe/zedBSD/src/hal/amd64/page.c:21) | 1 GiB 分の静的二 bitmap、32-bit page count、先頭から IRQ-off 走査 | range ごとの動的管理と制約付き探索 |
| [system map](/home/awe/zedBSD/src/hal/amd64/space.c:140) | 一つの PD の 512 個の 2 MiB page で恒久 1 GiB map | firmware RAM range を列挙して page table を構築 |
| [user map](/home/awe/zedBSD/src/hal/amd64/space.c:702) | user page の physical range も 1 GiB 未満に制限 | 正規化された管理 RAM と実 mapping の検証へ変更 |
| [DMA allocation](/home/awe/zedBSD/src/drivers/dma.c:169) | 通常 RAM を確保後、address_bits 超なら解放して失敗 | 最初から DMA mask 内を探索 |
| [AP trampoline](/home/awe/zedBSD/src/hal/amd64/ap-trampoline.S:26) | long mode 前の CR3 を 32 bit でロード | AP 起動用 root は 4 GiB 未満に予約 |
| [kernel linker](/home/awe/zedBSD/platform/amd64/vmunix.ld:15) | image の固定 VMA/LMA と初期 map の到達範囲を検証 | image の制約を RAM 総量の制約と混同しない |

既存の [BR-T24 high-memory boot](../ws003/tests/uefi-high-memory-usb-boot.sh) は 4/8/16 GiB 設定の boot と高位 RSDP を確認する。allocator がその RAM を使った証拠ではないため、本 WS は別に高位 PFN の実使用を検証する。

## 2. アドレス空間を分離する

4-level paging を維持し、次の仮想領域を予約する。ここでの窓サイズは仮想アドレスの配置であり、全域を物理確保する意味ではない。

| 用途 | 仮想範囲・方針 |
| --- | --- |
| user | 既存の下位 canonical half。既存 ABI を維持 |
| RAM direct map | `[0xffff800000000000, 0xffffc00000000000)`、64 TiB の窓。PA を offset とし、実在し型を確認した RAM だけ present |
| kernel vmap | `[0xffffc00000000000, 0xffffe00000000000)` を予約。p023 までは未割当て |
| 将来用 | vmap 終端から既存 kernel 固定窓の手前まで未割当て |
| kernel image | 既存 `0xffffffff80200000` の VMA を維持。loader/ELF の意味を変えない |
| ACPI / MMIO / framebuffer / ECAM | 既存の専用窓を維持。RAM の cacheable map と alias を作らない |
| 低位 identity | BSP transition と AP trampoline のためだけに限定。初期 1 GiB identity を恒久的な RAM 上限にはしない |

この配置は現在の image/MMIO 窓を拡張して衝突させる方法を避けるための設計判断である。物理終端が CPU の実装幅またはこの窓を超える場合は、検出範囲と理由を報告して未対応とする。1 GiB/4 GiB へ黙って切り捨てて成功扱いにはしない。64 TiB 超や LA57 は新しい配置を必要とする別拡張であり、今回の実機に設定する cap ではない。

CPU の物理幅は現行 `cpu_physical_max()` の CPUID 0x80000008 判定を共通化して利用し、PTE mask、range 終端、加算/丸めを同じ上限で検証する。CPUID leaf がない場合の既存 fallback と、実測した利用範囲を区別して記録する。

変換を次の三種類に分ける。

- `kernel_image_to_phys`: linker の VMA/LMA 内だけを変換。静的 page table、trampoline の source、kernel symbol に使用する。
- `phys_to_direct` / `direct_to_phys`: RAM map の present range を確認する。holes、MMIO、overflow、別の仮想窓は拒否する。
- 汎用 kernel VA の物理ページ取得: p023 の vmap を含む場合は page walk/descriptor を用いる。単純な base 減算に戻さない。

全利用箇所を変換種別で点検する。`system_pml4` 等の静的オブジェクトに新しい direct base を引くバグ、VGA `0xb8000` を RAM と扱うバグを防ぐ。VGA/ROM は専用の適切な cache 属性の窓を使う。kernel text/rodata を RAM 側で writable に alias せず、初期 trampoline の executable 範囲も限定する。

user address space が現在コピーする `pml4[511]` 一個だけでは新しい RAM/vmap 窓を継承できない。kernel 用 upper-half root を全 space で共有する設計へ改め、process teardown は共有 table を free しない。後から追加する vmap table の公開と SMP shootdown も同じ規則に従う。

## 3. loader → kernel の memory contract

### ZBL6 v6

維持対象 BIOS/UEFI loader と kernel を一緒に更新し、v6 の型付き map と boot 所有範囲を導入する。BIOS/UEFI の v5 prefix は保ち、form と size を明示して拡張を解釈する。既存 framebuffer、UUID、boot parameters の offset を壊さない。v6 の byte layout、offset/static assertions は p002 の最初に header と C/assembly fixture で固定する。

| 情報 | 要件 |
| --- | --- |
| memory ranges | 64-bit base/size、entry size/count、型、属性。未使用域は範囲の穴として扱う |
| boot allocations | kernel、handoff/map 本体、bootstrap tables/stack/trampoline、loader 作業領域の PA/size と解放時点 |
| firmware source | BIOS E820 / UEFI を区別。E820 が無い legacy loader は degraded と明示 |
| diagnostics | usable/reclaimable の合計、最高 RAM 終端、range 数、map/allocator 公開量。単一 mem_upper だけを真実にしない |

旧 v1–v5 は検出して既存の保守的な読取りを維持する。旧 BIOS の scalar memory から 4 GiB hole や高位 RAM を推測しない。新しい通常 image は v6 を必ず使い、legacy fallback を全 RAM 対応の PASS に数えない。

map の格納アドレスが初期 map 内にあることと、map に**記録する PA**が高位であることは別である。handoff/map の低位配置制限は bootstrap の到達性として残せる。kernel 所有へコピーした後は、元 buffer を lifetime に従って解放する。

### BIOS E820

INT 15h E820 を continuation token が終わるまで収集する。signature、返却長 20/24 bytes、token の前進、無効 entry、zero size、64-bit overflow、capacity を確認し、途中欠落を成功扱いしない。usable はページ内側、予約領域は外側へ丸め、overlap は予約側を優先して正規化する。未知の型は reserved とする。出力は昇順・非重複の共通 range へ変換する。

初段の上限は現在と同じ正規化済み 256 ranges とするが、overflow は診断して失敗し、末尾を捨てない。大きい descriptor 数が実機で必要なら、table 容量と loader layout を同時に増やす。AH=88h/E801 は E820 不可時の明示的 legacy fallback に限り、全 RAM と表示しない。BIOS の range interface は [ACPI System Address Map Interfaces](https://uefi.org/specs/ACPI/6.5/15_System_Address_Map_Interfaces.html) に従う。

BIOS loader は i386 と共有されている。ELF64/v6 handoff の拡張で i386 Multiboot、FAT traversal、stage2 size/checksum、DOS relocation、PC-98 の別 loader を壊さない。

### UEFI の再利用可能領域

現行 normalizer の高位 range 保持を維持し、BootServicesCode/Data と LoaderCode/Data を永続 reserved へ潰さない。v6 では解放待ち RAM を区別し、ExitBootServices 成功後に boot 所有領域を差し引いて段階的に解放する。RuntimeServices、NVS、MMIO、unusable/unaccepted、未知の型はそのまま保持する。ACPI reclaim も現在の参照者が離れるまで解放しない。[UEFI 2.10A §7.2, ExitBootServices 後の memory type](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html#memory-allocation-services)

final GetMemoryMap と ExitBootServices の間に割当てを挟まない現行契約を維持する。古い kernel と新しい reclaim 型の組合せを許さず、loader/kernel の artifact identity を試験する。旧 v5 producer/consumer は既存の reserved 規則を保つ。

## 4. boot-time allocation の循環を解く

現在は `amd64_page_init()` → `amd64_space_init()` の順である。新 allocator が返す高位 page をまだ map していない状態は許さない。以下の順へ分割する。

1. boot map を検証・コピーし、kernel/loader/firmware の所有範囲を固定する。通常 allocator はまだ非公開。
2. 初期 map で触れる usable RAM から early arena を予約する。kernel heap や通常 reclaim を呼ばない。AP 用 root は 4 GiB 未満に固定する。
3. RAM direct map の table を early arena で作る。2 MiB の完全 RAM・同一属性の範囲は large page、端と mixed/reserved 境界は 4 KiB leaf とする。1 GiB huge-page 対応は必須にしない。
4. arena が足りないときは、一時 mapping 窓または中間 CR3 への切替えで実際の到達性を確認した RAM を段階的に early allocator へ追加する。未使用の table に entry を書いただけの page を dereference しない。構築に必要な量を算出し、確保不能なら具体的な不足 bytes を出して停止する。
5. CR3 を切り替え、kernel・stack・console・handoff・AP trampoline の到達性を確認する。最初の試験段階では高位 RAM を通常 allocator にまだ公開しない。
6. direct-map 上に range 別 allocator 管理情報を構築し、early arena/table 使用分を予約のまま移管する。新旧 descriptor の VA/PA が混ざらないよう一度だけ所有者を移す。
7. DMA/VM の高位対応が揃った p005 で全 usable RAM を通常公開する。loader の不要部分はこの後に解放する。AP・既存 space の参照が残る table/identity を先に消さない。

初期 map は boot に必要な限定領域でよく、loader が全 RAM の page table を二重構築する必要はない。loader の初期 1 GiB map と kernel image のロード範囲検証は、この役割を明記して残す。恒久 map と利用可能 RAM の 1 GiB cap は撤去する。

## 5. allocator と DMA

管理単位は正規化済みの実在 RAM extent とする。各 extent に基準 PFN、page 数、free/reserved bitmap と free count/rotor を持ち、holes に巨大な bitmap を割り当てない。PFN・page 合計・rounding と中間積は 64 bit/checked size_t で計算する。管理情報の必要 bytes は事前算出して予約する。

`physical_total` は管理 RAM の合計とし、highest physical end を RAM 容量と表示しない。`total = reserved + allocated + free` を常に満たす。firmware 報告の usable/reclaimable 合計と、まだ公開していない量・map table bytes・DMA32 reserve は別に表示する。RAM 上の予約と address hole を混同しない。

初段は extent/word の rotor と free-run summary で全域先頭走査を避ける。探索量と IRQ-off を計測する。lock 外探索へ進める場合は bitmap の読み方・世代・候補の lock 内再検証・失敗時の retry budget を定義し、未同期の bit 読取りは入れない。

DMA には制約付き allocation API を追加する。既存 `hal_pmem_request` に未初期化の新 field を足すのではなく、例えば新しい `hal_pmem_alloc_range(request, min_pa, max_pa_exclusive, result)` を用意し、既存 API は全範囲の wrapper とする。境界の表現・64-bit 上端は API fixture で固定する。全 HAL の互換実装も範囲を満たすか検査し、満たさない allocation を成功返却しない。

- amd64 は DMA32 と高位 normal RAM を分けて探索し、通常 VM/cache は高位を優先できるようにする。低位 DMA reserve は計測値と active device の必要量から設定する。
- `drv_dma_alloc_coherent()` は device の address_bits、alignment、segment_boundary を allocator に渡す。確保後に高位だったら失敗、という探索方法をやめる。
- streaming map は現行 capability 内に限定し、対応外は bounded low-address bounce または明示的な失敗を返す。高位 address を下位 32 bit に切り詰めない。
- xHCI の controller address 幅、EHCI/UHCI/OHCI、NVMe、AX211 等の descriptor を確認し、機器固有の実効 DMA mask を守る。全 driver の無条件 64-bit 宣言にはしない。
- `hal_space_map` の物理範囲チェック、page table の確保、VM page descriptor/accounting、swap/cache の PA/size 変換を点検する。32-bit x86 は既存の利用範囲を維持する。

## 6. 高位 RAM の受け入れ

通常 boot が通るだけでは不足する。synthetic map と production allocator の fixture に加え、guest で bounded な test allocation を **1 GiB 超および 4 GiB 超の PA に強制**し、書込み・読戻し・user map・fork/COW・unmap・再割当てを確認する。全 RAM を埋めて偶然高位へ到達させる必要はない。

SeaBIOS/OVMF、256 MiB/1 GiB/2 GiB/4 GiB/8 GiB/16 GiB を基本行列とする。8/16 GiB の少なくとも両 firmware で高位 PFN、SMP 4 CPU、USB-root を確認する。実メモリ穴、最高終端と合計の差、複数高位 extent、予約 overlap、CPU 幅超過、256 entry 超、low arena 不足を synthetic fixture で扱う。

DMA32 は低位 pool の使用を強制し、高位 normal page が割り当たっても USB/WLAN 等が正常に動くことを確かめる。SMP の AP 起動と全 address space からの kernel map、TLB 更新、ACPI 高位 table、MMIO cache 属性、framebuffer を維持する。旧 loader の degraded 起動は別の互換セルにし、全 RAM の成功数へ足さない。

実機の reported/managed/mapped/free bytes と高位 page 使用の証拠を残し、過去に起動できなかった機種でも fresh image を一度確認する。実機未確認なら automatic complete と physical pending を分ける。

## p016 implemented policy and controls

`vfs.cache_memory.stats` separates page-rounded resident and pending ownership for
file data (including mandatory object mappings), descriptor slabs, block data and
headers, I/O pool, coherent DMA and dedicated worker scratch. Physical-free RAM is
sampled separately from this ownership snapshot. Kernel virtual reservations and
RAM holes are not credits. Private VM consumption remains reflected in physical
free RAM and its existing VM accounting, rather than being double charged here.

The default soft target is page-rounded managed RAM / 4; the physical free floor
is managed RAM / 64 bounded to 64 KiB–8 MiB (and reduced on tiny RAM). Root may set
`vfs.cache_memory.target_bytes` up to managed RAM minus that floor. Mandatory pool,
DMA and mapped object frames can exceed the target and then exclude optional
cache admission. Shrink uses a pending gate and clean-only reclaim; EBUSY restores
the published target. Clean copies already reclaimed need not be recreated.

File metadata resides in reclaimable page slabs and uses an intrusive balanced
index. The former 16-page retention cap is removed; 16 pages still bound a single
read preparation. The cache-object count remains 32. Empty cache objects close at
explicit lifecycle checkpoints, never from the allocator pressure path. The
worker reserve is 64 KiB of payload plus one control page, independently borrowed
without waiting; it is visible in the worker category and does not enable delayed
writeback. Dirty per-device budgets and dispatch fairness belong to p018.
