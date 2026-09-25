<!-- awesome-plan project=zedbsd record=ws046p014 -->

# ws046-p014: private の file の mapping で page cache の page を直接 map する（p011 の当て直し）

Phase ID: `ws046-p014`
Parent: [WS046](../ws.md)
Status: uncleared
Queue: q434-i01（uncleared）

## きっかけ

[ws046-p012](../phase012/phase.md)（q430）で object の寿命（LRU、mapping の object の cache 化、clean な walk の省略）を直した。p012 の設計 5（[p011 の差分](../phase011/attempt-q420.diff) の当て直しと expat の make の失敗の再現）は、cache の object が file の表を使う点（[F-013](../../future-work.md)）と合わせて設計し直すため、この Phase に分けた。

## 目的

p011 の効果（private の file の mapping で object の page を read-only で map し、書き込みで COW。link 1.0 → 0.61 秒）を、p012 の後の object の寿命の上で当て直し、expat の make の失敗を再現して原因を直す。

## 受け入れ

- expat の configure・make・check が status 0、configure は p012 の 91 秒より遅くならない。link・compile は p011 と同じ程度。COW の試験（`cowtest`）、`ffault`、sh・make の差分試験、`SMP-STRESS.ELF`、4 platform の boot。

## q434-i01（2026-09-25）: uncleared（kernel の変更は完了・適用。受け入れの「check が status 0」だけが bash の不在で未達、判断待ち）

### 変更

[p011 の差分](../phase011/attempt-q420.diff)を現行の tree に当て（衝突なし）、次を直した（全体は [applied-q434.diff](applied-q434.diff)）:

- `mprotect` の commit: private の file の region が object の page を map する（`VM_REGION_PRIVATE_OBJECT`）と `region->object` が非 NULL になり、従来の「object があれば共有」の判定で書き込み可能にしても commit を取らなかった（over commit の穴）。共有でない region と、private の object の region は commit を取る（`vmspace_protect_locked` の 2 箇所）。
- 規約の指摘（複数行の本体、段落の comment）。

p011 の失敗の原因（object の寿命: 最後の unmap で object を壊し、次の process が作り直す）は p012 で直してあった。p011 で見た expat の make の失敗（status 2）は再現しない（8 GiB・512 MiB とも status 0）。

### 測定（QEMU、amd64 guest 8 GiB、ws061-p002 の後と比べる）

| 測定 | p002 の後 | p014 |
| --- | --- | --- |
| kbench file fault | 6.7〜10.1 µs | **3.2 µs** |
| `ffault libLLVM` | 9.4〜10.7 µs/page | **3.0〜3.5 µs/page** |
| `clang --version` | 80〜102 ms | 60〜67 ms（fault 5797 → 5862: 書く page は読みの後に COW の fault がもう 1 回） |
| `ld.lld --version` | 50〜70 ms | 40〜55 ms |
| `cc t.c -o t` | 344〜426 ms | **245〜276 ms** |
| expat の configure（tmpfs） | 35〜38 秒 | **30〜33 秒** |
| expat の configure（root の overlay） | 59〜64 秒 | 58 秒（sh の差分試験と同時に走らせた） |
| expat の make -j4（tmpfs） | — | 23 秒、status 0 |
| 512 MiB の guest の configure・make | — | 34 秒・status 0 |

### 受け入れの照合

| 条件 | 結果 |
| --- | --- |
| expat の configure・make が status 0、configure は 91 秒より遅くない | 達成（上の表） |
| expat の check が status 0 | **未達**: `make check` は status 2。`tests/xmltest.sh` が `#! /usr/bin/env bash` で、base に bash が無い（`env: bash: No such file or directory`）。kernel とは関係ない。test の本体 `tests/runtests` を直接走らせると **4932/4932 pass**、`xmlwf` も動く |
| link・compile が p011 と同じ程度 | 達成（p011: `cc t.c -o t` 0.61 秒、今 0.25〜0.28 秒） |
| COW の試験 | OK（8 GiB・512 MiB） |
| `ffault` | 3.0〜3.5 µs/page |
| sh の差分試験 | 1388/1425（p002 と同じ 2 件の差: glob の 1 件が通り、`noclobber on &> >` は background の出力の順序の競走） |
| make の差分試験 | 91/91（8 GiB・512 MiB） |
| `SMP-STRESS.ELF` | 4/4、5/6（1 回 status 7、HEAD でも 12 回に 1 回の BUG-045 の類） |
| boot | amd64: `plan/tools/boot-test.sh` PASS（`build/boot-test-amd64-q434/login.png`）。rpi4・pcat・pc98 の kernel の build は diag 0、boot は未実施（変えたのは arch に依らない `src/kern/vmspace.c` だけ） |
| 規約 | 変えた行に `style-check.py` の指摘 0 |

実機: 未実施。

要る判断（ユーザー）: 「check が status 0」を `tests/runtests` の全 pass（4932/4932）で満たしたとみなすか、bash を package として足して `make check` を通すか。
