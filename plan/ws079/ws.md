<!-- awesome-plan project=zedbsd record=ws079 -->

# WS079: 手書きノート（Notes）と PDF Viewer、上の右端からのスワイプ

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p012 は表の cleared（main 2026-09-28）に phase.md を合わせた。p017 は ws175-p010 の T1-265b（needs-person 2、fail 0）で Q1 の判定待ち。残りは S8・S9 の実機（ユーザー））
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1
Parent: [Master](../master.md)
Queue: なし
Resume point: 2026-10-07 q831 P2（記録の直し）: 実装の残りは無い。残りは (a) 5330 の実機（mouse）と Windows の QEMU（touch）での S8・S9（ユーザー、`demo-s8-s9-manual.md`）、(b) 5330 の passthrough での S8・S9 の自動の確かめ（[guide](guide.md) §3.1 の p020 の案、実機なので Q1 の指示があれば）、(c) L3 の実機のペン（機材なし、Future Work へ移すかは人間の判断）、(d) [p017](phase017/phase.md) の text box（実装済み、画面は ws175-p010 の T1 の再実行待ち）。それ以前: 2026-09-30: L2 の p016 cleared（QEMU の S8・S9 と頁送り ≤ 200 ms）。残りは実機（5330 の mouse）と Windows の QEMU（touch）での S8・S9 の確認（ユーザー、`demo-s8-s9-manual.md`）と、L3 の実機のペン
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-28 ユーザー）

「デスクトップですが、
・スクリーン上部の右上から左下に向かってスワイプすると、手書きノートアプリが起動or起動済みなら最前面化、全画面表示
・手書きノートアプリは圧力4096段階のペンを使って手書きで書類を作れ、PDFで保存できる（PDFにベクトルグラフィックで保存しつつ、メタデータで詳細な編集データを持たせる）
・ついでにPDFビューアアプリも作る。スクロールもできるし、ページ単位でスワイプするモードもある。書き込みはノートアプリで行える。」

同日のユーザーの回答:
- ペンの装置は **USB のペンタブレット**（Wacom など USB HID の digitizer）。QEMU では合成の入力で試験する。
- PDF Viewer は **段階を切る**: ① Notes が書く PDF（ベクタの線・画像）→ ② 一般の PDF の図形・画像・埋め込みの TrueType → ③ CFF・Type1・暗号化など。各段の後に評価する。
- 名前: `userland/desktop/notes`（`/bin/notes`、画面の名前「Notes」）、`userland/desktop/pdfviewer`（`/bin/pdfviewer`、「PDF Viewer」）、
  共有の PDF の library は `userland/base/libpdf`。

同日のユーザーの回答（design-input-notes §8）:「pen tablet はノーブランドのUSB-C or HDMI+USBの10インチタッチLCD＋AES Penを買いました。
HDMIをメイン出力にする方法を確立して、全画面1スクリーンのみでデモに使いたいです。ペンのボタンはまだ届いてないのでわかりませんが、AES/WGPなので2つくらいあるかも。
ノートの保存は保存ボタンだけでなく、一定時間操作がないときに自動保存、ジャーナリング的に復元できるのがいいと思います。
キーボードショートカットは標準的なものにしたほうがわかりやすいと思います。マニアックな使いこなしよりも標準を目指しましょう。
libtruetypeにはアウトラインを返すAPIを追加しましょう。」
- D1: 10 インチの touch LCD（USB-C、または HDMI + USB）と AES の pen。pen・touch は USB HID の digitizer として来る見込み（届いたら descriptor を確かめる）。
- 出力: i915 の実機で **HDMI を主な出力**にし、全画面の 1 画面だけでデモする方法を確立する（新しい Phase、WS075 の display と Keiland）。
- D5: pen の button は 2 つ程度（届いてから割り当て）。D6: 保存 button に加えて、操作の無い時間の後の自動保存と、journal からの復元。
- D8: keyboard の shortcut は標準的なもの（Ctrl+S・Ctrl+Z・Ctrl+Shift+Z/Ctrl+Y・Ctrl+N・Ctrl+O 等）。
- stage ② の glyph: libtruetype に outline を返す API を足す。
- main の判断（2026-09-28、p004 の報告の設計の食い違い）: stroke の形は `pdf_outline_stroke()` を唯一の元にする。Catmull-Rom の平滑化と丸い join は
  この関数に入れ（p005 の前）、Notes は画面と PDF の両方にこの輪郭を使う（design-input-notes §5.3 の `stroke-geometry.c` はこれを呼ぶ）。
