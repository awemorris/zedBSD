<!-- awesome-plan project=zedbsd record=ws102 -->

# WS102: スクリーンキーボード（compositor に直接、flick と QWERTY と手書き）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合の訂正: p001〜p009・p015〜p025 は cleared だが、表の p010・p011（L3 の計測と数値への直し）・p012（IME と組んだ日本語の変換、WS095 の続き＝ベータ3）・p013（Windows の QEMU の物理の touch、WS085）・p014（全文規約＝ベータ3）が planned のまま。sweep-beta2-rc2 §2.2 の「全部 cleared」は phase の directory だけを見た誤りで、WS の完了は保留）
Primary Milestone: MG006
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: L1 を満たした。L2 の p006・p007・p008・p009・p015・p016・p017・p018・p020・p021・p023 と L3 の p019 cleared 2026-09-30。p024 cleared 2026-09-30（全手順の回帰は未実施）。ユーザーの指示で優先を下げ P3 の担当は終了（2026-09-30）。再開はユーザーが言うとき（次は p022 の絵文字の面、BUG-125）
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-30 ユーザー）

「スクリーンキーボードを実装したいです。画面右下からスワイプで、画面右側にフリック入力パネル。画面左下からスワイプで、画面下側にQWERTY
キーボード、手書き入力。フリック入力は、アルファベット、記号、日本語。スクリーンキーボードはWaylandコンポジタに直接実装するのがいいと思います。
システムの一体感を重視します。手書き入力の認識処理はあとで実装して、スタブでいいです。」

## 達成基準（案、2026-09-30 main）

| # | 基準 | 確かめ方 |
| --- | --- | --- |
| K1 | 画面の右下の角から内へ swipe すると、画面の右側に flick の入力の panel が出る。もう一度の swipe（外へ）か閉じる key で消える | QEMU の注入の touch、画面 |
| K2 | flick の panel で、アルファベット（大文字・小文字）、数字と記号、日本語（かな）を flick で打てる。種類の切り替えの key がある | 注入の touch で打った文字が app（Text Editor・Terminal）に届く |
| K3 | 画面の左下の角から内へ swipe すると、画面の下側に QWERTY の keyboard が出る。shift・記号・数字・backspace・enter・矢印がある | 同上 |
| K4 | QWERTY の panel から手書きの入力の面に切り替えられる。書いた線を描き、認識は stub（「認識はまだ」と出すか、決まった候補を返す）でよい | 画面 |
| K5 | keyboard が出ている間、作業の領域を keyboard の無い範囲に縮める。最大化の窓は animation で縮み、浮いた窓は animation で収まる位置へ動く（はみ出た分はそのまま）（2026-09-30 ユーザー） | 画面（Text Editor の caret が見える） |
| K6 | 見た目は Kei のすりガラスと部品（system bar・menu）に揃い、compositor の一部として描く（別の process の app ではない） | 画面、ユーザーの目視 |
| K7 | 既存の角と端の gesture（左上の App Home、右上の Notes、下端の上への swipe）とぶつからない | 回帰の試験（WS099 の C9） |
| K8 | 文字の送り先は focus の app。日本語は IME（WS095）があれば IME を通して変換でき、無ければかなをそのまま送る | 注入の touch |

## 設計で決めること（p001）

- 文字の届け方: compositor の内部から focus の client へ key を合成する（`zwl_seat_key_deliver` の経路）か、text-input-v3 の commit で送るか。
  日本語の flick は、かなを IME（WS095 の engine）に渡して変換するか、かなをそのまま送るか。**WS095 は今、人間が作業中**（master）なので、
  IME の file（`ime.h`・`text-input.c`・`input-method.c`）と seat.c の IME の hook を変えずに済む形を先に考える。
- 角の gesture: `corner.c`（右上の Notes）・`home.c`（左上、下端の swipe）の既存の認識器と同じ型で、右下・左下の角を足す。下端の上への swipe
  （Wiseview・Home を閉じる）との区別。
