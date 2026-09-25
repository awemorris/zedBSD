<!-- awesome-plan project=zedbsd record=ws055 -->

# WS055: zedBSD の clang が link に `--undefined-version` を既定で渡す

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG001
Related Milestones: MG002
Objectives: O4
Parent: [Master](../master.md)
Queue: なし
Resume point: p001 から（WS054 の後）
<!-- awesome-plan-current:end -->

## 目標

zedBSD の target（`x86_64-unknown-zedbsd`、`i386-unknown-zedbsd`、`aarch64` の zedbsd）の clang が、link の command に `--undefined-version` を既定で付け、
version script に未定義の symbol があっても GNU ld と同じく通す。移植する package（zlib など）の configure が共有 library を作れると判断する。

## きっかけ

ws046-p008（q412-i01）: guest の ld.lld が version script の未定義の symbol を error にする（lld 16 からの既定）ので、zlib の configure が共有 library 非対応と判断した（F-009）。
2026-09-24 ユーザーの問い「--undefined-versionとはなんですか？」への説明（既定にすることを推した）の後、ユーザー決定「すべて承認します。」。

## 範囲

- zedbsd の driver（`toolchain/llvm/patches/0001-add-zedbsd-x86-target.patch` の ToolChain）で、共有 object を作る link と実行 file の link に `--undefined-version` を足す。利用者が `-Wl,--no-undefined-version` で戻せること。
- host の cross の clang と guest の clang（sysroot・image に入るもの）の両方。
- 試験: 未定義の symbol を持つ version script での link、zlib の configure が共有 library を作る（guest）、既存の userland の build が変わらない。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws055-p001](phase001/phase.md) | 調査と実装: driver のどこで link の引数を作るか、patch、LLVM の再 build、試験 | planned | — |
| ws055-p002 | 規約の確認と回帰 | planning | p001 |
