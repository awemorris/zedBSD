# ws025-p001 results

Date: 2026-09-07  
Status: completed — Queue q088  
Baseline HEAD: `dd4f31c`; working-tree changes are retained without a commit.

## 実装したもの

[観測 API と所有権の契約](../observability.md) を production code に接続した。
syscall、再入を含む file transfer、UFS1/2、buf、leaf driver、BIO 完了、
loop、BOT CDB、DMA と USB staging の確保/解放/保持を別計数する。
RAM は range 合計・最高終端・直接マップ・allocator 公開量を分離した。

`sysctl vfs.io.stats` と `sysctl hw.memory.stats` は読取り専用。
syscall 上限、write-through、buffer line、USB 上限、IMOD は変更していない。
新しい共通 counter module を各 platform と既存 host fixture のリンクへ追加した。

実行中に sysroot の public header 更新が kernel object の依存に入っていないことを
確認した。最初の native API-size check が不一致を検出し停止したため、
x86 kernel と amd64 user object を sysroot 完了 stamp に依存させた。
更新後は手動の clean/強制再コンパイルなしで新しい ABI を使って再構築される。
失敗した探索 run は削除せず、最終結果と分けて保存した。

## 検証

| 検証 | 結果・範囲 |
| --- | --- |
| Counter host | ordinary / ASan+UBSan PASS。4 thread の更新、64-bit bytes、live snapshot、無効 event ID を確認。 |
| UFS counter host | UFS1/2 の ordinary / ASan+UBSan PASS。full/partial/error の data/metadata 呼出し数と bytes を production 実装へ照合。 |
| UFS metadata audit | UFS1 は6010、UFS2 は6595 checks、各 ordinary / ASan+UBSan PASS。共有 block、失敗時 unlock、allocation/indirect/truncate/rollback の既存契約を維持。 |
| FS50 | q088 の fresh 実行で **50/50 PASS**。USB-root の二回起動、MAP_SHARED、pipe/vector、wifi-store と再起動後の内容も含む。 |
| Wi-Fi30 | FS50 runner 内で ordinary / ASan+UBSan とも **30/30 PASS**。実 RF 試験ではない。 |
| BOT no-medium | ordinary / ASan+UBSan / analyzer とも165 checks PASS。 |
| Supported build | `make -j16 ZEDBSD_CONFIG=config/ci/config-{amd64,pcat,pc98}.mk` を直列実行し、全て exit 0。 |
| Native observation | QEMU xHCI USB-root / OVMF / SMP4、512 MiB と4 GiBで API の sizing/短い出力/書込み拒否、内容照合、各202反復 PASS。 |
| Sample oracle | 全404 samplesの連番、35 event、syscall 4回/262144 bytes、エラーなし、loop/physical bytes の対応、quantile 再計算 PASS。 |

FS50 の後に UFS 計数 wrapper の NULL disk guard を補った。これは引数拒否を元の
disk API に任せるための観測上の修正で、UFS audit、三 x86 build、最終 native
観測はその後の source で実行した。古い q086/q087 の PASS を新結果へ流用していない。
追加リンクのみを補った全ての歴史的 runner を再実行した、という主張はしない。

## 時間と分割の baseline

QEMU は TCG、IMOD=4000、native UFS block=8 KiB、buf line=4 KiB。
各 workload は初期確保・fsync 後に **4 × 64 KiB pwrite + 最終 fsync** を101回。
mode 0 は同じ offset=0、mode 1 は異なる連続256 KiB。測定外で内容を照合した。

| RAM / workload | n | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: |
| 512 MiB / 同一 offset | 101 | 20 ms | 30 ms | 30 ms |
| 512 MiB / 連続256 KiB | 101 | 20 ms | 30 ms | 30 ms |
| 4 GiB / 同一 offset | 101 | 20 ms | 30 ms | 30 ms |
| 4 GiB / 連続256 KiB | 101 | 20 ms | 30 ms | 30 ms |

時計粒度は10 ms。これは実機USBの性能保証ではなく、計数を有効にした現行の比較基準。
mode 1 の一反復では次の書込み数だった。

