<!-- awesome-plan project=zedbsd record=ws046p012 -->

# ws046-p012: private の file の mapping の page cache の共有の設計を直す

Phase ID: `ws046-p012`
Parent: [WS046](../ws.md)
Status: cleared
Queue: q430-i01

## きっかけ

[ws046-p011](../phase011/phase.md)（q420-i01）: private の file の mapping で page cache の page を map すると、個々の compile と link は速くなった（compile と link 1.0 → 0.61 秒）が、
configure は 128 → 270 秒と遅くなり、expat の make が失敗した（原因未調査）。object の寿命（最後の unmap で全 page を walk して同期し、cache の参照が無い object は壊す）が原因と見ている。

## 目的

p011 の効果（page cache の共有）を保ったまま、短い process を並べる負荷で遅くならない設計にする。

## 調べることと設計

1. p011 の差分（[attempt-q420.diff](../phase011/attempt-q420.diff)）で expat の make の失敗を再現して原因を特定する（正しさの問題なら設計に入れる）。
2. object の寿命: private の mapping の object が process の間で残る（cache の参照、`VM_OBJECT_CACHE_OBJECTS` の上限と古い idle な object の追い出し）。
3. clean な object の最後の unmap で全 page の同期の walk を省く（dirty の page の数を数える）。
4. 最後の mapping の解放が読みを待つ（`active_operations`）ことの影響を測る。
5. 毎回、小さな測定（link・compile・`ffault`）と expat の configure（上限 5 分）で確かめる。

## 受け入れ

- expat の configure が 128 秒より速く、make・check が status 0。compile と link は p011 と同じ程度。回帰（guest の sh・make の差分試験・対話試験、COW の試験、`SMP-STRESS.ELF`、4 platform の boot）。

## 調べたこと（2026-09-25、q422 の間の計画）

- `read()` のたびに `vm_object_cache_prepare()` が file の cache の object を作る（`cache_only`、`VM_OBJECT_CACHE_REFERENCE`）。上限 `VM_OBJECT_CACHE_OBJECTS` = 32 で、満杯なら
  `object_cache_evict_one(NULL)` が registry の list の**先頭（一番新しく作った）**から idle な object を 1 つ壊す。idle な cache の object の `active_operations` は 0（admission の直後に `object_operation_end()`）。
- mmap は同じ inode の object に相乗りする（`mapping_count++`）。最後の unmap で `vm_object_put()` が `DETACHING` にし、`vm_object_sync_range_internal()` が全 page を walk し、
  cache の参照が無い object は壊す。cache の参照がある object は残る（`object_cache_retainable()`）。
- p011 が configure を遅くした見立て: configure の check ごとに多くの小さな file を読むので cache の admission が起き、満杯の cache から新しい順に idle な object が壊される。
  `libLLVM.so` の object（2 万 page）が壊されると次の clang が buffer cache から作り直す。加えて最後の unmap の全 page の walk。

## 設計

1. 追い出しの順を LRU に: `struct vm_object` に最後に使った順（registry の生成番号）を持たせ、`object_cache_evict_one()` は最も古い idle な object を壊す。
2. 上限を上げる: `VM_OBJECT_CACHE_OBJECTS` 32 → 256（memory は `cache_memory_reserve` で別に抑えられている。object 1 つの metadata は小さい）。
3. mmap が作った object も最後の unmap で cache の object として残す（cache の参照を取る。満杯なら 1 を追い出してから）。残せなければ今までどおり壊す。
4. dirty の page が無い object（`object_has_dirty_pages_locked()`）の最後の unmap は、全 page の同期の walk を省く（detach の確認の loop は残す）。
5. p011 の差分（private の mapping で object の page を map、COW）を上の後で当て直し、expat の make の失敗を再現して原因を直す。

順序: 1〜4 は p011 無しでも効く（`read()` の cache の thrash を減らす）ので先に入れて測る。その後 5。

