# WS025 受け入れ項目と実行行列

日付: 2026-09-07。状態: 設計済み・未実行。Parent: [WS025](ws.md)。

## 判定の仕方

以下の ID は再利用可能な確認項目であり、一項目ごとに guest を再起動する指定ではない。正常・準正常の file/cache/allocator 試験は同じ起動で連続実行し、各項目の前後で owner/dirty/FD/予約の基準状態を確認する。boot memory と crash/replay のセルだけ必要な再起動を行う。

各 ID の outcome、対象 Phase、source/config、実行 command、counter、失敗時の残状態を結果に残す。baseline の既知未達、未実行、非対応、PASS を区別する。high-memory テストが login だけで PASS、write-back テストが write の返値だけで PASS、という oracle は採用しない。

複数 Phase にまたがる ID は累積の到達条件を示す。初期 Phase は今回変更した層の条件を満たし、未実装の下位層は baseline と比較して残条件を記録する。例えば p006 では 64 KiB の file transfer 一回を受け入れ、USB command 一回の条件は p007–p009 統合後に判定する。後段未実装を初期 Phase の失敗と混同せず、ID 全体の最終 PASS は全前提が揃った時点だけで付ける。

## MEM: memory/handoff/direct map

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| MEM01 | p002 | UEFI の 1/4 GiB を跨ぐ usable/予約 range が v6 round trip で保持される。 | 64-bit base/size/型と boot reservation が一致する。 |
| MEM02 | p002 | BIOS E820 の 20/24-byte、継続 token、1/4 GiB 超 range を読む。 | 全 range を保持し、scalar 値へ縮約しない。 |
| MEM03 | p001/p005 | highest end と usable/managed 合計が異なる sparse map を使う。 | hole を RAM 容量に加算せず、各差分を説明できる。 |
| MEM04 | p002 | zero/overlap/非整列/overflow/未知型/256 entry 超を注入する。 | 予約側優先の正規化または診断。黙った末尾切捨てなし。 |
| MEM05 | p002/p005 | v1–v5、新 v6、旧新 loader/kernel 不整合を試す。 | 対応済みのみ受理。legacy degraded を全 RAM の成功にしない。 |
| MEM06 | p003 | 初期 arena 不足と table 確保途中失敗を起こす。 | 未 mapping page を触らず、不足量を示す。予約が重複しない。 |
| MEM07 | p003/p004 | 2 MiB 境界に RAM/hole/MMIO/予約を混在させる。 | large page が属性境界を跨がず、穴は allocation されない。 |
| MEM08 | p003/p023 | image/direct/vmap/MMIO の VA→PA と窓上端を試す。 | 正しい窓だけ変換。overflow と別窓への単純減算を拒否する。 |
| MEM09 | p003/p005 | 全 address space と AP から kernel map を利用する。 | 共有 table の寿命、低位 AP CR3、TLB、text/rodata 属性を維持する。 |
| MEM10 | p004/p005 | PA >1 GiB の bounded allocation/map/readback/free を強制する。 | PFN と検証値を記録。低位 allocation で代用しない。 |
| MEM11 | p004/p005 | PA >4 GiB の user map、fork/COW、unmap/realloc を強制する。 | 内容と高位 bit を保持し、終了後 accounting が戻る。 |
| MEM12 | p004/p009 | 高位 normal RAM がある状態で DMA32 allocation を要求する。 | 空き低位 RAM から mask/boundary 内に割り当たる。 |
| MEM13 | p004 | 低位 DMA 枯渇、alignment、concurrent alloc/free を試す。 | bounded な失敗、二重割当てなし、PA truncation なし。 |
| MEM14 | p005 | ExitBootServices 後に boot 所有領域を順に解放する。 | loader/page table/ACPI/Runtime/NVS の早過ぎる解放なし。 |
| MEM15 | p005/p016 | 256 MiB/1/2/4/8/16 GiB の firmware 行列を走らせる。 | boot のほか reported/managed/mapped と high PFN 使用を照合する。 |
| MEM16 | p005/p026 | 高位 RAM、USB-root、DMA32、SMP、WLAN/HID を併用する。 | I/O/IRQ/回復が退行せず、実機と QEMU の結果を分ける。 |

