<!-- awesome-plan project=zedbsd record=ws049 -->

# WS049: kernel 内の ACPI AML interpreter

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG003
Related Milestones: MG006, MG008
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: p001（調査と設計）から
<!-- awesome-plan-current:end -->

## 目標

kernel の中に ACPI の AML interpreter を持ち、DSDT・SSDT を読み込んで namespace を作り、driver が AML の method（`_STA`・`_CRS`・`_PS0`・`_PS3`・
`_DSM`・`_Lxx`/`_Exx` ほか）を評価できるようにする。UCSI（WS050）、USB-C の DisplayPort Alternate Mode（WS051）、S0i3 の電源管理（WS052）の土台。

## きっかけ

2026-09-24 ユーザー指示: 「下記をそれぞれWSとして追加してほしいです。カーネル内ACPI AMLインタプリタの実装。…」

## 今あるもの

- ACPI の table の解析は HAL の中にだけある（`src/hal/amd64/bsp-pcat/acpi.c`、`src/hal/i386/acpi.c`: RSDP（loader が渡すものか firmware の走査）・XSDT・MADT・MCFG）。
- HAL に ACPI の table の専用の口は無いが、**機種依存の情報を名前で問い合わせる `hal_get_arch_handoff(name)`**（`include/hal/hal.h`）がある
  （2026-09-24 ユーザーの指摘）。今の名前は `boot.command-line`・`boot.selector`・`pcat.boot-font`・`pcat.framebuffer`（`src/hal/amd64/bsp-pcat/boot.c`）。
  kernel は、ここに足す名前（例: RSDP の物理番地か、検証済みの table の一覧）で ACPI の table を受け取り、`hal_space_map_device()` で読める。
  `hal.h` は変えずに済むが、`src/hal` の既存の宣言の実装に名前を足すので、**差分ごとの承認が要る**（p001 が案を作る）。
- SCI の割り込みは `hal_irq_register()`、PM1・GPE の register は `hal_io_inp*`・`hal_io_outp*`（port I/O）と `hal_mmio_*` で扱える見込み（p001 で確かめる）。
- 対象機は Dell Latitude 5330（Alder Lake-P、WS029・WS031 と同じ）を想定（仮定。ユーザーの確認が要る）。QEMU の q35 の DSDT は小さく、単体の試験に使える。

## 範囲

- AML の byte code の解析と namespace（Scope・Device・Method・Name・OperationRegion・Field・Mutex・Event・Alias、外部参照）。
- 評価器: 整数・文字列・buffer・package、制御（If・While・Return）、演算、`Notify`、`Sleep`・`Stall`、`Acquire`・`Release`。
- OperationRegion の access: SystemMemory、SystemIO、PCI_Config、EmbeddedControl（EC の driver が要る）、GenericSerialBus は要否を調べる。
- SCI と GPE の割り込み、`_Lxx`・`_Exx` の実行、`Notify` を driver へ。
- `_OSI` の答え方（Windows の版を名乗るかどうか。Alder Lake の機種は多くの機能を Windows の版で切り替える）。

範囲外（別 WS）: UCSI（WS050）、DP Alt Mode（WS051）、電源管理（WS052）。

## 受け入れ

- QEMU（q35）と対象機の DSDT・SSDT を読み込んで namespace を作り、全 method を評価せずに列挙でき、`_STA`・`_CRS`・`_HID` を評価できる。
- 対象機で EC を介する `_Qxx`（例: 電源 button・lid）か GPE の event が driver に届く。
- AML の試験の集合（自作の ASL を iasl で作った AML と、実機の table の抜き出し。license に注意）で評価の結果を確かめる。
- 規約（`plan/coding-style.md`）の全文、build（warning 0）、boot test。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| ws049-p001 | 調査と設計: AML の仕様（ACPI 6.5）の範囲、対象機の DSDT・SSDT が使う opcode と OperationRegion の種類（実機の table を読む。取り出しはユーザーの手を借りる）、kernel の中の置き場（`src/kern/acpi` か `src/drivers/acpi`）、`hal_get_arch_handoff()` に足す名前の案（承認が要る差分）、試験の方法 | planning | — | 設計文書 |

p001 の結果で p002 以降（解析、namespace、評価器、OperationRegion、SCI・GPE・EC、規約）に分ける。

## 人間の判断が要る点

- `hal_get_arch_handoff()` に ACPI の名前を足す差分の承認（p001 が案を作る。`hal.h` は変えない見込み）。
- 対象機（Latitude 5330 でよいか）と、`_OSI` でどの Windows を名乗るか。
