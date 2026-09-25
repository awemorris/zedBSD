<!-- awesome-plan project=zedbsd record=ws066 -->

# WS066: 動的 link の program の起動を速くする（`ld.so` の最適化）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG002
Related Milestones: MG004
Objectives: O1
Parent: [Master](../master.md)
Queue: none
Resume point: p001（調査と設計）から。優先度は低い（2026-09-26 ユーザー「ld.soの最適化をあとでやるリストとしてwsにしておこう」）
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー: 「単体だとスタティックリンクが速くなるけど、大量のワークロードだと大差ないのね。じゃあとりあえずダイナミックリンクに戻して、ld.soの最適化をあとでやるリストとしてwsにしておこう。」

base の program は動的 link のまま（静的 link にしない）。起動の固定費用のうち `/lib/ld.so` と `/lib/libc.so` の読み込み・再配置・symbol の探索の分を減らし、全ての動的な program の起動を縮める。

## 出発点（2026-09-26、guest、QEMU 8 GiB 4 vCPU NVMe）

- `sh -c :` の起動は動的 1.0 ms、同じ sh を静的に link すると 0.72 ms。差の 0.28 ms（約 28%）が動的 link の費用（[F-020](../future-work.md) の測定）。
- expat の configure・make では差は 0〜3%（時間の大半は clang と libtool の解釈）。
- ws061-p009 の後も `cc` の loop の guest の CPU の 32% が `/lib/ld.so`。clang の driver が実行 file を `--hash-style=sysv` で link し、同じ symbol を指す再配置が多い（libLLVM で 5828 件中 4211 種）（[F-017](../future-work.md)）。

## 候補（p001 で測って選ぶ）

- 再配置の結果の cache（同じ symbol を指す再配置）、探索の順の短縮。
- `--hash-style=both`（または gnu）を clang の driver の既定に（cross の toolchain と guest の LLVM の rebuild が要る）。
- `libc.so` の再配置の数の削減（内部の参照を `-Bsymbolic` や protected visibility で解決）。
- `-z now` を外した lazy binding（`relro` との兼ね合い）。
- mapping と TLS の準備の固定費用。

## 受け入れ（p001 で確定する）

`sh -c :`・`/bin/true`・`cc t.c -o t` の起動が今より縮み、静的 link との差（0.28 ms）の半分以上を取り戻す（仮の目標）。loader の既存の試験と、dash との sh の差分試験・make の差分試験・expat の build が変わらない。規約。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws066-p001](phase001/phase.md) | 起動の費用の内訳（`ld.so` の各段階、再配置の数、探索の回数）と、候補の選択・設計 | planning | — |

p002 以降（実装）と規約の Phase は p001 の結果で定める。