- 描画: compositor の glass の panel と文字（`glass.c`・`panel.frag`）で描く。panel の大きさ（1280x800 と 1920x1080 の画面、5330 の内蔵 LCD は 1920x1080）。
- flick の配列: 日本語は 12 key のかな（あ段の key を中心に上下左右で い・う・え・お）、濁点・小書きの切り替え。英字は 12 key の abc 配列か。記号。
- 手書き: 線の記録と描画、認識の interface（後で認識の engine を差す）。
- K5 の作業の領域の縮め方（WS099 の「作業の領域」、BUG-114 の配置の規則との関係）。
- 試験: 注入の touch（`build/main-pen` の image と touchinject）で角の swipe と flick を打つ。
- Phase の分け方。

## Phase（案）

| Phase | 段 | 目的 | Status | 依存 |
| --- | --- | --- | --- | --- |
| ws102-p001 | — | 設計（[design.md](design.md): compositor の `keyboard.c`、IME の file を変えない送出、角の gesture、配列、作業の領域、段と数値目標） | cleared（2026-09-30） | — |
| [ws102-p002](phase002/phase.md) | L1 | `keyboard.c` の骨組み: 右下・左下の角の認識器（pointer と touch）、下端の Wiseview と desktop の swipe から角を除く、開閉、空の glass の panel、overlay と scanout、log | cleared（2026-09-30: osk-guest の pointer・edges・touch PASS（注入の swipe 10/10、真上 0/10）、WS079-p010・C9（10 本）・boot test PASS） | p001 |
| [ws102-p003](phase003/phase.md) | L1 | `keyboard-layout.c`（flick の 3 つの face の表、方向の判定、濁点の巡り）、key と花びらの描画、host の試験 | cleared（2026-09-30: host PASS（表の網羅: かな 46＋ー・英字 26・数字 10・記号 32・空白、重複 0、方向・濁点・大小）、guest の flick の手順 PASS（pointer と指）） | p002 |
| [ws102-p004](phase004/phase.md) | L1 | 文字の送出（evdev と Shift、text-input の commit、組み立て中・text-input の無い app）、guest の試験（Text Editor・ime-probe） | cleared（2026-09-30: Text Editor の file が「aiueO123」、ime-probe の text が「あいうえおかが」（濁点の置き換えを含む）、text-input の無い app へは送らない。host PASS） | p003 |
| [ws102-p005](phase005/phase.md) | L1 | L1 の仕上げ: 閉じる gesture と toggle、lock・Home・Wiseview で閉じる、K7 の回帰（C9・Notes の角・Wiseview）、1920x1080 | cleared（2026-09-30: 帯の swipe で閉じる・Home・Wiseview で閉じる、1920x1080 の配置、osk-guest 全手順・WS079-p010・C9（10 本）・boot test PASS。**L1 の (a)〜(d) を満たした**。lock は試験の compositor が session でないので未確認） | p004 |
| [ws102-p006](phase006/phase.md) | L2 | QWERTY の面と左下の gesture | cleared（2026-09-30: 注入の 30 文字「Hello, World! Kei 2026 (a+b)=c」を 4.9 秒で Text Editor に誤り 0（L2 の (a)）、Shift の lock・矢印、C9（10 本）・WS079-p010・boot test PASS） | p005 |
| [ws102-p007](phase007/phase.md) | L2 | 作業の領域（design.md §2.8、2026-09-30 ユーザーの方針: flick・QWERTY のどちらでも縮める、最大化の窓は animation で縮めて戻す、浮いた窓は大きさを変えず animation で収まる位置へ動かし、はみ出た分はそのまま） | cleared（2026-09-30: QWERTY で最大化の Text Editor に 1280x426 の configure、浮いた窓は 390,243→390,98、閉じて戻す、flick で 962x762、利用者が動かした窓は戻さない。shell.c は reserved を引く 2 か所だけ。全手順・p010・C9・boot test PASS。animation の間隔は L3） | p006 |
| [ws102-p015](phase015/phase.md) | L2 | keyboard の inset の知らせ（design.md §2.8、2026-09-30 ユーザー）: `keiland_keyboard_inset_v1`（zdesktop の inset.c、libwayland、libkeiland の wrapper（KEILAND_VERSION 18）、libkeiui の `kui_window` の受け口と callback と caret の中央寄せ（KUI_VERSION 7））。keyboard の開閉で知らせ、p007 は configure の前に `zwl_keyboard_inset_notify` を呼ぶ | cleared（2026-09-30 P6: Text Editor の 200 行の文書で QWERTY を開くと caret の行が keyboard の上の範囲の中央から 0.54 行、wlshm は変わらず、host 10/10、C9・WS079-p010・boot PASS。実機は未実施） | p007 |
| [ws102-p008](phase008/phase.md) | L2 | 手書きの面（線・stub の認識・候補） | cleared（2026-09-30: 手書きの面・線・stub の認識・候補の送出、guest PASS。(c): 点は入力の後の最初の frame で描かれる（9/9、lag 20〜53 ms）。QEMU の frame の間隔は 130〜143 ms で、60 Hz の 17 ms は実機か Windows の QEMU で測る） | p006 |
| [ws102-p009](phase009/phase.md) | L2 | `touch.c` の `ROUTE_OSK`（多指の連打） | cleared（2026-09-30、QEMU: 2 本指 50 ms の重なり 100 打鍵で取りこぼし 0（変更の前は 50）、L1・L2 の全ての手順・C9・WS079-p010・boot PASS） | p006 |
| [ws102-p010](phase010/phase.md) | L3 | 計測の道具と基準値（遅れ・開く動き） | in-progress（2026-10-08 P1 q913: 道具を実装・build、基準値を T1 へ） | p009 |
| [ws102-p011](phase011/phase.md) | L3 | 数値目標への直し | planned（p010 の基準値を待つ） | p010 |
| ws102-p012 | L3 | IME と組んだ日本語の変換、右の列の変換候補（独自の拡張、design.md §2.10） | planned | p005、D2、WS095 |
| ws102-p013 | L4 | Windows の上の QEMU（WS085）での物理の touch の確認 | planned | p009、WS085 |
| [ws102-p016](phase016/phase.md) | L2 | 右の列の道具の面（design.md §2.10、列の配置は p021 で済み）、編集の面（矢印・行頭行末・頁・BS を key で、選択の toggle は Shift）、直前の app の button。Text Editor で「選択 → → ×5 → コピー → 直前の app → 貼り付け」で同じ文字が別の app に入る（3/3） | cleared（2026-09-30: 道具の面（前の app・Del・tab・編集の面）、「選択 → → ×5 → コピー → 前の app → 貼り付け」を 3 回で b.txt が hellohellohello。p007 の直に替えた時の元の位置も直した。全手順・p010・boot PASS、C9 は p076 の画面の判定が不安定（単独 4/5）） | p007 |
| [ws102-p017](phase017/phase.md) | L2 | 編集の操作の拡張 `keiland_edit_v1`（libkeiland の wrapper（KEILAND_VERSION 19）・libkeiui（KUI_VERSION 8）、状態で button を灰色に）、拡張の無い窓の key への落とし（Terminal は app の表）、compositor の `zwl_edit_action`・`zwl_edit_state`・`zwl_focus_previous`（直前の app）。試験の口は Super+Alt の key。keyboard の button は p016 | cleared（2026-09-30 P6: Text Editor A で select_begin → → ×5 → copy → 直前の app → B で paste、B に "HELLO"、Terminal は Ctrl+Shift+C、状態の flag、host・WS079-p010・boot PASS、C9 は p076 の 1 回の不安定さの後 2 回 PASS。実機は未実施） | p015 |
| [ws102-p018](phase018/phase.md) | L3 | クリップボードの履歴（記憶の中だけ、10 件、secret の複写は残さない、lock・Log Out で消す）。`clipboard.c`、口は `zwl_clipboard_history_count`・`_get`・`_paste`、試験の口は Super+Alt+H・数字。一覧の UI は p016 | cleared（2026-09-30 P6: 新しい順・10 件・貼り付け・secret（`x-kde-passwordManagerHint`）の除外・lock と Log Out で消える、C9・WS079-p010・boot PASS。text-input の purpose による除外は IME の file の許可が無く残り、きっかけは IME の作業がエージェントに戻ったとき） | p017 |
| [ws102-p023](phase023/phase.md) | L2 | Text Editor が本当の編集の状態（選択がある・貼れる・取り消せる・やり直せる）を `kui_window_edit_state` で言う（p017 の灰色の表示を正しくする） | cleared（2026-09-30 P6: 開いた時 0x0 → 全選択 0x1 → 複写 0x3 → 打つ 0x6 → 取り消し 0xb、`edit-guest.sh` の期待値を本当の状態に更新して PASS、inset・host-core PASS） | p017 |
| [ws102-p019](phase019/phase.md) | L3 | 色付きの絵文字 その 1（2026-09-30 ユーザー「色付きにする」）: font の選定（Noto Color Emoji の CBDT か COLRv1）と license の監査、userland/desktop/fonts への追加、libtruetype の色の glyph、libkeiui と compositor の文字の描画の fallback で色の絵文字を描く。Text Editor に貼った絵文字が色で出る | cleared（2026-09-30、QEMU と host: CBDT の Noto Color Emoji（OFL-1.1、取得と検証の package）、libtruetype の `truetype_color_glyph`、libkeiui（KUI_VERSION 9）・compositor・Text Editor の fallback で色の絵文字、C7・C9・boot PASS、rootfs +10.7 MB） | p016 の前でよい（keyboard に依らない） |
| [ws102-p024](phase024/phase.md) | L2 | 右の列の道具の面の「履歴」の tab（p018 の `zwl_clipboard_history_*` を一覧にし、tap で貼る。2 段の操作） | cleared（2026-09-30 QEMU: 3 つの app で複写 → 履歴の 2 番目を tap で receive.txt が bravo。全手順の回帰・C9・boot はユーザーの指示のラップアップで未実施。優先を下げ、再開はユーザーが言うとき） | p016・p018 |
| [ws102-p022](phase022/phase.md) | L3 | 色付きの絵文字 その 2: keyboard の絵文字の面（種類の tab と格子、tap で text-input の commit、text-input の無い app へは送らない） | cleared（2026-10-05 Q1: T1-175 の FAIL（2 つ目以降の絵文字が空、glass_cache_glyph の欠陥と試験の grep）を e2e76062 で直し、T1-175b で osk-guest PASS、20 個が全部描かれることを目視）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（2026-10-05 P1 q736: 実装・host 試験（host-keyboard・host-emoji PASS）・build …） | p019・p016 |
| [ws102-p021](phase021/phase.md) | L2 | 縁に組み込んだ見た目（design.md §2.3 の改め、2026-09-30 ユーザー）: flick の panel を右の列の全体に、QWERTY・手書きを下端の全幅に、余白・外の角丸・影をやめ内側に 1 px の区切り、縁から伸び出す動き。1280x800・1920x1080 で panel の外の辺が画面の縁と 0 px で接する（log の矩形と画面） | cleared（2026-09-30: flick は右の列の全体（1280x800 で 962,34 318×766）、QWERTY・手書きは下端の全幅（0,496 1280×304）、1920x1080 も縁に 0 px。影・外の角丸をやめ内側に 1 px の線、200 ms の伸び出し。全手順・p010・C9・boot test PASS） | p008 |
| [ws102-p020](phase020/phase.md) | L2 | QWERTY の面の補助の key の列（Esc・Tab・Ctrl・`|`・`~`・矢印、一度だけ効く修飾） | cleared（2026-09-30: 補助の列 Esc・Tab・Ctrl・Alt・\|・~・/・-・Home・End・PgUp・PgDn、Ctrl・Alt は次の 1 key だけ。Text Editor で abc\|<tab>、Ctrl+A → z で z。全手順・p010・C9・boot test PASS） | p006 |
| ws102-p014 | — | 全文の規約と回帰（WS の最後） | planned | 最後の段 |

