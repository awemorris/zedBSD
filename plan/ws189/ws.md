<!-- awesome-plan project=zedbsd record=ws189 -->

# WS189: app の間の drag and drop（画像・受け入れの見た目・dock・desktop・画面をまたぐ）

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ2）
Queue: q892（P1 の新しい世代）
<!-- awesome-plan-current:end -->

## 由来（2026-10-08 ユーザー）

「Text Editorなどアプリで選択範囲があったり、画像オブジェクトがあるアプリで、選択範囲をマウスドラッグ開始したり、画像オブジェクトをドラッグ開始したときに、コンポジタに通知しておき、別なウィンドウの上に持って行くと、そのウィンドウに通知が行って、ドロップ受け入れ可能かをコンポジタが問い合わせて、可能ならビジュアル的にそれがわかり、さらにドロップすると、クリップボード経由でデータが別アプリに送られる。システムが一体となって操作できる感じ。これを実現したい」

## 決定（2026-10-08 ユーザー、クリックの回答）

- 受け渡し: **DnD の専用の受け渡し**（Wayland の wl_data_source・wl_data_offer。型の一覧と pipe、clipboard の中身は変えない）。
- 同じ app の中の drag は app の中で処理する（Text Editor は今は何もしない、将来は移動を実装してよい）。この WS は app の間の drag。
- 最初に足す型: **画像（PNG）**（今は文字とファイルの一覧だけ）。
- 受け入れの見た目: **compositor と app で分担**（compositor は drag の badge を「コピー可・不可」で変える、受ける app は落とす場所（文の挿入点・画像の枠）を光らせる）。
- 一体感の演出: **dock bar の app の icon の上で止まるとその app が前に（spring-loaded）**、**desktop に落とすと file**、**画面をまたいで drag**。X11 の app との橋渡し（XDND）は入れない。

## 既にある物（2026-10-08 Q1 の確認）

- compositor の wl_data_device の drag and drop（ws035-p084、pointer と指、icon か badge）。
- libkeiland の drop の口（KL_WINDOW_DROP_ENTER・MOTION・LEAVE・DROP、受ける action の答え、KL_VERSION 44・46）、`kl_window_drag_text`（KL_DROP_TEXT・KL_DROP_URIS）。
- Files の DnD（自動の scroll・spring-loaded）、Terminal の文字の drag。

## 原則

- 型の一覧は drag が通る窓に見せるが、データは落とした窓にだけ渡す。
- 規約は plan/coding-style.md の全文、Guardrail（compositor は libvulkan だけ、OS の操作は backend）。

## Phase（案、p001 の設計で確定）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: 画像の型（image/png）の source と offer、libkeiland の口（kl_window_drag_image・KL_DROP_IMAGE、受け入れの答え）、compositor の badge の見た目（コピー可・不可）、app の落とす場所の光らせ方、dock の spring-loaded、desktop への file の保存、画面をまたぐ drag、試験（AAT）。design-reviewer | planned | — |
| p002 | libkeiland と compositor（画像の型、badge、dock の spring-loaded、画面をまたぐ） | planned | p001 |
| p003 | app: 画像の drag の元（Photos・Notes・PDF Viewer・Browser）と受け（Notes・desktop の file）、文字の drag の元と受け（Text Editor・PDF Viewer） | planned | p002 |
| p004 | Mail の作成の添付（multipart/mixed・base64、添付の一覧の UI）と drop の受け（2026-10-08 ユーザーの決定「WS189 で添付も作る」） | planned | p002 |
