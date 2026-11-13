<!-- awesome-plan project=zedbsd record=ws299-p004 -->

# ws299-p004 待ちを重ねる: 連続の読みの先読みを非同期の worker で（raw の block device）

- 親: [WS299](../ws.md)。Queue: q1617 / q1617-i01（担当 P1、2026-10-27）。前提: p003（cleared 2026-10-27、main be47a24df）。
- Status: in-progress（2026-10-28 Q1 の判定待ち。2026-11-13 q2044 P2: 乱読みの後の連続の読みで run が cache の line で切れる件を直した、下の「2026-11-13」。QEMU は未実施）
- Disposition: normal

## 測った事（2026-10-27、5330、kernel は main be47a24df ＋ 下の測る口、`nvme-bench-5330.sh zedbsd zb-p004m … read`）

測る口: `pci-nvme.c` に command ごとの時刻（post の時、割り込みが完了を見た時、待っていた thread が走った時）。4096 command ごとに平均と最大を dmesg へ（`nvme: 4096 commands: post to completion avg … completion to waiter avg …`）。

| 区間 | 256 KiB の command（raw の連続の読み、chunk の段） |
| --- | --- |
| post → 割り込みが完了を見る（device と割り込みの配達） | 平均 259〜283 µs、最大 0.5〜0.8 ms |
| 完了 → 待つ thread が走る（起床） | 平均 3 µs、最大 14〜77 µs |
| 256 KiB の 1 周（256 MiB を 0.42〜0.46 秒 / 1024 run） | 約 430 µs |
| うち command の外（CPU の仕事: line の帳簿、line → staging、staging → user の写し） | 約 160 µs（sys の時間 0.14〜0.19 秒 / 256 MiB = 約 0.65 ms/MiB と合う） |

- 起床の遅れ（割り込み・VMD・scheduler）は 3 µs で、問題ではない。queue を CPU ごとにする・VMD の vector を子に配る事は、1 本の流れの速さには効かない（下の「範囲の変更」）。
- device の時間 270 µs / 256 KiB（QD1 で約 970 MB/s）と CPU の 160 µs が直列に並んでいる。Debian の page cache 経由（1.1〜1.2 GB/s）は先読みで device と写しを重ねている。
- 並べた時の上限の目安: CPU の側は cache の hit の読み（`reread`）が 2.4〜2.7 GB/s、device は QD を上げれば fio の QD32 で 3.1 GB/s（p001）。重ねれば 1.1 GB/s（Debian の 95% は約 1.05 GB/s）に届く見込み。

## 範囲の変更（2026-10-28 Q1 が案のとおり決定）

- p004 の元の案（非同期の BIO・queue の深さ・複数の queue・VMD の vector）のうち、数が効くと示すのは「device の待ちと CPU の仕事を重ねる事」だけ。複数の queue・VMD の vector は起床 3 µs なので後へ（Future Work か、乱読みの QD32 を測る p007 の後に要否を決める）。
- 重ねる道は「driver を完全に非同期にする（割り込みから BIO を完了）」より、「buffer cache が先の run を worker の thread で読む（先読み）」の方が小さく、今の同期の driver と recovery の決まりをそのまま使える。これは元の p005（readahead）の raw の部分に当たる。p004 = raw の block device の先読み、p005 = UFS の file の先読みの窓と cluster・書きの合流、と分け直す案。

## 設計（2026-10-28 改訂、P1。調べ直した事実を元に D1〜D3 を書き直した）

### 調べ直した事実

