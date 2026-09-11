# p006 実装・受け入れ結果

Date: 2026-09-07. Queue: q093. Status: completed。

## 実装

- `src/kern/io-pool.c` と `include/kern/io-pool.h` が共有 scratch の唯一の owner。
  64 KiB と 4 KiB の slot を、全 CPU の scheduler 参加後・最初の exec 前に構築する。
  初期 desired=max(16, 2×CPU)、resident budget=min(managed RAM/64, 4 MiB)。
  small は desired 本かつ budget の 1/4 以下、large は残予算以内へ減数する。
  descriptor table 自体と HAL が返した page-rounded backing 全体も resident に加算。
- 各 slot は immutable backing descriptor と atomic ownership。borrow は bounded CAS 探索で
  待機も HAL/heap allocation も行わない。小さい request は small 優先、large 枯渇時も
  small→既存 stack の順で前進する。返却は全 data access 後に release する。
  HAL atomic abstraction を用い、非 native atomic HAL へ libatomic 依存を増やさない。
- 六 syscall の通常 file bounce は pool borrow/return。stream/PIPE_BUF と既存の lease、
  offset、short/partial/error semantics を維持する。旧 256 KiB per-call heap scratch は削除。
- exec copy_segment_snapshot は同じ pool を借り最大 64 KiB。枯渇時は 512-byte stack。
  file content lease を保持したまま借りても待機しない。copy/short/error 後は返却する。
- io stats は append-only で 5 events を追加し version 2。backing allocation、large/small borrow、
  fallback、return を計数。旧 sample は stable event prefix で再集計可能にした。

## 検証と発見

- Production pool: 40 capacity/failure boot cases、complete exhaustion、8 threads × 1000 borrowing。
  descriptor+payload budget、重複所有なし、全返却、warm borrow の allocation calls 増加ゼロ。
- Verbatim production exec helper + production pool: 64 KiB / 4 KiB / stack の三状態で、
  partial segment、short read、backend/copy failure、offset overflow、lease保持と返却を照合。
- 六 syscall の verbatim production functions を既存 boundary fixture で検証。
  pool の availability はそこで制御し、production pool 自体は上記別 fixture で検証する。
  歴史的な任意 chunk override を production から削除し、現在の 64 KiB と枯渇ケースへ更新。
- `host-leaf-page`: pool / exec × ordinary / ASan+UBSan の4 variants PASS。
  `syscall-stories.log`: 6 paths、vector、PIPE_BUF、EOF、short、backend/copy/begin failure、signal PASS。
- 初回 amd64 build は include の追加漏れで失敗、補完して build PASS。
- 初回 `pool-uefi-4096` は login したが、pool が未確保だった。HAL leaf page の level は 1 であり、
  誤って 0 を渡していた。production と host HAL model を修正して上記 host tests を再実行。
  初回 login を有効 pool の受け入れには数えない。初期確保不可のログも追加した。

## 最終検証

- amd64 (`build-pool-active.log`)、pcat、pc98 の `make -j16` は PASS。
- ARM64 / m68k / SPARC v9 向け production io-pool.c の focused compile は PASS。
  完全な他アーキテクチャの link/runtime は本結果に含めない。`hal-compile/commands.json` に引数を保存。
- `observation`: counter と両 UFS の ordinary / ASan+UBSan 回帰 PASS。
- 通常 UEFI USB-root SMP4 の 4 GiB / 16 GiB で各 202 samples、readback と sample oracle PASS。
  pool は large=16、small=16、resident=1,118,208 bytes。各 sample で backing allocation=0、
  large borrow=4 / 262,144 bytes、return=4 / 262,144 bytes、small/fallback=0。
- UEFI 256 MiB でも同じ容量の pool が予算 4,090,560 bytes 以内で構築され login PASS。
  さらに小さい RAM と確保失敗は production pool の host fixture で確認した。
- 4 GiB の両 mode は p50=40 ms / p95=p99=50 ms、16 GiB は p50=30 ms / p95=p99=40 ms。
  計測時計は 10 ms 刻み。p006 による速度改善とは判定しない。
- `git diff --check` PASS。証拠は `temp/p006/`。source hash は `final-source.sha256.json`。

## 配置による比較条件

p004 の data.img は FAT cluster 17095 / LBA 202928（4 KiB aligned）だったが、
p005/p006 は cluster 17100 / LBA 202948（4 KiB 境界から 2 KiB ずれ）だった。
FAT cluster は 2 KiB。上位 4 KiB write が下位の二つの cache line にまたがり、
USB write は 76 / 311,296 bytes から 152 / 622,592 bytes に増えている。
この配置差は p004→p005 の下位 write 数の倍増を説明する。新 runner は layout.json に
配置と chain を保存する。同じ配置でも 4 GiB p006 の時間が増えた点は未解明として
p007 以降と p026 の比較へ引き継ぐ。64 KiB syscall の下位 UFS 分割は p008 の担当。

実機 gate はユーザーの明示判断による user-accepted。agent による実機起動は行っていない。
