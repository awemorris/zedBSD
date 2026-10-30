<!-- awesome-plan project=zedbsd record=ws333-p010 -->

# ws333-p010: module — 0xCE と電力の register（cpux86-pstate.c・cpux86-power.c）

Status: in-progress（実装済み・試験 PASS、cleared は Q1）
Parent: [WS333](../ws.md)、設計 [design.md](../design.md) §15
依存: p007（header の確定と `cpux86.c` の実装、基点の SHA `39f4a5597`）。p008 とは独立（`struct cpux86_identity` は試験で直に組む）。p007 が main に merge された後に main から始める
担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順 [docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md) §5）。header は p007 の設計で、変えない。

design.md §9 の p010 の元の範囲のうち、ACPI の `_PSS` の package の解析と AMD の HwPstate の CPUID の bit は、仕様が取れないので外した（design.md §15.1・§15.8 の Q1-b）。0xCE の比を PERF_CTL に書く encoding は SDM に無く（§15.8 の Q1-d）、PERF_CTL・PERF_STATUS と AMD の C001_0061〜0063 の decoder は使い道が無いので、design-reviewer の M4 で header から外した（§15.3）。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md。
2. plan/coding-style.md の全文（§14 の checklist で自分の diff を見直す）。
3. plan/guardrail.md の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
4. design.md §15（特に §15.2 の表、§15.4 の 4〜6、§15.5）。
5. 実装する header: `src/drivers/platform/pcat/cpu-x86/cpux86-pstate.h`・`cpux86-power.h`。使う header: `cpux86.h`（補助は p007 で実装済み）・`cpux86-cpuid.h`（`struct cpux86_identity` の定義だけ。試験では `cpux86_cpuid_identity()` を呼ばずに構造体を直に組む）。
6. 仕様（git に入れない）:
   - Intel SDM Vol. 4 rev. 093 `/home/awe/zedBSD-claude1/build/sources/specs/335592-093-sdm-vol-4.pdf`（SHA-256 `79f54b67b9bb3836856f04e0c45f50406c4d4b932c64800eb788b6b705a83dbd`）。PDF の頁 = 印刷の頁 2-N の N + 10。§2.17 の頭の model の一覧と「These processors support … Table 2-20, …, 2-39」（PDF 369〜370）、§2.17.5（PDF 410）、Table 2-20 の 0xE2（PDF 237）・3F8H〜3FEH（PDF 248〜249）・606H（252）・60DH（253）・611H（254）、Table 2-21 の 641H（259）、Table 2-29 の CEH（298）、Table 2-35 の E2H（346〜347）、Table 2-39 の 639H・64DH（376〜377）、Table 2-46 の 664H（413）。
   - Intel SDM Vol. 3B rev. 093 `/home/awe/zedBSD-claude1/build/sources/specs/325384-093-sdm-vol-3abcd.pdf`（SHA-256 `77919be320f35da2782771ef2cb65a868cf68e62c8f8279f2ca61199cb70d1f8`）: §17.10.1〜17.10.4（PDF 576〜581、Figure 17-35・17-37、Table 17-12）。
   読み方は `pdftotext -layout -f N -l N <pdf> -`、表は頁の画でも照らす。

## 作る物