## 段（詳しくは [design.md](design.md) §3）

| 段 | 内容 | 数値目標（要約） |
| --- | --- | --- |
| L1 | 右下の swipe で flick の panel。かな・英字・数字の face。英数は Text Editor、かなは text-input の app へ | 注入の右下の swipe 10/10 で開き、真上への swipe 0/10。表の網羅（host）。「aiueo123」「あいうえお」が誤り 0 で届く。C9 の 10 本 PASS |
| L2 | QWERTY・手書き（stub）・作業の領域・多指の連打 | 30 文字を 5 文字/秒で誤り 0。2 本指の 100 打鍵で取りこぼし 0。線の遅れ 1 frame 以内。最大化の窓が keyboard の上に収まる |
| L3 | 速さと日本語の変換 | 送出まで p95 ≤ 5 ms、app の frame まで p95 ≤ 50 ms。開く動きの frame の間隔 ≤ 20 ms。D2 の後、「きょうはいいてんき」→「今日はいい天気」 |
| L4 | Windows の上の QEMU で物理の touch | 台本 S14 の操作が 3/3 通る |

## 判断待ち（design.md §5）

- D1: Text Editor・Terminal は text-input-v3 を持たないので、かなを送れない。WS095-p007 を待つか、WS102 で Text Editor に text-input-v3 を入れてよいか（ユーザー）。
- D2: 日本語の変換を IME とどう組むか（L3、WS095 の担当と）。

