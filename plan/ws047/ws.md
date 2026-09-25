<!-- awesome-plan project=zedbsd record=ws047 -->

# WS047: build.sh と Noct による build system

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG001
Related Milestones: MG008, MG007
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: p001（今の build の調査と設計）から
<!-- awesome-plan-current:end -->

## 目標

2026-09-24 ユーザー指示:

> あと、これは重たい作業になるのですが、make worldしているビルドシステムを、まずはMakefileも残しつつ、build.shという単一のシェルスクリプトを
> エントリーポイントにした、Noctによるビルドシステムに置き換えたいです。設計としては、TUIのメニューのNoctスクリプト、カーネルビルドのNoctスクリプト、
> ユーザランドのbase,ユーザランドのpackages、などと、分けていって、baseはプログラム単位、packagesはパッケージ単位、さらに依存関係の管理など、
> 少しリッチで拡張性のある設計にしたいです。kernel, baseとpackagesで別なシステムがいいです。

今の make による build（kernel・userland・rootfs・disk image、`make world` 相当）を、`build.sh` を唯一の入口とし Noct の script が行う
build に置き換える。移行の間は Makefile も残し、両方で同じ成果物ができることを確かめながら進める。

## 設計の要件（ユーザー指示）

- 入口は単一の shell script `build.sh`。Noct の host toolchain を用意し、Noct の script を呼ぶ。
- Noct の script を役割で分ける: **TUI のメニュー**（今の `tools/menuconfig.py` の役割）、**kernel の build**、**userland の base**、
  **userland の packages**、image の組み立てなど。
- **kernel・base・packages は別の system** にする（それぞれ独立した記述・規則・依存の管理を持つ）。
- **base はプログラム単位**、**packages はパッケージ単位**で記述する。
- **依存関係の管理**を持ち、少しリッチで拡張性のある設計にする（新しい program・package・platform を記述の追加だけで足せる）。
- 当面は Makefile も残す（段階的な移行）。

## 方針（案。p001 で確かめて決める）

- 各 unit（base の program、package、kernel の構成要素）は宣言的な記述（source・依存・flag・install 先・対象 platform・menuconfig の項目）を持ち、
  Noct の script がそれを読んで依存の graph を作り、必要なものだけを作る（時刻か内容の hash で変化を判断）。
- menuconfig の設定（`config.mk` 相当）を 3 つの system が共通に読む。
- 既存の Makefile の成果物（`build/<platform>/vmunix`、rootfs、disk image）と byte 単位、または意味の上で同じになることを受け入れの軸にする。
- 並列度など build の速度の扱いは p001 で決める。

## Phase 一覧（案。p001 の結果で見直す）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| ws047-p001 | 調査と設計: 今の Makefile・platform の mk・package.mk・`tools/build/*.noct`・menuconfig の役割の一覧、3 つの system の記述形式と依存の graph、`build.sh` の流れ、移行の順と受け入れ（成果物の同一性）。設計文書 | planning | — |
| ws047-p002 | `build.sh` と Noct の共通部分（設定の読み込み、依存の graph と変化の判断、実行と log） | planning | p001 |
| ws047-p003 | TUI のメニュー（menuconfig を Noct へ） | planning | p002 |
| ws047-p004 | kernel の build system（amd64 から、他の platform へ） | planning | p002 |
| ws047-p005 | userland の base の build system（program 単位） | planning | p002 |
| ws047-p006 | userland の packages の build system（package 単位、取得・検証・patch・クロスビルド） | planning | p002 |
| ws047-p007 | rootfs と disk image の組み立て、Makefile の成果物との比較 | planning | p004〜p006 |
| ws047-p008 | 規約の確認と全 platform の回帰 | planning | p007 |
