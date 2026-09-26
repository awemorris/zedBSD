<!-- awesome-plan project=zedbsd record=ws070-spec -->

# zdesktop System Menu Extension 仕様案（ユーザー提供、2026-09-27）

2026-09-27 にユーザーが示した仕様案の原文（第 1 版）。設計（ws070-p001）はこれを元にし、変える所は design.md に理由と共に書く。

ユーザーの指示: 「この画像は、フローティングタイトルバーとドッキングタイトルバーにメニューを追加してみたものです。下記が仕様です。
X11サーバの実装が終わったら、OpenGLよりも、これを先に実装してもらえませんか？あとでGTK4やQt6のネイティブメニューバーとしても
利用可能にするつもりです。XDG拡張ではあるものの、libzdesktopでラップします。記録したら作業を続けてください。」

添付の画像（2 枚、会話の中だけにあり、ファイルは無い）の内容:

1. フローティング: 壁紙の上の Terminal の窓。浮いたタイトルバーの左に青い角丸の「T」の icon と「Terminal」、続いて
   「Shell Edit View Session Help」、右に — □ ×。上部のシステムバーは左に zedBSD の icon と「zedBSD」、右に仮想デスクトップ 4 つの
   縮小図、電波、電池、日時。
2. 最大化（ドッキング）: 窓のタイトルバーが消え、上部のシステムバーが
   「[zedBSD] | [T] Terminal Shell Edit View Session Help | — ▢ × | [仮想デスクトップ 4 つ] | 電波 電池 日時」になる。窓の本体は
   バーの下の全面。

---

## 1. 概要

本機能は、Wayland クライアントがアプリケーションのメニュー構造とアクションを compositor に公開し、compositor がそれを
システム所有の UI として描画・操作するための Wayland 拡張である。

従来のクライアント描画型メニューバーとは異なり、クライアントはメニューの外観や配置を描画しない。クライアントが提供するのは、
メニュー項目のラベル、階層、状態、アクション、ショートカット、アイコンなどの意味情報のみとする。

メニューの実際の表示、テーマ、配置、ポップアップ、入力処理、アクセシビリティ、アニメーションなどは compositor が担当する。

zedBSD では、同じメニューモデルをウィンドウ状態に応じて異なる場所へ表示する。通常時は、

```
[icon] Terminal   Shell  Edit  View  Session  Help              — □ ×
```

のようにフローティングタイトルバー内へ表示する。最大化時は、タイトルバー自体を画面上部のシステムバーへ統合し、

```
[zedBSD] | [Terminal] Shell Edit View Session Help | — ▣ × | desktops | status
```

のように表示する。クライアントから見れば、表示場所が変化しただけであり、同じメニューモデルを継続して使用する。

## 2. 目的

- **UI の統一**: すべてのアプリケーションで、フォント、色、余白、メニューの開き方、サブメニュー、hover、keyboard navigation、
  touch target、animation などを system shell 側で統一する。
- **ウィンドウ状態との統合**: 通常ウィンドウと最大化ウィンドウで、メニュー表示を自然に移動できるようにする。zedBSD では
  floating window → floating titlebar menu、maximized window → system top bar menu という表示ポリシーを採用する。
- **クライアントとシステム UI の分離**: クライアントはメニューの「意味」を定義し、compositor は「見た目」と「操作」を定義する。
  UI デザインを変更しても、アプリケーション側の実装変更を最小限にできる。
- **アクセシビリティの統一**: compositor が menu tree を把握することで、screen reader、high contrast、large text、keyboard-only
  operation、touch mode を system-wide に提供できる。
- **trusted system UI の実現**: クライアントに任意の menu pixels を描画させず、system shell が描画することで「これは system menu
  である」という視覚的・操作的な一貫性を保つ。

## 3. 基本モデル

クライアントは `xdg_toplevel` に対してメニューモデルを関連付ける。

```
wl_surface
  └─ xdg_surface
       └─ xdg_toplevel
            └─ xdg_toplevel_menu_v1
```

プロトコル名は仮称として `xdg_toplevel_menu_v1` を使用する。

## 4. 責務分担