## ユーザーの判断（2026-09-30）

- **D1**: 「libkeiui に入れる」。text-input-v3 の受け口を libkeiui の窓の土台（`kui_window`）に入れ、Text Editor と今後の libkeiui の app がかなを受け取れるようにする（WS090 の Phase。IME の file は変えない）。
- **D2**: 「IME にかなの口を足す」。IME に「かなを入力として受け取る」口を足し、keyboard はかなをそのまま渡す。IME の変更なので、人間の作業（WS095）と調整してから（L3）。

### 進め方とハーネス（2026-09-30 Q1 の補足）

- **L1 の作業像**: まず panel が出る・消える（p002）→ flick の表と face（p003）→ 送出（p004）→ 回帰（p005）の順に、毎 Phase で「画面に出て、
  何かが届く」ところまで動かす。見た目の細部は L1 では後回し（すりガラスと文字の大きさは既存の system bar の部品を流用）。
- **ハーネス**: Linux の QEMU の注入の touch（`build/main-pen` の image と touchinject）で swipe と flick を打つ script（`plan/ws102/tests/kbd-guest.sh`）。
  判定は 2 つ: (1) compositor の log の `ZWL KEYBOARD` の行（開く・閉じる・送った文字）、(2) 受け取った側の記録（英数は Text Editor の保存の file、
  かなは ime-probe の log）。画面は各 Phase で撮る。