- raw の読みの道: `block_pread_data`（`src/kern/devfs.c:1624-1716`）が claim の無い open では `disk_read`（`disk.c:2417`）→ `disk_cached_transfer`（`disk_cache_enter` の admission の下で）→ `buf_read`（`buf.c:698`）→ `transfer_run`・`transfer_run_lines`（`buf.c:2371-2607`）。claim 付き（admin の open）は `disk_read_direct` で cache を通らない。
- `buf_read` は UFS の metadata・file の読みにも使われる。UFS の journal（`ufs.c:20332`）と swap（`swap-source.c:1401`）は `disk_write_direct*` で cache を通さず書き、cache を無効にしない。先読みがこの範囲の line を作ると、その後の direct の書きで古くなる（demand の読みでも同じ危険は今もあるが、先読みは読まれていない所まで広げる）。→ 先読みは **devfs の raw の block device の読みからだけ** 起こし、その open した disk（区画か全体）の範囲を越えない。
- 既存の file の先読み（`src/kern/readahead.c`、窓 64〜128 KiB、vm object の prefetch）は UFS の file 用で、p005 の対象。p004 では触らない。
- `DISK_PAGE_VECTOR` を立てる driver は NVMe だけ（`pci-nvme.c:5648`）。
- 管理の操作は「使用中なら即 EBUSY」の検査をする: `disk_admin_idle`・`disk_reload_idle`（`d_inflight`・`d_cache_users`、`disk.c:236-264`・`3513`）、`disk_media_idle_locked`（`3225`）、`disk_gone_if_idle`（`d_refs`）、`evict_one` の範囲の evict（busy・inflight の line で EBUSY、`buf.c:2690-2702`）→ `buf_invalidate`（devfs の claim 付きの書き `devfs.c:1813`、swap の設定 `swap-source.c:287`）、`buf_invalidate_disk`（admin の open `devfs.c:2002`、区画の reload `partition.c:535`、`disk_gone_if_idle`）、`buf_discard_media`。読み手が去った後に走る先読みが、これらを偶然 EBUSY にしてはいけない。
- NVMe の driver は複数の thread からの data の BIO を同時に受ける（`nvme_io_begin_bio`、`pci-nvme.c:4402-4491`、flush だけが排他）。5330 の zedBSD は 12 CPU。
- 管理の経路で待ってよいか: `disk_gone_if_idle` は NVMe の detach（`nvme_detach_owned`、controller の lock の下、`pci-nvme.c:1948-1961`）と USB の detach（`control_lock` の下）から呼ばれる。そこで NVMe の I/O の終わりを待つと、I/O が同じ lock を要る時に止まる恐れがある。両方とも EBUSY なら後で再試行する（USB は core が繰り返す、NVMe は error を返す）。

### D1. 流れの検出（`include/kern/buf-readahead.h` の純粋な関数、`src/kern/buf.c` の表）

- devfs の `block_pread_data` が claim の無い open で `disk_read` を成功した後、`buf_readahead_observe(file, disk, block, run)` を呼ぶ（`disk` は open した disk、block はその disk の上の番号）。`block_close` は `buf_readahead_forget(file)`。
- buf.c に流れの表（8 entry、key は open の記述 `file`。同じ disk の二つの読み手は別の流れ。無ければ一番古い entry を使い回し、使い回す時は generation を必ず上げる）。entry: owner・disk・leaf・`next`・`issued`・`sequential`（2 で飽和）・`generation`・最後に使った順。表と queue と worker の状態は一つの spinlock（`LOCK_RANK_READAHEAD_REGISTRY`）で守る。この lock の下では他の lock を取らず、`disk_ref`・`disk_release` もしない。
- 純粋な関数 `buf_readahead_plan(stream, block, count, limit, window, unit, &start, &length)`:
  - `block == next` なら連続（`sequential` を上げる）、違えば作り直し（`generation` を上げ、`issued = next = block + count`、`sequential = 1`）。
  - `sequential >= 2` の時、`issued = max(issued, next)`、`[issued, min(next + window, limit))` を返す。1 unit（1 run = 256 KiB）に満たない時は、disk の終わりに届く場合だけ返す。
  - 頼んだ分は `buf_readahead_commit(stream, end)`、仕事を捨てた時（満ち・止め・memory・誤り）は `issued = next` に戻す（穴を作らない）。
