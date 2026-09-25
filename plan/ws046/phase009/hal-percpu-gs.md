# HAL の差分の提案: `amd64_percpu_current()` を `rdmsr` から `%gs` の load に（未適用、承認待ち）

2026-09-25、ws046-p009（q418-i01）。AGENTS.md の「HAL の変更には差分ごとの事前承認が要る」により、この差分は適用していない。

## 根拠

guest（amd64、KVM、host は bare-metal の Xeon Gold 6130、VT-x）で trivial な program の link（`cc t.o -o t`）を繰り返し、gdbstub で vCPU の program counter を標本にとった
（LTO を外した kernel で関数の境界を保った）。kernel の中の標本の **約 24〜25%**（186 のうち 47、修正の後も 182 のうち 44）が `asm_read_msr` の 1 命令にいた。
呼ばれ方は `spin_lock`・`spin_unlock`・`mutex_*`・`thread_current()` → `hal_cpu_current()`・`hal_task_get_current()` → `amd64_percpu_current()` → `rdmsr IA32_GS_BASE`。
KVM の VT-x は GS_BASE の読みを VM exit させないが、`rdmsr` は 1 回に 100 cycle 前後かかり、lock の 1 回ごとに呼ばれる。

## 差分

`struct amd64_percpu` の最初の field は `self`（offset 0）で、`amd64_percpu_select()` が `cpu->self = cpu` を書いてから `IA32_GS_BASE` に cpu を書く。
HAL は `swapgs` を使わないので、GS の base は kernel でも user でも選んだ CPU の state を指す。`%gs:0` の読みは `rdmsr(GS_BASE)` が返す address にある `self` を読むのと同じ値になる。

```diff
--- a/src/hal/amd64/percpu.c
+++ b/src/hal/amd64/percpu.c
@@ amd64_percpu_current(
 	struct amd64_percpu *cpu;
 
-	/* Reads the state pointer published in IA32_GS_BASE. */
-	cpu = (struct amd64_percpu *)(uintptr_t)
-	    asm_read_msr(AMD64_MSR_GS_BASE);
+	/*
+	 * Reads the state's self pointer through GS, whose base is the state
+	 * IA32_GS_BASE was set to: one load instead of an rdmsr, which every
+	 * lock and every thread_current() pays for.  self is the first field.
+	 */
+	__asm__ volatile("movq %%gs:0, %0" : "=r"(cpu));
 
 	/* Rejects an absent or self-inconsistent selection. */
 	if (cpu == NULL || cpu->self != cpu)
 		HAL_FATAL("amd64 per-CPU state is not selected");
```

`_Static_assert(offsetof(struct amd64_percpu, self) == 0, ...)` を `percpu.h` に足す（`self` の位置に依存するため）。

## 違い

- 選ぶ前（GS の base が 0）に呼ぶと、今は `HAL_FATAL("... not selected")`、変えた後は address 0 の読みの page fault になる。今の起動では選ぶ前に呼ばれていない（呼ばれれば今でも起動が止まる）。
- user が GS の selector を読み込むと GS の base が変わる危険は、今の `rdmsr` と同じ（どちらも GS の base を読む）。

## 試験（承認の後）

build（4 platform、warning 0）、boot test、guest の sh・make の差分試験・対話試験、SMP の stress（`SMP-STRESS.ELF`）、kbench、link の時間と gdbstub の標本（`asm_read_msr` が消えること）。
