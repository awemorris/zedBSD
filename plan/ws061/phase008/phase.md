<!-- awesome-plan project=zedbsd record=ws061p008 -->

# ws061-p008: make を host と同等に（libc の同期の system call）

Phase ID: `ws061-p008`
Parent: [WS061](../ws.md)
Status: cleared
Queue: q444-i01（cleared）
Disposition: normal

## 目的

2026-09-26 ユーザー「makeの時間はまだ遅いので、こちらも同等を目指しましょう。また、configureの方も、おそらくページキャッシュを最適化すれば、Linuxホスト並みにできると思いますので、目標を維持して取り組んでください。」

## 調べたこと（QEMU 8 GiB 4 vCPU NVMe、root に journal、expat の make（直列））

- make の子の時間: guest user 10.78 秒・system 6.06 秒（実時間 17.9 秒）、host user 13.0 秒・system 2.57 秒（実時間 15.2 秒）。差は kernel。
- host の perf: `amd64_syscall_entry` 9.2%、`kernel_syscall_handler` 3.3%、`kernel_user_return_handler` 2.3%。system call の出入りが kernel の時間の半分。
- gdb で system call の入口の番号を 2,000 回数えると **1,984 回が `usync`（116）**。libc の同期が待つ者がいなくても kernel に入っていた:

| 場所 | 問題 |
| --- | --- |
| `word_lock`/`word_unlock`（malloc の `heap_lock`、環境変数、registry） | unlock のたびに wake の system call（malloc・free のたび） |
| `pthread_mutex_unlock` | 同上 |
| `pthread_cond_signal`・`broadcast` | 待つ者がいなくても wake |
| `pthread_rwlock_unlock` と rwlock の guard | 同上。待つ側に、trylock の失敗と sequence の読みの間の unlock を取りこぼす窓 |
| `pthread_once` | 済んだ後も呼ぶたびに状態を書き換え全員を wake |
| barrier の guard、semaphore の guard | unlock のたびに wake |

## 変更

| # | 変更 | 場所 |
| --- | --- | --- |
| 1 | lock の word を 3 状態に（0 解放、1 保持、2 保持で待ち有り。futex の mutex の定番）。unlock は 2 のときだけ wake。`word_lock`、`pthread_mutex_*`、rwlock と barrier の guard、semaphore の guard | `userland/base/libc/pthread.c`、`semaphore.c` |
| 2 | 条件変数と rwlock は sequence の最上位の bit を「待つ者がいる」印にし、signal・broadcast・unlock は印があるときだけ wake。rwlock の待つ側は trylock の失敗の後に印を立てて trylock をやり直してから眠る（取りこぼしの窓も閉じる） | `pthread.c` |
| 3 | `pthread_once` は済んでいれば書かずに戻る。状態 3（走っていて待つ者がいる）を足し、済んだときに 3 なら wake | `pthread.c` |

構造体（ABI）は変えない（同じ word の値を増やすだけ）。

## 受け入れ

make（直列）が host の `make -j1`（15.2 秒）と同程度。configure も維持か改善。回帰（make・sh の差分試験、SMP、COW、boot）と pthread を使う試験。

## 進行の記録（q444-i01、2026-09-26）

### 結果（QEMU 8 GiB 4 vCPU NVMe、root に journal、host は Linux 64 core・同じ clang）

| 測定 | 前 | 後 | host |
| --- | --- | --- | --- |
| make（直列、`/root`） | 17.7〜17.9 秒（user 10.78・system 6.06） | **12.8〜13.1 秒**（user 9.1〜9.5・system 3.0〜3.2） | `make -j1` 15.2 秒（user 13.0・system 2.57） |
| configure（`/root`） | 11.29〜11.34 秒 | 11.28〜11.52 秒（user 4.6〜4.8・system 6.1〜6.8） | 10.9〜11.2 秒（user 6.2・system 5.8） |

make は host より速くなった。configure の直後の計測（11.3〜11.5 秒）は build の直後の起動で cache が冷えていた回で、温まった状態で 5 回測り直すと **9.7〜10.35 秒**（host 10.95〜11.0 秒、同じ時に 3 回）。`cc t.c -o t` は 88〜109 ms（host 83〜86 ms、1.2 倍）。

### 回帰

| 試験 | 結果 |
| --- | --- |
| `plan/ws061/tests/lock-stress.c`（mutex・once・semaphore を 4 thread で各 20 万回、rwlock の読み書き、条件変数の producer/consumer） | 3 回とも LOCK-STRESS-OK（競合させると 12〜16 秒。adaptive spin の材料） |
| `userland/base/tests/posix-r2.c` | `conformance-identity errno=52`: stdout の `ttyname_r` が `/dev/console` を求める検査で、SSH から file に向けると `ENOTTY`。console から走らせる前提で、今回と無関係 |
| boot test | PASS（`build/boot-test-amd64-p008/login.png`） |
| make の差分試験 | 91/91（8 GiB・512 MiB） |
| `smp-resource-stress` | 512 MiB 6/6、8 GiB 26 回中 2 回 7: 差は `bio` 1 → 0（基準の時に flusher か journal の I/O が進行中。減る方向で漏れではない） |
| COW・itimer・swaphog 450 MiB・`dir-grow.sh` | OK |
| sh の差分試験 | 1389/1425（`noclobber on &> >` の順序に依存する case のみ） |

2026-09-26 ユーザー提案（記録）: lock が取れないときにすぐ眠らず、ユーザ空間で少し回ってから眠る（adaptive spin）を性能の改善の時に試す。Outlook に置いた。
