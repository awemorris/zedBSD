<!-- awesome-plan project=zedbsd record=ws178 -->

# WS178: OpenGL を Desktop へ、GLX を X11 server（xserver）へ

Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p001 を cleared（x11-p004 の段 3 は BUG-273）。2026-10-08 q910 P2 の照合: p001 は T1-340 で p005 PASS・x11-p004 の段 3 FAIL（T1-341 で WS178 の前の main でも同じ＝既存）→ Q1 の判定と x11-p004 の段 3 の Bug 化）（2026-10-07 追加、ベータ2、優先度は低い。p001 を P1 が実装、T1 待ち）
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-07）:「make menuconfigの X11 -> OpenGL and GLX ですが、Desktop -> OpenGL に移動して、GLXは Desktop -> X11 server for the compositorのライブラリの1つに統合しましょう。ベータ2の範囲にして、優先度は低くていいです。」

## 目標

- 今の package `libgl`（label「OpenGL and GLX」、menu の X11、`userland/x11/libGL/`）を分ける。
  - OpenGL（libGL の GL の部分: gl3.c・fixed.c・immediate.c・shaders）は menu の Desktop の「OpenGL」へ（tree も `userland/desktop/` の下へ移す）。
  - GLX（glx.c）は Desktop の「X11 server for the compositor」（`xserver`、`userland/desktop/xserver/`）の library の 1 つにする（xserver を選ぶと GLX も入る）。
- glxtest・zgears など libGL を要る X11 の program の依存（REQUIRE）と link を新しい置き場所に合わせる。（2026-10-07 ユーザーの決定「完全に分ける」で置き換え）libGL.so は GL だけ、glX* は libGLX.so（xserver の package）だけにする。tree の program は -lGL -lGLX に直し、外から移植する X の GL の program は link の修正が要ることを文書に書く。
- vmunix.mk（libGL の link の規則）・config の program の一覧・menuconfig の host 試験を追従する。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 分割と移動（package・tree・link・menu・config）、build と menuconfig の試験、T1 で zgears・glxtest の回帰 | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-340 で p005 PASS。x11-p004 の段 3 の FAIL は WS178 の前から（T1-341）で BUG-273（tracking）。旧: in-progress（P1、実装と host 確認済み、T1 待ち）） | — |
