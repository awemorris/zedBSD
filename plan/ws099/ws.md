<!-- awesome-plan project=zedbsd record=ws099 -->

# WS099: Keiland の compositor（zdesktop）のデモの基準

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p019（T1-220）は cleared（表を直した）、p023（q609 で P2 が cleared を提案）・p034（p034b cleared）・p035（b〜d、T1-229・231）・p038（T1-264 の PNG、T1-277 PASS）は Q1 の判定待ち。残り: p012（5330 の目視、ユーザー）、p022 の回帰（criteria.sh の C9 の一覧に消えた p072 が残っている、T1-401）、p006（C6、記録だけ））
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1、2026-10-17）
Queue: なし（q589 で p017 は uncleared で終端）
Resume point（2026-10-02 ベータ1の計画）: **最初は p020（BUG-125 の原因の特定と compositor の直し、blocking）**。並行して p019（湖の壁紙の既定、build の file だけ）と p012（5330 の目視、ユーザーの時間）。p020 の後に p021（C2 の geometry と BUG-127）、最後に p022（全文規約と回帰）。下の「ベータ1 の到達目標」。
2026-10-02 user: WS099 の優先度を上げる。**BUG-125 は WS099 の blocking**、WS099 を担当するエージェントが直す。C1 などの実機の目視確認はユーザーに依頼する（Q1 が声をかける）。
2026-10-02 user:「窓が多いときの遅延の基準（50 ms）はそのままでいいです。理由があれば見直していいです。BUG-127 が再現しないとき、tracking のままで WS099 を閉じてよいです。」
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-30 ユーザー）

WS035 を閉じた後継。ユーザー:「WS099のゴールも、明確な達成基準がないような気がします。それはFGに入れて、WSでは、このソフトがこういう基準を
満たす、という明確なゴールを設定したいです。ソフトごとにそれをWSで作りましょう」→ デモの台本は fg010 に移した（master）。この WS の対象は
**compositor の zdesktop**（`/bin/wayland`、`userland/desktop/wayland/`）と、それが起こす greeter・session の遷移だけ。

## 達成基準（2026-09-30 main の案、同日朝ユーザーが「案のまま確定」）

全て QEMU の Venus の自動の試験で確かめ、印の付いたものは 5330 の実機でも確かめる（実機）。

| # | 基準 | 確かめ方 |
| --- | --- | --- |
| C1 | 起動から greeter、login からデスクトップ、Log Out から greeter、Shut Down の各遷移で、黒い画面と文字の console が 0 枚（実機） | p126 の替わり目の試験、実機はユーザーの目視 |
| C2 | 窓の移動、四隅と四辺の resize、最大化と戻し、最小化と戻しが、どれも意図した位置と大きさになる | 自動の試験で位置と大きさを log と画面で比べる |
| C3 | 全画面・最大化を解いた窓は、title bar が system bar に重ならず、drag できる（BUG-114 の形） | 自動の試験（Notes の swipe → Esc を含む） |
| C4 | 窓を閉じたとき、同じ desktop の次の窓に keyboard の focus が移る。desktop の層には移らない | p137 の試験 |
| C5 | App Home と Wiseview の開閉が、窓 10 個でも途中で止まらない。開閉の最初の frame まで 100 ms 以内（実機） | 計測の log、実機は WS075 の計測 |
| C6 | 窓 10 個で、pointer の移動から表示まで中央値 50 ms 以内（実機）。compositor の GPU は WS075 の p023 が担う | WS075 の measure-apps.sh |
| C7 | すりガラスの上の文字の contrast が、既定と生成の 5 枚の壁紙の全てで 4.5:1 以上（WCAG AA） | 撮った画面の文字と背景の画素から計算する試験 |
| C8 | Terminal の窓は本体の四隅が直角、title bar は丸い。他の窓は両方丸い | p134 の試験 |
| C9 | zdesktop の回帰の試験（`plan/ws035/tests/zdesktop-p*.sh` のうち基準の一覧に載せたもの）が全て PASS（[BUG-115](../bugs/BUG-115.md) の p072 を含む） | 一括の回帰の script |
| C10 | 1 時間の連続の操作（窓の開閉を繰り返す試験）で zdesktop が落ちず、`ZWL ERROR` が 0 | 長時間の自動の試験 |

