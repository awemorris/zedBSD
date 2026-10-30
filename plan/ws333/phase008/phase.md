<!-- awesome-plan project=zedbsd record=ws333-p008 -->

# ws333-p008: module — CPUID の解析（cpux86-cpuid.c）

Status: planned
Parent: [WS333](../ws.md)、設計 [design.md](../design.md) §15
依存: p007（header の確定と `cpux86.c` の実装、基点の SHA `39f4a5597`）
担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順 [docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md) §5）。header は p007 の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md。
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、Boolean を式で作らない、最後の文は成功の return、など。§14 の checklist で自分の diff を見直す）。
3. plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの `Values referenced:` の複数行の comment、file の先頭に出典を書かない）と「source の中の開発の管理情報」。
4. design.md §15（特に §15.4 の決めた事 7、§15.5 の試験の道具）。
5. 実装する header: `src/drivers/platform/pcat/cpu-x86/cpux86.h`・`cpux86-cpuid.h`（各関数の comment が仕様。誤りの値・出力を変えない約束・信用できない入力の約束は cpux86.h の先頭）。
6. 仕様: Intel SDM Vol. 1 rev. 093 `/home/awe/zedBSD-claude1/build/sources/specs/253665-093-sdm-vol-1.pdf`（SHA-256 `f6ffc430dfa2dada4f791b1ecb462810e59827befe9a9c23f9160941eea642f9`、git に入れない）の第 21 章: Table 21-4（PDF 445）、Table 21-6・21-7 と DisplayFamily/DisplayModel の規則（PDF 446〜447）、Table 21-8・21-9（PDF 447〜448）、Table 21-14 と cache の大きさの式（PDF 459〜460）、Table 21-15（PDF 461）、Table 21-16（PDF 462〜463）、Table 21-21（PDF 467〜468、EDX[15]）、Table 21-31・21-32（PDF 477〜478）、Table 21-63（PDF 504）、Table 21-70・21-71 と後の文（PDF 509〜510）、Table 21-89（PDF 525）・21-97（PDF 532）・21-98（PDF 533）。Vol. 3A §11.9.4 の 3（`/home/awe/zedBSD-claude1/build/sources/specs/325384-093-sdm-vol-3abcd.pdf`、SHA-256 `77919be320f35da2782771ef2cb65a868cf68e62c8f8279f2ca61199cb70d1f8`、PDF 356）。読み方は `pdftotext -layout -f N -l N <pdf> -`、表は頁の画（Read の tool の `pages`）でも照らす。PDF が無ければ止めて Q1 に聞く（Q1 が置き場所を移していることがある）。

## 作る物

- `src/drivers/platform/pcat/cpu-x86/cpux86-cpuid.c`: 骨格（13 関数）を header どおりに実装する。static の補助関数は自由（規約どおり前方宣言）。bit の field の取り出しは p007 で実装済みの `cpux86.c`（`cpux86_field32` など、`unit/test-cpux86.c` で確かめ済み）を使ってよい。
- 単体試験 `plan/ws333/tests/cpux86/unit/test-cpuid.c`（`check.h`・`regbuild.h` を使う。期待の値は仕様の表か手の計算。他の実装（Linux・FreeBSD・cpuid の道具など）の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- identity: regbuild の Intel の leaf 00H と 01H EAX から family 6・model 9AH（拡張 model 9、model AH）・stepping、family 0FH の拡張 family の加算（0FH + 拡張 1 → 10H）と拡張 model、family 5 では拡張の field を無視、vendor が Intel でない文字列（1 文字違い）は UNKNOWN、leaf 00H の EAX 0 で ENOTSUP と出力が変わらない、NULL で EINVAL。`extended_range` の 80000000H 未満で 0。`has_leaf` の境界（max ちょうど・max + 1・拡張の範囲・max_extended 0）。
- features: ECX の bit 3・7・21・24・31 を 1 つずつ立てて対応の flag だけ、他の bit を全部立てても flag に出ない、EBX[23:16]・[31:24]。
- mwait: EDX の 8 つの 4 bit を別々の値で、ECX[0] が 0 の時は全部 0、flag の 3 つ。
- power: EAX の定義済みの bit を 1 つずつ（`CPUX86_POWER_DEFINED` と一致）、予約の bit（3・12・21・24〜31）は features に出ない、EBX[3:0]、ECX[0]・[3]・[15:8]、EDX の [7:0]・[11:8] + 1（0 → 1、15 → 16）・[31:16]。5330 の値（WS301 §2.3 の CPUID.06H: EAX 0x00DF8FF7、ECX 0x409、EDX の行 0・1・2・3）を組んで手で期待を出した場合も 1 つ入れてよい（期待は表から手で出す）。
- hybrid（07H EDX[15]）、core_kind（0x40000001・0x20000001、EAX 0 で ENOTSUP、0x10 の型は通す）。
- cache: 型 1・2・3、型 0 で ENOTSUP、型 4・31 で EINVAL、level、sharing_ids（EAX[25:14] 0 → 1、0xFFF → 4096）と share_shift（1 → 0、2 → 1、3 → 2、4096 → 12）、大きさの式（手で出した例 2 つ以上）、全部 0xFFF…（ways 1024 × partitions 1024 × line 4096 × sets 2^32 = 2^64）で ERANGE、flag の 5 つ。`cache_group` の shift 0 と 12、31 を超える share_shift で EINVAL。
- topology（1FH と 0BH）: 5330 の形（sub-leaf 0 THREAD shift 1、1 CORE shift 6、2 INVALID、count 3; x2APIC 0x09 → thread 1・core 4・package 0、0x1E → thread 0・core 15）を手で出す。module・die を持つ 4 段（THREAD 1・CORE 4・MODULE 6・DIE 8、package は >> 8）、tile のような未知の型（予約の 7 や 0BH での 3）が core の field を延ばす事、INVALID の terminator が count の中に無い（EINVAL、count が最大 8 で全部有効も EINVAL）、有効な sub-leaf が THREAD だけ（sub-leaf 1 が INVALID、または count 1 で INVALID 無し → EINVAL）、ECX[7:0] の不一致、shift の減少、EDX の不一致、型の重複・逆順、sub-leaf 0 の INVALID・EBX[15:0] 0 で ENOTSUP、leaf の値が 0BH・1FH 以外、count 0・9 で EINVAL、shift 31 と package の 0、すべての誤りで出力が変わらない。
- extended: 両方 NULL で 0、80000007H EDX[8]、80000008H EAX[7:0]。
- 乱数の入力: 固定の種の擬似乱数で register を 100000 組作り、全関数に通して ASan・UBSan が何も言わず、戻りが header に書いた値（0・EINVAL・ENOTSUP・ERANGE）だけであること。