- 窓は min(2 MiB, cache の上限 / 8) を unit に切り下げた物。1 unit 未満なら先読みしない（cache の最小 1 MiB の機械では働かない）。
- 仕事への分け方: 窓を leaf の上の run の境界（64 line）で切る。最初の仕事だけ境界までの短い物になり得る（demand の run と格子を合わせる）。
- 条件（全部を満たす時だけ）: leaf が `DISK_PAGE_VECTOR`、block の大きさが page 以下、その leaf に止め（D3）が無い、`disk_readahead_private(disk, start, length)` が真。最後の物は disk.c の新しい口で、disk の lock の下で「読み手の disk の `d_open_count == 1`（読み手だけ）、かつ同じ leaf の他の disk（祖先を除く）で範囲が重なる物に open・opening が無い」を見る。mount された区画・swap の区画・他の読み手と重なる所へは先読みしない（direct の書きで古くなる line を、利用者が読んでいない所に作らない。レビュー H4）。
- 乱読み・逆向き・二つの読み手の交互の読みは毎回作り直しになり、先読みを頼まない。

### D2. 先読みの worker（`src/kern/buf.c`）

- 仕事: {open した disk、leaf、block（open した disk の上）、count（最大 64 line）、owner、流れの generation}。queue は固定の 16 個。満ちていれば頼まない（待たない）。入れる前に lock の外で `disk_ref`、入らなかった・捨てた・終わった時に lock の外で `disk_release`。
- worker は `BUF_READAHEAD_WORKERS` = 4 本の kernel thread（`kthread_create`、最初に頼む時に作る）。worker ごとに run の帳簿（`struct buf_run_scratch`）を作る時に一度だけ確保する（失敗すれば worker を作らない）。
- 取り出しは lock の下で一度に: 仕事を外す → leaf の止めと流れの generation を見る（だめなら捨てる）→ worker の「実行中の leaf」に記録。終われば lock の下で記録を消して待ち手を起こす（レビュー H1）。
- 仕事ごと: `disk_cache_acquire(disk, &leaf)`（admission。admin・reload・media の無い時は EBUSY・ENXIO で捨てる）→ `disk_resolve_range` → `transfer_run_lines` を先読みの形で → `disk_cache_release`。
- `transfer_run_lines` の先読みの形（引数 `prefetch`、`data == NULL`）:
  - 最初の line が既に有れば（valid・busy・inflight）何もしない。途中で valid・busy に当たれば、そこまでに縮める。2 line 未満なら何もしない。
  - page 列の道だけを使う。page 列にならない（`b_memory.size != KERN_PAGE_SIZE`）・driver が `EOPNOTSUPP` の時は caller の buffer の道に落ちず、line を valid にせず終える。写しは無い。
  - 読めた line は `BUF_VALID`。読めなかった line は `BUF_VALID` も `BUF_ERROR` も付けず `b_error = 0`。demand の読みが自分で読み直す。
  - `io_stats_record` は早く戻る所を含めて一つも記録しない。`stat_read_bios`・`stat_hits`・`stat_misses` は数える（`vfs.bufcache.stats` の misses には先読みの line が入る、と記録する）。先読みは自分の数（D4）に数える。
  - line の確保の失敗（`reference_line` の ENOMEM）は今どおり（2 line 未満なら捨てる）。worker は dirty の書き戻しをしない（`reserve_bytes` は clean だけ）。
- demand の側の直し（レビュー M1）: 読みの `transfer_run_lines` は、busy・inflight の line に当たった時、既に 2 line 以上を持っていればそこまでに縮める（持っている line だけで、他を待たないので「持ったまま待たない」規則は保つ）。今は run 全体を諦め、1 line（4 KiB）ずつの同期の読みに落ちる。
- 先読みの line は終わった時に LRU の頭に入る（今の `release_run` のまま）。

### D3. 止め（管理の操作と先読みの排他、leaf ごと）

