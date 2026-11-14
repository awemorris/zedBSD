<!-- awesome-plan project=zedbsd record=ws310-p002 -->

# ws310-p002: aml-host の option と `_OSC` の ASL の試験

Status: in-progress（2026-11-15 P2 q2106-i01: 実装済み・試験 PASS。cleared は Q1）

- Queue: q2106-i01（P2、worktree `p2-ws310`、branch `agent/p2-ws310`、基点 main 52b2e7e351）。
- 範囲: `plan/tools/aml-host/`（`aml-host.c`・`aml-host-wmi.c`・`aml-host-wmi.h`、`asl/osc.*`・`asl/hostopts.*` の新しい試験）、`plan/ws310/tests/osc-host/`（新）、この phase.md。`plan/tools/aml-host/Makefile` は変えていない（新しい .c を足さず、既存の file に入れた）。
- 設計: [design.md](../design.md) §6.1 の option の表と §8 の p002 の行。

## 受け入れ条件（design §8）

`make -C plan/tools/aml-host`・`run-asl.py`・`check-latitude5330.sh` が PASS、新しい ASL の試験が acpiexec と一致、tools.md の行の更新を Q1 に頼む。

## 実装

design §6.1 の 6 つの option のうち 4 つを足した。2 つは既存の option が同じ働きなので足さない（design §6.1 の表に理由）:

| option | 働き |
| --- | --- |
| `--set PATH=VALUE`（`i:N`・`s:TEXT`・`b:HEX`） | 表の load・`_REG`・`_INI`・events の開始の後、呼びの前に、名前か field unit へ AML の Store と同じ規則で値を書く（interpreter の中で `drv_acpi_store()`）。`SET PATH VALUE` を 1 行出す |
| `--memory-file A=FILE` | 表の load の前に、模した system memory の番地 A に file の byte を置く（`Load(OpRegion)` が動的な表を見つける） |
| `--trace` | `--eval`・`--eval-args`・WMI・Dell の呼びごとに `TRACE begin`、各 address space の書き（space・番地・幅・値。PCI_Config は function も）と I/O port の書き、interpreter の log の行（`Notify … has no handler`、AML の `Debug`）、呼びで load・unload された表、`TRACE end LABEL: N writes, L loads, U unloads, slept M ms` |
| `--diff` | 呼びの前（`--set` の後）と後で namespace を取り、`DIFF +`（増えた node）・`DIFF -`（消えた）・`DIFF ~ 旧 -> 新`（値が変わった）と要約。値を読むのは data object と、system memory・system I/O・PCI_Config の普通の Field の field unit だけ（index・bank field は selector を書き、EC の field は EC と話すので型だけを並べる）。method は読まない |
| `--acpi-mode`（足さない） | 既存の `--events` が ACPI mode に入れてから呼びが走る |
| `--call-every-method`（足さない） | 既存の `--methods` が呼びの後に走る |

- `aml-host-wmi.c` の `parse_argument()` を `host_parse_argument()` として公開し、`--set` と `--eval-args` が同じ語の読みを使う。
- 新しい ASL の試験（`plan/tools/aml-host/asl/`）:
  - `osc.asl`（acpiexec と比べる）: `_OSC` の型 8 つ（受け入れる・bit を落とす・failure・UUID と revision の拒否・Integer を返す・短い Buffer・query で状態を書く・AML の例外（`osc.evals` で `\XEXC._OSC` が両方で error）・Sleep 100 ms × 10）と PCIe の host bridge の型 3 つ（hotplug・PME・AER を手放さない、知らない control の bit で failure、query で control を記録する）。
  - `hostopts.asl`（harness だけ。acpiexec にこの option は無い）: `--set` が Name と system memory の field unit に書く、`--memory-file` の表を `Load` が読む、`--trace` が SystemIO 0x72・0x73・PCI_Config・SystemMemory の書き・Debug・Notify・Sleep 5 ms・load を出す、`--diff` が増えた名前と変わった値を出す（`hostopts.output` の行）。