- **L4 の作業像**: Windows の QEMU（WS085 の boot.bat と usb-multitouch）で、ユーザーが台本 S14 を 3 回通す。エージェントは手順の 1 枚を用意する。
  WS081 の L2（物理の touch が届くことの確認）が先に済んでいる必要がある。
- **危険の早めの確かめ**: 下の角と Wiseview・desktop の swipe の衝突（D3）は p002 の回帰で先に見る。ここで問題が出ると角の位置の設計を変えるため。

### Q1 の判断（2026-09-30、p008 の L2 の (c)）

- QEMU の guest は 7 枚/秒前後しか描けないので、線の遅れの 17 ms（60 Hz の 1 frame）は QEMU では測れない。L2 の (c) は QEMU では
  「どの点も入力の後の最初の frame で描かれる」（p008: 9 frame 中 9）で満たしたとし、17 ms の数値は L3 の p010 で 5330 の実機か Windows の QEMU で
  同じ log（`hand frame lag_ms`）を使って測る。WS094 の判断（QEMU の値は参考、合否は実機）と同じ扱い。


## 2026-10-06 UAT のフィードバック

- BUG-229 App Home の開閉で OSK が消える、BUG-230 引き出しを Notes と同じ扇形＋文字に、BUG-231 full keyboard を IME に通す（a → あ、漢字の変換）→ **新しい Phase**

## Phase（2026-10-06 追加: 再設計）

- [ws102-p025](phase025/phase.md) 設計と実装: OSK の残り（App Home で消える・引き出し・full keyboard の IME）（test-wait、q789 で実装 4b956b9d）