- `src/drivers/platform/pcat/cpu-x86/cpux86-pstate.c`（2 関数）と `cpux86-power.c`（10 関数）: 骨格を header どおりに実装する。allow list の表と滞在の counter の表は、block ごとに `Values referenced:` で表の番号と頁を書いた data にする。
- 単体試験 `plan/ws333/tests/cpux86/unit/test-pstate.c` と `unit/test-power.c`（`check.h`・`regbuild.h`。期待の値は仕様の表と手の計算。Linux の turbostat・intel_rapl・intel_idle などの source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- platform_info: 5330 の値 0x0804043DF0811900 から手で: max_non_turbo 0x19（25）・max_efficiency 4・min_operating 4・config_tdp_levels 2（bit 34:33 = 10b）・flag は 3 つとも立つ（bit 28・29・32 が 1。予約の bit 30・31 も 1 だが EINVAL にしない）。各 field を 1 つずつ、config_tdp_levels 3 で EINVAL、予約の bit を立てても EINVAL にならない。`ratio_mhz` の 0・25・255・256（EINVAL）。
- model_registers: Intel の family 6 の 0x97・0x9A・0xB7・0xBA・0xBF で全部の bit（15 bit、DRAM が無い事）、0x99・0x9B・family 0x0F・vendor UNKNOWN で ENOTSUP と 0、NULL で EINVAL。
- rapl_units: 5330 で読む値は手元に無いので、手で組んだ値（例 power 3・energy 14・time 10）と各 field の両端、予約の bit（4〜7、13〜15、20、63）で EINVAL。
- energy_status: [31:0]、bit 32 で EINVAL。
- energy_counter: 最初の読みで total 0、増える読み、一周（last 0xFFFFFFF0 → reading 0x10 で +0x20）、同じ値で +0、total が 2^64 − 1 を超える時の ERANGE と不変、started 2 で EINVAL。
- energy_microjoules: energy_shift 0（units × 10^6）・14（1 unit = 61.03515625 µJ → 1 → 61、16384 → 1000000）・16・31、units の最大付近の ERANGE と境界の 1 つ下の成功（手で出す）、shift 32 で EINVAL。
- residency_info: 8 つの counter の MSR・model_bit・scope・state・rate を表から手で（MC6 は 1 MHz・module）、8 で EINVAL。
- residency_delta: 増え・同じ・減り（ERANGE）。residency_nanoseconds: rate 1000000（1 count = 1000 ns）、2500000000（TSC 2.5 GHz: 2500000000 count = 1 s）、rate 0・10^10 + 1 で EINVAL、10^10 の成功、結果が 64 bit を超える counts の ERANGE と 1 つ下の成功。
- pkg_cstate: limit の 0〜8 と flag の 8 つ、limit 9・15 で EINVAL、予約の bit は見ない。limit の encode: 読んだ値の他の bit を保つ、limit 9 で EINVAL、bit 15 が立つ値で EACCES と出力の不変。
- 乱数の入力: 固定の種で 64 bit の値を作り全関数に通し、ASan・UBSan が何も言わず、戻りが header に書いた値だけであること。

## 注意

- 定数に `ULL`・`long long` を使わない（run-unit は `-std=c89 -Wpedantic -Werror`）。64 bit の値は `((uint64_t)0x08040439U << 32) | 0xF0811900U` のように 32 bit の半分から組むか、LP64 の `UL` で。浮動小数点を使わない（kernel は浮動小数点の register 無しで build する）。
- 0xCE・0xE2 は model-specific で、予約の bit を検めない（header の通り、design.md §15.4 の 5）。
- RAPL の µJ の変換は (units >> shift) × 10^6 と (units の下位 shift bit) × 10^6 >> shift に分けると 64 bit の中で溢れない（下位 × 10^6 < 2^31 × 10^6 < 2^51）。各段で溢れを検めてから掛ける。
- 滞在の ns の変換も商と余りに分け、余り × 10^9 は rate ≤ 10^10 なので 64 bit に収まる。

## 変えてはいけない物

- `src/drivers/platform/pcat/cpu-x86/cpux86*.h`（全部）。header の不備は直さずに止めて Q1 に返す。
- 他の module の .c、`platform/amd64/vmunix.mk`、試験の道具（`run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`regbuild.h`・`feedbackbuild.h`・`test-builders.c`）。不備は Q1 へ。
- HAL・toolchain・共有の build/。rm などの削除をしない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws333/tests/cpux86/run-unit.sh pstate power` → `run-unit: PASS`。`run-unit.sh all` も PASS。
- `BASE=39f4a5597 timeout 300 sh plan/ws333/tests/cpux86/check-skeleton.sh` → `check-skeleton: PASS`。
- `timeout 1800 make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk CONFIG_CPU_PERF=y BUILD=build/p010 build/p010/vmunix` → exit 0、warning 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読む）。
- わざと壊す確かめ（例: allow list に 0x99 を足す、MC6 の rate を TSC に、energy の一周の補いを外す、µJ の下位の項を落とす、lock の検査を外す）を git の外の写しで行い、FAIL を確かめて戻す（`cmp`）。見逃しは試験を足す。
- coding-style §14 の checklist、表の block ごとの `Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-…` の複数行の comment、開発の管理情報が無い事。
- `git add` の後 `git commit -m WIP -- <path>`（push しない）。SHA・command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果

