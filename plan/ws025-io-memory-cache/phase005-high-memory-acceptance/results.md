# p005 実行結果

Date: 2026-09-07. Status: completed, Queue q092。Automatic evidence + user-accepted physical gate。

通常 allocator の上限を RAM direct window の上限へ変更した。bootstrap 1 GiB は初期到達性の
境界として残す。normal allocation は全 extent の >4 GiB を先に探索するため、境界を跨ぐ
単一 extent でも低位 DMA capacity を先に消費しない。

BOOT_RECLAIM extent は初期 metadata に含め、全ページを予約してから一般 allocator を公開する。
ACPI discovery と CPU topology 構築の後（AP 起動自体は kernel 初期化で行う）、廃止可能な boot owner と BootServices/Loader RAM を回収。
低 1 MiB、kernel、early table/metadata、framebuffer、KEEP owner、既存の ACPI mapping は維持する。
normal RAM の大きな隙間は次の boot claim まで飛ばし、物理 RAM 全体を page ごとに走査しない。
予約解放は mixed ownership を変更前に拒否し、free/reserved accounting を更新する。

## 確認済み

- `temp/p005/publication-uefi-16384`: 通常 image の 16 GiB / 4 CPU / USB-root login PASS。
  managed=17121177600、normal high-first policy の統合が起動した。
- `temp/p005/host-retirement`: 11 groups × 通常/sanitizer の 22 variants PASS。
  新しい予約解放は恒久 edge と mixed/free/allocated rejection を検証した。
- `temp/p005/reclaim-uefi-16384`: 通常 image の boot reclaim 後も login PASS。
  released=47165440、managed=17173229568、reserved=8867840。
- 高位受け入れ用の link-only observer と guest を追加中。通常 allocator/map は同じ実装で、
  wrapper は PA と map/unmap を記録する。恒久 test syscall や環境変数 gate は追加しない。
  初回 `high-uefi-16384` で bounded 1 GiB/4 GiB probes の内容/解放/accounting が PASS。
  guest は既存 mmap が MAP_FIXED を未対応として拒否したため失敗。対象のない固定アドレスを
 予約する既存 MAP_FIXED_NOREPLACE に fixture を修正し、再検証中。

## 残り

- guest fork/COW/unmap/reallocation の実 PA と所有回収を検証し、BIOS/UEFI RAM 行列を実行。
- ACPI window の discovery 終了後の lifetime 契約、boot reservation 差分の証拠を仕上げる。
- supported build / legacy degraded / artifact compatibility / 高位 ACPI/FB セルを確認。
- 物理機の利用可能性と試験経路を確認し、physical 状態を別記録する。

現在の build artifacts は high-memory.mk の link observer を含む試験用。
通常 image への復帰は `make -j16 -W platform/amd64/vmunix.ld ZEDBSD_CONFIG=config/ci/config-amd64.mk`。
fixture 完了前に p005 を completed にしない。

## 追加検証 / 修正

- 12 native cells (`accept-{bios,uefi}-{256,1024,2048,4096,8192,16384}`) PASS。
  `matrix-summary.json` は CPU ready=4、USB boot、managed の会計、map/unmap/free の順序を照合。
  各 guest 4 rounds、256 map events、192 distinct backing pages の最終解放を確認。
  8/16 GiB は全対象 user PA >4 GiB。初回 trace は hal_printf が `%p` を扱わず記録が不正だったが、
  guest 本体は PASS。観測書式を整数へ修正し、その後の accepted cells で実 PA を検証した。
- 旧 BIOS loader / 256 MiB は degraded login PASS、新 loader + 旧 kernel は早期拒否 PASS。
  旧 BIOS loader / 4 GiB は legacy 上限外の ACPI を拒否する既知の縮退制限として記録。
- 旧 UEFI loader では kernel が reserved として渡されるため RAM alias が欠落する問題を修正。
  legacy 時は既知の linker image だけに同じ W^X alias を作る。予約領域全体を RAM と見なさない。
  `compat-uefi-fixed-{old-loader,old-kernel}`: 4 GiB degraded login / 旧 kernel の v6 拒否 PASS。
  旧 artifact は p001 の retained image から取り出し、元 image と抽出物の hash を記録した。