| 層 | calls | requested / completed bytes |
| --- | ---: | ---: |
| syscall → file transfer | 4 | 262144 |
| 全 file transfer（overlay/loop 再入込み） | 84 | 835584 |
| UFS content block write | 32 | 262144 |
| UFS 全 write（content以外も含む） | 38 | 311296 |
| buf write（UFS と backing FAT の合計） | 114 | 622592 |
| leaf driver write（loop と物理の合計） | 152 | 622592 |
| loop backing write | 76 | 311296 |
| USB WRITE(10) | 76 | 311296 |
| USB SYNCHRONIZE CACHE | 3 | 0 |

全152 BIO write は完了し、error completion は0。leaf を「物理デバイスのみ」と
読み替えない。32 content block と6個の他の UFS write が4 KiB単位へ分割され、
loop/FAT を経て USB へ到達している。read 系の一部は directory content も含み、
バックグラウンド/名前解決の活動で変動する。

warm 64 KiB read は syscall 1回、file 2回、UFS content 8回、USB read 0回。
新規256 KiB create/write/fsync の一回観測は USB WRITE(10) 352回、
1441792 bytes、flush 5回。初期化・metadata・directory 操作も含む値であり、
純粋な user data 転送量ではない。

## メモリ baseline と既知未達

| QEMU RAM | BSP usable 合計 | usable 最高 end | map span | allocator 初期公開 |
| --- | ---: | ---: | ---: | ---: |
| 512 MiB | 478257152 | 535031808 | 1073741824 | 477605888 |
| 4 GiB | 4236353536 | 6442450944 | 1073741824 | 1051602944 |

どちらも全種別の最高 end は1099511627776。これは MMIO 等を含む address extent で、
RAM 容量ではない。4 GiB guest の usable 最高 end が6 GiBなのも穴と高位配置による。
現在の physical_total/span に予約や穴が含まれる事実を隠さず、新しい合計と並記した。

| Acceptance ID | p001 時点の判定 |
| --- | --- |
| MEM01 | UEFI の高位情報は観測できるが v6 / boot reservation round trip は未実装。p002 へ。 |
| MEM02 | BIOS は旧 scalar handoff。E820 全 range は未実装、BIOS runtime の新しい PASS は付けない。 |
| MEM03 | sparse map の usable 合計と最高 end、map/allocator の差を区別できる。高位 RAM の通常利用と全 RAM accounting は p004/p005 に残る。 |
| IO01 | syscall 64 KiBは一回。warm read と既存 overwrite を測定。FS/USB の一回化は未達。cold-read 全条件は後段で実行する。 |
| IO02 | new allocation の既存 failure ordering と総量を観測。CG/super/inode/zero の transaction 別最適化・細分化は p008/p010/p011 の未達として残す。 |
| IO03–04 | 六 syscall の size/partial/error、UFS indirect/truncate/初期化、native content の現行契約は回帰 PASS。run 境界の最終受け入れは後段へ。 |

p001 は「基準と観測の実装」の完了であり、これら ID 全体や WS025 の完了ではない。
次は p002 の typed handoff。物理実機、全 RAM allocation、高位 DMA、write-back、
async BIO はこの Phase の PASS に含めない。

## 証拠と再実行

- [build commands/results](../temp/p001/final-builds.json)
- [native commands/results](../temp/p001/final-runtime.json)
- [512 MiB named summary](../temp/p001/accepted-512/summary.json)、[4 GiB named summary](../temp/p001/accepted-4096/summary.json)
- [counter/UFS host](../temp/p001/observation-host.log)、[UFS audit](../temp/p001/ufs-audit.log)、[BOT](../temp/p001/usb-no-media.log)
- [FS50 log](../temp/p001/storage-final.log)、[individual results](../../ws018/temp/q088-storage-final/results.json)
- [source/config hashes](../temp/p001/source.sha256)、[artifacts](../temp/p001/artifacts.sha256)、[QEMU version](../temp/p001/qemu-version.txt)
- [fixtures](../tests/README.md)。各 native directory 内に QEMU argv、guest log、raw samples、元 image の SHA-256 がある。

元 image の hash は各 run の前後で一致。破壊的書込みは disposable copy のみ。
実行した command は上記 JSON と fixture runner に保持している。