2026-10-31 S-333p010（Sonnet 5.5 high、q1767-i01）。worktree `/home/awe/zedBSD-worktrees/s-ws333-p010`。

作った物: `cpux86-pstate.c`（2 関数）、`cpux86-power.c`（10 関数。allow list の表と滞在の counter の表は block ごとに `Values referenced:` で表の番号を書いた const の data）、単体試験 `plan/ws333/tests/cpux86/unit/test-pstate.c`・`test-power.c`。header・試験の道具・他の module は変えていない。

確かめた事（実行したコマンドと結果）:
- `sh plan/ws333/tests/cpux86/run-unit.sh pstate power`: `test-pstate: PASS (120186 checks)`・`test-power: PASS (275202 checks)`・`run-unit: PASS`。`run-unit.sh all`: test-builders 21・test-cpux86 4140・test-power・test-pstate すべて PASS、最後の行 `run-unit: PASS`（ASan・UBSan、`-std=c89 -Wpedantic -Werror`）。
- `BASE=39f4a5597 sh plan/ws333/tests/cpux86/check-skeleton.sh`: `check-skeleton: PASS`（cc・clang、header は基点から無変更）。
- `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk CONFIG_CPU_PERF=y BUILD=/tmp/s-ws333-p010/b /tmp/s-ws333-p010/b/vmunix`（出力は host の空きの都合で /tmp）: exit 0、log に warning 0 行。`cpux86-pstate.o`・`cpux86-power.o` が生成された。
- `git diff --check`: 出力なし。
- わざと壊す確かめ（git の外の写し `/tmp/s-ws333-p010/mut/`、元の file は無変更）18 通りを全部試験が捕えた（FAIL）: allow list に 0x99 を足す、MC6 の rate を TSC に、energy の一周の補いを外す（飽和の引き算に）、µJ の下位の項を落とす、lock の検査を外す、ConfigTDP 3 の検査を外す、ratio 256 を通す、滞在の減りの ERANGE を外す、limit の encode で他の bit を保たない、ns の余りの項を落とす、µJ の和の溢れの検査を外す、energy の total の溢れの検査を外す、RAPL の予約の bit 63:20 の検査を外す、started の検査を外す、limit の予約の code の境界をずらす、0xCE の field の位置をずらす、µJ の shift を 15 bit に切る、CORE C3 の scope を PACKAGE に。見逃しは無し。

自分で決めた点:
- allow list の 5 つの model（0x97・0x9A・0xB7・0xBA・0xBF）の register の集合は全部同じ 15 bit（0x7FFF）。header が 5 model を同じ表（§2.17・§2.17.5）の下に置いているため、model ごとの差は付けていない。
- `cpux86_model_registers()` の ENOTSUP の時は `*registers = 0`（header の「0 on ENOTSUP」）。NULL の時は EINVAL で出力を触らない。
- 誤りの順: `cpux86_pkg_cstate_limit_encode()` は引数の検査（NULL・limit）が先で、その後に lock の EACCES。両方に当たる入力は EINVAL。
- µJ の溢れの境界は shift 0 と 14 で手で出した（limit = 18446744073709 の整数部、余り 9037 で成功・9038 で ERANGE）。shift 20 以上は 64 bit の最大値でも溢れない事を手で確かめた。shift 31 では 64 bit の units の整数部が 2^33 までなので、ERANGE は起きない（試験でその事を確かめた）。
- 乱数の試験は xorshift の固定の種。pstate は手で書いた shift と照合、power は header が許す戻りの値だけである事を確かめた。

未実施: 実機・QEMU の確認（この Phase は host の単体試験まで。0xCE・0xE2 の実機の値は p011 以降）。`CONFIG_CPU_PERF` 無しの build（Phase の command に無い）。
残り: Q1 の判定と merge。
