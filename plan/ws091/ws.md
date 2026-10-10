<!-- awesome-plan project=zedbsd record=ws091 -->

# WS091: 画像 viewer

<!-- awesome-plan-current:start -->
Status: completed（2026-09-29）
Primary Milestone: MG006
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし（main の依頼で worktree `wt/ws091` の subagent が p001〜p003 を実行）
Resume point: —（完了の処理: Phase の directory の削除と試験の `plan/tools/` への移動は main）
<!-- awesome-plan-current:end -->

## 目標（2026-09-29 ユーザー）

「画像ビューアの追加。」

- PNG・JPEG・GIF（libpng-compat・libjpeg-compat・libgif-compat）の表示、拡大・縮小・fit・pan（touch の pinch と慣性、WS090 の部品）、
  同じ folder の前後の画像、Files からの起動（WS093）。

## 結果

`/bin/imageview`（Image Viewer、`userland/desktop/imageview/`、設計は [design.md](design.md)）を足した。

- 形式: PNG（全ての colour type・深さ、interlace は理由を示して表示しない）、JPEG（baseline・progressive、CMYK・YCCK は RGB に、EXIF の向き 1〜8）、
  GIF（静止と動く GIF、disposal の合成、Space で再生・一時停止）。形式は中身の magic で決める。読めない画像は理由の card を出し、前後へは進める。
- 表示: Vulkan で画像の texture（CPU で作る 1/2 ずつの段、3 倍以上は nearest）と UI の canvas の 2 層。fit（拡大しない）・実寸・1.25 倍ずつの
  zoom（最大 16 倍）・pan・90° の回転・全画面（黒）。透明は 8 px の市松に合成。64 Mpx と device の上限を超える画像は半分ずつ縮める。
- 操作: key（矢印・PageUp/Down・Home/End・+/-・0・1・R・F/F11・Esc・Ctrl+O/W/Q、切り替えの key は repeat しない）、pointer（drag・wheel で
  pointer の位置を中心に zoom・double click・右 click の context menu）、touch（1 本指の pan と慣性、2 本指の pinch、fit の横の swipe で前後、
  double tap、long press の context menu）。
- 同じ folder の画像を自然順（数字を数として比べる）で前後に移り、端で止まる。前後の 2 枚を空き時間に先読みする。
- 見た目: すりガラスの card、浮いた chip（名前・位置・大きさ・zoom）、zdesktop の titlebar の control と System Menu、空の画面の Kei の印と Open。
  画面の文字に Keiland・libkeiland を出さない。App Home に「Image Viewer」の tile（緑、額の絵）。
- 規約: `plan/coding-style.md` の全文と照合した（p003）。`style-check.py` の残りは design の例外の `setjmp` の 1 件。

確認（すべて QEMU、Venus）: host の試験 `plan/tools/imageview/run-host.sh`（PIL と比べた復号・folder・EXIF・view）PASS、build の warning 0、guest の
`imageview-guest.sh` の 16 step（App Home・空・fit・次・zoom・wheel・縦・回転・透明・GIF・画素・壊れた画像・swipe・全画面・chooser）ok、
注入の touch の `touch-guest.sh`（pinch・flick の慣性・double tap・swipe・long press）PASS、boot test PASS。画面は `build/ws091-shots/`（worktree）。

## 制限・移管

- 実機（i915、素の 5330 の touch panel）は未確認。確認はすべて QEMU の Venus。
- Files からの起動は WS093（`files/apps.c` の MIME の表は触っていない）。
- file の選択（Ctrl+O）は最小の一覧。WS092 の共有の `keiland_file_chooser_*` ができたら置き換える（main の指示）。
- 復号は同じ thread（大きな JPEG の復号の間は入力が少し遅れる。別 thread は後の改善、design の J7）。編集・保存・slideshow・EXIF の詳細・
  色管理・他の形式（WebP・BMP・TIFF・HEIC）は範囲外。
- 見本の画像は image に入れていない（design の J8、デモの画像は main が選ぶ）。
- PDF Viewer にも key の repeat を dispatch の前に発火する同じ型がある（p002 で見つけた。担当外、main に報告済み）。
- Venus の scanout の取り込みは全画面・swipe の直後に古いことがある（試験は pointer を数回動かしてから撮る）。

## Phase の一覧

| Phase | 内容 | 結果 |
| --- | --- | --- |
| [ws091-p001](phase001/phase.md) | 設計（[design.md](design.md)、J1〜J11 は既定） | cleared（2026-09-29） |
| [ws091-p002](phase002/phase.md) | 実装: 復号・表示・操作・folder・touch・App Home への登録（他の担当の file は main の許可の最小の差分） | cleared（2026-09-29） |
| [ws091-p003](phase003/phase.md) | 全文の規約との照合と回帰（host・build・guest・注入の touch・boot） | cleared（2026-09-29） |

## 試験（完了の処理の候補）

| file | 役割 | 候補 |
| --- | --- | --- |
| `tests/run-host.sh`・`tests/host-imageview.c` | 復号（PIL と比較）・folder・EXIF・view の host の試験 | `plan/tools/imageview/` へ移して Tools 節に登録 |
| `tests/imageview-guest.sh`・`tests/make-images.py` | Venus の guest の操作と画面 | 同上 |
| `tests/touch-guest.sh` | pen の image での touch の注入の試験 | 同上 |
| `tests/style-extra.py` | style-check.py が見ない規則の発見的な走査 | `plan/tools/style-check.py` に取り込むか削除（main の判断） |

## 2026-10-10 大きなJPEGのUAT修正

ユーザー提供4080×3072 JPEGが実機でdecode成功後にvkAllocateMemory=-2となった。[ws157-p008](../ws157/phase008/phase.md)でGPU resource上限16MiBを超える画像のCPU sampling fallbackを補完し、原本/1:1 zoom/四方向rotation/clip/mipを維持する。[ws157-p009](../ws157/phase009/phase.md)がこの有限scopeの最終規約/短いhost/build/main統合を扱う。旧WS完了の履歴を置き換えず、修正と実機の証拠は追加Phaseへ記録。

2026-10-10結果: ws157-p008/p009のJPEGメモリ制限対応はmain `e654733f1eb6e1296eacf34b5dff6002a5dd17e1`、scoped cleared。提供原本の実機decode/CPU presentationはexit0。WS091の従来completed履歴とscopeを維持。