## IO: 粒度/pool/run

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| IO01 | p006–p009 | 連続 64 KiB の cold/warm read と既存 full overwrite。 | 上位 transfer・FS run・leaf BIO・USB data command を別計数。条件内では各一回。 |
| IO02 | p008/p011 | 新規 64 KiB と複数 batch の create/write。 | data/zero/CG/super/inode/flush を分離し、必要な順序を維持する。 |
| IO03 | p008/p011 | aligned/partial append と indirect/CG 跨ぎ。 | 正しい size/offset、初期化、short result。理由のある境界だけで分割する。 |
| IO04 | p006–p008 | 1 B、511/512/513 B、4095/4096/4097 B、64 KiB 前後、EOF/hole。 | 隣接データと現行 syscall 返値を維持する。 |
| IO05 | p006 | 大小 pool をすべて貸出し、exec/通常 I/O を競合させる。 | 小 reserve/最終 fallback で前進し、lease 内の pool 待ちなし。 |
| IO06 | p006 | 六 syscall、iovec、PIPE_BUF、EFAULT、signal、backend partial/error。 | offset/atomicity/short result と全資源返却。 |
| IO07 | p007 | run 内に busy line を置いて順番を変える。 | 取得済み line を解放して短縮し、循環待ちを作らない。 |
| IO08 | p007 | line 準備中に reclaim/resize と loop 再入を起こす。 | 上位所有 line の解放を自分で待たない。 |
| IO09 | p007/p008/p012 | fragmented LBA、cache hit/miss、FAT extent 境界を混在させる。 | 実 mapping に沿って分割し、内容を保持する。 |
| IO10 | p007 | run の途中成功/失敗/結果不確定を注入する。 | 未完了を clean にせず、後の sync に必要な内容を残す。 |
| IO11 | p009 | 64 KiB 対応 HCD と 8 KiB 互換 HCD/予約失敗を試す。 | 実効上限と CDB/TRB 長を一致させる。 |
| IO12 | p009/p026 | 複数 storage と reclaim reserve、timeout 隔離を競合させる。 | normal 枯渇が emergency reserve を奪わず、実 bytes を計上する。 |

## META: metadata

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| META01 | p010 | allocation adapter の begin/allocate/commit/abort を通す。 | 保留なし段階は現行返値/順序/counter と同等。 |
| META02 | p010 | 初期 active_cg=0、CG switch、cache miss/hit を試す。 | valid と世代を持つ場合だけ hit。 |
| META03 | p010 | indirect cache に truncate/free/reuse/rollback を重ねる。 | stale pointer を使わず、inode 数比例の無制限常駐なし。 |
| META04 | p011/p013 | allocation の各永続化境界で fail/crash。 | 未初期化参照/二重割当てなし。未到達 allocation の recovery を明示。 |
| META05 | p011 | 全面 data の新 block と部分 block を書く。 | 前者は zero 省略可、後者の隙間は初期化される。 |
| META06 | p011/p013 | ENOSPC、batch 上限、CG/sector slot 跨ぎ、部分成功。 | checked short result と残りの予約/旧像の整合。 |
| META07 | p011/p013 | pointer 公開後の abort/clear/flush を失敗させる。 | 不確定 block を free にせず、保留/書込み停止を報告する。 |
| META08 | p012/p013 | 別 open の chain extend/truncate と cursor を競合。 | 共有 generation 変更で cursor/検証結果を破棄する。 |
| META09 | p012/p013 | FAT table/mirror/directory の更新と rollback を注入。 | 操作内の順序を守り、不確定 chain を公開/再利用しない。 |
| META10 | p012/p013 | 直接 FAT と loop backing に clean slot と同期 batch を適用。 | claim/coherence を維持し、sector 往復と chain 歩数を測る。 |