- `struct disk` に `d_readahead_stops`（buf.c の lock の下だけで触る）。`buf_readahead_stop(disk, wait)` は lock の下で leaf の止めを上げ（入れ子可）、その leaf の queue の仕事を局所の list に外し、その leaf の流れの generation を上げる。`wait` なら、その leaf で実行中の worker が無くなるまで `WAITQ_INTERRUPTIBLE` で眠る（信号なら止めを戻して EINTR）。`wait == 0` なら、実行中があれば EBUSY（止めは掛かったまま、呼び手が resume）。lock を放してから外した仕事を `disk_release`。`buf_readahead_resume(disk)` は止めを下げる。worker が一度も作られていなければ何もしない形になる。
- 止めの間、その leaf の `buf_readahead_observe` は頼まず、worker はその leaf の仕事を始めない。
- 呼ぶ所（どれも `disk_lock()` の外、レビュー H1）:
  - 待つ: `disk_admin_begin`（devfs の admin の open）・`disk_reload_begin_claimed`（区画の reload）の、idle の検査の前に stop、関数の終わりで resume。始まった後は admin・reload の owner が worker の admission（`disk_cache_enter`）を EBUSY にする。
  - 待たない: `disk_gone_if_idle`・`disk_media_retire` の始めに stop、実行中なら EBUSY を返す（呼び手は今も EBUSY で再試行する）、終わりで resume。
  - `buf_invalidate`・`buf_invalidate_disk`・`buf_discard_media` には入れない（レビュー H3。devfs の claim 付きの書きは admission で worker が除かれている）。swap の設定（`swap-source.c:287`）は今の demand の読みと同じ危険のまま（変えない。D1 の条件で swap の区画には先読みしない）。
- 待つ側が持っている物と worker が要る物: worker は読みだけで、backing mutation・file・vnode の lock・他の line の busy を取らない（`disk_transfer_pages_context` は書きだけ guard を取る）。reclaim は clean の evict と `vm_object_reclaim_clean`（spinlock だけ）。
- suspend: 特別な扱いはしない（flusher・demand の読みと同じ）。

### D4. 測る口

- p004 の測る口（command ごとの時刻）は WS299 の間だけ残し、p008 で消すか `CONFIG` で切り替えるかを決める。
- 先読みの数（頼んだ・始めた・読めた run と byte、捨てた: 満ち・止め・古い generation・既に有る・admission・memory・誤り、demand が busy の line を待った回数）を static の counter に数え、1024 個の仕事ごとに 1 行を log（`buf: readahead …`）。UAPI の io-stats は変えない。
- source の comment の開発の管理情報（`ws299`）は、触る file（buf.c・disk.c・pci-nvme.c）で消す（Guardrail、レビュー L1）。

### レビュー（2026-10-28、design-reviewer）の反映

- H1（原子性・lock の順序）→ D2 の取り出し、D3 の「disk_lock の外」。H2（全体の止め）→ D3 を leaf ごと、devfs の書きには入れない。H3（detach の中の待ち）→ `buf_invalidate*` に待ちを入れない。H4（古い line）→ D1 の `disk_readahead_private`。
- M1 → demand の側の縮め、仕事の格子。M2 → owner ごとの流れ、捨てた時の `issued` の巻き戻し、使い回しで generation。M3 → 先読みの形の NULL の道と io_stats。M4 → 4 GiB の曲線（`nvme-bench-5330.sh` の chunk の 16 段 = 4 GiB）と待ちの数。M5 → 割り込める待ち。M6 → 受け入れの試験を足した（下）。
- L2: ws.md の受け入れ 3 の「single の落ち込み」は `IO_BUF_SINGLE_MEMORY` とする（先読み済みの line の読みは `SINGLE_HIT` が増えるのが正しい）。L3: 既存の file の先読み（readahead.c）との統合は p005 で見る。L5: 先読みの誤りは数と log で分ける。

### D5. 実装の中で足した物（5330 の測定で cache_lock の取り合いが分かった後、2026-10-28）