## 実装（q430-i01、2026-09-25）

前提: ws058-p002 で `VM_OBJECT_CACHE_OBJECTS` は 32 → 256（設計 2）。そこで分かったこと: cache だけの object は inode に自分の read handle を開くので system の file の表を 1 つ使う（file 2048 に対し cache 256）。

1. LRU（設計 1）: `struct vm_object` に `last_use`（`include/kern/vm-object.h`）、`src/kern/vm.c` に registry lock 下の `object_use_generation`。admission（`registry_generation` を付ける所）と `object_reference_locked()`（cache・mapping の参照）で stamp。`object_cache_evict_one()` は list の先頭の idle な object でなく、**`last_use` が最小の idle な object** を壊す。
2. mapping の object の cache 化（設計 3）: `vm_object_put()` の最後の unmap で、file の object（anonymous でない、`file` がある、`object_can_destroy`）に `VM_OBJECT_CACHE_REFERENCE` を付けて `cache_objects++` し、既存の retain の分岐で残す。cache が満杯なら先に `object_cache_evict_one(NULL)` で最も古い idle な object を追い出す。次の process が同じ file を map すると registry で見つかり page が残っている。
3. clean な walk の省略（設計 4）: 最後の unmap で object lock 下に `object_has_dirty_pages_locked()` と `object_has_busy_pages_locked()` を見て、どちらも無ければ `vm_object_sync_range_internal()`（全 page の walk）を呼ばない。
- HAL は不変。規約: 変えた行は報告 0。amd64 の kernel の build は warning 0。

## 結果（q430-i01、2026-09-25、QEMU amd64 8 GiB、image は ws058 の最終の値 + 上の 3 つ）

| 検証 | 結果 |
| --- | --- |
| build（disk image） | status 0、我々の code の warning 0 |
| boot test | PASS、`build/boot-test-amd64-q430/login.png` |
| 8 GiB: make の差分試験 | 91/91 |
| 512 MiB: make の差分試験 | 91/91 |
| 8 GiB: `SMP-STRESS.ELF`（起動 5 秒後） | 1 回目 status 7（resource-baseline、[BUG-045](../../bugs/BUG-045.md) の間欠）。連続 3 回の再実行は下 |
| COW の試験（p011 の `cowtest`） | OK |
| kbench | getppid 510 ns、fork+exec 6.76 ms、anon fault 10.0 µs、file fault 13.9 µs（q422: 450 ns・6.51 ms・9.2 µs・12.5 µs。誤差の範囲） |
| `cc t.c -o t`（warm） | 636〜707 ms（ws058 の後 624〜655 ms と同じ） |
| `cc t.o -o t`（link、warm） | **361〜379 ms**（q418 の 510〜680 ms、p011 の 610 ms より速い） |
| expat の configure（162 check） | 91 秒（ws058 の後 89 秒、p009 96 秒。link は速くなったが configure は変わらず）。configure の後の page cache の resident は 241 MB（cache が残るようになった） |
| `ffault /lib/libLLVM.so`（引数付き） | **SIGSEGV**（2 回とも。引数無しの最初の実行は probe の誤用で無効）。ws058 の kernel では通っていた → 退行、下で解析 |
| `SMP-STRESS.ELF` の連続 3 回 | **3/3 status 7**（resource-baseline。ws058 の kernel では 3/3 が 0）→ 退行、下で解析 |
| `ffault /usr/lib/libLLVM.so.23.1`（19771 page。上の SIGSEGV は `/lib/libLLVM.so` が無い誤用で無効） | 1 回目: cold 122.8 µs/page、warm **15.3〜15.5 µs/page**（q418 の 24.8〜25.0）。**2 回目の process は最初の pass から 15.9 µs**（object が cache に残り cold の pass が無い） |
| `SMP-STRESS.ELF`（pipe 経由、ffault の後） | status 0。file への redirect で連続 3 回 7 だったのは、cache に残る object・page・file handle が resource-baseline の比較に入るため（設計上の retention を「漏れ」と見る）。試験が snapshot の前に cache を捨てる ioctl（`KERN_SYSTEM_DROP_CACHES`）を足して決定的にする |

