<!-- awesome-plan project=zedbsd record=ws333-p007 -->

# ws333-p007: x86 の decode の module の設計（標準廉価実装手順の設計の Phase）

Status: 実装済み・試験 PASS（骨格の build・単体試験の道具・design-reviewer の反映。cleared は Q1 の判定の後）
Parent: [WS333](../ws.md)、設計 [design.md](../design.md) §15
Queue: q1759-i01（P7）

## 目的

design.md §9 の p007: SDM Vol. 4 で ADL（family 6 model 0x9A）の 0xCE・RAPL・C-state の滞在・0xE2 を確かめ、x86 の decode の module の header（仕様の comment）・.c の骨格・単体試験の道具・骨格の照合・p008〜p010 の phase.md を作り、design-reviewer を 1 回通す。手順は [docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md) §4。

## 判断（Q1）

- 2026-10-31 ユーザーから、昼頃まで不在の間は実装・試験・設計を進める、という趣旨の指示を受けた（Q1 経由）。
- 仕様の PDF は Intel の公開の物を取得してよい、置き場所は `build/sources/specs/` の外の自分の worktree の `build/`、SHA-256 を記録、git に入れない（Q1）。
- 5330 は止まっているので 0xE2 の lock の bit の読みは未実施にする（Q1）。
- AMD APM Vol. 3 の取得が bot の確かめの判定で拒まれた件は、回避せず止めた。Q1 は止めた対応を正しいとし、AMD の HwPstate・CPB の bit を「仕様未取得」として範囲から外して進め、ユーザーに PDF を手で置くかを聞く（cockpit）と決めた。ACPI 6.5 も同じ扱い（Q1 へ報告済み）。

## 成果

（design.md §15 が本体。ここは一覧）

- 仕様: Intel SDM rev. 093 の Vol. 1・2・3・4 を取得（`build/specs/dl/`、SHA-256 は design.md §15.1）。AMD APM Vol. 2 r3.45 は WS301 の写しを置いた。AMD APM Vol. 3 と ACPI 6.5 は取れず（再試行しない）。
- Vol. 4 の確かめ（design.md §15.2）: ADL の組（§2.17 の Table 2-20・21・25・29・35・39、§2.17.5 の 2-44〜48）に 0xCE（2-29）・0xE2（2-35、lock は bit 15）・RAPL の 606H・611H・639H・641H・64DH・C-state の滞在の PC2・3・6・7 と CC3・6・7・MC6（2-46、1 MHz）が有る。PC8〜PC10（630H〜632H）は無い。PERF_CTL の比の encoding は SDM に無い（model-specific）。5330 の 0xE2 の lock は未実施。
- header（`src/drivers/platform/pcat/cpu-x86/`）: `cpux86.h`・`cpux86-cpuid.h`・`cpux86-hwp.h`・`cpux86-pstate.h`・`cpux86-power.h`（46 関数。review の M4 で PERF_CTL・AMD の 5 関数を外した）。**基点の SHA（header の確定）は 39f4a5597**。.c の骨格 4 つ（ENOSYS）と、p007 で実装した `cpux86.c`（bit の補助 4 関数、review m10）。`platform/amd64/vmunix.mk`: `cpux86.c`・`cpux86-cpuid.c` は全部の amd64 の kernel、`hwp`・`pstate`・`power` は `CONFIG_CPU_PERF=y` の時だけ（review M1）。
- 試験の道具（`plan/ws333/tests/cpux86/`）: `run-unit.sh`・`check-skeleton.sh`（cc と clang の両方を要し、`BASE` が commit でなければ FAIL）・`unit/check.h`・`unit/regbuild.h`・`unit/feedbackbuild.h`・`unit/test-builders.c`・`unit/test-cpux86.c`。
- 実装の Phase（p007 の merge の後に 3 つ並べて走れる）: [p008](../phase008/phase.md)（cpux86-cpuid.c）、[p009](../phase009/phase.md)（cpux86-hwp.c）、[p010](../phase010/phase.md)（cpux86-pstate.c・cpux86-power.c）。

## 確かめた事（2026-10-31、review の反映の後、39f4a5597 の tree）

- `sh plan/ws333/tests/cpux86/run-unit.sh all` → `test-builders: PASS (21 checks)`、`test-cpux86: PASS (4140 checks)`、`run-unit: PASS`（host の cc、ASan・UBSan）。`CC=clang` でも test-cpux86 PASS。
- `cpux86.c` のわざと壊す確かめ 5 通り（field32 の範囲の検査の 1 ずれ、ceil_log2 の `<` を `<=`、insert64 の mask の shift 抜け、field64 の mask の −2、2^31 の境界）: 4 つは FAIL（1〜3 of 4140）、1 つ（`<=`）は 2^31 で止まらず timeout で捕えた。元の file は `cmp` で不変。
- `sh plan/ws333/tests/cpux86/check-skeleton.sh` → PASS（cc と clang、-std=c89 -Wpedantic）。`BASE=39f4a5597` → PASS、`BASE='<BASE>'` → FAIL（BASE が commit でない）。
- amd64 の kernel: `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=/tmp/p7-ws333b/kn /tmp/p7-ws333b/kn/vmunix`（既定、`cpux86.o`・`cpux86-cpuid.o` が入る）と同じ config に `CONFIG_CPU_PERF=y`（5 つ全部）→ どちらも rc 0、warning 0。
- `git diff --check` → 問題なし。

## design-reviewer

1 回（2026-10-31）、blocker 0・major 5・minor 12。全部を反映した（行き先は design.md §15.9）。要点: CPUID の decoder を既定の kernel にも（M1）、Vol. 4 Table 2-2 と Vol. 3B の食い違いは Vol. 3B（M2）、PACKAGE_THERM_STATUS の ack を定数の log の bit に（M3）、使い道の無い PERF_CTL・AMD の decoder を外した（M4）、topology の list の終わり（M5）、`cpux86.c` を p007 で実装して p008〜p010 を並べられるように（m10）。

## 未実施

- 5330 の 0xE2 の lock の bit の読み（5330 が止まっている）。
- AMD APM Vol. 3・ACPI 6.5 の取得（bot の確かめ、ユーザーの判断待ち）。
- 仕様の PDF の消えない置き場所（Q1-a）。p008〜p010 の phase.md の path は P7 の worktree を指す。Q1 が写したら path を直してから起動する。
- 骨格の module の実装（p008〜p010）。

## 判断が要る点（Q1）

design.md §15.8 の Q1-a〜Q1-e（PDF の置き場所、AMD APM Vol. 3・ACPI 6.5 を手で置くか、PC8〜PC10 の滞在を ADL で読むか、legacy の 0xCE の比の道、5330 の 0xE2 の lock）。