- 最初の実装（ded5ae2e9）の 5330 の曲線は、最初の 256 MiB 1.79 GB/s から最後 0.89 GB/s へ下がった（`evidence/zb-p004a-summary.md`）。測る口で、worker の line の準備の時間が 1 run 25 µs → 600 µs に伸び、物理の allocator（`hw.memory.stats` の scan_words は一定）でも LRU の walk（evict 1 回に 1 個）でもなく、`cache_lock` の取り合いが 1024 run ごとに約 40 万回・合計 0.4 秒と分かった（一時の測る口、消した）。line ごとに cache_lock を約 10 回（確保・reserve・evict・free・commit・挿入）、reader の hit の道も 1 line に約 4 回取っていた。
- 直し 1 `recycle_line_locked`: cache が上限の時の miss は、LRU の末尾近く（8 個まで）の、同じ disk・同じ大きさの clean で参照の無い line を、その場で新しい block に付け替える（memory・計上・disk の pin はそのまま、header を新しくする）。free + 確保の代わり。
- 直し 2 `copy_cached_lines`: `buf_read` は先に、cache に有る valid な line を最大 16 個、cache_lock の 1 回の保持で参照し、1 個ずつ busy を取って写し、1 回の保持で返す。無い・valid でない・使用中の line で止まり、そこからは今の道（読む・待つ）。
- 結果: 4 GiB の曲線が 1.68〜2.24 GB/s で平ら（`evidence/zb-p004f-summary.md`）、reread 2.24〜2.44 GB/s。
- design-reviewer（2026-10-28、D5）: 重大な誤りは無し。反映: M1 付け替えの idle の条件に b_lock と dirty_index_lock の下での参照数の再確認を足した、L1 medium が revoke された disk では付け替えない、L2 hit の数は写した line だけ、L3 prepare・transfer の和は read-ahead の lock の下で足し戻す・差が負なら足さない、L4 何も無い時は返しの lock を取らない・lock の前に dirty の line を除く・見る数を `BUF_RECYCLE_LOOK`、Nit の空行と `BUF_GENERATION_EXHAUSTED` の引き継ぎ。勧めの負荷試験は `qemu-cache-stress.sh`（下）。

## 変えない物

- HAL の API、UAPI、NVMe の driver の同期の形と recovery、書きの道（書きの重ねは p005）、UFS の file の先読み（p005）。

## 受け入れ

1. 5330 の raw の連続の読み（`nvme-bench-5330.sh` の chunk の 16 段 = 4 GiB、cache の上限の 4 倍）の中央値が、Debian の page cache 経由の冷えた読み（`deb1` の b-chunk6〜15 の中央値 1.1 GB/s）の 0.95 倍（1.05 GB/s）以上。最初と最後の 256 MiB の比 0.8 以上。届かなければ原因を数で（D4 と測る口）。
2. 先読みが働かない読み（bs 64k 以下の飛び飛びの位置）で遅くならず、先読みを頼まない事を数で。
3. QEMU（NVMe）: boot-test、`qemu-nvme-shapes.sh`、`qemu-grow-reboot.sh`。加えて新しい `qemu-readahead.sh`: (a) raw の読みの cksum が先読みの有無（2 回目は cache）で同じ、(b) 二つの dd を同時に読んで cksum が正しい、(c) dd の最中・直後に区画の reload（`partition reload` の口）と admin の open（書きの open）を繰り返して偽の EBUSY・止まりが無い、(d) 先読みの後に admin の open で書いた内容が、閉じた後の普通の読みで見える、(e) bs=4k の連続の読み。QEMU の NVMe の hot unplug は driver に PCIe の hotplug が無いので試さない（未実施と書く）。
4. build の warning 0（5330・pcat・visionfive2・rpi4・m68k-virt の代表）。
5. host 試験: 流れの計画（連続・飛び・作り直し・disk の終わり・満ちた後の巻き戻し・unit 未満）を純粋な関数で。

