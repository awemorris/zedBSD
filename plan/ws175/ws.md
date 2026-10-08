<!-- awesome-plan project=zedbsd record=ws175 -->
# WS175: Notes で PDF の画像と文字を編集する

Master: [master](../master.md)
Status: incomplete（2026-10-08 q910 P2 の照合: p002（走査・抽出）と p010（T1-265b で fail 0・needs-person 2）は Q1 の判定。準正常系は WS177 の案 K）（2026-10-06: p001・p003・p004・p005・p006・p007 cleared、p002（a・b）実装。p008 は画像の段 cleared、文字の段（ws079-p017 と一つ）を実装し host PASS、画面は p010。次は p010 の T1）
Primary Milestone: MG006

## 由来

ユーザー（2026-10-06）「Notesアプリの機能追加をお願いしたいです。PDF内の画像、テキストを編集できるようにしたいです。コメント的にペンを入れるだけでなくて、画像の位置を変えたり、サイズを変えたり、画像そのものを差し替えたり、新たに画像を入れたり。テキストも編集したり、削除したり、新たに挿入したり、フォントを変えたり。Acrobatみたいになんでもできる必要はないんです。ただ、基本的な編集ができればうれしいと思います。複数ページあるPDFも対応していなければ対応したいです。」

## 目標（単一）

Notes で PDF を開き、基本の編集ができて PDF として保存できる:

- 画像: 移動・大きさの変更・差し替え・新しい画像の挿入・削除。
- 文字: 既存の文字の編集・削除・新しい文字の挿入・font の変更。
- 複数の頁の PDF（頁の移動、頁ごとの編集）。
- 今の pen の書き込み（注釈）は保つ。

範囲外: Acrobat の全機能（form・署名・OCR・頁の並べ替え以上の頁の操作など。必要なら別に）。

## Phase

| Phase | 内容 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws175-p001](phase001/phase.md) | 設計: libpdf の書き出し（増分の更新か作り直しか）、content stream の text・image の object の取り出しと書き換え、font（埋め込みの subset の扱い、置き換えの font）、複数頁、Notes の UI、試験。design-reviewer | cleared（2026-10-06 Q1、D1〜D7 は推奨どおり） | — |
| [ws175-p002](phase002/phase.md) | libpdf の走査: p002a 画像と図形（先の段）、p002b 文字の行と Unicode の抽出（後の段） | p002a・p002b 実装済み（2026-10-06、host PASS） | p001 |
| [ws175-p003](phase003/phase.md) | libpdf の画像・図形の editor（削除・移動・大きさ・差し替え・挿入）、content の組み立て、preview、update の PLACE_EDIT、名前の接頭辞 [M4] | cleared（2026-10-06 Q1） | p002a |
| [ws175-p006](phase006/phase.md) | libz-compat の deflate、圧縮する stream [H6][N11]、画像の取り込み（PNG の素通しと検査・向き・上限）、私的な key と共有・読み戻し [M6] | cleared（2026-10-06 Q1、QEMU は p010） | p003、D4 |
| [ws175-p007](phase007/phase.md) | Notes の model: 物の編集と画像、undo・Reset、ZNOT 2.0、journal 版 2、保存（PLACE_EDIT・blank）と開く時の照合・rebase・読み戻し | cleared（2026-10-06 Q1、画像の段。QEMU は p010） | p006 |
| [ws175-p008](phase008/phase.md) | Notes の UI: 画像の段（Select の道具・挿入・差し替え・削除・Reset・drag・描画・log）、文字の段（Text の道具・編集の box・IME・Font/Size、ws079-p017 と一つ） | 画像の段 cleared（2026-10-06 Q1、resize の画面は p010）、文字の段は実装済み（host PASS、画面は p010）、判定待ち | p007・p004・p005 |
| [ws175-p004](phase004/phase.md) | libpdf の文字の書き換え（移動・削除・元の font での内容の変更・正規化 [H1]・ActualText [M10]）と Notes の文字の model | cleared（2026-10-06 Q1、QEMU は p010） | p003・p007 |
| [ws175-p005](phase005/phase.md) | 置き換えの font（subset・埋め込み・fallback・font の変更）、文字の挿入 | cleared（2026-10-06 Q1、QEMU は p010） | p004 |
| [ws175-p010](phase010/phase.md) | T1 の QEMU で AAT 5 本（補助 `helpers_notes_edit.py`、試料 edit-basic.pdf、シナリオ active）、FAIL の直し 1 回分 | planned（準備済み、T1 の結果待ち） | p008 |
| [ws175-p009](phase009/phase.md) | Save Clean Copy: libpdf の全体の書き直し（clean.c、番号の付け直し・object stream の展開・古い版と使わない resource を落とす [M11]・edit data を落とす）と Notes の File > Save Clean Copy… | cleared（2026-10-07、T1-285） | p008 |
| p011 | 規約は p010 の後 | 未作成 | — |

見積もり: design.md §10（全体 17.5〜21.5 LW、画像を先にする段は約 10 LW）。Q1 の当初の概算は 6 LW。

2026-10-06 / Q1: D2 の font の更新（p005 の前）: D2（Inter・JetBrains Mono・Droid Sans Fallback）の後に、ユーザーが UI の font を Mahora に替え、fallback は JetBrains Mono と Droid Sans Fallback の 2 つだけ残し Inter は使わないと決めた（ws090-p020）。p005 の置き換えの font は install される `keiland.ttf`（Mahora Regular）・`keiland-bold.ttf`（Mahora Bold）・`keiland-mono.ttf`（Mahora Mono）、無い字は `keiland-fallback-mono.ttf`（JetBrains Mono）→ `keiland-fallback.ttf`（Droid）。Mahora は Zlib（ユーザーの著作）で subset の埋め込みは問題ない。
