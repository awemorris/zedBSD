<!-- awesome-plan project=zedbsd record=ws031p024 -->

# ws031-p024: 確認と修正: compiler の境界

Phase ID: `ws031-p024`
Parent: [WS031](../ws.md)
Status: blocked（UAT 待ち）（2026-10-09 P1: 増分 1〜3 の host の分と T1-190b の実機の vke2（EDGE・KILLOOP・NOINPUT・UNDEF）は済み。残りは spill の組み合わせの vke2 の step で、実機の i915 が要る。下の「2026-10-09」）
Phase disposition: normal
Queue: q751（P1、2026-10-05。Q1 の承認「Plan approved under q751. Start with p024」）
承認の元: 2026-10-05 ユーザー「GPU computeは言語側が完成しておらず、進められないんです。i915のSPIR-V lowringだけ進められますか？」（Q1 の中継）

## 範囲（ws.md の表と [phase015](../phase015/phase.md) の compiler の項目）

0 除算・`INT_MIN / -1`、mod・FRem（商が整数に近い）、ループの中の discard・texture の sample、32 以上の shift、入れ子の上限を超える
ループ（拒否）、入力を読まない fragment shader（SBE の属性 0）、spill の組み合わせ（SPILL と VIO16 を同じ command buffer で交互に、
discard と spill の同居、1 thread 4 KiB を超える spill、spill する VS と PS を同時に、sample の reply の一部が spill される形、scratch の作成の失敗）。
見つけた不具合の修正は範囲に入る。

依存: WS035 の refactor は済み（Q1 2026-10-05: WS035 は 2026-09-30 にユーザーが閉じた。WS075 がその後に compiler を作り直した）。

## 未定義の動作の扱い（Q1 の委任「picking the Vulkan-conformant rule (any value, no hang) is within your technical discretion」）

SPIR-V は次の結果を未定義とする: OpSDiv・OpUDiv・OpSRem・OpSMod・OpUMod の除数 0、OpSDiv の `INT_MIN / -1`（と同じ形の
OpSRem・OpSMod）、shift の量が 32 以上（OpShiftLeftLogical・OpShiftRightLogical・OpShiftRightArithmetic）。Vulkan はこれらで
任意の値を許し、停止や異常は許さない。zedBSD の規則:

- compiler はこれらを検出も補正もしない（Mesa の brw の Gen12.0 と同じ。除算は math の INT_QUOTIENT・INT_REMAINDER、shift は
  EU の SHL・SHR・ASR がそのまま）。値は hardware の出す値。
- 守ること: GPU が止まらない（その draw と後の draw が終わる）、同じ draw の定義された pixel が正しい、kernel の C の code が
  shader の値で 0 除算・範囲外の shift をしない（compiler は C で shader の値を畳み込まない。下の確認）。
- hardware の値は vke2 の UNDEF の step が log に記録する（T1 の passthrough の run の後に下に書く）。

## 設計

### 増分 1（host）: kernel と host の道具の安全

1. kernel: `i915_spirv_lower_array_length()`（spirv-compute.inc）の stride の 2 の冪の探索は、ArrayStride が 2^31 を超えると
   `1U << 32` に達して終わらない（kernel の中で止まる）。上限を付ける。他に shader の値で回る loop・shift・除算が kernel の C に
   無いことを grep で確かめる（compiler の C の `/`・`%`・`<<` は全て定数か上限付きの値）。
2. host の道具: `i915-vk-lower-test.c` の interpreter は IDIV・IREM の未定義を assert で落とし、`i915-vk-compile-test.c` の EU の
   model は INT_QUOTIENT・REMAINDER を守らずに割る（host で SIGFPE）。未定義の入力では「定義されない値」として 0 を出し、
   比較は UNDEF の step の規則（定義された行だけ）に従う。
3. 入れ子の上限: spirv.c の MAX_LOOP_DEPTH 8・MAX_CONSTRUCT_DEPTH 32、compile.c の COMPILE_MAX_LOOPS 16 を超える shader が
   拒否されること（壊れないこと）を host の lower の試験に足す。

### 増分 2（vke2 の step）

| step | shader | 判定 |
| --- | --- | --- |
| EDGE | `edge.frag`: 定義された整数の境界（`INT_MIN` を割る・割られる、`INT_MAX`、-1、符号の組の剰余、shift 0・31、ASR の負、UDIV の 0xFFFFFFFF） | 全 pixel の bit 一致 |
| UNDEF | `undef.frag`: 偶数の行は定義された対照、奇数の行は除数 0・`INT_MIN / -1`・shift 32〜63 | 偶数の行の bit 一致、draw が終わる。奇数の行の値は操作ごとに最初の値を log |
| FREM | `frem.frag`: 商が整数に近い mod（x - y floor(x/y)）と FRem | 期待値か、floor が 1 ずれた値（Vulkan の精度は x - y floor(x/y) から継ぐ） |
| KILLOOP | `killoop.frag`: per-pixel の回数の loop の中の discard（途中で全 channel が消える thread を含む） | 残った pixel の bit 一致、消えた pixel は clear の色 |
| NOINPUT | `noinput.vert`（varying を書かない）・`noinput.frag`（gl_FragCoord だけ） | 全 pixel の bit 一致（SBE の属性 0） |