クライアントが所有するもの: メニュー階層、項目ラベル、action ID、enabled / disabled、visible / hidden、checked state、radio state、
shortcut、semantic role、icon name、submenu structure。

compositor が所有するもの: 描画、配置、popup geometry、font、color、blur、shadow、padding、pointer handling、keyboard navigation、
touch handling、submenu handling、dismissal、animation、accessibility presentation、HiDPI scaling、icon resolution、
active-menu selection。

## 5. メニューモデル

メニューは tree 構造として表現する。

```
Shell
 ├─ New Tab
 ├─ New Window
 ├─ Close Tab
 └─ Quit

Edit
 ├─ Copy
 ├─ Paste
 └─ Select All

View
 ├─ Zoom In
 ├─ Zoom Out
 └─ Fullscreen

Session
 └─ ...

Help
 └─ About
```

## 6. Menu Item

各 menu item は最低限 `id label action type enabled visible checked role icon_name shortcut submenu` を持つ。`id` は menu 内で一意とする。

## 7. Menu Item Type

最低限 `normal separator checkbox radio submenu` をサポートする。

```
View
 ├─ Show Toolbar      [✓]
 ├─ Theme
 │   ├─ Light         (●)
 │   ├─ Dark          ( )
 │   └─ System        ( )
 └─ Fullscreen
```

## 8. Action

menu item と action は論理的に分離する（例: item id = 10、label = "Copy"、action = 42）。ユーザーが `Copy` を選択すると、
compositor はクライアントへ `activate(42)` を送信する。同じ action を menu、shortcut、command palette、context menu などから再利用できる。

## 9. Menu Activation

compositor からクライアントへの activation event は概念的に `activated(item_id, seat, serial)` とする。input event と対応付けられる
ように、`seat` および `serial` を保持できる形が望ましい。

## 10. Dynamic Update

メニューは実行中に更新可能とする（例: Undo "Delete Line"、Recent Files、Paste enabled=true、Copy enabled=false）。更新操作として
`set_label(id, string)`、`set_enabled(id, bool)`、`set_visible(id, bool)`、`set_checked(id, bool)`、`set_shortcut(id, ...)`、
`set_icon_name(id, ...)`、`insert_item(...)`、`remove_item(...)` を提供する。

## 11. Transaction / Batch Update

複数項目を同時更新するときに中間状態が表示されないよう、transaction をサポートする。`begin_update(serial)` … `commit(serial)`。
compositor は `commit` 単位で表示を更新する。

## 12. アイコン

アイコンは client-provided pixels を基本とせず、semantic role または icon name を compositor が解決する方式を採用する。
優先順位は 1. semantic role に対応する system icon、2. icon_name、3. icon なし。

- Semantic Role の代表例: copy、paste、cut、undo、redo、preferences、about、quit、open、save、delete。label = "Copy"、role = copy であれば、
  compositor が system icon theme から copy icon を選択する。
- Icon Name: アプリ固有の操作には `icon_name = "git-symbolic"` のような symbolic icon name を指定できる。compositor は active icon
  theme に従って解決する。
- アイコン描画（size、color、symbolic / filled、light / dark、HiDPI、touch mode）は compositor policy とする。menu item はアイコン表示を
  必須としない。top-level menu（Shell Edit View Session Help）は原則 text-only とする。

## 13. Shortcut

各 menu item は accelerator / shortcut（例: Ctrl+Shift+T、Ctrl+C、Ctrl+V）を公開可能とする。compositor は shortcut を表示できる。
実行については focused application scope に限定する。system shortcut → application menu accelerator → client key event の優先順位を
持たせることができる。

## 14. Semantic Role

`label` とは別に `role`（quit、preferences、about、copy、paste、undo など）を持たせる。compositor は platform policy に応じて並び順変更、
system icon 適用、accessibility label、shortcut policy を適用できる。role は optional とする。

## 15. Focus と Active Menu

system menu に表示される menu model は、基本的に focused `xdg_toplevel` に従う（focused_toplevel → active_menu_model）。
例: Terminal は Shell Edit View Session Help、Browser は File Edit View History Bookmarks Help。

## 16. 通常ウィンドウ時の表示

