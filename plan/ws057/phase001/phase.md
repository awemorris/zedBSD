<!-- awesome-plan project=zedbsd record=ws057p001 -->

# ws057-p001: commit の課金と上限の現状の調査と、方針との差の設計

Phase ID: `ws057-p001`
Parent: [WS057](../ws.md)
Status: cleared
Queue: q425-i01

## 目的

kernel の VM で、どの操作が commit を課金し（`commit_size`、`vm_commit_resize_swap()`）、上限が何で（swap の総量、主記憶）、超えたときに何が起きるか（ENOMEM、OOM、遅延）を code と guest の試験で確かめる。
[design policy 10](../../master-design-policy.md)（reserve は無制限、commit は over commit 禁止で swap の裏打ち）との差を表にし、直す設計を書く。

## 受け入れ

- 経路ごと（`mmap` の anonymous/private file/`MAP_NORESERVE`、`brk`、fork の COW、stack の伸長、shm、`mprotect` で書けるようにする）の課金の表。
- guest（8 GiB、swap あり/なし）での試験: 裏打ちを超える commit が失敗するか、reserve だけの mapping が成功するかの実測。
- 方針との差と、直す設計（変える関数・データ構造・試験）。裏打ちの定義（swap だけか swap + 主記憶か）はユーザーに問う。
- code は変えない（設計の Phase）。

## 事前の所見（2026-09-25、code を読んだ範囲。試験は未実施）

- commit の会計は `src/kern/vm.c` の Commit 節にある: `vm_commit_reserve(bytes)` は「使用中 + 要求 > 上限」で **ENOMEM**（この層で over commit はしない）、`vm_commit_release()` で返す。統計は `vm_commit_get_stats()`。
- 上限 = 初期化時の空き物理 page − `VM_COMMIT_SYSTEM_RESERVE_PAGES`（64）+ swap の page（`vm_commit_resize_swap()` で swap の付け外しに追随。使用中が新しい上限を超える縮小は ENOMEM）。**物理 + swap** であり「swap だけ」ではない。方針の「swap の裏打ち」の定義（物理 + swap か、swap だけか）はユーザーの判断待ち。物理の分は起動時の空き量で固定（総量ではない）。
- reserve と commit の分離: `prot == 0`（`PROT_NONE`）の mapping は commit を課金しない（`vmspace.c`「Inaccessible memory is not committed until it becomes accessible」）。`mprotect` で読み書き可能にした時（`vmspace_protect_locked`）に課金。これが reserve に当たる。
- 課金の経路: anonymous private（`prot != 0`）は map 時に全量、shared anonymous は region では 0 で object 側（`vm.c:434`）、file の private で書ける mapping は全量、stack は作成時（`vmspace_map_stack_locked`）、brk は伸長分（`vmspace_brk_locked`）、tmpfs は page ごと（`tmpfs.c`）。fork は子の region を親の `commit_size` で作る（`vmspace_fork_locked`）ので子も課金される。
- `MAP_NORESERVE` は kernel の code に出てこない（無視 = 方針どおり）。
- p001 で残る確認: 各経路の guest での実測（上限で ENOMEM、`PROT_NONE` の大きな reserve は成功）、`mprotect` で `PROT_NONE` に戻した時の release、object 側の課金の対称性、上限の物理の分を総量にするかどうか。

## 調査（q425-i01、2026-09-25）

### code から分かること（経路ごとの課金）

| 経路 | 課金 | 場所 |
| --- | --- | --- |
| `mmap` anonymous private、`prot != 0` | map 時に全量 `vm_commit_reserve(size)`。失敗は ENOMEM | `vmspace.c` の anonymous の map（`commit_size = size`） |
| `mmap` anonymous private、`PROT_NONE` | **0**（reserve）。後で `mprotect` で accessible にしたとき全量 | 同上、`vmspace_protect_locked` |
| `mmap` anonymous shared | region では 0、object の作成で全量（`vm_object_create_anonymous`） | `vm.c` |
| `mmap` file private、書ける | 全量（COW の可能性の分） | file の map |
| `mmap` file private、読むだけ / file shared | 0（page cache が裏打ち） | 同上 |
| `brk` の伸長 / 縮小 | 伸長分を reserve / 縮小分を release | `vmspace_brk_locked` |
| stack | 作成時に全量 | `vmspace_map_stack_locked` |
| fork | 子の region を親の `commit_size` で作る → 親と同量を子も課金 | `vmspace_fork_locked` |
| `munmap`・region の解放 | `commit_size` を release（split は比例配分） | `vmspace_unmap_locked`、`split_region` |
| `mprotect` accessible → `PROT_NONE` | **release しない**（unmap まで保持。保守的で over commit にはならない） | `vmspace_protect_locked` |
| tmpfs の page | page ごとに reserve | `tmpfs.c` |