ループの中の texture の sample は vke2 に sampler が無いので、vke1 か host の compile の試験で扱う（増分 3 で決める）。

### 増分 3: spill の組み合わせ

SPILLMIX（spill.frag と vio16.vert の draw を同じ command buffer で交互に、scissor で半分ずつ）、spill.frag の discard 入りの変種、
4 KiB を超える spill、vio16.vert と spill.frag の同じ pipeline、scratch の作成の失敗（host の stub で確保を失敗させ、draw が error で
終わり漏れが無い）、sample の reply の一部の spill（host の compile の試験）。

## 確認

### 2026-10-05 q751-i01（P1 generation18）: 増分 1 の途中で区切った

済み:
- kernel の修正: `i915_spirv_lower_array_length()` の stride の探索に上限 31（ArrayStride が 2^31 を超える SPIR-V で kernel の中の
  loop が終わらなかった。`1U << 32` は x86 で 1 になる）。
- grep の確認: compiler の C の `/`・`%` は定数か 0 を確かめた値だけ。変数の shift は上の 1 か所の他は上限付き（rank < 32、location < 8）。
  word を歩く loop は count 0 を EINVAL で断る。slot の store の倍化は MAX_VARIABLE_SLOTS と確保の失敗で止まる。
- host の道具: `i915-vk-lower-test.c` の IDIV・IREM・UDIV・UMOD と `i915-vk-compile-test.c` の INT_QUOTIENT・REMAINDER は、除数 0 と
  `INT_MIN / -1` で assert や SIGFPE をせず 0 を出す（未定義の値の代わり）。

| command | 結果 |
| --- | --- |
| `make BUILD=build/p013-k -j16 vmunix` | exit 0、warning 0 |
| `sh plan/ws031/tests/run-vk-host-tests.sh` | PASS |
| `sh plan/ws101/tests/host/run.sh` | PASS |

増分 1 の 3（2026-10-05、合間の仕事）: `plan/ws031/tests/p024/run.sh` と GLSL 8 本（ASan・UBSan の下の i915-shader-check）: loops8 受ける・
loops9 断る（loops nested too deep）・ifs31 受ける・ifs40 断る（constructs nested too deep）、noinput・divzero（実行時の 0 と INT_MIN/-1）・
shift（32〜63）・killoop を -O0 と -O で受ける。PASS。

増分 2（2026-10-05、合間の仕事）: vke2 に EDGE（境界の値の整数の演算、未定義の除算は marker）、KILLOOP（loop の中の discard）、
NOINPUT（入力の無い fragment shader、varying の無い vertex shader）、UNDEF（最後: 0 除算・INT_MIN/-1・32〜63 の shift、偶数の行を判定し
奇数の行の値を log）を足した（`generality-shaders/edge.frag`・`undef.frag`・`killoop.frag`・`noinput.vert`・`noinput.frag`、`regenerate.py`、
`generality.c` の比較の mode GUARD）。**既定の試験の kernel（I915_TEST_SET=all）は今の main で既に AMD64_KERNEL_MAX_BYTES を超える**
（この変更の前の tree でも `ld.lld: error: amd64 kernel exceeds AMD64_KERNEL_MAX_BYTES`、2026-10-05 に確認）ので、新しい step は試験の組
`boundary`（runner と vke2 だけ、`-DI915_VKE2_BOUNDARY`、`platform/amd64/vmunix.mk`）にだけ入れ、生成の file の新しい data も
`I915_VKE2_IN_KERNEL` の kernel では `I915_VKE2_BOUNDARY` の時だけにした。host の lower（IR の interpreter）と compile（EU の model）の
試験は EDGE と UNDEF を全 pixel で照合する（UNDEF の奇数の行は model の値: 未定義の除算は 0、shift は下位 5 bit）。