基準に無いものはこの WS で作らない（見つけたら Future Work か Bug Board）。期限は 2026-10-10 ごろ。

## 既知の項目

| 項目 | 基準 |
| --- | --- |
| 暗い壁紙の上のすりガラスの文字（旧 ws035-p135） | C7 |
| [BUG-115](../bugs/BUG-115.md): p072 の試験の失敗 | C9 |
| BUG-114（直った）・BUG-113（試験の誤り）の試験を回帰に入れる | C3・C4 |

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [ws099-p001](phase001/phase.md) | C1〜C10 を確かめる一括の試験（`plan/ws099/tests/criteria.sh`、新しい C2・C5・C7・C10）、今の状態での実行と不足の一覧、BUG-115 の切り分け | cleared（2026-09-30、QEMU の Venus: C1〜C4・C7〜C10 PASS（C1・C7 は範囲が一部）、C5 FAIL（最初の frame まで 102〜215 ms）、C6 未実施（実機）。BUG-115 は試験の固定の待ち（古い image の起動 6 秒超）） | — |
| [ws099-p002](phase002/phase.md) | C5: App Home と Wiseview の開閉の最初の frame を早める。開閉の要求で次の frame を frame pacing の待ち無しに描き、`ZWL FIRST_FRAME` を log に出す。アプリの一覧は先に読み、`vkResetCommandBuffer` は呼ばない。試験 `c5-hw.sh` | cleared（2026-09-30 P6: 5330 で 34 回とも 100 ms 以内、最大 55 ms。QEMU は 102〜105 → 90〜95 ms（App Home の最初の開きだけ 201 ms、QEMU だけ）。C3・C9・WS079-p010・boot PASS） | p001 |
| [ws099-p003](phase003/phase.md) | BUG-115: `plan/ws035/tests/` の固定の `sleep N`（42 本・43 箇所）を、compositor の log の `ZWL READY` を待つ形に（試験の側の直し） | cleared（2026-09-30: 古い image で p072 PASS（前は FAIL）、C9 の 9 本 PASS） | p001 |
| [ws099-p004](phase004/phase.md) | C1 の残り: 起動と Shut Down の替わり目を両方の画面（VGA と Venus）で撮る試験（`c1-watch.py`・`c1-boot-shutdown.sh`）、criteria.sh の C1 に入れ、p126 に `--no-black` | cleared（2026-09-30: 起動・Shut Down とも黒 0・文字 0、Shut Down の後は greeter の絵のまま機械が止まる） | p001 |
| [ws099-p005](phase005/phase.md) | C7 の残り: client が描くガラスの上の文字の contrast を測る（c7-contrast.sh に Settings と Files）。compositor のガラスに明るさの下限（panel.frag）、Settings・Files・libkeiui の副次の文字の色 `0x6b7585` → `0x56606f`（main の許可） | cleared（2026-09-30: C7 72/72 が 4.5 以上、最小 4.68（前 1.91）） | p001 |
| [ws099-p007](phase007/phase.md) | BUG-118: client の cursor（隠す・surface・shape）をその client の窓の本体の上だけに、他は矢印（cursor.c・compose.c）。試験 cursor-owner.sh を C9 に | cleared（2026-09-30: 変更前 FAIL・変更後 PASS、C8・C9 PASS） | p001 |
| [ws099-p008](phase008/phase.md) | BUG-119: Shut Down で電源が切れない。QEMU の monitor で確認（VM running、CPU は全て HLT）、原因（init が poweroff を HALT にし、PC の kernel に ACPI の S5 が無い）と直し方の案（UAPI・system-device・platform・ACPI の driver・init、HAL の API は変えない）、greeter の「Shutting down…」の案 | cleared（2026-09-30、確認と案まで。実装は別に割り当て） | p004 |
| [ws099-p009](phase009/phase.md) | greeter の Shut Down・Restart で「Shutting down...」「Restarting...」と spinner を出し、その絵の後に POWER を送る。c1-boot-shutdown.sh に最後の絵の判定と QEMU の終わりの判定（今は WARN） | cleared（2026-09-30: 変更前 FAIL・変更後 PASS、C9 PASS（p076 は 1 回の不安定さの後 2 回 PASS）） | p008 |
| ws099-p006（案） | C6: 実機（5330）の pointer の遅延の計測（WS075 の measure-apps.sh）。WS075 の p023 と合わせる。ベータ1 では記録だけ（2026-09-30 午後 user の速さの優先の引き下げ） | planning（要る判断: C6 50 ms を 10/10 に保つか見直すか、user） | p001・WS075 |
| [ws099-p010](phase010/phase.md) | BUG-122: compositor が落ちた後に greeter が 3 回の失敗で文字の console に落ちる。sessiond の起こし直しを延ばす（6 回、待ちを 1〜16 秒に伸ばす）、失敗の理由を sessiond の log に。試験 `bug122-recovery.sh` | cleared（2026-09-30: `bug122-recovery.sh` 直しの前 FAIL・後 PASS、C1 PASS、boot PASS。5330 は未確認） | p001 |
| [ws099-p011](phase011/phase.md) | BUG-121: 窓の角の drag で窓が消える。試験 `resize-stress.sh`（QEMU、角の drag 100 回）・`resize-hw.sh`（5330 の passthrough、20 回）。原因は上の窓の frame の帯の press を下の窓の title bar の control が取って下の窓を前に出したこと（`titlebar-shell.c`）と、端の帯が frame を覆う置き場所（`shell.c`）。client を外す log `ZWL CLIENT gone` | cleared（2026-09-30: 5330 直しの前 20 回中 6 回消失 → 後 0、QEMU Model viewer・wlshm 各 100 回 0、C2・C9 PASS、boot PASS。QEMU の Venus の swapchain の失敗は別件） | p010 |
| [ws099-p015](phase015/phase.md) | 全画面を常に合成する（全画面の直の scanout（`display.c` の fullscreen mode）を消す）、全画面の窓を下の端から上への swipe で窓に戻す（pointer と touch。全画面の間は下の端の swipe を Wiseview より先に全画面の解除に使う。下の左右の角は WS102 の keyboard のまま）。C3 の試験に swipe の解除を足す | cleared（2026-09-30: P6 の実装と `c3-swipe-back.sh` PASS、demo-s8-s9 PASS（page の frame 最長 139→135 ms）、pen の遅れの中央値 74→117 ms・frame の間隔 107→113 ms（QEMU、1 frame の合成の費用の分）。P6 の uncleared の理由だった他の WS の試験 3 本（p010・p052・p053、消した直の scanout を前提）は、Q1 が P6 の差分を当てた。P6 が同じ差分の複写で最後の image で PASS を確かめている。5330 は未実施） | p010 |
| [ws099-p016](phase016/phase.md) | zdesktop の buffer の import の短縮（ws094-p009 の発見: 1 枚約 210 ms、うち layout の変更の submit と `vkQueueWaitIdle` が 100 ms。次の合成の command buffer の barrier にまとめる）。app の起動から最初の frame まで（C5 と WS094 の (a')）を前後で測る。QEMU と 5330 | cleared（2026-09-30、QEMU: WS094 (a') 2977 → 2652 ms、App Home → Files の最初の frame 2655 → 2407 ms、Model viewer 4439 → 4182 ms。C9・WS079-p010・boot PASS。5330 は lock が使用中で未実施） | —（p015 と file を分ける: import.c・compose.c の周り。display.c・shell.c・seat.c・backdrop.c は他の Phase が作業中） |
| [ws099-p012](phase012/phase.md) | C1 の 5330 の目視（ユーザー）と、5330 での BUG-119（電源断）・BUG-122 の確かめ。B4 | planned（agent 1h + ユーザー 15 分。WS094 p012 などと同じ回にまとめられる） | p004・p009・p010、ws073-p043、ユーザーの時間 |
| [ws099-p014](phase014/phase.md) | C10 i915 passthroughの60分連続操作。試験script/短時間試走とsoak | cleared（q578、i915 passthrough: 3602秒/278周、errors0/restarts0、main最終確認） | p001/p002/WS075 hdmi-h4の実出力 |
| [ws099-p017](phase017/phase.md) | BUG-125のmove/resize再現と試験同期の切り分け。実compositor defectは別Phaseへ | cleared（2026-10-03 Q1、p020・p024 の証拠） | p003/p007のC9実出力 |
| [ws099-p019](phase019/phase.md) | ユーザー追加: 白樺・湖と発見された抽象版を共通source/3 OS release dataへ。ぼやけた湖を起動default、既存背景の選択を維持 | cleared（2026-10-06 Q1 判定: T1-220 PASS（QEMU、壁紙 7 件・既定 Birch-Lake・Settings で切り替えと既定への戻り）。抽象版は既存の緑の壁紙とユーザーが確認。実機は UAT）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: planned（2026-10-02 ベータ1の計画で再開可に。user の決定は済み、exact scope は [再開資料](../ws094/phase0…） | 旧p061資産・q588安全な終端 |
| [ws099-p020](phase020/phase.md) | **BUG-125（blocking）**: p076 の popup の 2 症状と left resize の settle の原因を guest で弁別し、compositor（と必要なら試験の同期）を直す。B1 | cleared（2026-10-03 Q1、BUG-125 resolved） | p017 の資産（q589 の helper）、Venus の renderer |
| [ws099-p021](phase021/phase.md) | C2 の geometry（q538: top-right の増分 20、bottom-left/left の settle）と BUG-127（最小化の直後に窓が残る）。B2 | cleared（2026-10-06 ユーザー: 再現せず非阻害で clear） | p020 cleared（同じ shell.c の周り） |
| [ws099-p022](phase022/phase.md) | WS099 の全文規約と回帰（WS の最後）。B5 | planning（最後。p019〜p021 の後） | p019・p020・p021、p012 |
| [ws099-p025](phase025/phase.md) | BUG-146: Venus の guest 試験が app の client の番号を IME の有無に関わらず求める（共有の helper `plan/tools/guest/zwl-clients.sh`、81 本の試験の `client=N` を置き換え） | cleared（q645、T1-039 PASS、97c019b5b の image） | — |
| [ws099-p026](phase026/phase.md) | BUG-147 の残り: cursor-owner の guest の消失は、criteria.sh の start_guest が guest の起動の失敗を確かめずに次の試験を流したもの（記録の読み）。start の出力を残し、SSH を確かめ、1 回起こし直す、失敗は INFRA と記録 | cleared（q645、T2-002 PASS） | p024 |
| [ws099-p027](phase027/phase.md) | BUG-095: oneshot の service の `/sbin/poweroff` と init の待ち合い。init が oneshot を待つ間も control socket に答え（system の action・読むだけの要求はすぐ、起こす・止めるは loop で）、action で待ちを終える。試験 `tests/bug095/` | cleared（q645、T2-003 PASS、実機は S2） | — |
| [ws099-p029](phase029/phase.md) | BUG-142（入力 device を時刻の順に merge して読む、`input.c`・`main.c`）と BUG-141（zdesktop が motion を取ったら client に leave、`seat.c`）。試験 `bug142-order.sh`・`bug141-hover.sh` | cleared（q645、T2-008 の再試験 PASS） | — |
| [ws099-p028](phase028/phase.md) | zdesktop の log に NUL が入る（`>` の log を次の run が切り詰めた後の古い offset への write）。stdout・stderr に O_APPEND | cleared（2026-10-03 Q1、T1-035） | — |
| [ws099-p030](phase030/phase.md) | タイトルバーの検索欄・menu の項目のドラッグで窓を動かす（閾値 mouse 2 px・touch 8 px、離してクリック、フォーカス中の欄はキャレットと選択）。2026-10-04 user | cleared（q670、T2-026） | — |
| [ws099-p031](phase031/phase.md) | 上部の system bar の高さを窓の title bar に揃える（34 → 44 px）（2026-10-04 ユーザー） | cleared（2026-10-05 Q1） | WS142・BUG-180 と順を合わせる |
| [ws099-p032](phase032/phase.md) | system bar の WiFi の icon の Alt+クリックで IP address と統計の情報の popup（2026-10-04 ユーザーの要望） | cleared（2026-10-05 Q1） | ws089-p022 と同じ network の情報の口 |
| [ws099-p034](phase034/phase.md) | 上部の bar のデザインの調整（黒い地、左の app の icon・中央の仮想 desktop の点・右の状態の icon・時刻を pill にまとめる、2026-10-06 ユーザーの画像 4 枚） | p034b は cleared（2026-10-06 Q1 判定、T1-228）。phase.md の頭の Status は planned のまま（p034 の第 1 版は p034b に置き換わった）→ Q1 の判定待ち（2026-10-08 q910 P2 の照合。旧: planning） | |
| ws099-p033 | 最大化の中で新しく起動した app の窓を最大化で開く（2026-10-05 午後 UAT、ユーザー「ウィンドウを最大化している状態で、新たにアプリを起動したら、そのアプリは最大化しているのがいいです。タブレットをスクリーン全体で使っているという認識にします。」）。今の desktop で前面の窓が最大化（docked）なら、新しい toplevel の最初の configure を最大化にする。dialog・popup・大きさの固定の窓・全画面は除く。決める点: 「最大化の状態」の判定（前面の窓か、その desktop のどれかか）と、最大化を外した後に開く窓の扱い | planning（P2、q の番号は開始時） |

## ベータ1 の到達目標（2026-10-02 計画、fg019）

期限の目安: 実装は 2026-10-10 ごろまで、その後は bug の修正と実機の調整だけ（凍結日は未確定、master の fg019）。C1〜C10 の基準は変えない。

| # | 受け入れ（ベータ1） | 測り方 | Phase |
| --- | --- | --- | --- |
| B1 | BUG-125 が resolved: 同じ image で `zdesktop-p076.sh` の単独 20 回と `criteria.sh … C9` の 5 回で FAIL 0。原因を log・画面の証拠で示し、試験の待ちで隠していない | QEMU の Venus、p017 の資産（`p076-framed.sh`・`tests/p017-popup-low-overhead.py`） | p020 |
| B2 | C2 が PASS（q538 の top-right の増分・left の settle の FAIL が 0）、BUG-127（最小化の直後の画像）が resolved か、原因つきの tracking でユーザーが非阻害を決めた | `criteria.sh … C2 C9` を 3 回 | p021 |
| B3 | ベータ1 の既定の壁紙がぼかした湖、既存の背景を Settings で選べ、保存済みの設定が優先（3 OS の data） | p019 の受け入れ | p019 |
| B4 | 5330（USB の素の起動）で C1 の黒・文字の画面 0 枚、Shut Down から電源断 10 秒以内、`SESSIOND GREETER failed` 0 | ユーザーの目視と SSH の log | p012 |
| B5 | `criteria.sh` の C1〜C5・C7〜C10 が同じ最終の image で全て PASS、変えた source の全文規約、boot test | QEMU の Venus | p022 |
| — | C6（50 ms）は記録だけ（p006）。2026-09-30 の決定「10/10 に見直す」の時点でユーザーに判断を求める | WS075 の `c6.py` | p006 |

### source の衝突（他の WS との並列の注意）

- p020・p021 は `userland/desktop/wayland/` の shell.c・popup.c・menu.c・toplevel.c・seat.c の周りを変える。同じ directory を変える WS094 p014（display.c・menu.c・desktop.c）、
  WS113 p004・p005・p007、WS102 p022（keyboard.c）、WS114・WS095 の compositor の Phase と同時に流さない（file が別なら Q1 の判断で並列可、`compose.c`・`shell.c` を触る組は直列）。
- p019 は `userland/desktop/wallpapers/`・`keiland-linux.mk`・`keiland-freebsd.mk`・root の Makefile の data の行・compositor の既定の壁紙の 1 行（backdrop.c か preferences.c）。p020 とは file が別で並列可。
  root の Makefile は WS112・WS129 と重なりうる。
- p012 と WS094 p012・WS089 の 5330 の確かめ・WS079 の S8/S9・WS100 の A7 は、同じ demo の image の 1 回のユーザーの時間にまとめられる（Q1 が声をかける）。

## 段の計画（2026-09-30 main 経由のユーザーの方針「広く浅く」: まず動く段をそろえ、磨き込みは段ごとの数値目標の小さな Phase）

| 段 | 内容 | Phase | 数値目標 | 測り方 |
| --- | --- | --- | --- | --- |
| **L1（まず動く、QEMU）** | 基準の QEMU で測れる分と、落ちた後の回復 | p001・p003・p004・p005・p007・p009（cleared）、**p010（BUG-122）** | `criteria.sh` の C1〜C4・C7〜C10 が全て PASS。compositor（session・greeter）が 5 回続けて落ちても graphical の login に戻り、console に落ちない | `plan/ws099/tests/criteria.sh`、`bug122-recovery.sh`（QEMU の Venus） |
| L2（実機でそろう） | 実機の不具合と C5 | p011: BUG-121（窓の角の drag で窓が消える）の切り分け（QEMU の resize の stress 100 回、実機の passthrough 20 回）と直し<br>p002: C5 の実機の計測と短縮<br>p012: C1 の実機の目視<br>p013: BUG-119 の電源断の実装（p008 の案）→ ws073-p043 で済んだので不要（2026-10-02 計画で確認、5330 の確かめは p012） | BUG-121: resize 100 回で窓の消失 0。C5: App Home・Wiseview の開閉の最初の frame まで 100 ms 以内（窓 10 個、5330）。C1: 黒・文字の画面 0 枚（5330）。BUG-119: Shut Down から電源断まで 10 秒以内（QEMU の終了、5330） | stress の script、WS075 の計測、ユーザーの目視、QEMU の終了の時刻 |
| L3（磨き込み、実機） | 実機の応答と長時間 | p006: C6 の実機（WS075 の p023 と）<br>p014: C10 の実機の 1 時間 | C6: 窓 10 個で pointer の移動から表示まで中央値 50 ms 以内（5330）。C10: 実機で 1 時間、zdesktop が落ちず `ZWL ERROR` 0 | WS075 の `measure-apps.sh`、`c10-soak.sh` を実機で |
| L4（後） | 基準の外（Future Work） | — | — | — |

- L2 以降は実機とユーザーの時間が要る。各 Phase は前の段がそろってから Queue に入れる。
- BUG-122 の 5330 での元の失敗（`ZWL EXIT error=21`、EOPNOTSUPP）の原因は QEMU では再現できない（i915 の GPU の状態）。p010 は起こし直しを延ばして
  理由を log に残す。5330 で再び起きたら、sessiond の log の `SESSIOND GREETER failed reason=` を読んで L2 で直す。

### ユーザーの判断（2026-09-30 昼、Notes の全画面の出口）

- 「画面を下からSwipeで戻しましょう。また、全画面でコンポジット無効のモードになっているなら、それは使わないように修正して、コンポジットを有効にした上で、
  スワイプ操作を可能にします。」→ ws099-p015。今の zdesktop は全画面の窓の最新の image を直に scanout する（`display.c` の fullscreen mode、
  WS035 の D0）。これを止め、全画面の窓も window mode で合成する。台本 S8 は swipe で窓に戻す（Esc も残す）。
- 性能の注意: 直の scanout を止めると、全画面の app でも合成の費用が掛かる。p015 で全画面の Notes の pen の線の遅れと frame の間隔を前後で測り、
  WS079 の L2 の値（頁送り 142 ms）を悪くしないことを確かめる。

### 進め方とハーネス（2026-09-30 Q1 の補足）

- **L2 の作業像**: 実機の分は、P1（WS075）の実機の passthrough の harness（`plan/ws075/tests/hdmi-h4-hw.sh`、`h4-ctl.py`、`measure-apps.sh`）を
  使い回す。C5 は `h4-ctl.py` に「App Home を開く要求から最初の flip まで」を測る命令を足し、WS075 の lock の下で測る。
- **BUG-121**: まず QEMU で resize の stress（`plan/ws099/tests/` に 100 回の角の drag）を作り、Model viewer（Vulkan の app）と wl_shm の app の
  両方で消えるかを見る。QEMU で出なければ passthrough で 20 回。消えたときに compositor と app のどちらの log が先に途切れたかで切り分ける。
- **C1 の実機**は、ユーザーに demo の image で起動・login・Log Out・Shut Down を 1 回通してもらう（BUG-119 の電源断もここで確かめる）。
- **C6**は WS075 の段（100 → 75 → 50 ms）に従う。WS099 では測った値の記録だけ。

2026-10-02 / n3-start: userのN=3継続指示でp017を具体化、p014をP9へ割当。共有source変更は重ねず、BUG-125はP8、C10試験はP9。acceptanceは従来どおり。GitHub publication保留。

2026-10-02 / ws099-p014-cleared-q578: [p014](phase014/phase.md)のC10 L3を5330 i915 passthroughで確認、3602秒/278周/errors0/restarts0。[結果](phase014/q578-result.md)。USB実機では未検証。WS099自体はincomplete、C6や他の必須条件は継続。P9モデル利用上限後にmainが証拠/cleanupを回収。GitHub progress/closure未公開。

2026-10-02 / q577-wrap-uncleared: [p017 result](phase017/q577-result.md)を回収。whole p076は16完了中15 PASS/1 FAIL、完全C9は1回、補足は途中5 PASS/1 FAIL。BUG-125をtrackingのまま維持し、owned runtimeを停止。B3が再開候補を所有するが、新しい有限Queue承認までは実行しない。

2026-10-02 / b3-q583-terminal-diagnosis: [p017部分結果](phase017/q583-result.md)をB mainが統合。original5/handshake5で残2症状非再現、計測overhead/非並列の限界を保存。whole p017とWS acceptanceは未達、BUG-125 tracking。部分itemのclearanceは全Phase clearanceではない。次Queueは新しい弁別条件を選定後。

2026-10-02 / user-common-wallpapers-20261002: userがテスト背景のsource/zedBSD・Linux・FreeBSD共通収録をB2へ追加指示。[p019](phase019/phase.md)をWS035後継として追加しasset goalをscopeへ加える。C1〜C10と実機/最終conformance条件は保持、asset追加だけでWS acceptanceを満たしたとしない。q588後に有限Queueで実行、現時点は読取調査。

2026-10-02 / user-lake-default-20261002: userが旧画像のぼやけた湖を起動defaultとし、収録済み背景を切り替えで維持するよう追加決定。[p019](phase019/phase.md)のcriteriaへ反映、保存済み設定の優先も維持する。予約済みq593のsnapshot準備に含め、同じB2へ受領を確認。既存C1〜C10/実機/最終conformance条件とq588のscopeは維持。

2026-10-02 / b3-q589-user-wrap-20261002: [p017 q589終端結果](phase017/q589-result.md)を回収、準備/hostチェックのみでguest実測未実行のためattemptとwhole p017はuncleared。user指示でB3終了、BUG-125 tracking/WS incomplete維持。証拠と再開手順は保存済み。p019は最新user全agent終了指示により未着手で再開待ち。

2026-10-02 / ws099-beta1-plan: fg019 の計画エージェントがベータ1 の到達目標 B1〜B5 と p020（BUG-125 の直し、blocking）・p021（C2・BUG-127）・p022（全文規約と回帰）を追加。p019 は既存の user の決定と q593 の再開資料で planned に。p013 は ws073-p043 で不要と注記。C1〜C10 と既存の Phase の結果は不変。Queue は未投入。

2026-10-03 / p017・p020: Q1 判定で cleared、BUG-125 resolved（user「Q1の判断で閉じられるものは閉じてください。」）。p021（C2 ×3）は T1-005 の結果で判定する。

2026-10-03 / C5: 2026-10-03 user「C5の200msは問題視しません。clearでOKです。理由は、あとでパフォーマンス改善のチケットを作ってまとめて改善するからです。」 → T1-006 の C5（QEMU、1 回目だけ first 201〜202 ms・gap 243 ms、2・3 回目は 92〜95 ms）を WS099 の受け入れでは PASS と扱う。速さは F-072 でまとめて改善する。

## 2026-10-04 UAT の結果（Q1）

タッチパッドで title bar（検索欄・メニューの項目）を押して動かしても窓が動かない（p030 の後のデグレ、[BUG-166](../bugs/BUG-166.md)）。タップ→別の指で移動・2 回タップ→移動はできた。押し込みで掴むかは仕様の検討漏れ（ユーザー）。**仕様をユーザーと決めてから**直す Phase を立てる。押し込みのクリックが効かない [BUG-167](../bugs/BUG-167.md) と関係する。

- 2026-10-05 WS138 の結果（Q1）: p019 の背景は PNG。c7 の測る箱は 2026-10-05 の配置に合わせた（plan/ws138/phase002/c7-boxes.diff、git の履歴の 7ce969dd）。Files・Settings の配置を変える WS は c7 の箱を確かめ直すこと。

## 2026-10-06 UAT のフィードバック（再設計の Phase の候補）

- BUG-217 最大化を desktop の session の状態にする（切り替え先も最大化、窓の app へは窓に）→ **新しい Phase で仕様と実装**（WS142 と共同）
- BUG-225・BUG-236 App Home の再設計（暗い stage・spotlight・光沢の床の反射、即座の覆いと icon の後追いの 2 層の animation）→ **新しい Phase: montage → 設計 → 実装**（P2 が montage の途中で中断）
- BUG-232 App Home で起動中の app は切り替え、BUG-235 Log Out の確認（暗くする演出）、BUG-219 title bar の menu に薄い下線、BUG-221 drag の範囲選択の frame rate の丸め（15〜30 fps）
- BUG-223 全画面の direct scanout（game mode）、BUG-208 の後の全画面と dock の整合
- ws099-p034 上部の bar: 保留の patch（held/）を main に当て直し中、icon は montage-4 と BUG-237 の穴（ad568302）

## Phase（2026-10-06 追加: 再設計）

- [ws099-p035](phase035/phase.md) 設計: App Home の stage と 2 層の animation・起動中の app の切り替え（planned、実装は p035a〜d）
- [ws099-p037](phase037/phase.md) 設計: App Home の Power Off と暗くする確認の dialog（planned）
- ws099-p034 第 2 版（p034b）: dock の時の窓の button を右上へ、時計を左へ、animation つき。**light の外観の bar は窓の title bar と同じ色味**（2026-10-06 ユーザー、dark は今の黒のまま）
