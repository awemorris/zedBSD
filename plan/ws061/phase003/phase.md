<!-- awesome-plan project=zedbsd record=ws061p003 -->

# ws061-p003: fault-around: file の fault で object に resident な隣の page をまとめて map する

Phase ID: `ws061-p003`
Parent: [WS061](../ws.md)
Status: uncleared
Queue: q435-i01（uncleared）

## 目的

Linux の fault-around（16 page）に倣い、1 回の fault で object に resident な前後の page を read-only で map する（private の mapping は複写、p014 の後は直接 map）。目標: `clang --version` の fault 5797 を 1/8 以下に。

## 受け入れ

上の目標の数字を guest で測る。回帰: boot、sh・make の差分試験、`SMP-STRESS.ELF`、COW の試験、`ffault`。規約。HAL の実装の変更は承認不要、hal.h の変更は差分ごとの事前承認（2026-09-25 の Guardrail）。

## q435-i01（2026-09-25）: uncleared（fault の数は 1/3 まで。目標の 1/8 に届かず、BUG-051 が未解決）

### 変更（[applied-q435.diff](applied-q435.diff)）

| 変更 | 場所 |
| --- | --- |
| fault-around: object の page の読みの fault が成功したら、同じ region の揃った 16 page の窓の中で、object が既に持ち busy でない page を同じ VM の lock の下で map する（COW の region は read-only と COW の印、共有の書き込み可能な region は対象外、private の region は file の data の全体の page だけ、snapshot は pin した page だけ）。読みも待ちもしない | `src/kern/vmspace.c`（`vmspace_fault_around_locked`・`vmspace_fault_around_page_locked`） |
| 待たずに cache の page を hold する `vm_object_fault_resident` | `src/kern/vm.c`、`include/kern/vm-object.h` |
| amd64 HAL: `hal_space_map` の成功後の shootdown を削除（全ての leaf が not-present だったことを確かめており、x86 は not-present を cache しない）。shootdown は user の 32 page 以下の範囲を `invlpg` で、他は従来の CR3 の再読み込み（全 flush）で | `src/hal/amd64/space.c`（hal.h は不変） |

### 測定（QEMU、amd64 8 GiB）

| 測定 | ws046-p014 の後 | TLB の変更の後 | fault-around の後 |
| --- | --- | --- | --- |
| kbench file fault | 3.2 µs | 2.3 µs | 1.4 µs |
| `ffault libLLVM` | 3.0〜3.5 µs/page | 2.3 µs/page | 1.3 µs/page |
| `clang --version` | 60〜67 ms / 5862 fault | 55 ms | 52〜60 ms / **1891 fault**（目標 725 以下: 未達） |
| `ld.lld --version` | 40〜55 ms / 4611 fault | 46 ms | 32〜52 ms / 2511 fault |
| `true` | 5 ms / 352 fault | 4 ms | 5〜6 ms / 225 fault |
| `cc t.c -o t` | 245〜276 ms / 約 1.8 万 fault | | 214〜240 ms / 6760 fault |
| configure（tmpfs） | 30〜33 秒 | | 31 秒 |
| configure（root の overlay） | 58 秒 | | 55 秒 |

残りの fault は書き込み（relocation の COW、anon）で、fault-around の対象外。configure の中身: wall 32 秒、子の user 7.4 秒・system 23.4 秒、fault 52.7 万（1 fault 2〜4 µs で 1〜2 秒）。system の時間の大半は fault 以外（process の生成と終了、system call）。overlay の上の 24 秒は UFS → loop → FAT → USB の同期の書き込み。ユーザーの指示でこれは layout の変更（WS062）で扱う。

### 回帰（QEMU）

| 試験 | 結果 |
| --- | --- |
| build | amd64 image warning 0 |
| COW・itimer・`SMP-STRESS.ELF` | OK・OK・3/3 |
| make の差分試験 | 91/91（kernel の crash の記録 0） |
| sh の差分試験 | 1388/1425（前と同じ 2 件の差） |
| boot | 未実施（最後の image で `boot-test.sh` は走らせていない）。起動と SSH は guest.py で確認 |
| 規約 | 変えた行に `style-check.py` の指摘 0 |

**BUG-051**: SSH 越しの configure の後に `sshd-session` の子が 1 回 SIGSEGV（address 0 の読み）。3 回の再実行と make の差分試験では出ない。この変更が原因かどうか未確定。並列の負荷での再現はユーザーが止めた（layout の変更を先に）。

実機: 未実施。