| command | 結果 |
| --- | --- |
| `python3 src/drivers/gpu/i915/tests/render/generality-shaders/regenerate.py` | 既存の data は変わらず、新しい物だけ増えた |
| `sh plan/ws031/tests/run-vk-host-tests.sh` | PASS（generality 7 × 4096 pixel、IR と EU の model） |
| `BRW_TOOLS=... sh plan/ws031/tests/run-vk-gentool-test.sh` | PASS |
| `I915_TEST_SET=boundary VKLOOP_BUILD_ONLY=1 BUILD=build/p024-vke2b sh plan/ws031/tests/vkloop-hw.sh test vke2` | image の build PASS（kernel 0xfeb000 byte、上限 16 MiB の中、warning は Noct の既存の 1 件だけ） |
| 同じく既定の組（all） | **FAIL（上限超え、変更の前から）** |

q762（2026-10-05）で試験の組を場面ごとに分けた後は、vke2 の組（`I915_TEST_SET=vke2`、vkloop-hw.sh が選ぶ）が boundary の step を持つ。
T1-190 は serial の mirror の無い構成で build されて行が出なかった（q762 で vkloop-hw.sh の既定の構成を直した）。再依頼:
`flock /tmp/i915-hw.lock env BUILD=... plan/ws031/tests/vkloop-hw.sh test vke2`。

増分 3（読みの確認、2026-10-05）: scratch の作成の失敗（`render/draw.c` の `i915_draw_scratch_grow()`）は、古い buffer を消して
`work->scratch` を NULL にしてから作り、失敗は error を返して draw を止める。次の draw は `roomy` が `work->scratch != NULL` を
要るので作り直しに入り、NULL の buffer を使わない（748〜772 行）。一般の状態の大きさを超える scratch は ENOTSUP（`XXX` の log）。
draw.c は host の試験に入っていない（GPU に出すため）ので、失敗の注入の試験は作っていない。spill の組み合わせ（SPILL と VIO16 を
同じ command buffer で、discard と spill、4 KiB 超、VS と PS の同時の spill、sample の応答の spill）の vke2 の step は、vke2 の組の kernel
が 16300 KiB（上限 16384）で余裕が無いので、別の組を作るか上限の問題（q762 の報告）の後にする。

残り（再開の点）: 増分 3 の step（組の余裕の後）、T1 の vke2 の結果（UNDEF の値の記録）。実機は使っていない。

## T1-190b の結果（2026-10-05 Q1）

5330 の passthrough（main fc2f8d9a）: `vke2: verdict PASS (21 of 21 steps passed)`（EDGE・KILLOOP・NOINPUT・UNDEF を含む）、GPU の hang 無し。UNDEF の実機の値（4 回の繰り返しで同じ）: operation 0 → 0x80000000、1 → 0x80000000、2 → 0xffffffff、3 → 0xffffffff、4 → 0x7fffffff、5 → 0x00000000、6 → 0xa99b44c0、7 → 0x02d53368（各 operation の意味は vke2 の UNDEF の表）。証拠 /tmp/claude-1000/t1-190b/。注: step と UNDEF の行は vkloop-hw.sh が 5330 から持ってくる serial を写した log にだけ出る（AGENTS.md の「serial の log で判定しない」との関係は Q1 がユーザーに確かめる）。

## 2026-10-09 P1（合間の仕事、ベータ3）: 増分 3 の host の分

- `plan/ws031/tests/p024/gen-spill.py` が spilltex.frag（96 の値）と spillbig.frag（224 の値）を作る（GLSL と glslc の .spv を tree に置く）。どちらも、値を生かしたまま channel ごとに回数の違う loop の中で texture を sample し（reply が spill の中に着く）、生きている間に discard し、最後に t[k] * t[(7k + 5) % N] を足す。
- `i915-vk-compile-test.c` の `test_p024_spill_mix`: EU の model（fake の sampler・scratch）で 8 channel を動かし、C で同じ順に計算した値と bit で一致を見る。discard された channel は書かれない（x = 0.3 の channel）。spillbig は 1 thread 4 KiB を超える（16384 byte）。spilltex は 2048 byte。
- VS と PS が同時に spill する pipeline（vio16.vert と spill.frag の scratch の欄）は pipe の host 試験に既にある。scratch の作成の失敗は 2026-10-05 の読みのとおり（draw.c は host 試験に入らない）。
- 確認: `sh plan/ws031/tests/run-vk-host-tests.sh`（全部、変更の前の main で PASS）と、変更の後の `run-vk-host-tests.sh compile`（ordinary と ASan・UBSan）PASS、`sh plan/ws031/tests/p024/run.sh` PASS。
- 残り（blocked、UAT 待ち）: spill の組み合わせを実機の vke2 の step にすること（SPILLMIX など）。vke2 の組の kernel の余裕（2026-10-05 は 16300 KiB、上限 16384）と、実機の i915（QEMU に無い）が要る。ユーザーの規則（2026-10-08 夜）で blocked（UAT 待ち）にして先へ進む。