floating 状態では、それぞれのウィンドウの floating titlebar に表示する（`[icon] Terminal   Shell Edit View Session Help   — □ ×`）。
この UI は compositor が描画する。クライアントは titlebar geometry を知らない。

## 17. 最大化時の表示

最大化時は floating titlebar を削除し、その内容を system top bar に統合する（[zedBSD] [Terminal] Shell Edit View Session Help — ▣ ×
[virtual desktops] [status]）。同じ menu model をそのまま使用する。クライアント側で menu の再登録は不要とする。

## 18. Menu Popup

top-level menu item を選択した場合、popup menu は compositor が生成する。compositor が popup placement、pointer grab、outside click
dismissal、keyboard navigation、submenu placement、animation、touch interaction を担当する。クライアントは popup surface を作成しない。

## 19. Menu Open Notification

必要であればクライアントへ `menu_opened(id)`・`menu_closed(id)` を通知できる。基本仕様では optional とする。

## 20. Accessibility

compositor は menu tree を直接知るため、screen reader、menu navigation、shortcut announcement、checked state announcement、
submenu hierarchy、high contrast、larger UI、touch target enlargement を system-wide に実装できる。

## 21. Touch Mode

input source に応じて compositor が presentation を変えてよい。pointer mode: row height = 28–32 logical px、icon = 16px。
touch mode: row height = 44–52 logical px、icon = 20–24px。client はこの違いを意識しない。

## 22. Fallback

compositor が `xdg_toplevel_menu_v1` をサポートしない場合、クライアントまたは toolkit は従来の client-side menu を使用できる
（available なら system menu、でなければ client-side menu）。

## 23. Toolkit Integration

アプリケーションが protocol を直接扱うのではなく、toolkit が吸収することを想定する。GTK: GMenuModel → Wayland backend →
xdg_toplevel_menu_v1。Qt: QMenuBar → QPA Wayland plugin → xdg_toplevel_menu_v1。通常のアプリコードは大きく変更せず利用できる。

## 24. Protocol Object Model

仮の object 構成: `xdg_menu_manager_v1`、`xdg_menu_v1`、`xdg_toplevel_menu_v1`。item ごとに Wayland object を生成すると数が増えるため、
menu item は numeric ID で管理する方式を推奨する（例: `xdg_menu_v1.append_item(id, parent_id, label)`、`set_enabled(id, bool)`、
`set_checked(id, bool)`、`set_role(id, role)`、`set_icon_name(id, name)`）。

## 25. 想定 API フロー

```
bind xdg_menu_manager_v1
menu = manager.create_menu()
menu.append_item(...)
menu.append_item(...)
menu.append_submenu(...)
toplevel_menu = manager.get_toplevel_menu(xdg_toplevel)
toplevel_menu.set_menu(menu)
```

ユーザー操作時は compositor → activated(item_id) → client。client が状態を更新: set_checked(...)、set_enabled(...)、commit(...)。

## 26. Security Model

クライアントは system menu の pixels を制御できない。指定可能: label、state、role、shortcut、icon name、hierarchy。
指定不可: font、color、arbitrary bitmap、popup position、shadow、blur、animation、system-bar geometry。system shell の visual trust を保つ。

## 27. zedBSD Shell Policy

protocol 自体は表示方法を規定しすぎない。zedBSD shell では policy として、Floating: menu → floating titlebar、Maximized: menu →
system top bar、Focus Change: active menu → focused toplevel とする。protocol は意味モデルを定義し、shell は presentation policy を決定する。

## 28. 仕様の要約文

The XDG Toplevel Menu protocol allows Wayland clients to expose application menu structure, state, icons, shortcuts, and actions to
the compositor. The compositor owns presentation, placement, input handling, accessibility, and interaction as part of the system
shell. Clients provide semantic menu data rather than rendering menu UI directly.

XDG Toplevel Menu は、Wayland クライアントがアプリケーションメニューの構造、状態、アイコン、ショートカットおよびアクションを
compositor に公開するためのプロトコルである。メニューの描画、配置、入力処理、アクセシビリティおよびユーザーインタラクションは
compositor がシステム UI の一部として所有し、クライアントはメニュー UI を直接描画せず意味情報のみを提供する。