## FLUSH: 永続化 frontier

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| FLUSH01 | p014 | write なしの同じ世代で fsync を反復。 | 証明済みの重複 flush のみ省略する。 |
| FLUSH02 | p014/p019 | 先の write が未完で後の write が完了。 | completed frontier は穴を跨がない。 |
| FLUSH03 | p014 | flush 中に新規 write と別の fsync を開始。 | 後続世代を先の flush 成功範囲へ含めない。 |
| FLUSH04 | p014/p025 | partial/unknown write、flush error、reset を注入。 | stable 証明を適切に無効化し、エラーを上位へ返す。 |
| FLUSH05 | p014/p017 | 上位に dirty があるが leaf に新 write がない。 | 論理 dirty を先に drain してから leaf の証明を使う。 |
| FLUSH06 | p014/p021 | overlay journal、temp fsync、rename、dir fsync を通す。 | 異なる永続化境界を合流させない。 |

## CACHE: file cache と回収

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| CACHE01 | p015 | read→最後の close→再 open/read。 | cache reference が保持し、予算内 warm data は device read ゼロ。 |
| CACHE02 | p015 | mapping/op/cache の最後の参照を競合して落とす。 | object/inode/mount を早期 free せず、最後には回収する。 |
| CACHE03 | p015 | read/write/MAP_SHARED/別 fd と partial page を競合。 | 同じ内容世代を観測し、populate 再帰なし。 |
| CACHE04 | p015 | overlay copy-up と claimed loop backing を使う。 | lower と upper を混同せず、第二の dirty owner を作らない。 |
| CACHE05 | p015/p016 | truncate/EOF、populate fail、unmount/media change。 | stale page を返さず、参照と resident bytes を整理する。 |
| CACHE06 | p016 | working set 増加後に VM pressure をかける。 | 共通予算内で成長し、clean buf/page を回収する。 |
| CACHE07 | p016/p018 | dirty/pinned/inflight/quarantined を含む pressure。 | 回収不能分を free と数えず、worker 最低資源を残す。 |
| CACHE08 | p016/p018 | 複数 device の一台を停止し、別 device の I/O を行う。 | 単一 device の dirty が全予算を占有しない。 |
| CACHE09 | p016/p018 | cache cap 縮小、policy OFF、close/exit と回収を競合。 | drain 成功後のみ変更確定。失敗 dirty を捨てない。 |

## WB: write-back

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| WB01 | p017 | 同じ inode を独立 open した二つの fd で fsync。 | 各 observer に writeback error が届く。 |
| WB02 | p017 | dup fd と独立 open を混在。 | dup は cursor を共有し、独立 open は共有しない。 |
| WB03 | p017 | fsync が error を読んだ直後に次の error を発生。 | 未報告 error を cursor 更新で飛ばさない。 |
| WB04 | p017/p018 | 過去 error 報告後に dirty の再送も失敗。 | 過去通知済みでも今回 fsync は失敗。 |
| WB05 | p017/p018 | close/exit 後に dirty/error が残る。 | owner が寿命を保ち、後続 sync に失敗を伝える。 |
| WB06 | p018 | 小 write 反復＋最後 fsync、毎回 write＋fsync を比較。 | 前者を合成し、後者の永続化境界は保持する。 |
| WB07 | p018 | 書戻し中に同じ page を再 dirty。 | 古い完了が新 dirty を消さず、DMA 中の内容は固定。 |
| WB08 | p018 | 上位 data drain が loop/FAT を通る。 | 下位で再遅延せず、上位 metadata の意味を保持。 |
| WB09 | p018 | dirty credit 枯渇、signal、worker 資源不足。 | lease 前の bounded 待機/失敗で前進。 |
| WB10 | p018 | disable/unmount/shutdown の途中で device error。 | 未永続化を成功とせず、policy/dirty を保持して診断。 |

