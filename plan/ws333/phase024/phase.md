<!-- awesome-plan project=zedbsd record=ws333-p024 -->

# ws333-p024: 規約の全文の見直しと文書

Status: planned（2026-11-12 P1 が design.md §9 から立てた）
Parent: [WS333](../ws.md)

## 目的

code を作る WS の最後の Phase（AGENTS.md §2）: WS333 が変えた C（`src/kern/thermal*.c`・`cpuperf.c`、`src/drivers/platform/pcat/cpu-x86/`、`src/drivers/acpi/acpi-dell*.c`・`acpi-wmi*.c`・`acpi-thermal.c`、`src/drivers/platform/rpi5/rpi5-fan.c`、`src/hal/x86_64/bsp-pcat/idle-suspend.c`・`lapic.c`・`clock.c` の WS333 の部分、`userland/desktop/sessiond/thermal-rules.c`、`/sbin/thermal`）を `plan/coding-style.md` の全文と照らして書き方だけ直す（conformance-reviewer）。`docs/architecture/power-management.md` を書く（document-writer）。

## 依存

- p005〜p022 の実装の Phase が全部 cleared。

## 受け入れ

- 規約の見直しの結果（直した所の一覧）、全 port の vmunix の warning 0、host 試験の PASS。
- 文書（`docs/` は目標の設計を書き、`plan/` を参照しない）。
