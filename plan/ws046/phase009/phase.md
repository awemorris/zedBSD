<!-- awesome-plan project=zedbsd record=ws046p009 -->

# ws046-p009: guest の configure が遅い原因を調べて直す

Phase ID: `ws046-p009`
Parent: [WS046](../ws.md)
Status: uncleared（q418-i01、2026-09-25。原因 2 つを直した。残りの最大の原因は HAL で承認待ち、次は p010）
Queue: q418（q418-i01）

## きっかけ

2026-09-25 ユーザー判断: 「ゲストでconfigureすると遅すぎるようです。その原因を探って修正する必要があります。漫然と長時間configureやビルドを実行しないでください。
妥当なビルド時間であるか検討して、遅すぎるなら原因を調査して修正しましょう。」

測った事実（amd64 の guest、QEMU・KVM）:

- coreutils の configure: 約 20 分で 951 の check（1 check 約 1.3 秒。Linux の host では 1 check 数十 ms）。
- expat の configure: 204〜252 秒（host は数秒）。子の CPU 時間は user 14 秒・**sys 211 秒**（ws046-p007）: 時間の大半は kernel。
- kbench（WS053、guest）: fork+exit+wait 約 1.4 ms、fork+exec+wait 約 10 ms、anonymous の page fault 1 page 約 40 µs、file の page fault 約 22 µs。
  KVM の Linux の guest ならそれぞれ 0.1 ms・0.5 ms・1〜2 µs 程度で、10〜40 倍遅い。
- （以前の p009 の計画だった 512 MiB の file の cache・USB の待ちは、guest の既定が 8 GiB になって（2026-09-24 ユーザー決定）compile の受け入れにほぼ届いたので、この Phase に含めない。要れば別に立てる。）

## 手順（長い configure を走らせず、小さな測定で進める）

1. 小さな測定: kbench、`sh -c` の loop で `/bin/true` の fork+exec、1 つの configure の check に相当する command（`cc -c conftest.c`、`sed`、`rm`）の時間。
2. gdbstub の標本（WS046 p007 と同じ方法）で、fork・exec・page fault・exit の kernel の中の時間の行き先を特定する。
3. 原因ごとに直す（根拠のあるものから、2 つまで）。HAL が要るなら差分ごとに承認を求める（uncleared にして記録）。
4. 1 の測定を繰り返し、最後に expat の configure を 1 回だけ測る（時間の上限 5 分）。

## 受け入れ

- fork+exec+wait と anonymous の page fault が根拠のある目標（p001 の測定の後に決める）に届く。expat の configure の時間が下がる。
- kernel の build（warning 0）、boot test（4 platform）、guest の sh・make の差分試験・対話試験、kbench が悪くならない。

## 結果（q418-i01、2026-09-25）

### 測った事実（小さな測定、長い configure は走らせない）

- `times` の子の sys の時間は当てにならない（`sed` 100 回で sys 48 秒と出るが、壁時計は 1 回 10〜20 ms。kernel の時間の勘定が過大）。以後は壁時計と gdbstub の標本で測った。
- 1 つの configure の check の中身: `cc -c` の空の file 0.31〜0.41 秒（clang の起動 0.2 秒）、**`cc t.c -o t`（compile と link）1.2 秒**（host では 0.03 秒）。
  configure の check の多くは compile と link なので、1 check 約 1.3 秒がこれで説明できる。
- 1 回の link の I/O（`vfs.io.stats` の差）: file の読み 14514 回・113 MB（全て buffer cache に当たり USB の読みは無い）、USB への書き 42・cache の flush 14。
  clang（145 KB）と ld.lld（6.9 MB）は `libLLVM.so`（81 MB、relocation 4.6 MB）・`libclang-cpp.so`（79 MB、relocation 5.9 MB）を読み込み、動的 loader の relocation と実行で数万の file の page を fault する。
- link の間の kernel の標本（LTO を外した kernel、関数の境界を保つ）: kernel 186・user 5。file の page の fault、process の後始末、HAL の `rdmsr`（下）。

### 原因 1: region の page の探索が list をたどる（`find_page()`、`src/kern/vmspace.c`）

file を map して page を触るだけの試験（`ffault`、`libLLVM.so` の 19771 page）で **1 page 145〜154 µs**（小さな `/bin/sh` では 22 µs）。標本の 34/45 が `vmspace_fault()` の中の loop:
`find_page()` が region の全 page の list をたどっていた。大きな library を fault で読み込むと 2 乗になる。

直し: region に page の index（address の hash、bucket は page の数の 2 倍の 2 の累乗、page が 32 以上で作る、作れなければ list に戻る）を足した。
`find_page()` は index の 1 bucket だけを見る。page を list に足す・外す・移す所（fault の placeholder、fault の失敗、共有の mapping の revoke、fork の複製、region の分割、brk の縮小、region の page の全解放）で index を保ち、region を解放するとき index も解放する。
- `include/kern/vmspace.h`: `struct vm_page` に `index_next`、`struct vm_region` に `page_index`・`page_index_size`・`page_count`。

### 原因 2: reclaim の queue から外すのが queue を先頭からたどる（`queue_remove()`、`src/kern/vm.c`）

process の後始末で page ごとに、system の全ての追跡中の private page の list を先頭からたどっていた（後始末が process の page 数 × system の page 数）。
直し: `struct vm_private_page` に `queue_prev` を足して双方向にし、`queue_insert()`・`queue_remove()` を定数時間にした（queue に無い backing は今までどおり何もしない）。

