# ws025-p004 実行結果

Date: 2026-09-07. Queue: q091. Status: completed。

## 実装済み

- `src/hal/amd64/pmem-range.c`：extent 別の used/reserved/head/tail bitmap、
  free-word summary、rotor、64-bit PFN と制約付き contiguous allocation。
  `minimum` / `maximum` は **inclusive byte limits**。boundary は 0 または power-of-two。
  allocation の先頭・末尾を検査し、部分 free、隣接 allocation を結合した free、二重 free を拒否する。
- amd64 `page.c`：通常 allocator を range metadata へ移管。bootstrap bitmap は初期 table 構築用の
  固定 1 GiB 管理だけに残し、通常 RAM の管理量は extent の合計とする。
  metadata は mapped usable RAM から boot/early/kernel/framebuffer 所有を避けて contiguous に予約し、
  高位を優先する。初期 metadata/table 所有は `amd64_early_reservation` で保持する。
- p004 では管理 extent の通常公開を 1 GiB 以下に制限し、p005 の gate を残す。
  1 GiB 超の metadata は boot 所有として使用可能。boot-reclaim の一般公開はまだ行わない。
- `hal_pmem_alloc_range`：既存 request の field を増やさない RAM 専用 API。
  amd64 は範囲内を直接探索。他 HAL は共通 weak compatibility helper で結果を検査し、
  制約外の結果を free して拒否する。compatibility helper は低位探索の最適化を主張しない。
- DMA coherent path は address_bits と boundary を API に渡す。
  sub-page boundary は page-aligned backing のうち公開 payload を検査する。
  64-bit address の終端 overflow も検査する。
- `hw.memory.stats` は version 2。`physical_managed_bytes` とし、boot-reclaim、metadata、
  scan words / max extent scan words、IRQ-off 最大 cycles、firmware source を別に報告する。
  cycles は同一 CPU の TSC 差で lock 待ちを含み、wall time に換算していない。

## 確認済み

- production pure allocator：5000 randomized operations を独立 page oracle と比較。
  全 mutation 後の free/reserved/allocated と summary を照合。高位・32-bit mask・boundary・
  部分/結合 free と、4 threads × 500 alloc/free を通常・ASan/UBSan で検証。
- production DMA + production extent allocator：32-bit device は高位 pool を使わず低位を取得。
  低位枯渇時は高位を切り詰めず ENOMEM。64-bit device は 4 GiB 超を保持。
  sub-page boundary、map/unmap、free/destroy も検証。
- `temp/p004/host-all`：上記に旧 DMA constraints / 8-thread DMA allocation-lock、
  RAM mapper / handoff を加えた 9 groups × 通常/sanitizer が PASS。
- amd64 build と range allocator の UEFI / 4 GiB USB boot が PASS。
- 新 DMA と sysctl v2 を含む `baseline-dma-4096`：202 native overwrite/fsync samples が PASS。
  syscall/backend/USB 呼出し数は batching 前の基準を維持した。

## 最終検証と範囲

- `temp/p004/host-complete`: 10 groups × 通常 / ASan+UBSan の **20 variants PASS**。
  compat 専用 fixture は inclusive 上端、丸めた backing 超過、32-bit mask、alignment、
  不正 request、HAL の rollback 失敗時の retained descriptor を検証した。
- 最新 DMA map は公開 payload の外側、segment 最大長超過、負の direction を拒否。
  padding の mapping 拒否と有効な部分 mapping を production fixture で確認。
- `build-amd64-final.log`, `build-pcat-retry.log`, `build-pc98-final.log`: supported build PASS。
  PC/AT 初回は userland compile が古い sysroot を読む依存欠落で失敗。
  PC/AT と PC-98 の userland pattern rule に sysroot 完了依存を追加して解決した。
- `hal-compile/commands.json`: arm64 / m68k / sparcv9 の共通 helper と既存 allocator、
  計 6 object を clang-19 の各 target で compile PASS。これは全体 link / 起動ではない。
- `final-bios-256`, `final-uefi-16384`: 最新 amd64 image の 4 CPU / USB-root login と stats v2 PASS。
  BIOS 256 MiB: managed 267898880 = reserved 6631424 + allocated 4218880 + free 257048576。
  UEFI 16 GiB: mapped 17173229568、managed 1052176384、metadata 155648。
  上限 gate が残っているため managed は 1 GiB 以下であり、高位通常利用の PASS ではない。
  最大 extent scan は両セル 3 words、最大 IRQ-off は 1513078 / 1521594 TSC cycles。
  QEMU での cycles を実機 latency 保証には使わない。
- `baseline-dma-4096/summary.json`: 202 overwrite/fsync samples の oracle PASS。
  両 mode の p50=20 ms、p95/p99=30 ms。これは最終 map 拒否チェック追加前の DMA allocation 統合結果。
  追加後は host bounds fixture と上記 native USB 起動で回帰を確認した。
- `git diff --check` PASS。証拠と source hashes は `temp/p004/final-checks.json`。

幅・driver 前提と公開条件は [address audit](address-audit.md)。MEM07/12/13 の allocator/DMA
契約は production-linked host evidence、MEM08/09 は p003 の mapper/SMP evidence を継承する。
MEM10/11 の native 高位 PFN / COW は p005 で実行する。

固定 DMA reserve の根拠のない導入は避け、現在は constrained search と所有統計を提供する。
p005 で境界を跨ぐ extent にも normal 高位優先を適用し、p009 で active-device reserve を統合する。
boot-reclaim 解放と実機は未実施であり、p005 以降に状態を引き継ぐ。