## 5330 の表での `_OSC` の試し（`plan/ws310/tests/osc-host/run.sh`）

memory は 0 の模型で、値は `--set` で入れた物の他は実機の物ではない（実機の値は p003）。結果（2026-11-15）:

1. platform-wide（DWORD2 0x60）: query・commit とも求めた値を返し、書きは 0。`\_SB.OSCP` 0 → 0x60、`\_SB.OSCM` 0xFF → 0（design §3.1 の 2 の通り）。
2. 同じに `U4SE` 1・`U4CM` 0xA1・`CMSK` 1 を入れると、query は書き 0、commit で `DOCM` → `CHCM`（Debug の "Set Force Power"、TBT の `DFPE` 0 → 1）が走り、模した hardware では切り替えが失敗する（"DOCM: Fail to apply CM mode"）が、`_OSC` の戻りは成功（00 00 00 00 60 00 00 00）。design §3.1 の 3（Force Power・失敗が戻りに出ない）を harness で再現した。この値の組では CMOS（SystemIO 0x72・0x73）への書きは出なかった（`DOCM` が失敗で抜けるため。実機の値で p003 が確かめる）。
3. processor（DWORD2 0）を platform の commit の後に、`\_SB.CFGD` bit 22 と、`\SSDT` の package の番地に `dynamic/` の 8 つの表を置いて: query は書き 0、commit で `HWPV` ← 2（0x61511039）、`Cpu0Psd`・`Cpu0Hwp` の Load、`\PF00` 0x80000000 → 0、`\_SB.PR00._CPC` が 23 要素・revision 3 の Package（FFH の記述子が MSR 0x771・0xCE・0x774・0xE7・0xE8 などを指す）。design §3.2 の通り。
4. PCIe（zedBSD の要求 support 0x11・control 0）: query・commit とも `\OSCC`（GNVS 0x6150C03C）に 0 を 2 回書くだけ。control 0x1D の query は戻り status 0x11・control 0x18 で、query の時点で `\OSCC` ← 0x18（design §3.3）。`NEXP` 1 で hotplug と PME（control 0x05）を query で求めると、全ての root port の config（ECAM の模型の SystemMemory）に書く（`NHPG`・`NPME`）。zedBSD の要求ではこの書きは出ない。

## 試験の結果（2026-11-15）

| command | 前（基点 52b2e7e351） | 後 |
| --- | --- | --- |
| `make -s -C plan/tools/aml-host` | 出力なし（warning 0） | 出力なし（warning 0） |
| `python3 plan/tools/aml-host/run-asl.py` | `asl: 21 passed, 0 failed` | `asl: 23 passed, 0 failed`（`osc` は aml-host と acpiexec、`hostopts` は aml-host だけ） |
| `sh plan/tools/aml-host/check-latitude5330.sh` | 1〜4 PASS の行、`lto: skipped`（build/llvm 無し） | 同じ（`methods: 1349 ran, only the known TSDD failure`） |
| `sh plan/ws333/tests/dell/run.sh`（`aml-host-wmi.c` を変えたため） | 未実施 | `dell: PASS` |
| `sh plan/ws310/tests/osc-host/run.sh` | — | `osc-host: PASS`（7 項目） |

- `osc.asl` の MAIN の確かめ 15 の期待をわざと 0x3D に変えた写しで、aml-host（`MAIN returned 0xf`）と acpiexec（`[Integer] = …0F`）の両方が同じ確かめで FAIL する事を見た。

## Q1 への依頼

- `plan/tools.md` の aml-host の行: option に `--set PATH=VALUE`・`--memory-file A=FILE`・`--trace`・`--diff` を足し、ASL の試験の数を 21 → 23（`osc`・`hostopts`）に。

## 未実施

- `make -C plan/tools/aml-host kernel-check`（`build/llvm` がこの worktree に無い）。harness の変更は `plan/tools/` の host の file だけで、kernel の source は変えていない（codec は p004）。
- 実機の値での評価（p003）。