- 注意: 新しい worktree で desktop の image を build すると、guest の config が host の LLVM（lldb 付き）を共有の `build/llvm` へ install しようと
  することがある（p004 の agent が install の前に止めた）。worktree の build/llvm の symlink の扱いを次の周期に確かめる。

2026-09-28 ユーザー:「ウィンドウのフローティングタイトルバーを二本指でタッチする（叩く）と、Zオーダーが後ろに回って奥に行き、次のウィンドウが表示されるようにしたいです。」
→ p012（kernel の multitouch）と p013（compositor の touch と二本指のタップ）。今の kernel は指の collection を無視し、compositor には wl_touch が無い。
touch の LCD の USB はまだ見えていない（H1）ので、QEMU の注入の device で先に作る。
同日のユーザー:「2本指でタッチというより、2本指で軽く短く上方向こすって、「あっちにいけ」というジェスチャー、というのがいいですね。とりあえず3回クリックで実装しつつ、マルチタッチが実現したら実装しましょう。」
同日のユーザー:「では、マウスで3回クリックすると同じ動作にしましょう。」→ 浮いたタイトルバーの mouse の triple click でも同じ（窓を後ろへ）。
main の注意: タイトルバーの double click は今ドッキング（ws035-p062）なので、triple click を見分けるために double click の動作を
click の間隔の上限（今の double click の 400 ms）まで待たせる（ドッキングがその分遅れる）。triple click は p013 で compositor と一緒に作るが、
mouse の部分は touch を待たずに先に入れてよい。

## 完了の条件（案、p001 で確定）

1. 画面の上の右端から左下へのスワイプ（pointer の drag と、ペン・touch があればそれも）で Notes が起動する。起動済みなら最前面に出て全画面になる。
2. USB の HID の digitizer のペンの筆圧（4096 段階）・傾き・消しゴム・側面の button を kernel が読み、compositor が Wayland の tablet の protocol
   （`zwp_tablet_manager_v2`）で app に渡す。QEMU では合成の入力で、実機では USB のペンタブレットで確かめる（実機は機種が決まってから）。
3. Notes で筆圧に応じた太さの線を書き、消し、page を足し、undo でき、PDF に保存できる。PDF はベクタの図形で描かれ（他の viewer でも読める）、
   編集の詳細（stroke の点・筆圧・時刻・道具）は PDF の中の metadata（埋め込みの file の stream など）に持ち、Notes は保存した PDF を開いて編集を続けられる。
4. PDF Viewer は縦の scroll と page 単位の swipe の 2 つの mode を持ち、段階 ① の PDF を正しく表示する。「書き込む」で Notes にその PDF を開かせる。
5. 段階 ②・③ は ① の後に評価して進める。

## 制約

