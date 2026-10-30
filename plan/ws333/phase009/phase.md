<!-- awesome-plan project=zedbsd record=ws333-p009 -->

# ws333-p009: module — HWP の register と HFI の表（cpux86-hwp.c）

Status: cleared（2026-10-31 Q1、9fb61c61e）
Parent: [WS333](../ws.md)、設計 [design.md](../design.md) §15
依存: p007（header の確定と `cpux86.c` の実装、基点の SHA `39f4a5597`）。p008 とは独立（`cpux86-cpuid.h` の `struct cpux86_power` は試験で直に組む）。p007 が main に merge された後に main から始める
担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順 [docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md) §5）。header は p007 の設計で、変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md。
2. plan/coding-style.md の全文（§14 の checklist で自分の diff を見直す）。
3. plan/guardrail.md の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
4. design.md §15（特に §15.4 の 1〜3・5、§15.5）。
5. 実装する header: `src/drivers/platform/pcat/cpu-x86/cpux86-hwp.h`。使う header: `cpux86.h`（約束、`cpux86_field64` などの補助は p007 で実装済み）・`cpux86-cpuid.h`（`struct cpux86_power` の定義だけ。`cpux86_cpuid_power()` は p008 の物で、試験では呼ばずに構造体を直に組む）。
6. 仕様: Intel SDM Vol. 3B rev. 093 `/home/awe/zedBSD-claude1/build/sources/specs/325384-093-sdm-vol-3abcd.pdf`（SHA-256 `77919be320f35da2782771ef2cb65a868cf68e62c8f8279f2ca61199cb70d1f8`、git に入れない）: §17.4.2 Figure 17-4（PDF 535）、§17.4.3 Figure 17-5（PDF 536）、§17.4.4.1 Figure 17-6 と field の説明（PDF 537〜538）、§17.4.4.2（PDF 539）、§17.4.4.4 Table 17-2（PDF 540〜541）、§17.4.5 Figure 17-9（PDF 541〜542）、§17.6.1 Table 17-4〜17-6（PDF 553〜554）、§17.6.2 Table 17-7〜17-9（PDF 555〜558）、§17.6.4〜17.6.7 と Table 17-10（PDF 559〜561）。Intel SDM Vol. 4 rev. 093 `/home/awe/zedBSD-claude1/build/sources/specs/335592-093-sdm-vol-4.pdf`（SHA-256 `79f54b67b9bb3836856f04e0c45f50406c4d4b932c64800eb788b6b705a83dbd`）の Table 2-2 の IA32_PACKAGE_THERM_STATUS（PDF 34）と HWP・HFI の MSR の行。Vol. 1 Table 21-16（leaf 06H、PDF 462〜463、`/home/awe/zedBSD-claude1/build/sources/specs/253665-093-sdm-vol-1.pdf`）。読み方は `pdftotext -layout -f N -l N <pdf> -`、図の bit の位置と表は頁の画でも照らす。

## 作る物