## ASYNC: 非同期 lifetime

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| ASYNC01 | p019 | submit の呼出元で device を意図的に遅くする。 | 受理処理は device latency を待たない。 |
| ASYNC02 | p019 | queue full/submit reject と受理後 error。 | 拒否/完了の通知規則が一意で、二重 callback なし。 |
| ASYNC03 | p019 | submit return 前の inline callback と別 CPU callback。 | 参照移管と waiter 解放後の BIO 使用を検証する。 |
| ASYNC04 | p009/p019 | cancel 成功と通常 completion を競合。 | 一回だけ完了し、retirement 後にだけ再利用。 |
| ASYNC05 | p009/p019 | cancel 失敗/遅い DMA を注入。 | request/page/map を隔離し、可変データとして再使用しない。 |
| ASYNC06 | p009/p019 | attach 途中失敗、detach と inflight。 | 予約資源を巻き戻すか保持し、無制限代替確保なし。 |
| ASYNC07 | p019/p025 | generation 更新後に旧 completion が到着。 | 新 request に誤適用せず、旧 owner を収束させる。 |
| ASYNC08 | p019 | loop/別 device/需要 read/flush を worker で混在。 | claim/context を保持し、公平性と flush 順序を維持。 |

## READ: 先読み

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| READ01 | p020 | 連続 read と cold miss を反復。 | 要求内 run と要求外 bytes を区別し、useful hit を測る。 |
| READ02 | p020 | seek/random/queue pressure/低 RAM。 | window を縮め需要 I/O を優先。 |
| READ03 | p020 | EOF/truncate/copy-up/並行 write 中の completion。 | 範囲と世代を確認し、古い先読みを採用しない。 |
| READ04 | p020 | 先読みだけの error と成功した需要 read。 | 成功範囲の返値を後から error にしない。 |

## EXEC: exec snapshot

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| EXEC01 | p006/p022 | 同じ immutable binary を複数回 exec。 | pool copy と共有の実効経路、対象 data read と text 共有を計測。 |
| EXEC02 | p022 | 非整列 PT_LOAD、BSS、末尾、権限の異なる segment。 | zero と private/COW の必要範囲を保持。 |
| EXEC03 | p022 | writable file の更新と exec を競合。 | 従来 copy snapshot の意味を保つ。共有は固定内容に限定。 |
| EXEC04 | p022 | interpreter、fork/COW、exit、exec 途中 ENOMEM。 | snapshot/object/page の lifetime と返値を維持。 |
| EXEC05 | p022 | 共有中 backing の可変化、remount、memory pressure。 | 実行中内容を変えず、pin と回収の accounting を保持。 |

## SG: 非連続 RAM / DMA

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| SG01 | p023/p024 | 非連続高位 pages を連続 VA/LBA に組み合わせる。 | vmap と SG を区別し、総長と page 所有権を保持。 |
| SG02 | p023/p024 | 境界跨ぎ、非整列、segment/TRB 超過、length overflow。 | 拒否または正しい bounded 分割。 |
| SG03 | p024 | DMA mask 外の page と未対応 driver。 | low bounce/互換路に戻し、PA truncation なし。 |
| SG04 | p024 | 直接 DMA 中の writer、cancel、late completion。 | 内容固定と mapping retirement を保証。 |
| SG05 | p024 | 同じ bytes で bounce/SG を比較。 | copy bytes/TRB/CPU/latency を実測し、LBA 断片を一 command としない。 |

## REC: 媒体/transport 回復

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| REC01 | p025 | 自前 reset の 06/29/00 と連続 UA。 | 規定回数だけ retry し、無限 reset なし。 |
| REC02 | p025 | mode change と capacity change。 | ASCQ ごとに再設定/世代変更を区別。 |
| REC03 | p025 | 06/28/00、absent、同容量の別媒体。 | INQUIRY 一致だけで旧 mount/cache を復帰させない。 |
| REC04 | p025 | 保留 BIO の control recovery と新 submit。 | 自分の drain を待たず、bounded に成功/最終 error へ進む。 |
| REC05 | p025 | idle device の世代交換と mounted/claimed device。 | 新 object 登録の条件を守り、古い namespace を付け替えない。 |
| REC06 | p025 | 旧 cache hit、旧 partition、旧 completion が残る。 | driver 以外の admission も世代で閉じる。 |

## CRASH: 永続化/replay