## 結果（2026-10-28、P1）

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| host 試験 | `cc -O2 -Wall -Wextra -I include -o build/tmp/buf-readahead-test plan/ws299/tests/host/buf-readahead-test.c && build/tmp/buf-readahead-test`、同じく `buf-slab-test.c` | `buf-readahead-test: PASS`、`buf-slab-test: PASS` |
| build | `make ZEDBSD_CONFIG=<config> BUILD=build/p1-p004-<name> build/p1-p004-<name>/vmunix`（`plan/tools/hw5330/config-5330.mk`・pcat・visionfive2・rpi4・m68k-virt） | 全部 rc 0、warning 0 |
| QEMU 起動 | `OUTPUT=build/p1-qemu-p004-boot2 plan/tools/boot-test.sh build/p1-qemu-p004/hdd-image.img`（`plan/tools/posix/config-amd64-serial.mk`） | rc 0 |
| QEMU 先読み | `sh plan/ws299/tests/qemu-readahead.sh build/p1-qemu-p004/hdd-image.img` | `PASS (6 equal checksums, new data seen, 0 reloads refused)`。冷えた 200 MiB の読みで先読みが 200 MiB を読んだ（`buf: readahead 1024 jobs: … read 200 MiB`）、2 本同時の cksum 一致、読みの直後の `diskpart reload` 10 回で拒否 0 |
| QEMU NVMe の形 | `sh plan/ws299/tests/qemu-nvme-shapes.sh build/p1-qemu-p004/hdd-image.img` | `PASS (5 checksums equal)` |
| QEMU 大きな file と reboot | `sh plan/ws299/tests/qemu-grow-reboot.sh build/p1-qemu-p004/hdd-image.img 3` | `PASS reset after 2 s`、rand.bin の cksum が読みの前後で一致 |
| QEMU cache の上限の負荷 | `make ZEDBSD_CONFIG=plan/ws299/tests/config-qemu-cache128.mk BUILD=build/p1-qemu-c128 disk-image`、`sh plan/ws299/tests/qemu-cache-stress.sh build/p1-qemu-c128/hdd-image.img` | `PASS (7 equal raw checksums, file intact, reload busy 8 of 8)`: 128 MiB の cache で raw の読み 4 本・UFS の 128 MiB の書きと sync・1 秒ごとの reload を同時に、evictions 495472、capacity_failures 0。reload は読み手が開いている間なので EBUSY が正しい。32 MiB の cache では UFS の 200 MiB の書きが 29 MiB で止まり exec も失敗した（cache が dirty の上限 32 MiB と同じ大きさで、新しい line が取れない。付け替えは clean の line だけなので関係しない見込みだが、元の kernel では確かめていない） |
| 5330 読み・書き | `sh plan/ws299/tests/nvme-bench-5330.sh zedbsd zb-p004f build/p1-p004a/vmunix read write`（kernel sha256 1c17a790…） | raw の連続の読み 4 GiB（chunk 16 段）: 1677〜2237 MB/s、中央値 1917 MB/s、最初と最後の比 1.0（p003c は中央値 583）。bs64k・1m・4m 1677〜1917。reread 2237。raw の書き 926〜1032 MB/s（p003c 926〜959）、UFS の書き 353・378 MB/s（p003c 373・433、p005 の対象） |

- review の反映の後の kernel（sha256 6acaa1e9…、commit は下の再開点）で流し直した（2026-10-28）: QEMU の boot-test rc 0・`qemu-readahead` PASS・`qemu-nvme-shapes` PASS・`qemu-grow-reboot` PASS（reset 1 s、cksum 一致）、build（config-5330・pcat・m68k-virt）warning 0。5330（`zb-p004g`）: raw の読み 4 GiB 1678〜2237 MB/s（中央値 1790）、reread 2684、raw の書き 1032・1119・559・455、UFS の書き 312・426。書きだけをもう一度（`zb-p004h`）: raw 1220・655・440・447、UFS 340・413。raw の書きの後半の低下は、sys の時間が同じ（0.11〜0.17 s/256 MiB）で real だけが伸びているので device の側の待ち（BG5 は DRAM-less、この日に p6 を何度も書いた後）と見るが、確かめていない（p006・p007 で見る）。`evidence/zb-p004g-summary.md`・`zb-p004h-summary.md`。
- 受け入れ（上）との照合: 1 は達（Debian の b-chunk の定常 1.1 GB/s の 1.7 倍、d-chunk 1.4〜1.6 GB/s も越える）。2 の乱読みは未実施（dd では作れない。p007 で道具を決める）。先読みを頼まないのは「連続 2 回」の条件と host 試験で確かめた。3 は (a)〜(e) PASS。hot unplug は driver に PCIe の hotplug が無いので未実施。4・5 は達。
- 5330 の証拠は zedBSD を NVMe から起動して ssh で測った物（`nvme-bench-5330.sh`）。`5330-run.sh` の素の起動（i915 の survey）は未実施（p007）。
- `vfs.bufcache.stats` の misses には先読みの line が入る。io-stats の `IO_BUF_SINGLE_HIT` は、cache に有る line を `copy_cached_lines` が写すようになったので減った（落ち込みの指標は `IO_BUF_SINGLE_MEMORY` とする）。
- 測る口: nvme の command の時刻（p004m）と `buf: readahead …` の 1024 job ごとの log（待ち・run・prepare・transfer の時間、捨てた理由）は残した。p008 で CONFIG にするか消すかを決める。

