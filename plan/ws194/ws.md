<!-- awesome-plan project=zedbsd record=ws194 -->

# WS194: make keiland-linux・keiland-freebsd の必要な package の確認と導入、build 後の install の確認

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q919（P1、2026-10-09、WS193 の後）
Target: **ベータ2**（2026-10-09 ユーザー、クリック「両方ベータ2」）
Resume point: p001 から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー、原文）

「make keiland-freebsdは、必要なFreeBSDパッケージを、ユーザに確認してからインストールしてほしいです。ビルドが成功したらinstallしていいか確認して、インストールを実行してほしいです。
あと、make keiland-linuxは、apt/yum/pacmanがあれば、必要なパッケージをユーザに確認してからインストールしてほしいです。テストはaptだけでいいです。ビルドが成功したらinstallしていいか確認して、インストールを実行してほしいです。」

## 目標

- `make keiland-freebsd`: 足りない FreeBSD の package（pkg）を一覧にして、ユーザーに確かめてから `pkg install` する。build が成功したら install してよいかを確かめ、実行する。
- `make keiland-linux`: apt・yum（dnf）・pacman のどれかがあれば、足りない package を一覧にして確かめてから導入する。build が成功したら install してよいかを確かめ、実行する。
- 確かめは端末の対話（y/N）。対話でない時（stdin が端末でない）は導入・install をせずに何が要るかを出して止まる（既定の安全側）。

## 完了の条件

- Linux: Debian 13 の QEMU+KVM の guest（WS105・WS131 の例外）で apt の経路を試す（足りない状態から、確認・導入・build・install の確認・install）。yum・pacman は試験しない（ユーザー「テストはaptだけでいいです」）、一覧の対応だけ。
- FreeBSD: 専用の FreeBSD 15 の guest（WS109 の例外）で pkg の経路を試す。
- 規約の全文の見直し（shell・make）。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 今の keiland-linux・keiland-freebsd の規則と依存の調べ、package の一覧（apt・yum・pacman・pkg）と確認の対話の実装、host 試験 | planned | — |
| p002 | T1 の Debian（apt）と FreeBSD（pkg）の guest での試験 | planning | p001 |
| p003 | 規約の全文の見直し | planning | p001 |