- 共有の library の置き場と header の方針は他の compat の library と同じ（base、`include/libc/` の適所）。libpdf は Wayland に依らない。
- 描画は Vulkan（Keiland の他の app と同じ）。画面の文字列に Keiland の名前を出さない。
- kernel の入力の変更は HAL の API に触れない（HID の driver と input の層）。

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| ws079-p001 | 設計: pen の入力（USB HID digitizer → kernel の input の event、QEMU の合成の入力）、`zwp_tablet_v2`、スワイプの gesture、Notes の文書 model、PDF の書き方と metadata、libpdf の構成（parser・content stream・描画の list）、PDF Viewer | cleared（2026-09-28、design-input-notes.md・design-pdf.md） | — |
| [ws079-p002](phase002/phase.md) | kernel: USB HID の digitizer（筆圧・傾き・消しゴム・button・in-range）と、試験用の合成の入力 | cleared（2026-09-28 main の判断、QEMU と host: pen の読み取り・注入の device・peninject の guest の試験 15/15。実機の pen は未実施） | p001 |
| [ws079-p003](phase003/phase.md) | compositor: `zwp_tablet_manager_v2`（pad なし）と tablet を bind しない client への pointer の fallback | cleared（2026-09-28 main の判断、QEMU の Venus: zwp_tablet_manager_v2 と pointer の fallback、28 の検査。実機の pen は未実施） | p001、p002 |
| [ws079-p010](phase010/phase.md) | compositor: 上の右端からのスワイプ（design-input-notes §4）で Notes を起動・最前面・全画面、端のジェスチャーの整理（同文書の追記） | cleared（pointer の範囲、2026-09-28、QEMU。pen の接続は p003 の後、本物の Notes は p005 の後） | p003（pen の部分だけ） |
| [ws079-p004](phase004/phase.md) | libpdf: 書き出し（page、ベクタの path、画像、編集の metadata）と自分の形式の読み込み | cleared（2026-09-28: writer・画像・/ID・日付、`pdf_outline_stroke()` の Catmull-Rom の平滑化と丸い join、自分の形式の読み込み（page・箱・content の SHA-256・添付・ID・日付）、host 試験（ASan/UBSan・破壊 60,000 回・qpdf）、amd64・pcat・rpi4 の `libpdf.so` warning 0。disk image の全体の build は未実施） | p001 |
| [ws079-p005](phase005/phase.md) | Notes v1: 筆圧の線・消しゴム・page・undo・PDF の保存と再編集 | cleared（v1 の一通り、2026-09-28、QEMU と host。pen は p003 の tablet で。自動保存 5 秒・journal・Ctrl+N は新しい page。残り: Ctrl+O の選択、他の PDF の背景、部分の消しゴム、描画の cache、PDF の中の名前） | p003、p004 |
| [ws079-p011](phase011/phase.md) | Notes の仕上げ（デモ向け）: PDF の中の名前を Kei に（`/Producer (Kei Notes)`・`kei-notes.bin`・`application/x-kei-notes`）、Kei の glass の toolbar と背景、pen の hover、確定した stroke の page の画像への cache、（時間があれば）部分の消しゴム | cleared（2026-09-28 subagent、QEMU の Venus と host。名前は writer の 2 行と Notes の定数、glass の card・desk・pen の印、page の画像の cache（500 本で frame 187〜210 → 108〜118 ms、空の page と同じ）、部分の消しゴム（Eraser をもう一度で Part Eraser、1 ドラッグ 1 undo）。実機は未実施） | p005 |
| [ws079-p006](phase006/phase.md) | libpdf の読み込み ① と PDF Viewer v1（scroll と page の swipe、Notes で書き込み） | cleared（2026-09-28、QEMU（Venus）と host。libpdf の display list・stroker・CPU rasterizer（pdftoppm と比較）、PDF Viewer v1（CPU の raster を Vulkan で表示）、App Home・Files の Open With。libc の qsort が O(n²) の発見。実機・image の build は未実施） | p004 |
| [ws079-p007](phase007/phase.md) | 段階 ②: 一般の PDF の図形・画像（DCT は libjpeg-compat、Flate は libz-compat）・埋め込みの TrueType（libtruetype） | cleared（2026-09-28 subagent、2 回に分けて: 文字（埋め込み TrueType・Type0 Identity-H/V・標準 14 の代用）・axial/radial の shading・xref stream・object stream・修復（1 回目、host と QEMU の Venus）、ASCII85/LZW/RunLength・inline image・拡大画像の最近傍・PDF Viewer の「Some content could not be shown」・`run-pdf-text.sh`・規約の見直し・pc98/rpi4 の build（2 回目、host と 4 platform の build だけ）。残り: thumbnail） | p006 |
| [ws079-p008](phase008/phase.md) | 段階 ③: CFF・Type1・暗号化など | cleared（2026-09-28 subagent、host と QEMU の Venus と 4 platform の build。charstring は libpdf で読む（`charstrings.c`・`type1.c`・`cff.c`・`cffdata.c`、libtruetype は不変）: Type 1（eexec、flex、seac）、CFF Type1C・CID-keyed CIDFontType0C・OpenType の FontFile3（Type 2 の全演算子）、Type 3（glyph の procedure を content として）。標準の security handler（`crypt.c`: RC4 40〜128、AES-128、AES-256 R5/R6、空の user password、他は `PDF_EPASSWORD`）、update と Notes は暗号化の文書を拒む。/usr/share/doc の 9 文書の SKIPPED の page 94 → 3（CCITTFax だけ）、生成の programs.pdf は pdftoppm とぼかし後 0.000%。実機は未実施） | p007 |
| [ws079-p012](phase012/phase.md) | kernel の multitouch: USB HID の digitizer の指の collection（Contact ID・Tip Switch・X/Y・Contact Count）→ `ABS_MT_SLOT`・`ABS_MT_TRACKING_ID`・`ABS_MT_POSITION_X/Y`・`BTN_TOUCH`、注入の device の touch の種類（試験用） | cleared（2026-09-28 main の判断、QEMU と host: protocol B・touch の注入・193 の host の検査。実機の touch の LCD（2 つの device・descriptor の flag）は未実施） | p002 |
| [ws079-p013](phase013/phase.md) | compositor の touch: client への `wl_touch`、touch の接触を端のジェスチャー（p010 の `zwl_corner_contact_*` と Home・Wiseview）へ、**浮いたタイトルバーの上で二本指を軽く短く上へこする（「あっちにいけ」）と窓を z-order の後ろへ回し、次の窓を前に**（2026-09-28 ユーザー。タップから訂正）。mouse の triple click でも同じ | cleared（2026-09-28、QEMU の Venus だけ。mouse の triple click（p062・p076 の回帰 PASS）と touch: touch screen を `wl_touch` に（wl_touch の無い client には pointer の fallback）、compositor が先に見る: 端のジェスチャー（Home・Notes・Wiseview・Home の下端）を touch で、浮いたタイトルバーの二本指の上への flick（24 px 以上、250 ms 以内）で `window_lower`、遅い・下・横は何もしない、一本指の drag と tap、取ったら client に `wl_touch.cancel`。libwayland に `wl_touch`、`tablet-probe --touch`、`zdesktop-p013-touch.sh` PASS、p013（mouse）・p010・p012 の回帰 PASS。実機は未実施） | p012、p003 |
| [ws079-p014](phase014/phase.md) | Notes で他の PDF に書き込む: 自前の編集の data の無い PDF・他で変わった page を背景（`pdf_page_render`・`pdf_display_list_rasterize`）にして上に線を足し、保存は増分の更新（PDF Viewer の Annotate in Notes の本来の動き） | cleared（2026-09-28 subagent、QEMU の Venus と host。libpdf に update（追加の API 6 つ）、Notes は他の PDF を base に持ち保存ごとに base＋1 revision（積まずに置き換え）、ZNOT 1.1 の BASE・SRC、背景は CPU の raster を texture に、暗号化・署名は notice で拒否。qpdf・pdftoppm・ASan・UBSan。実機は未実施） | p005、p006 |
| [ws079-p015](phase015/phase.md) | p007・p008 の残り: CCITTFax（Group 3・4）の画像の decode、暗号化の文書の password（libpdf の user・owner password の API と PDF Viewer の card）、PDF Viewer の thumbnail の sidebar | cleared（2026-09-29 subagent、host と QEMU の Venus と 4 platform の build。`ccitt.c`（ghostscript の符号化 80 例と bit 単位で同じ、実文書の SKIPPED 3 → 0）、`pdf_document_open_password()`・`_memory_password()`（R2〜R6 の user と owner、qpdf の copy が平文と同じ画素）、PDF Viewer の password の card と左の thumbnail の sidebar（F9・View・titlebar、待つ間に描く、click で page、一覧が追う）。実機は未実施） | p007、p008 |
| [ws079-p016](phase016/phase.md) | L2: デモの S8・S9 の通しの試験（`demo-s8-s9.sh`、注入の touch と pen）、頁送りの時間の log（`TURN done`）、実機と Windows の QEMU の手順の 1 枚 | cleared（2026-09-30: QEMU で S8・S9 PASS、頁送り 10 回の最長 142 ms（scroll）・frame 140 ms（page）、目標 200 ms 以内。実機・Windows の QEMU はユーザー） | p015 |
| [ws079-p017](phase017/phase.md) | Notes に文字を打つ text box（IME 対応）。ws175-p008 の文字の段と一緒に実装（2026-10-06 ユーザー「Notes に text box を作る（別の Phase）」） | in-progress（2026-10-06 P2 が実装、build warning 0・host PASS、main へ統合。画面と IME は ws175-p010 の T1 の再実行待ち） | p005、ws175-p008 |
| [ws079-p009](phase009/phase.md) | 全文規約確認と回帰（必須の最終確認） | cleared（2026-09-29: WS079 の source と main の許した範囲の外の file は `style-check.py` の違反 0、host 8 本・描画の比較・4 platform の build・QEMU の PDF の demo と corner・三回 click、main が pen の image で p003・p012・p013-touch の guest 試験 PASS。input-inject・touchinject の規約は WS081 p002 へ移管。実機は未実施） | 全 Phase |