## 残りの確かめ（2026-10-28、P1、q1640）

- 書きの後半の 440〜560 MB/s: 測る前に Debian で `fstrim -av`、書きの先の p6 を `blkdiscard`（swap の頭 4 KiB を戻す）、休み 30〜60 秒の条件で、同じ main の kernel（p004 の review の反映の後、`build/p1-base/vmunix`）の raw の書き 1 GiB を 256 MiB ずつ測り直した（`zb-base`）: 1118・1074・994・959 MB/s で後半の落ち込みは無い。前の落ち込みは drive の状態（同じ日に p6 を何度も書いた後の DRAM-less の BG5 の書きの遅れ）で、review の後の直しではない。
- 乱読み: `plan/ws299/tests/randread.c`（thread の数 = 深さ、Linux と zedBSD で同じ道具）で、Debian の ext4 の 12 GiB の乱数の file の範囲（trim されていない flash）を 4 KiB で 15 秒ずつ（`zb7-base-read`、`deb2-read`）。zedBSD（main の kernel）QD1 35.9 MB/s（113.5 µs）、QD32 445 MB/s（108653 IOPS）。Debian は HMB on で 57.8・964、HMB off で 36.4・448（fio も同じ数）。zedBSD は HMB off の Debian と同じで、差は HMB（p006）。複数の queue・VMD の vector は QD32 で HMB off の Debian と同じ数が出るので、今は要らない。
- 32 MiB の buffer cache で UFS の 200 MiB の書きが止まる件: p004 の前の main（73135a9ec）の kernel でも同じく止まり（`qemu-cache32-write.sh`、exec が ENOMEM、dirty の上限 32 MiB が cache と同じ）、p004 の退行ではない。p005 の D1（dirty の上限を cache の 1/4）で直り PASS。
- 以前の読みの測定（disk の 100 GiB の辺り）は trim 済みの空きの block を読んでいた（p005 の「測定の条件」）。trim されていない範囲での raw の連続の読み（`zb-base`）は 1.58〜1.79 GB/s で、同じ範囲の Debian の page cache 経由 1.5 GB/s を越える（受け入れ 1 は満たしたまま）。

## 出典

- 無し（zedBSD の中の仕組みだけ。Linux・FreeBSD の readahead の code・値は見ていない）。

## 再開点（2026-10-28、P1）

- 実装・D5 の review の反映・host・QEMU（5 本）・5330 は済み（上の「結果」）。残り: Q1 の判定。未実施: 乱読み（道具が要る、p007）、`5330-run.sh` の素の起動（p007）、元の kernel での 32 MiB の cache の書きの確かめ。
- 一時の測る口（cache_lock の取り合いの数）は消した。`plan/ws299/tests/config-qemu-cache32.mk` は git に入れていない一時の file（消すのは Q1 に頼む）。

## 再開点（2026-10-27 ラップアップ、P1）