- `src/drivers/platform/pcat/cpu-x86/cpux86-hwp.c`: 骨格（17 関数）を header どおりに実装する。
- 単体試験 `plan/ws333/tests/cpux86/unit/test-hwp.c`（`check.h`・`regbuild.h`・`feedbackbuild.h` を使う。期待の値は仕様の図・表と手の計算。Linux の intel_pstate・intel_hfi などの source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- PM_ENABLE: 0・1、bit 1〜63 のどれかで EINVAL。
- CAPABILITIES: 4 つの field を別の値で（5330 の P 0x01141538 → highest 0x38・guaranteed 0x15・efficient 0x14・lowest 0x01 を手で）、bit 32〜63 のどれかで EINVAL と出力の不変。
- REQUEST の encode: 各 field の幅の上端（255・1023・1・31）と 1 つ上で EINVAL、capabilities ありの順序の検査（minimum が lowest − 1・maximum が highest + 1・minimum > maximum・desired が lowest − 1 と highest + 1 で EINVAL、desired 0 は可）、capabilities NULL では幅だけ、予約の bit 58:43 が 0、各 field の bit の位置（regbuild_put64 で組んだ期待と比べる）。decode は予約の bit で EINVAL、encode → decode の往復（全 field、乱数 10000 組）。
- window: 0 → 0、1 → mantissa 1 exp 0、127 → (127, 0)、128 → (127, 0)、1270 → (127, 1)、1275 → (127, 1)、100 → (100, 0)、1000 → (100, 1)、上限 1270000000 → (127, 7)、上限 + 1 で ERANGE。decode の全 1024 の field の値（mantissa × 10^exp を手の式で）、1024 で EINVAL。encode の結果を decode した値が入力以下で、それより大きい表せる値が入力を超える事を、0〜2000000 の範囲と乱数で確かめる。
- source: Table 17-2 の 4 行、2 で EINVAL。
- STATUS: 定義の 5 bit を 1 つずつ、bit 1 と 6・63 で EINVAL。ack: events の 0 → 0x3D、全部 → 0、1 つずつ（例 GUARANTEED → 0x3C）、定義外の bit で EINVAL。
- feedback_layout: CP 2・CL 1（Thread Director 無し）→ header 16・row 8・頁 1 で row_count (4096 − 16) / 8 = 510、CP 2・CL 4（5330: EAX に THREAD_DIRECTOR、ECX[15:8] 4）→ 16・8、CP 2・CL 5 → header 24・row 16、CP 3（EDX[7:0] = 0x07、bit 2 は数える）・CL 4 → header 8 + 16 = 24・row 16、CP 2・CL 255、頁 16、HW_FEEDBACK の bit 無しと capabilities 0 で ENOTSUP（予約の bit が立っていても ENOTSUP が先）、bit 0 だけ・bit 1 だけ・bit 2 だけで EINVAL、Thread Director で class 0 で EINVAL。期待は feedbackbuild.h の式でなく手で計算した数を書く（式の独立のため）。
- timestamp: feedbackbuild_timestamp で書いた値（上位の byte に意味のある値、little-endian）、table_size が header 未満で EINVAL。
- change と cell: feedbackbuild で表を書き（5330 の形と CL 5 の形、row・class・capability を全部別の値に）全部の cell を読み戻す、最後の行の最後の cell、row = row_count・class = classes・capability = capabilities で EINVAL、table_size を 1 byte 足りなくして EINVAL、NULL で EINVAL、全ての誤りで出力の不変。偽の layout（row_size 0x80000000、capabilities 0xFFFFFFFF、header_size 0xFFFFFFFF、row_count 0xFFFFFFFF と大きな row）で EINVAL か表の中の byte だけ（ASan が何も言わない）。
- pointer: 4 KiB の倍数で valid、bit 11:1 が 0、未整列・2^physical_bits 以上・physical_bits 11 と 53 で EINVAL。
- config: Table 17-10 の有効な組（0/0、1/0、1/1）、thread_director だけで EINVAL（supported 0 でも EINVAL が先）、支えの無い thread_director（enable 1）で ENOTSUP。
- updated と ack: bit 26 の 0・1。ack は raw に依らず bit 3・5・7・9・11 が 1、bit 26 と予約・status の bit が 0、bit 1 だけが raw の bit 1（raw 0・全部 1・bit 1 だけ・bit 26 だけ で値を手で出す）。
- 乱数の入力: 固定の種で 64 bit の値と表の byte 列を作り、全関数に通して ASan・UBSan が何も言わず、戻りが header に書いた値だけであること。

## 注意

- 定数に `ULL`・`long long` を使わない（run-unit は `-std=c89 -Wpedantic -Werror`）。64 bit の値は `((uint64_t)0x08040439U << 32) | 0xF0811900U` のように 32 bit の半分から組むか、LP64 の `UL` で。浮動小数点を使わない（kernel は浮動小数点の register 無しで build する）。
- **SDM Vol. 4 Table 2-2 は HWP_STATUS（63:3 予約）・HWP_REQUEST（63:43 予約）・HW_FEEDBACK_CONFIG（63:1 予約）・PACKAGE_THERM_STATUS の bit 1（R/W）を Vol. 3B と違って書く。Vol. 3B の §17.4・§17.6・§17.8 の図が正**（design.md §15.4 の 8）。header は Vol. 3B に従っている。Vol. 4 の表は MSR の番地の出典にだけ使う。
- Table 17-9 は class ごとの padding のようにも読めるが、header は Table 17-7 の注 1〜3 の式（行の終わりに 1 つの padding）に従う（design.md §15.4 の 1）。header に従う。
- IA32_HWP_STATUS の Highest_Change は bit 3（Figure 17-9）。
- REQUEST の desired の範囲は SDM の文（lowest〜highest）で、minimum〜maximum ではない（header の通り）。
- 表の offset の計算は `size_t` で、table_size との比べで足し算が溢れない形にする（巨大な row・class の値でも範囲の外を読まない）。

## 変えてはいけない物