| ID | 所有 Phase | 操作・条件 | 合格条件 |
| --- | --- | --- | --- |
| CRASH01 | p011/p013/p021 | data 初期化前後と pointer 公開前後の crash。 | 未初期化参照と二重割当てなし。残存 allocation の扱いを明示。 |
| CRASH02 | p021 | journal commit record 前後の volatile cache 消失。 | committed と未 committed を区別して recovery。 |
| CRASH03 | p021 | write reorder と契約内 torn sector/record。 | checksum/順序/replay で検出・回復または明示停止。 |
| CRASH04 | p021 | replay/checkpoint の途中で再 crash。 | 冪等な recovery、log full/backpressure の前進。 |
| CRASH05 | p021 | rename/unlink/truncate、quota/xattr/snapshot、overlay journal。 | 各所有者の順序を維持し、旧 image を誤解釈しない。 |
| CRASH06 | p021/p026 | 成功 fsync の後の crash/remount と未 fsync の更新。 | 成功済み対象は保持。未 fsync の許される消失を別に判定。 |

## 実行環境の行列

| 環境 | 必須セル・用途 |
| --- | --- |
| host focused | production helper の boundary/failure/concurrency fixture。strict compile と適用できる ASan/UBSan。大きい sparse PA は synthetic で検証 |
| amd64 SeaBIOS | 256 MiB/1/2/4/8/16 GiB の v6 boot。8/16 GiB は 4 CPU・USB-root・PA >4 GiB 利用を含む |
| amd64 OVMF | 同じ RAM 行列。高位 RSDP/GOP、EBS reclaim、USB-root を含む |
| 旧 loader | v1–v5/degraded の互換セル。全 RAM の successful cell へ加算しない |
| amd64 storage | native/overlay、連続/断片 image、write-through/data-WB/metadata-WB。QEMU の volatile/write-order fault device または同等の production-linked backend |
| pcat/pc98 | supported build と、共通 HAL/FS 変更後の維持 runtime。32-bit ABI/低 RAM/PIO または既存 DMA 互換を確認 |
| 実機 | 現に取得できる対象機の firmware map/全 RAM/high PFN と USB-root。WLAN は AX211/RTL8822BU の既存受け入れを必要範囲で併用 |
| 条件付き device | NVMe depth、UAS、IMOD は対象実機と当該 Phase 選択後。未取得の機器の結果を仮定しない |

全組合せを無条件の巨大直積にしない。まず各機能を単独受け入れし、p026 では 8/16 GiB の両 firmware に WB/先読み/SG を組み合わせた代表セルと、256 MiB の縮退セルを選ぶ。各 cell の RAM、CPU、root、HCD、FS、policy を実行前に固定し、除外理由を記録する。

host RAM が足りない 16 GiB cell は synthetic 試験だけで runtime PASS にしない。実行可能な別環境へ移すか、runtime pending とする。高位強制 allocation は小さい bounded 作業量で足り、guest RAM 全量を書き潰す必要はない。

既存 q086 FS50、q087 syscall、VM content lease、USB retirement/recovery、Wi-Fi30 の fixture を再利用する。path と実行方法は [tests/README.md](tests/README.md) と各 Phase の実行結果に記録する。

## 性能と既定化

- 64 KiB overwrite の data command 一回は、連続配置・上限対応・予約利用可能・エラーなしの条件付き oracle。metadata/flush を隠して総一回としない。
- 新規/append は zero/CG/super/inode/mirror/journal の各 count を baseline と比較する。必要な永続化を減らして数字だけ改善しない。
- warm-up 後に反復し、p50/p95/p99、throughput、CPU、IRQ-off、free/resident/dirty/quarantined を保存する。単発 guest 10 ms 値で判断しない。
- write-back OFF/resize/drain が失敗しても、実効 policy と保持データを正しく表示する。既定化は機能・mount・device ごとに決める。
- fresh image/hash を使い、破壊可能な storage 試験は disposable copy。終了後に元 image 不変を確認する。commit と aggregate make check は行わない。

## 2026-09-07 の実機受け入れ判定

p005 中に現物の USB 起動媒体と console/recovery を確認したところ、ユーザーは手作業となる
実機動作をクリア扱いにして先へ進むよう明示した。本件の WS025 physical gate は
**user-accepted（agent runtime 未実施）** とする。p026 までこの判定を継承する。
ホスト・QEMU・native synthetic relocation の実測結果を、実機実測として書き換えない。
