# p007 実装・受け入れ結果

Date: 2026-09-07. Queue: q094. Status: completed。

- buffer reference の準備を busy ownership から分離。最大 64 KiB / 16 lines の参照・領域を
  先に準備し、その後は try-acquire のみ。競合時は全解放し単一 line に短縮する。
- cold read の hit 境界、partial line は分割。write は全 line の retry copy を各 cache line に
  保存した後、caller の連続 buffer を下位へ渡す。共有 staging allocation は増やしていない。
- disk_transfer_progress は完全に成功した BIO だけを confirmed block prefix に加算。
  short、submit failure、wait failure による uncertain BIO は prefix に含めない。
  cache は prefix 内の完全 line のみ clean/valid とし、write の残りは dirty/error と内容を保持。
  新しい dirty generation を消さない。
- I/O counter v3 に run read/write と single-line fallback 理由（geometry、memory、busy、hit）を追加。
- production buf.c と verbatim disk helper に HAL/BIO fault model を接続した `host-final` は
  ordinary / ASan+UBSan とも PASS。64 KiB read/write 一回、partial の隣接保持、hit境界、
  partial prefix/short/uncertain、allocation failure、64 KiB cap、reclaim/resize中の再入、
  二段 cache と並行 writer の一致を確認。
- `fat-loop` の既存 production FAT + loop + buf 回帰は ordinary / sanitizer とも PASS。
  fragmented extent、shared-line RMW、alias invalidate、flush error、map/claim lifetime を確認。
- 最初の新 host fixture は assert の host ABI と spin mock の警告を修正。
  buf_reset は先頭 metadata slab を常駐させる仕様なので、全 data line 解放と
  resident metadata 一枚を oracle にした。production をテスト都合で変更していない。

## 最終検証

- `host-prefix-boundary` ordinary / ASan+UBSan PASS。追加の generation 再変更、wait failure、
  BIO prefix が cache line の途中に終わるケースも PASS。最後は data resident=0、buffer=0、
  仕様どおり metadata head slab 一枚だけを保持。
- disk foundation の実 production BIO/claim 回帰は各版 20,376 checks PASS。
  amd64/i386 の storage ABI gate も PASS。
- amd64 (`build-amd64-final.log`)、pcat、pc98 の `make -j16` は PASS。
- native UEFI USB-root SMP4 / 4 GiB は 202 samples、readback、pool/counter oracle PASS。
  両 mode p50=p95=30 ms、p99=40 ms。10 ms の測定粒度に留意する。
- 配置は data.img LBA 202956、4 KiB 内 offset=2048。p006 と同じ misalignment 条件。
  256 KiB overwrite+fsync ごとに LOOP_WRITE 76→38（311,296 bytes は不変）、
  USB_WRITE10 152→114（622,592→466,944 bytes）。run write=38 / 311,296 bytes。
  pool の追加 allocation/fallback=0、run の memory/busy fallback=0。
  UFS content write=32 / 262,144 bytes は未変更で p008 に引き継ぐ。
- 証拠は `temp/p007/`。`final-source.sha256.json`、各 command/log、native `layout.json` と
  `summary.json` を保存。`git diff --check` PASS。

## 範囲と限界

run は初期最大 64 KiB。管理配列は caller stack 上の line pointers/generation だけで、
データ staging は持たない。device 上限の分割は従来同様 disk が行う。競合・容量不足で
全解放して単一 line に短縮するため、連続要求が常に一回となるわけではない。
fragment/hit/partial と既存 UFS block 分割は残り、後者は p008 の対象。
実機 gate は既定の user-accepted、agent runtime は未実施。
