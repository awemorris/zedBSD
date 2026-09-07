# p009 実装・受け入れ結果

Date: 2026-09-07. Queue q096. Status: completed。

## 実装

- USB HCD に paired optional urb_reserve / urb_unreserve と capability を追加。
  drv_usb_urb_reserve_transfer は core staging と HCD backing を一括準備し、失敗時には旧予約を維持。
  active / HCD-owned URB の変更は EBUSY。予約は URB の最後の参照でだけ解放する。
- xHCI は各 URB に request + coherent DMA を保持。busy と単調 generation を検証し、
  未 retirement の request は再利用・代替確保しない。世代上限で wrap せず EOVERFLOW。
  既存 controller 8 KiB reclaim reserve は独立し、normal reservation が先に選ばれる。
- DMA の alignment を payload 以上の 2 の冪とし、control の 64 KiB 境界を跨がない。
  最大 64 KiB normal bulk は既存 TRB 境界処理と DMA mask を利用する。
- storage は probe 成功後、control 8 KiB と bulk IN/OUT 各 64 KiB を core/HCD に予約し、
  全成功後に実効 BIO 上限を 64 KiB にする。非対応 HCD は 8 KiB。
  64 KiB 超の logical sector は既存 core-only / 1 block 互換路を残す。
  No-media の idle reader は従来どおり probe 前の core reserve だけを保持する。
- IO stats v4 に HCD reservation alloc/free と動的 xHCI request allocation を追加。
  warm path のゼロ確保と resident reservation の解放を区別する。

## 現時点の検証

- 初回 amd64 build PASS。その後に reservation generation を追加したため、最終 build は未実施。
- `temp/p009/host-storage-final`: core / xHCI / storage × ordinary / ASan+UBSan の全6 variants PASS。
  core: 2 段階の allocation failure / HCD failure、旧予約保持、warm100回で割当てゼロ、
  timeout / caller free / late completion 後までの予約 lifetime。
  xHCI: 64 KiB / 4 GiB 超 DMA、warm1000回で割当てゼロ、busy所有者の隔離、別 URB と
  独立 reclaim reserve の使用、generation 上限、最終 backing 全解放。
  storage: 3 予約段階の attach failure rollback、512/4096-byte sector の 64 KiB BOT data 一回、
  legacy 8 KiB と oversized-sector 一 block の実効上限。
- fixture の初回 compile では host libc の tid_t 不在と test の SCSI opcode 定数名を修正した。

## 最終検証

- USB recovery は core 1111 checks（ordinary/sanitizer）、function 1833、unregister、
  binding transaction 971（各版）と production source/object gate が PASS。
- concurrent URB / HID（92 checks + hot-unplug）/ zero-packet / no-media / RTL8822BU 回帰 PASS。
  zero-packet runner に共通 io-stats.c の include path が欠けていたため補修し、再実行した。
  `regression-commands.json` と `regression-resume-commands.json` に実行を記録。
- 最終 amd64 / pcat / pc98 の `make -j16` PASS。test config の部分 object build 後に
  通常 CI config で全 build を完了した。
- Native USB-root / UEFI / SMP4 の 4 GiB と 16 GiB で各202 samples・readback・sample oracle PASS。
  16 GiB は disposable data.img を4 KiB境界へ配置し、layout/chain を保存した比較セル。
  元の source image は SHA256 不変。配置変更は実験コピーだけに行った。
- 全 sample で core staging allocation、HCD reservation alloc/free、xHCI dynamic request allocation、
  DMA allocation、syscall pool backing allocation/fallback がゼロ。

| 条件 | mode | UFS content calls | loop writes | USB WRITE10 calls / bytes | p50 / p95 / p99 ms |
| --- | --- | --- | --- | --- | --- |
| 4 GiB・2 KiB offset（p008と同一配置） | 0 | 4 | 10 | 30 / 352256 | 10 / 20 / 20 |
| 同上 | 1 | 5 | 11 | 33 / 356352 | 20 / 20 / 20 |
| 16 GiB・4 KiB aligned | 0 | 4 | 10 | 10 / 311296 | 10 / 20 / 20 |
| 同上 | 1 | 5 | 11 | 11 / 311296 | 10 / 20 / 20 |

mode0 は同じ64 KiBを4回、mode1は別々の領域へ連続256 KiBを上書きしてfsyncする。
mode1は direct/indirect 境界を跨ぐため run が一つ増える。aligned 条件では上位 loop の
各要求がそのまま USB data command 一回となる。両 mode に metadata write 6回を含み、
fsync の全処理が data command 一回になるという意味ではない。guest clock は10 ms刻み。
高位 DMA address の host fixture と高メモリ native boot は別証拠であり、native DMA PA を
個別 trace したとは主張しない。HID/WLAN は host回帰であり RF 実機測定ではない。

Source hashes は `temp/p009/final-source.sha256.json`、各 config/log/summary/layout は同tempに保存。
`git diff --check` PASS。実機 gate は継続して user-accepted、agent runtime 未実施。

未実施の後続: resident の全体 memory budget 統合は p016、真の SG は p024、
非対応 HCD の64 KiB予約拡張と実機 IMOD/UAS は本 Phase の実装範囲外。