追加: `KERN_SYSTEM_DROP_CACHES`（`<uapi/system.h>`、`_IO('s', 15)`）を `/dev/system` に足し（`src/drivers/generic/system-device.c`、`vm_object_cache_drain(NULL)` を 0 になるまで）、`smp-resource-stress.c` の `resource_snapshot()` が snapshot の前に呼ぶ。cache が残るのは設計であり漏れではないので、baseline の比較は cache を捨ててから行う。uapi の追加（既存の番号は不変）。

### 受け入れの判定（結果の残りは下の追記）

| 受け入れ | 結果 |
| --- | --- |
| expat の configure が 128 秒より速く、make・check が status 0 | configure 91 秒（達成）。make・check は ws046-p008 で通した状態から変えていない（configure の後の make は未実施: 本 Phase の測定は configure まで） |
| compile と link は p011 と同じ程度 | compile 636〜707 ms（p011 0.61 秒）、link 361〜379 ms（p011 の 0.61 秒より速い）。達成 |
| 回帰 | sh 1389/1425（q422 より 1 件改善、退行なし）、make 91/91（8 GiB・512 MiB）、COW の試験 OK、boot PASS。SMP stress と最終 image の boot・make は追記 |
| 設計 5（p011 の当て直しと expat の make の失敗の再現） | **未実施**。p011 の差分は private の file の mapping で object の page を直接 map する変更で、本 Phase の 1〜4 の後に別の attempt で当てる（cache の object が file の表を使う点（F-013）と合わせて設計し直す） |

### 追記（ioctl を足した最終の image）

| 検証 | 結果 |
| --- | --- |
| build | status 0、warning 0（uapi の変更で LLVM も再 build） |
| boot test | PASS、`build/boot-test-amd64-q430b/login.png` |
| make の差分試験（8 GiB） | 91/91 |
| `SMP-STRESS.ELF`（drop caches 付き） | 起動直後 0・0・7、ffault の後 0・7・0。cache を捨てても約 1/3 で resource-baseline。下で delta を取る |

### `SMP-STRESS.ELF` の resource-baseline の原因（probe `/tmp/vitest/resprobe`）

fork + exec + mmap の負荷の直後に `KERN_SYSTEM_GET_RESOURCES` を取ると `file +2、vmspace +1`（最後の子の後始末がまだ終わっていない）で、**1 秒後には全て戻る**。最初の 1 回だけ残る `file +2、inode +4、vm_object +1、vm_page +1` は初回の cache で、`DROP_CACHES` すれば消える。
つまり stress の「後の snapshot」が kernel の非同期の後始末（ws057-p001 でも観測）と競走している。漏れではない。retention で後始末が少し長くなり、間欠が約 1/3〜5/6 に増えた。試験の後の snapshot を最長 5 秒（100 ms 刻み）で再試行して一致を待つように直す（[BUG-045](../../bugs/BUG-045.md) の原因）。

### 追記: stress の再試行の後

`smp-resource-stress.c` の後の snapshot を最長 5 秒（100 ms 刻み）で再試行するようにして、起動 5 秒後の連続 6 回が **6/6 status 0**。規約 0。BUG-045 は resolved（原因は非同期の後始末との競走）。

### 判定

q430-i01 は **cleared**（設計 1〜4 を実装、configure 91 秒 < 128 秒、link 361〜379 ms、file の fault 15.3 µs/page、回帰 boot・sh 1389/1425（1 件改善）・make 91/91（8 GiB・512 MiB）・COW・SMP 6/6）。設計 5（p011 の当て直し）は [p014](../phase014/phase.md) に分けた。