あわせて `fill_file_page()` は page 全体を 0 で埋めてから読んでいたのを、読まない部分だけ 0 にするようにした（短い読み・溢れのときは page 全体を 0 に）。

### 効果（guest amd64 8 GiB、full LTO の kernel）

| 測定 | 前 | 後 |
| --- | --- | --- |
| `ffault` の file の page の fault（`libLLVM.so`、warm） | 145〜154 µs/page | **24.8〜25.0 µs/page** |
| kbench の anonymous の fault | 35.7〜40.8 µs | **12.9 µs** |
| kbench の file の fault（`/bin/sh`） | 22.2 µs | 18.1 µs |
| `cc t.o -o t`（link） | 0.78〜0.89 秒（LTO 無し） | 0.51〜0.68 秒 |
| `cc t.c -o t`（compile と link） | 1.13〜1.27 秒 | 0.99〜1.08 秒 |
| expat の `./configure`（163 check） | 204〜252 秒（512 MiB）・150 秒（2 GiB） | **129 秒**（8 GiB） |

### 残り（受け入れに届かない）

host の数十倍はまだ遅い（1 check 約 0.8 秒）。修正の後の link の間の kernel の標本（182）:

1. **HAL の `amd64_percpu_current()` の `rdmsr IA32_GS_BASE`**（標本の約 24%）。lock・`thread_current()` のたびに呼ばれる。`%gs:0` の load にする差分を
   [hal-percpu-gs.md](hal-percpu-gs.md) に置いた（**未適用、差分の承認待ち**。確認事項）。
2. file の page の fault の中の複写: buffer cache → VM object の page → process の private の page と、prefetch の複写（`memcpy` の標本 29）。
   read-only の file の mapping で page cache の page をそのまま map する（Linux のように）には VM の設計の変更が要る → **ws046-p010**（設計から）。
3. process の後始末の page table の解体（HAL の `detach_empty_tables()`）、private の page の metadata の解放。

調べる範囲（根拠のある原因 2 つまで）を使い、残りは HAL の承認と設計の Phase が要るので uncleared。

### 回帰

| 検証 | 結果 |
| --- | --- |
| build（4 platform の disk image と guest、full LTO） | status 0、我々の code の warning 0 |
| boot test | amd64（`build/boot-test-amd64-q418`）・rpi4・pcat で login prompt、pc98 は `pc98-boot.py` で login |
| guest の sh の差分試験・make の差分試験・対話試験 | 1388/1425（落ちる集合は前と同じ）・91/91・41/41（[evidence/](evidence/)） |
| `SMP-STRESS.ELF`（mmap・fork の stress） | status 0 |
| 規約 | 変えた行は `style-check.py` の報告 0 |
| 実機 | 未実施 |

## 結果（q422-i01、2026-09-25、HAL の差分の適用）

承認: ユーザー「まず、承認待ちのHALの変更を許可します。」（2026-09-25）。[Guardrail](../../guardrail.md) の表に登録。

適用: [hal-percpu-gs.md](hal-percpu-gs.md) のとおり `src/hal/amd64/percpu.c` の `amd64_percpu_current()` を `rdmsr IA32_GS_BASE` から `movq %gs:0, %0`（`self` の load）に、`percpu.h` に `self` が先頭の field である `_Static_assert` を足した。hal.h は不変。

### 効果（guest amd64 8 GiB、full LTO の kernel、kbench）

| 測定 | 前（q418 の修正の後） | 後 |
| --- | --- | --- |
| syscall `getppid` | 1340 ns | **450 ns** |
| pipe の往復 | 74450 ns | 43600 ns |
| fork + exit + wait | 1.29 ms | 0.75 ms |
| fork + exec `true` + wait | 9.10 ms | **6.51 ms** |
| anonymous の page fault | 12.9 µs | 9.2 µs |
| file の page fault（`/bin/sh`） | 18.1 µs | 12.5 µs |
| expat の `./configure`（163 check） | 128〜129 秒 | **96 秒** |

### 回帰（全 lock と `thread_current()` に効くので広く流した）

| 検証 | 結果 |
| --- | --- |
| build（amd64 の CI の disk image、intelmac の vmunix、guest、full LTO） | status 0、我々の code の warning 0 |
| boot test | amd64 で login prompt（`plan/tools/boot-test.sh`、PNG をユーザーに提示） |
| guest の sh の差分試験 | 1388/1425（落ちる集合は q418 と同じ 37） |
| guest の make の差分試験・対話試験・`SMP-STRESS.ELF` | 91/91・41/41（2 回）・status 0（新しい guest で実行。最初の実行は expat を同じ guest に展開して /root が満杯になり無効） |
| 規約 | 変えた行は `style-check.py` の報告 0 |
| 実機 | 未実施 |

### 受け入れと残り

configure は 204〜252 秒 → 96 秒（2.1〜2.6 倍）。host（数秒）との差は残る。残りの原因は file の fault の複写と page cache の object の入れ替え（p011 の実装で分かった。p012 で設計を直してから p011 を再適用する）。
この Phase の調べる範囲（原因 2 つ + HAL）は使い切ったので q422-i01 は **cleared**（HAL の差分の適用と回帰）。BUG-033 の残りは p012 に引き継ぐ。
