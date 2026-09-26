<!-- awesome-plan project=zedbsd record=ws035p070 -->

# ws035-p070: App Home から X11 の app を起動（Xzed の rootless を必要なときに）

Phase ID: `ws035-p070`
Parent: [WS035](../ws.md)
Status: cleared（q479-i01、2026-09-26）
Phase disposition: normal
Queue: q479-i01
承認: 2026-09-26 ユーザーの自律実行の指示（ホーム画面・X11 機能を含むデスクトップ関連を優先）
設計: [app-home-design.md](../app-home-design.md)、ws069（Xzed の rootless、GLX）

## 背景

X11 の app（zterm、GLX の zgears）は Xzed --rootless を先に起動しないと動かない。Xwayland の「必要なときに X server を起動」に
当たる仕組みを、App Home の起動の道に足す。

## 範囲

1. `/usr/libexec/zdesktop-x11`（sh の script、zwl の package の data）: `$XDG_RUNTIME_DIR/xzed.pid` の Xzed が生きていなければ
   `Xzed --rootless` を起動して socket（`/tmp/.X11-unix/X0`）を待ち（lock は mkdir）、`DISPLAY=:0` で app を exec する。
2. zwl の App Home の既定の一覧に「X terminal」（zterm）と「Gears」（zgears、閉じるまで回る）。zgears の `--frames=0` は無限。
3. demo image（i915 実機用）に Xzed・zterm・libgl・zgears。
4. 試験: App Home の icon の click で X terminal と Gears が Wiseman の窓として出て、Xzed は 1 つだけ。p069 の試験の app の数を
   6 に。

## 受け入れ

1. Venus で App Home の icon から X terminal と Gears が起動し、Xzed の process が 1 つ（2 回目は既存を使う）。
2. p069（App Home）と x11-p004・p005 の試験が通る。build warning 0、新しい C の style-check 0。
3. i915 実機: 未実施（demo image は作る）。

## 結果（2026-09-26、q479-i01）

cleared。受け入れ 1〜3 を満たした（3 の実機は未実施、demo image は作った）。

### 実装

- `userland/base/zwl/zdesktop-x11`（zwl の package の data、`/usr/libexec/zdesktop-x11`。ZEDBSD_PACKAGE_INPUTS に入れて image を
  作り直させる）: `$XDG_RUNTIME_DIR/xzed.pid` の Xzed が生きていなければ（`ps -o comm` は `/bin/Xzed` と path で出るので
  basename で比べる）古い socket を消して `Xzed --rootless` を起動し、socket を待つ（最大 10 秒、mkdir の lock）。そして
  `DISPLAY=:0` で app を exec。
- zwl の App Home の既定の一覧: 「X terminal」（zterm）、「Gears」（zgears --frames=0）。
- zgears: `--frames=0` は閉じるまで。角度は 3600 frame で一巡。
- demo image（`plan/ws031/tests/config-zdesktop-hw.mk`）: libgl・glxtest・zgears（Xzed・zterm は元から）。

### 検証（QEMU・Venus。i915 実機は未実施）

- `plan/ws035/tests/zdesktop-p070.sh` PASS: zwl だけの状態から App Home の「X terminal」で Xzed（1 つ）と zterm、続けて「Gears」で
  同じ Xzed の上に zgears（Xzed は 1 つのまま、Xzed の窓の MAP が 2 つ）。画面 build/ws035-p070/{xterm,gears}.png を目視（zterm の窓と
  回る歯車の窓）。
- 回帰: zdesktop-p069 PASS（app の数の期待を 6 に）、x11-p005 PASS。build warning 0、home.c・zgears の style-check 0。
- demo image: `plan/ws035/demo/build-demo-image.sh` で build/zdesktop-demo/hdd-image.img（実機での起動は未実施）。
- boot test PASS（build/ws035-p070-boot/login.png）。
