# p008 実装・受け入れ結果

Date: 2026-09-07. Queue q095. Status: completed。

- 現在の両 UFS に既存 mapping の最大 64 KiB run を実装。direct/indirect leaf 境界、
  hole、fragment、partial、EOF と optional lookahead の失敗で短縮する。
- full read と既存 size 内の full overwrite は caller storage を直接利用。
  scratch は partial/hole/allocation の互換路でだけ遅延確保する。新 allocation の
  zero/pointer publication と inode size の更新順序は維持した。
- UFS2 の write は write_sectors を経由し snapshot→journal の順序を維持する。
  journal payload capacity を block に丸めて run 上限へ反映する。
- loop_submit と fat_loop_transfer は既に request と retained extent を保っており、
  この Phase では別の extent owner を追加していない。
- `temp/p008/host-1`: 両 UFS × ordinary / ASan+UBSan、counter concurrent fixture PASS。
  間接 block 上の連続 64 KiB read/write 一回、全面上書きの事前 data read ゼロ、
  direct/indirect 境界、partial、EOF、hole、断片、後続 invalid mapping の partial prefix、
  write failure、UFS2 journal 8 KiB cap と snapshot-before-journal hook を確認した。
  Journal/snapshot の fixture はここでは境界 model であり、その永続化実装の証明ではない。
- amd64 supported build PASS。

## 最終検証

- 最終 `host-lookahead-final` は両 UFS / ordinary / sanitizer PASS。
  optional mapping lookahead のエラーを保持し、既知の prefix を転送した後にそこで返す。
  エラーを再試行で隠さない read/write セルを追加した。
- q086 host 側 39 項目、QEMU の 2 回起動で残り 11 項目も PASS。
  `acceptance-final.json` に FS50 と WiFi30（ordinary/sanitizer）の全 PASS を集計。
  host-only runner の初回 exit 1 は未実行 native 11 項目のためだった。
  最終 lookahead 修正後に影響する UFS host セルを再実行した。
- 両 UFS metadata audit は shared dinode exclusion、read/write error unlock、directory bounds、
  allocation/truncate の失敗順序を ordinary / sanitizer で確認（UFS1 6010、UFS2 6595 checks）。
- 最終 observation fixture も両版 PASS。UFS2 の full overwrite の事前 data read はゼロとなるため
  旧「UFS2 だけ一回」の期待値を更新。partial は引き続き RMW 一回。
- amd64 (`build-amd64-final.log`)、pcat、pc98 の `make -j16` PASS。
- native 4 GiB / UEFI / USB-root / SMP4: 202 samples、readback と sample oracle PASS。
  mode0 p50/p95/p99=20/20/20 ms、mode1=20/20/30 ms（clock 10 ms）。
  mode0（同じ 64 KiB を4回）で syscall write=4、UFS content write=4 / 262,144 bytes、LOOP_WRITE=10 / 311,296 bytes、
  USB_WRITE10=58 / 352,256 bytes。mode1（別々の領域へ連続256 KiB）では UFS5 / LOOP11 / USB60、
  USB bytes=356,352。direct/indirect 境界を跨ぐため data run が一つ増える。
  p007 は両 mode とも UFS32 / LOOP38 / USB114。
  data.img LBA202956、4 KiB 内 offset2048 は p007 と同一。
  pool backing allocation、fallback と DMA allocation は計測中ゼロ。
- ここで確認した 64 KiB 一回は FS data run の値。device data command は依然 8 KiB 制限で
  分割される。USB normal 64 KiB reservation は p009 の担当。
- source hashes は `temp/p008/final-source.sha256.json`。native source image hash、commands、
  layout、summary は各実行ディレクトリに保存。`git diff --check` PASS。

新 allocation の batching は未実施で p011 へ、indirect mapping の繰返し読取りは p010 へ、
動的 metadata scratch は p010 以降へ引き継ぐ。WS024 に統合する際は同じ run を単一 driver に移す。
実機 gate は user-accepted、agent runtime は未実施。