## ユーザーの判断（2026-09-29 に master から移した）

| 項目 | 決定 | 記録先 |
| --- | --- | --- |
| 手書きノートと PDF Viewer（2026-09-28） | ユーザーの指示（[WS079](ws079/ws.md) に原文）と回答: ペンは USB のペンタブレット、PDF Viewer は段階を切る（① Notes の PDF → ② 一般の図形・画像・TrueType → ③ CFF・Type1・暗号化）、名前は notes・pdfviewer・libpdf |

## 段の計画（2026-09-30 Q1、広く浅くの方針）

| 段 | 数値目標 | 測り方 | Phase |
| --- | --- | --- | --- |
| L1（済み） | QEMU で Notes の書き込み・PDF の注釈・PDF Viewer の頁送りと拡大が動く（p002〜p015） | 各 Phase の試験 | 済み |
| L2 | デモの台本 S8・S9 が 5330 の実機（mouse）と Windows の QEMU（touch）で通る。PDF の頁送り 1 回の描画 ≤ 200 ms（A4 の文書 10 頁） | 実機はユーザーの目視、時間は PDF Viewer の log | p016 |
| L3 | 実機のペン（外付けの touch LCD + AES pen、間に合えば）の筆圧・傾き・消しゴム | ユーザー | 完了の条件 2 |

### 進め方とハーネス（2026-09-30 Q1 の補足）

- **p016（L2）**: 新しい実装は無く、台本の S8（Notes の swipe・書く・Esc）と S9（PDF の頁送りと拡大）の通しの試験を作る。
  - Linux の QEMU: 注入の touch（`build/main-pen` の image、touchinject）で S8・S9 を自動で通し、画面を撮る script（`plan/ws079/tests/demo-s8-s9.sh`）。
  - 頁送りの時間: PDF Viewer の log の描画の時刻（無ければ 1 行足す）で、A4 の 10 頁の文書の頁送り 10 回の最大を出す。200 ms を超えたら、
    描画の cache（次の頁の先読み）を小さな Phase で足す。
  - 実機（mouse）と Windows の QEMU（touch）はユーザーが台本どおりに触る。エージェントは手順の 1 枚を用意する。
- **L3（実機のペン）** は外付けの touch LCD の計画次第。今は動かない。