## 注意

- 定数に `ULL`・`long long` を使わない（run-unit は `-std=c89 -Wpedantic -Werror`）。64 bit の値は `((uint64_t)0x08040439U << 32) | 0xF0811900U` のように 32 bit の半分から組むか、LP64 の `UL` で。浮動小数点を使わない（kernel は浮動小数点の register 無しで build する）。

- leaf 01H ECX[31] は SDM では「Not Used、Intel は 0 を返す、software の emulation 用」（Table 21-9）。header の名前は HYPERVISOR だが、仕様の文はこの通りで、意味の解釈（hypervisor が立てる）は driver の方針。
- 1FH・0BH の「知らない型は下の既知の domain の延長」は Vol. 3A §11.9.4 の 3b の文。module の field は CORE の shift から MODULE の shift まで（間に知らない型があればそれも CORE に含めた後）。
- DisplayFamily の規則は family 0FH の時だけ拡張 family を足し、DisplayModel の規則は family 06H か 0FH の時だけ拡張 model を前に置く（他の family では拡張の field を読まない）。
- 試験の期待は手で出す。header の例と同じ値を写すだけにせず、仕様の bit の位置から組む（regbuild.h）。

## 変えてはいけない物

- `src/drivers/platform/pcat/cpu-x86/cpux86*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す（どの header のどの関数か、何が足りないか、案の差分）。
- `cpux86.c`（p007 で実装済み）と `unit/test-cpux86.c`、他の Phase の `cpux86-hwp.c`・`cpux86-pstate.c`・`cpux86-power.c`、`platform/amd64/vmunix.mk`、`plan/ws333/tests/cpux86/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/regbuild.h`・`unit/feedbackbuild.h`・`unit/test-builders.c`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws333/tests/cpux86/run-unit.sh cpuid` → 最後の行 `run-unit: PASS`。`sh plan/ws333/tests/cpux86/run-unit.sh all` も PASS（builders・cpux86 を含む）。
- `BASE=39f4a5597 timeout 300 sh plan/ws333/tests/cpux86/check-skeleton.sh` → `check-skeleton: PASS`。
- amd64 の kernel の 2 つの build（`cpux86-cpuid.c` は既定の kernel にも入る）: `timeout 1800 make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p008 build/p008/vmunix` と `timeout 1800 make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk CONFIG_CPU_PERF=y BUILD=build/p008y build/p008y/vmunix` → どちらも exit 0、`grep -c 'warning:'` が 0。toolchain・sysroot は main の build への symlink（無ければ Q1 に聞く。LLVM を build しない）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通り（例: DisplayModel の拡張を family 6 で足さない、topology の field の上端を 1 つずらす、INVALID の terminator の検査を外す、cache の +1 を落とす、ERANGE の検査を外す）壊し（git の外の写しで）、試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。値の block ごとに `Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-…` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、わざと壊した数と結果、header が黙っていて自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

（実装の担当が書く）