- 上限（`vm_commit_init`、`src/kern/main.c` から起動時に 1 回）= 起動時の空き物理 page − 64 + swap の page。swap の付け外しは `vm_commit_resize_swap` で追随（使用中を下回る縮小は ENOMEM）。物理の分は以後更新されない。
- `MAP_NORESERVE` は `<uapi/mman.h>` に**無く**、mmap は未知の flag に **EOPNOTSUPP** を返す。方針（reserve は無制限、commit は厳密）では `MAP_NORESERVE` は「無視」が正しいので、定数を定義して受け付けて無視すべき（Linux 向けの allocator が渡す）。同様に `MAP_STACK`・`MAP_POPULATE` などの無視できる flag も検討。
- 統計は `/dev/system` の `struct vm_statistics`（`vm_commit_limit`・`vm_commit_used`・`vm_commit_available`）で読める。

### guest での実測（QEMU amd64 8 GiB、swap 64 MiB、`/tmp/vitest/commit/commitprobe{,2,3,4}`）

| 試験 | 結果 |
| --- | --- |
| 上限（`KERN_SYSTEM_GET_VMSTAT`） | limit 8237 MiB、used 21 MiB（起動直後）。= 物理 8 GiB − 予約 + swap 64 MiB |
| `PROT_NONE` の reserve | 1・2 GiB は成功し commit は増えない（reserve と commit は分かれている）。**4 GiB 以上は EINVAL**（BUG-048） |
| RW の commit | 6・7 GiB 成功、8 GiB EINVAL、**9 GiB 成功**（実際は 1 GiB、BUG-048）。上限を超える要求は正しく試せていない |
| 4 GiB + 4 KiB・5 GiB・9 GiB の mapping | offset 4 GiB を触ると **SIGSEGV**（silent に短い、BUG-048） |
| `mprotect(len = 4 GiB + 4 KiB)`（2 GiB の region） | 成功（4 KiB と解釈）。`munmap(len = 4 GiB)` EINVAL（BUG-048 は全 syscall 共通） |
| fork（親 1 GiB RW） | 子が生きている間 used +1 GiB（2070 MiB）。子の終了直後はまだ 2070、3 秒後に 1045（後始末が非同期で返る）。書いた子も同じ |
| `munmap` | used が元に戻る（21 MiB） |
| `MAP_NORESERVE`（Linux の 0x4000） | **EOPNOTSUPP**（定義されておらず拒否） |

### 方針との差と設計（p002 で直す）

1. **BUG-048**（長さの 32 bit 切り捨て）: `src/kern/syscall.c` の `SYSCALL_PAGE_MASK` を `((uintptr_t)KERN_PAGE_SIZE - 1U)` にする（9 箇所の `& ~SYSCALL_PAGE_MASK` が直る）。`src/kern/vmspace.c` の `& ~(PAGE_SIZE - 1U)`（2554・5132・5220・5254・5294 行）も `~(uintptr_t)(PAGE_SIZE - 1U)` にする。これで reserve は address 空間（128 TiB）まで取れる。
2. **`MAP_NORESERVE`**: `<uapi/mman.h>` に定義（Linux と同じ 0x4000 を使うか独自の値かは ABI の選択。他の flag は独自の値なので独自でよい）し、mmap は受け付けて無視する（方針: reserve は無制限、commit は厳密）。`MAP_STACK`・`MAP_POPULATE` などは対象外（別の判断）。
3. **上限の定義**（ユーザーの判断待ち）: 今は「起動時の空き物理 − 64 page + swap」。案 (a) 物理 + swap のまま、(b) swap だけ。どちらでも `vm_commit_init` の 1 箇所。物理の分を「起動時の空き」でなく「総量 − kernel の予約」にするかも合わせて決める。
4. `mprotect` で accessible → `PROT_NONE` に戻しても commit を返さない: 方針（over commit 禁止）には反しないので現状維持（release するなら p003 以降）。
5. fork 後の子の commit の返却が非同期: 方針に反しない。記録のみ。

### 受け入れの判定

経路ごとの課金の表、guest の実測（上限で ENOMEM は BUG-048 のため 8 GiB 超を正しく試せず、9 GiB が 1 GiB として通ることを確認）、方針との差の設計、裏打ちの定義の問いを記録した。code は変えていない。q425-i01 は **cleared**。