- `src/drivers/platform/pcat/cpu-x86/cpux86*.h`（全部）。header の不備は直さずに止めて Q1 に返す。
- 他の module の .c（`cpux86.c`・`cpux86-cpuid.c`・`cpux86-pstate.c`・`cpux86-power.c`）、`platform/amd64/vmunix.mk`、試験の道具（`run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`regbuild.h`・`feedbackbuild.h`・`test-builders.c`）。不備は Q1 へ。
- HAL・toolchain・共有の build/。rm などの削除をしない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws333/tests/cpux86/run-unit.sh hwp` → `run-unit: PASS`。`run-unit.sh all` も PASS。
- `BASE=39f4a5597 timeout 300 sh plan/ws333/tests/cpux86/check-skeleton.sh` → `check-skeleton: PASS`。
- `timeout 1800 make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk CONFIG_CPU_PERF=y BUILD=build/p009 build/p009/vmunix` → exit 0、warning 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読む）。
- わざと壊す確かめ（例: Highest_Change を bit 4 に、desired の範囲を min〜max に、row の padding を class ごとに、ack で raw を書き戻す、layout の offset の溢れの検査を外す）を git の外の写しで行い、FAIL を確かめて戻す（`cmp`）。見逃しは試験を足す。
- coding-style §14 の checklist、block ごとの `Values referenced: Intel SDM Vol. 3B rev. 093 …` の複数行の comment、開発の管理情報が無い事。
- `git add` の後 `git commit -m WIP -- <path>`（push しない）。SHA・command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果

2026-10-31 実装の担当 S-ws333-p009（q1766-i01）。基点 `39f4a5597` から header は無変更。

作った物: `src/drivers/platform/pcat/cpu-x86/cpux86-hwp.c`（17 関数）、`plan/ws333/tests/cpux86/unit/test-hwp.c`。

確かめ（worktree の root、実行して出力を読んだ）:

| command | 結果 |
| --- | --- |
| `timeout 600 sh plan/ws333/tests/cpux86/run-unit.sh hwp` | `test-hwp: PASS (12804888 checks)`、`run-unit: PASS` |
| `timeout 900 sh plan/ws333/tests/cpux86/run-unit.sh all` | test-builders 21・test-cpux86 4140・test-hwp 12804888 check、`run-unit: PASS` |
| `BASE=39f4a5597 timeout 300 sh plan/ws333/tests/cpux86/check-skeleton.sh` | `check-skeleton: PASS` |
| `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk CONFIG_CPU_PERF=y BUILD=/tmp/s-ws333-p009/b /tmp/s-ws333-p009/b/vmunix`（出力を / の空きのため /tmp に） | exit 0、log の warning 0 件、`cpux86-hwp.o` が build された |
| `git diff --check` | 出力なし |

わざと壊す確かめ（git の外の写し、`/tmp/s-ws333-p009/mut/`。.c の写しを変えて同じ flag で test-hwp を build して流した）: 24 通りで全部が FAIL（捕えた）、見逃し 0。
desired の範囲を min〜max に、window の同点を大きい exponent に、header の padding を外す、ack が raw を書き戻す、offset の溢れの検査を外す、valid を bit 58 に、status ack の反転、window の exponent の数、pointer の上限の検査を外す、config の誤りの順、capabilities の予約の範囲、Thread Director の無い時に class 数を使う、cell の class の項を外す、status が bit 1 を許す、timestamp の byte の順、source の反転、HW_FEEDBACK 無しの ENOTSUP を外す、minimum < lowest の検査を外す、window decode の exponent の幅、row の padding を class ごとに、ack が bit 1 を落とす、page 数の溢れの検査を外す、Highest_Change の bit、PM_ENABLE の予約の範囲。元の `cpux86-hwp.c` は /tmp の写しにだけ変えたので無変更。

自分で決めた点（header が決めていない所）:

1. `cpux86_feedback_layout()` は `feedback_capabilities` の bit 7:0 だけを見る（EDX[7:0] の定義）。bit 8 以上だけが立つ値は 0 と同じ（ENOTSUP）、bit 8 以上は数えない。
2. `feedback_pages` は偽の値でも溢れないよう 64 bit で `pages * 4096` を作り、32 bit に入らない時（pages が 1048576 以上）は EINVAL。0 頁は「header と 1 行が入らない」で EINVAL。16 を超える値（processor は返さない）は表が 32 bit に入る限り受ける。
3. `feedback_change`・`feedback_cell` は offset を 64 bit で作り、各項を足す前に「table_size に入る」ことを検べる（溢れも範囲の外も EINVAL）。layout の `header_size` が小さく偽でも表の中の byte しか読まない。
4. `cpux86_hwp_window_encode()` は exponent 0..7 を小さい方から試し、各 exponent の最大の mantissa（127 まで）の window が真に大きい時だけ採る（同点は小さい exponent が残る）。
5. 試験は `check_table()` が表を `feedbackbuild.h` で書き全 cell・change byte を読み戻し、5330 の形（CP 2・CL 4）・CL 5・CP 3・CL 255 と頁 16・CP 8 と CL 255 を通す。偽の layout は malloc した 4096 byte の表で ASan に見せる。

未実施: 実機・QEMU の確認（この Phase の範囲外、純粋な関数の host 試験のみ）。clang での build は check-skeleton が通した（警告なし）。
cleared は Q1 の判定。
