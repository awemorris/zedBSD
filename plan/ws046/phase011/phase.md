<!-- awesome-plan project=zedbsd record=ws046p011 -->

# ws046-p011: private の file の mapping で page cache の page を map する（実装）

Phase ID: `ws046-p011`
Parent: [WS046](../ws.md)
Status: uncleared（q420-i01、2026-09-25。実装したが configure が遅くなったので戻した）
Queue: q420（q420-i01）

## 目的

[ws046-p010 の設計](../phase010/phase.md)の 1〜5 を実装し、試験の節の全てを確かめる。

## 受け入れ

- `ffault`（`libLLVM.so`）の warm の fault が 1 page 数 µs、link と compile の時間が下がる。
- 正しさの試験（COW、fork、mprotect、write の後、truncate）が通る。guest の sh・make の差分試験・対話試験、`SMP-STRESS.ELF`、4 platform の build（warning 0）と boot。
- 変えた code は規約の全文。

## q420-i01（2026-09-25、uncleared: 戻した）

設計の 1〜5 を実装した（差分は [attempt-q420.diff](attempt-q420.diff)、試験の program は [cowtest.c](cowtest.c)・[ffault.c](ffault.c)）。途中で、COW の複写が
`vm_object_page_pin_read()` の「pin されていない page は EINVAL」で失敗し `init` が起動時に SIGSEGV になったので、private の region の page は複写の間 `vm_object_page_pin()` するようにした。

| 測定（guest amd64 8 GiB、full LTO） | p009 の後 | この試み |
| --- | --- | --- |
| COW の試験（private の書き込み、fork、mprotect、write の後） | — | COWTEST OK |
| link（`cc t.o -o t`） | 0.51〜0.68 秒 | 0.40〜0.45 秒 |
| compile と link（`cc t.c -o t`） | 0.99〜1.08 秒 | 0.61 秒 |
| `cc -c` | 0.34〜0.37 秒 | 0.20〜0.23 秒 |
| kbench の file の fault（`/bin/sh`） | 18 µs | 9.7 µs |
| `ffault`（`libLLVM.so` を map して全 page を触って unmap、を繰り返す） | 25 µs/page | **102 µs/page**（悪化） |
| **expat の `./configure`** | 128〜129 秒 | **270 秒（悪化）** |
| expat の `make` | 91〜97 秒（status 0） | **status 2（477 秒）**。原因は調べていない（guest の disk は捨てた） |

個々の compile と link は速くなったが、configure（短い process を数千）は 2 倍遅くなり、make が失敗したので戻した（`src/kern/vmspace.c`・`include/kern/vmspace.h` を q418 の commit の状態に）。
戻した kernel で expat の configure は 128 秒（p009 の後と同じ）。

分かったこと（`ffault` の標本）: 最後の unmap で object の mapping の参照が 0 になると、`vm_object_put()` が object を `DETACHING` にし、`vm_object_sync_range_buffer()` が
object の全 page を walk して同期し、cache の参照（`VM_OBJECT_CACHE_REFERENCE`、上限 `VM_OBJECT_CACHE_OBJECTS` = 32、満杯だった）が無い object は page ごと壊す。
mmap が作った object は cache の参照を持たないので、process が終わるたびに library の object が壊れ、次の process は buffer cache から作り直す。
また、最後の mapping を落とすとき、その object に読みの操作が走っていると待つ（`active_operations`）。短い process を並べる configure ではこの寿命の費用が効いたと見ている（未検証）。

再開の条件: 設計を直す（ws046-p012）。private の mapping の object を process の間で保つ（cache の参照を取る、満杯なら古い idle な object を追い出す）、clean な object の最後の unmap で全 page の同期の walk を省く、
最後の mapping の解放が読みを待たない、make の失敗の原因。
