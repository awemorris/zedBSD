<!-- awesome-plan project=zedbsd record=ws198 -->

# WS198: zedBSD の上で zedBSD を self-build する（host の clang を使う）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG002
Related Milestones: MG001
Objectives: O1
Parent: [Master](../master.md)
Queue: 未割当（P1、menuconfig の Noct・Emacs の後に p001 の調べ）
Target: ベータ2（優先度は低い、2026-10-09 ユーザー「工数もそれほど大きくないので、ベータ2に入りそうです」）。p001 の調べで工数を確かめ、ベータ2 に入らなければ Q1 がユーザーに聞く
Resume point: p001 から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「これは優先度は低いですが、zedBSD上でzedBSDをセルフビルド可能にするWSを作りましょう。このケースではホストclangのみ使えばいいですね。工数もそれほど大きくないので、ベータ2に入りそうです。」

## 目標

- zedBSD の上で、その tree から zedBSD の kernel・userland・image を build できる。compiler は zedBSD の上の clang（userland/packages/lang/clang・devel/libcxx、host の clang として使う）で、toolchain/ の LLVM の build は行わない。
- 対象は amd64 の image（release の config）から。

## p001 で確かめる前提（2026-10-09 Q1 の初見、未確認）

- **make**: root の Makefile は GNU make の機能を使う。zedBSD の base の make（userland/base/make）が足りるか。足りなければ [WS046](../ws046/ws.md)（GNU 互換の make、incomplete）の残りか、GNU make を package にするか。
- **Python**: root の Makefile は `PYTHON ?= python3` で、tools/build に .py が 20、menuconfig も Python。[WS126](../ws126/ws.md)（Python、ld.so の dlopen の直しを 2026-10-09 に merge、T1-508 で確かめ中）が要る。
- **Noct**: host 用の Noct（build/NoctLang、tools/build の .noct が 13）は CMake で build している。zedBSD の上の CMake の有無、または Noct を直に build する手順。
- **その他の道具**: sh・awk・sed・tar・gzip・xz・sha256sum・patch・find・install・mtools 等の image の道具、distfile の取得（curl か事前の download）。
- 必要な物の一覧と、足りない物ごとの工数を表にして Q1 へ（ユーザーにベータ2 に入るかを示す）。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 前提の調べ（上の一覧）、足りない物と工数、self-build の image の config の案 | cleared 候補（2026-10-09 深夜 P1: 段 S0〜S3 の表、S1 は約 27〜33 LW でベータ2 に入らない見込み、D1〜D5 はユーザーの判断） | — |