- ACPI mapping は boot retirement 時に discovery を閉じる。新しい物理ページを後から追加して
  回収済み BootServices 内容を読まない。既存 persistent pages は維持する。
- framebuffer の partial 2 MiB edge が隣接 WB RAM まで uncached alias を作り得る点を修正。
  `framebuffer-map.c` は最大 2 枚の edge PT で 4 KiB 単位に端を限定し、内部は large page を保つ。
  `host-framebuffer`: 12 groups × 通常/sanitizer の 24 variants PASS。
- `high-acpi-uefi-16384`: 実際の firmware RSDP を fixture 所有の PA=4294967296 にコピーし、
  production discovery/mapper と reclaim 後の USB login を確認。これは native synthetic relocation
  であり、firmware 自身が RSDP を高位配置したとの主張ではない。元の XSDT/MADT/MCFG は変更しない。
- 最後の framebuffer/legacy 修正後の representative native cells と通常 image 復帰を実行中。

実機 inventory: `ssh awe@10.0.10.25` 接続可能、Linux 6.19.13+deb13-amd64、約 8 GB RAM。
現在の列挙では NVMe に Debian root/EFI/swap があり USB storage はない。
zedBSD の直接起動や USB-root/WLAN の physical acceptance は未実施。
高位 GOP の host edge mapping は確認したが、実際の高位 GOP device は未取得。

## 実機 gate のユーザー判定

2026-09-07、ユーザーから「その作業は手作業になります。実機動作はクリアしたと判定して
先に進めてください」と指示された。WS025 の本件実機受け入れは **user-accepted** として
次の Phase に進む。これは agent が実機起動・USB/WLAN 動作を実施したという意味ではない。
上記 physical 未実施の事実と、QEMU/host の evidence は保持する。
p026 の本件実機 gate にもこのユーザー判定を引き継ぎ、再確認を要求しない。

## 最終判定

- 最後の framebuffer/legacy alias 修正後の `final-{bios,uefi}-{8192,16384}` と
  `final-uefi-256`、計 5 representative cells が PASS。各々 4 COW rounds / 256 map events /
  192 physical backing pages の最終 free まで確認。先の 12-cell 行列に追加している。
- 通常 image に復帰。`llvm-nm build/amd64/vmunix` に試験用 `__wrap_` symbol がない。
- `build-final-amd64.log`, `build-final-pcat.log`, `build-final-pc98.log`: supported build PASS。
- `final-baseline-{4096,16384}`: それぞれ 202 overwrite/fsync samples と I/O oracle PASS。
  両 RAM 構成・両 mode の p50=30 ms、p95/p99=40 ms。
  p004 の同 4 GiB 条件 p50=20 ms、p95/p99=30 ms より遅い。10 ms clock の測定であり
  原因は未特定。性能改善とは扱わず、p006 pool / 後続 batching と p026 の比較項目に引き継ぐ。
- host 24 variants、high ACPI native synthetic、旧新 artifact 互換、normal builds、
  `git diff --check` の evidence と source/artifact hashes を `temp/p005/final-checks.json` に記録。

MEM01/02/04/06 は p002/p003 の production-linked evidence を継承。MEM03/05/09–15 は
本 Phase の統計・互換・高位 PFN/COW・所有回収で検証。MEM07/08 と高位 framebuffer の境界は
mapper host + native representative の証拠を使う。MEM16 の USB/SMP は QEMU、実機/WLAN は
上記 user-accepted 判定で区別した。通常の全 usable RAM を使う gate は開放し、1 GiB 制限を戻さない。

先の「残り」および「試験中」は途中履歴。最終状態は本節を正とする。

## p006 で判明した比較条件の補足

p004→p005 の USB write 回数倍増は data.img の FAT 配置が 4 KiB aligned から
2 KiB offset に変わり、各 4 KiB write が二つの下位 cache line にまたがることと一致する。
詳細と後続計測は [p006 results](../phase006-io-pool-exec/results.md)。時間差の全てが
この要因と証明できたわけではないため、配置も記録して p026 まで追跡する。
