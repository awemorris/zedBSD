<!-- awesome-plan project=zedbsd record=ws070 -->

# WS070: zdesktop の System Menu（`xdg_toplevel_menu_v1`、libzdesktop で包む）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: none
Resume point: p001（設計）。WS069 の X11 server（p008〜p010）の後、WS068 の GLSL・desktop GL より前（2026-09-27 ユーザー指示）
<!-- awesome-plan-current:end -->

## 目標

2026-09-27 ユーザー: 「この画像は、フローティングタイトルバーとドッキングタイトルバーにメニューを追加してみたものです。下記が仕様です。
X11サーバの実装が終わったら、OpenGLよりも、これを先に実装してもらえませんか？あとでGTK4やQt6のネイティブメニューバーとしても
利用可能にするつもりです。XDG拡張ではあるものの、libzdesktopでラップします。」

Wayland のクライアントがメニューの意味（階層・ラベル・状態・action・shortcut・role・icon name）を zdesktop に渡し、zdesktop が
システムの UI として描き操作する。通常の窓では浮いたタイトルバーに、最大化（docked）ではシステムバーに同じメニューを出す。
仕様案の原文と添付画像の説明は [spec.md](spec.md)。

## 方式

- protocol は仕様案の `xdg_menu_manager_v1`・`xdg_menu_v1`・`xdg_toplevel_menu_v1`（item は数値の ID、transaction の commit）。
  zdesktop の非標準の拡張なので、client は libzdesktop の API を使い protocol を直接話さない（2026-09-27 ユーザー決定）。
  後で GTK4・Qt6 の native menubar の backend がこの API（または protocol）を使う。
- zdesktop が描く: 浮いたタイトルバーの題名の右、docked のときはシステムバーの題名の右に top-level の項目（text だけ）、選ぶと
  zdesktop の popup（submenu、checkbox・radio、separator、shortcut の表示、keyboard の操作、外の click で閉じる）。
- 最初の使い手は zdesktop-terminal（Shell・Edit・View・Session・Help、画像のとおり）。

## 受け入れ（案、p001 で決める）

1. zdesktop-terminal のメニューが浮いたタイトルバーと docked のシステムバーに出て、選ぶと terminal の action が動く（Venus、実機）。
2. 動的な更新（enabled・checked・label）が commit の単位で反映される。focus の窓のメニューがシステムバーに出る。
3. 規約の全文との照合。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| ws070-p001 | 設計: protocol の定義（request・event・型・error）、libzdesktop の API、zdesktop の model・描画・入力・popup、試験 | planning | WS069-p008〜p010 |
| ws070-p002 | protocol（libwayland の client 側と zdesktop の server 側）と zdesktop の menu model（transaction・更新） | planning | p001 |
| ws070-p003 | zdesktop の描画と操作: 浮いたタイトルバーとシステムバーの項目、popup、keyboard、activation | planning | p002 |
| ws070-p004 | libzdesktop の API と zdesktop-terminal のメニュー（Shell・Edit・View・Session・Help） | planning | p002、p003 |
| ws070-p005 | i915 実機、規約の全文との照合と回帰（最後） | planning | 全 Phase |