- やった事: 測る口（`pci-nvme.c` の `posted_ns`・`done_ns`・`latency_*`・`nvme_now_ns`、4096 command ごとの log）を実装して commit。build（config-5330、`build/p1-p004m`）rc 0・warning 0。5330 で `nvme-bench-5330.sh zedbsd zb-p004m build/p1-p004m/vmunix read` を流し、上の表の数（log は P1 の worktree の `build/hw5330/ws299/zb-p004m-read-dmesg.txt`、要約は作っていない）。
- 未実施: 上の設計（D1〜D3）の design-reviewer、Q1 への範囲の変更の案（p004 = raw の先読み、複数の queue・VMD の vector は後へ）の判断、実装、QEMU・5330 の試験。測る口の QEMU の回帰も未実施（5330 の起動と読みだけで確かめた）。
- 次の一手: Q1 に範囲の変更の案を出す → design-reviewer → D1・D2 の実装 → host 試験（流れの検出）→ QEMU（boot・nvme-shapes・grow-reboot）→ 5330 で `nvme-bench-5330.sh zedbsd zb-p004a <vmunix> read write`。
- 5330 の使い方: `plan/tools/hw5330/README.md`。NVMe の kernel だけの差し替えは `nvme-bench-5330.sh zedbsd NAME VMUNIX read|write`（`/tmp/i915-hw.lock` を自分で取る、Debian から BootNext で 1 回 zedBSD を起動して戻る）。image ごとは `5330-run.sh --nvme-image IMG NAME`（root は `ufs-grow.py` で p5 いっぱいに広がる）。遠隔の電源は無い（止まったら Q1 → ユーザー）。2026-10-27 の終わりの時点で 5330 の NVMe の p4 の kernel は `build/p1-p004m/vmunix`（測る口入り）。

## 2026-11-13（q2044、P2）: 乱読みの後の連続の読み

- 測った事（main fe6ea0f967 の kernel、[ws.md](../ws.md) の「2026-11-13」）: read mode の乱読み（cache 経由の QD32 15 秒で cache の約 1 GiB が 4 KiB の line で満ちる、範囲 8064 MiB の約 1/8）の直後の連続の読みは、最初の約 900 MiB が 327〜671 MB/s（Debian の page cache を捨てた後の 1.5 GB/s の 22〜44%）。bs の段の 384 MiB で drv_rd 10545（平均 37 KiB）、乱読みの line が LRU から追い出された chunk2 からは 1 run 256 KiB（1024 / 256 MiB）で 1.8〜2.7 GB/s。乱読みの前に同じ読みをした seqcold は 1491〜1917 MB/s、chunk0〜15 の中央値 1790（Debian の 118%）、最初と最後の比 1.00。
- 原因: `transfer_run_lines` の run は最初の cache の hit の前で終わる。readahead の job（64 line、256 KiB）は散った line ごとに約 7 line の小さな command に切れ、worker 4 本の同期の読みでは device が遅い。
- 直し（35b527f23c、`src/kern/buf.c`）: prefetch（readahead の worker）が page list で読む時、run の中にある cache の line（2 本目から、run の半分まで）は ref を落として slot を NULL にし、その block の device の bytes は 1 page の bucket（`readahead_bucket`、workers を作る時に `kern_pmem_alloc` で 1 度、取れなければ前と同じ動き）に読ませて捨てる。cache の line には触らない（dirty でも clean でもそのまま）。NULL の slot は busy を取らず、状態の更新・完了・`release_run` で飛ばす。run の末尾の NULL は切る。prefetch でない読み、page list でない読み、書きは前と同じ。ついでに page list の名付けで 1 page でない line を見た時に `pages` を NULL にした後も `pages[index]` に書く形だったのを、そこで止めるようにした。
- 確かめ（kernel ea880d69a7b53468、q2044b、5330）: 乱読み QD32（987 MB/s）の直後の 1 GiB の連続の読み 1471 MB/s（Debian の cache を捨てた後の chunk の中央値 1511 の 97%）。その 1 GiB を zedBSD で読んだ cksum（260050290 1073741824）が Debian で同じ block を読んだ cksum と一致。read mode の bs 64 KiB・1 MiB・4 MiB は 1917・2684・1491、chunk0・1 は 2237・2065（Debian の 97〜177%、256 MiB の段は 10 ms 刻みで ±8%）。seqcold の chunk の中央値 2065、乱読み QD1 57.6・QD32 1004.9（main の kernel の q2044a は 1790・57.6・996.4、chunk は 10 ms 刻みのぶれの内）。build（x86_64）rc 0・warning 0。
- 未実施: QEMU の回帰（試験の image の作り方が今の build system に合っていない、p007）、他の機種の build、file system の prefetch（`buf_prefetch`）の道での確かめ（同じ関数を通るが測っていない）。
- 受け入れ 1 の材料: 乱読みの前（seqcold）の最初と最後の 256 MiB の比 1.00（main の kernel）、乱読みの後（read mode、bucket の kernel）0.92。
