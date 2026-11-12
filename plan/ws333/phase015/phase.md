<!-- awesome-plan project=zedbsd record=ws333-p015 -->

# ws333-p015: amd64 の既定で MSR の道を有効にする（Q333-7 A）

Status: planned（2026-11-12 P1 が design.md §9 から立てた）
Parent: [WS333](../ws.md)

## 目的

Q333-7 の答え A（2026-11-01）: p014 の 5330 の受け入れが通ったら、amd64 の既定の build で x86 の CPU の driver の MSR の道（HWP・HFI・RAPL・滞在）を有効にする。ユーザーの server の build の設定は変えない。

## 依存

- p014（5330 の受け入れの PASS）。
- 2026-11-12 の build（`mk/`）の名前の整理（design.md §16.2、[../proposed/mk-cpuperf-config.diff](../proposed/mk-cpuperf-config.diff) へのユーザーの答え）。今の mk は x86_64 の既定で既に MSR の道が有効（`CONFIG_DRIVER_CPU_PERF := y`）なので、この Phase の中身は答えに従って変わる。
- Makefile・`mk/` の変更はユーザーの確認が要る（案を示してから）。

## 受け入れ

- 変更の案をユーザーが了承し、当てた後に全機種の vmunix の warning 0、QEMU amd64 の起動の回帰（hypervisor の関門で MSR に触らない）。
