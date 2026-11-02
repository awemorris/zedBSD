<!-- awesome-plan project=zedbsd record=ws345 -->

# WS345 ブラウザの thread の分け方: JavaScript を窓の event の処理と別の thread に、通信（connect を含む）を独立した thread に

- Master: [plan/master.md](../master.md)（2026-11-02 14:03 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-11-02 P8: p001〜p003 cleared、設計 [design.md](design.md)。p004〜p014 は planned・in-progress）
- 目標: ブラウザ（libbrowser と browser の shell、zedBSD・Windows・Linux の Keiland）で、JavaScript の実行と通信がどれだけ長くかかっても、窓の event の処理（入力・再描画・resize・title bar の操作）が止まらない形にする。JavaScript は窓の event の thread と別の名前付きの thread で動かす。通信は同期の呼び出し（connect・TLS の handshake・read・write）のままでよく、それを独立した名前付きの thread で行う。
- 置き場所: userland/desktop/libbrowser/、userland/desktop/browser/（shell）、Windows の OmniGrammaton の build（WS334）。

## ユーザーの決定

- （2026-11-02 14:03）ユーザーから、ブラウザで JavaScript の処理の間に GUI の event の処理が止まるので、JavaScript を別の thread で動かして窓の event の処理を止めないようにする、通信も connect() が同期なら同期のままでよいが、通信は独立した別の thread にする、という趣旨の指示を受けた。
- 全体の方針（Keiland、2026-10-28・29）: 待つ処理は数を明示した名前付きの thread で同期に行う。数の無い pool や場当たりの直しにしない。
- （2026-11-02、Q1 経由）ユーザーから、libbrowser は自分で thread を作らない、thread の作成は shell の側で実装し、libbrowser は shell が登録する関数の表（名前付きの thread の開始・join、libbrowser に要る待ち・起こし・lock の最小の組）を出す、どの thread が幾つあり、名前と何を走らせるかは libbrowser が決める、という趣旨の決定を受けた（下の Q1 の判断の「pthread を libbrowser の中で使う」部分を置き換える）。
- （2026-11-02、Q1 経由）ユーザーから、次の趣旨の決定を受けた。
  1. 窓の event の thread は libbrowser の仕事を一切しない（XHR・fetch・script・layout をしない）。窓の event を受けて渡すだけ。
  2. 通信は全部、専用の thread で行う（同期の XHR も。それは呼んだ script の thread を止め、窓の thread は止めない）。
  3. script は専用の thread で動き、iframe ごとに上の文書と別の script の thread を持つ。
  4. style sheet と CSS（cascade、computed style・描画の属性の更新）は専用の「描画の属性の更新」の thread で行う。
  5. 描画（paint・GPU の submit）は、できるなら shell が作る thread で、event の処理と別に行う。
  6. thread の作成は上の決定のとおり shell の側で、登録した関数の表を通す。
- （2026-11-02、Q1 経由、J2 への答え）ユーザーから、長く走る script の頁には Chrome・Firefox のように「このページは応答していません（待つ／止める）」の問いを出す、決まった時間（10 秒ほど）script が走り続けたら出し、「止める」は p004 の打ち切りで script を終わらせる、という趣旨の決定を受けた（ws345-p014）。

## Q1 の判断

- （2026-11-02）ユーザーの「thread の作りを libbrowser の中に置けるか」の問いに対し、Q1 が、page（script）と通信の thread は libbrowser が持って libbrowser の口の後ろで始め、zedBSD・Windows・Linux の shell が一つの実装を共有し、各 shell は窓の event の thread だけを持って thread-safe な口（入力を積む、確定した frame・状態の変化を今の wake で受ける）で libbrowser と話す形にする、と決めた。thread の道具を libbrowser の中の pthread にする部分は、上のユーザーの決定（shell が登録する表）で置き換わった。
- （2026-11-02）Q1 が、WS345 の Phase が shell の thread の表を登録するために WS074 の host の試験と host-build.sh（`plan/ws074/tests/host-*.c`、`host-build.sh`）と、要れば WS334 の Windows の試験の harness を変えてよい、と許可した。変えた試験は通るように保ち、各 phase.md の結果に変えた試験の file を全部書く。

- （2026-11-02 14:46、p001 の J1・J3〜J7 への答え）
  - J1・J3・J4・J5: P8 の判断のとおり（非同期の loader の thread を保つ、libmedia の内部の lock は pthread のまま thread の作成だけ表を通す、view あたり frame 64、media 8）。loader の thread はユーザーの決定の「通信は個別の thread」を満たす。
  - J6: 窓の thread は入力を libbrowser の FIFO に積むだけにし、iframe への振り分け（確定した iframe の矩形の参照）も libbrowser の側の thread（top の script の thread か style の thread）で行う。窓の thread では libbrowser の処理を一切しないという決定 1 を字義どおり守る。
  - J7: 決定 5 は「display list を style の thread で作り、shell の render の thread で記録・提出・表示する」と読んでよい。
  - 所有: p013 の libkeiland の変更と、p002 の plan/tools/browser-component/client.c の変更を、WS345 の範囲として認める。
  - J2（長く応答しない頁の「応答しない頁」の確かめの表示）は製品の判断なのでユーザーに聞く。答えまでは表示無しで進める。→ 2026-11-02 ユーザーが答えた（上のユーザーの決定、ws345-p014）。

## P8 の技術の判断（p001、Q1 が覆してよい）

- 通信は今の非同期の loader（`browser-net` 1 本、`browser-resolve` 1 本、view ごと）を保ち、窓・script の thread に残った同期の socket の道（同期の XHR、先取りに無い parser の script、iframe の script、sheet の待ち）を loader に移す。blocking の connect の thread の pool にはしない（理由は [design.md](design.md) §3.1、判断の点 J1）。
- libmedia の中の lock は pthread のまま、engine の thread の作成だけ shell の表を通す（J3）。1 つの view の frame は top を含め 64 まで（J4）。
- 再生中の media の engine は view に 8 まで（J5）。J2・J6・J7 は Q1 とユーザーが答えた（Q1 の判断、ユーザーの決定）。

## 計測（p001、host、2026-11-02）

今の 1 本の loop の窓の event の最長の遅れ（[design.md](design.md) §1.3、[tests/stall-probe.sh](tests/stall-probe.sh)）: idle 55〜67 ms、timer の script 3 s で 2997 ms、mousedown の listener 1 s で 995 ms、同期の XHR の connect の掛かりで 40 s を超えてまだ止まる、read の掛かりで 30029 ms、`document.write` の `<script src>` の connect の掛かりで 41 s を超えてまだ止まる、`<img>`・`fetch()`（loader）は 56.6 ms で止まらない、6000 個の box の頁の resize で 492 ms、3000 × 3000 の PNG で 1463 ms。QEMU・実機・Windows は未実施。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws345-p001](phase001/phase.md) | 調べと設計: 今の thread と event loop、止まる所の計測、thread の表（名前・数・持つ物・渡し方）、DOM・layout と JS の持ち主、event の受け渡し、取り消し、試験の道 | cleared（2026-11-02 14:46 Q1、87feb4707 を main に merge 1ec103957）。cleared（2026-11-02 P8 q1959-i01。[design.md](design.md)、design-reviewer の指摘 19 件の反映は §12） | — |
| [ws345-p002](phase002/phase.md) | shell が登録する thread の表（`struct browser_threads`、`browser_threads_set`）と libbrowser の base の包み。libbrowser の pthread を全部替え、shell（kl_thread）・試験（POSIX）の実装、libmedia の engine の thread の作成 | cleared（2026-11-02 15:59 Q1、4a4b68de0 を main に merge 743b46df5、main で host-build warning 0・cross-build PASS）。cleared（2026-11-02 P8 q1968-i01: libbrowser の thread・lock・condition を全部 shell の表で、段 1・tsan・cross-build・Wine の回帰。[phase.md](phase002/phase.md) の結果） | p001 |
| [ws345-p003](phase003/phase.md) | 通信を全部 loader に: loader の client（要求した thread ごとの完了と起こし、どの thread からでも取り消し）、同期の XHR・漏れた parser の script・iframe の script・sheet の待ちを client の待ちに | cleared（2026-11-02 P8 q1972-i01: loader の client・waited の request・abort と epoch、頁の待つ取得を全部 loader に。[phase.md](phase003/phase.md) の結果） | p002 |
| [ws345-p004](phase004/phase.md) | VM の打ち切り（agent ごとの atomic の flag、loop の印・関数の入口・長い native で見る、catch・finally を走らせない終わり） | cleared（2026-11-02 P5 q1973-i01: VM_TERMINATED、heap の task の番号と atomic の要求、loop・入口・unwind・長い native で見る。host-terminate 39/39（tsan も）、bug411・test262・段 1 が前と同じ。[phase.md](phase004/phase.md) の結果） | p001 |
| [ws345-p005](phase005/phase.md) | view の front と top の script の thread（`BROWSER_THREADING_OWN`）: mailbox・入力の合わせ方と順・snapshot・callback を窓の thread で・navigation と destroy の打ち切りと join・process に一つの可変の static の追い出し・CPU の確定した frame | cleared（2026-11-02 P5 q1980-i01: `_OWN` の view の front と `browser-script` の thread、mailbox・snapshot・通知、打ち切りを navigation と destroy に、CPU の絵は暫定（Q1）。host-threaded-view 18 checks 0 failed（tsan の報告 0）、stall-probe の own で窓の event の最長 0.1〜7 ms、段 1 と cross-build が前と同じ。[phase.md](phase005/phase.md) の結果） | p002、p003、p004 |
| [ws345-p006](phase006/phase.md) | 描画の口（D5）: 確定した frame の箱と frame の item、`browser_view_render`・`_render_pixels`・`_presented`、renderer と glyph の face を描画の thread に、描画の機会と rAF、確定の後の木を書かない | cleared（2026-11-02 P8 q1984-i01: 確定した frame（`view_commit`・`paint_scene`、画像の pixel・font の bytes・iframe の文書を持ち木を指さない）、`browser_view_render`・`_render_pixels`・`_presented`、描画の thread の text と GPU、rAF は present を待つ。host-threaded-view 29/29（CPU・GPU とも `_CALLER` と画素で同じ）、tsan の報告 0、stall-probe own 0.1〜0.2 ms、段 1 前後同じ、zedBSD 3 target warning 0。iframe の分離は p009 へ移す提案。[phase.md](phase006/phase.md) の結果） | p005 |
| [ws345-p007](phase007/phase.md) | shell（zedBSD・Linux の Keiland、Windows）: 窓の thread は event だけ、shell の描画の thread、resize と modal loop、drag の画像・文字入力・touch・mail の新しい口 | in-progress（2026-11-02 20:23 P8 q1988） | p006、p013 |
| [ws345-p008](phase008/phase.md) | style の thread（D4）: DOM の同期の貸し、request の種類、古くない時は貸さない、style・layout から JS を呼ぶ所を後で出す event に、style の thread の loader の client | cleared（2026-11-02 21:40 Q1、d8517c082 を main に merge dc48463ef、main で host-build warning 0・zedBSD amd64 の libbrowser.so warning 0）。in-progress（2026-11-02 P5 q1985-i01: `browser-style` の thread と agent の lender、貸しの間は collect しない、layout・display list・cascade・iframe の layout を貸しに、古くない時は直に読む。host-threaded-view 39/39（tsan の報告 0）、段 1・DOM・test262・bench・raster の前後が同じ、zedBSD 3 target warning 0。WPT は suite が無く未実施。cleared の判定は Q1。[phase.md](phase008/phase.md) の結果） | p005 |
| [ws345-p009](phase009/phase.md) | iframe ごとの script の thread（段 A: 一つの agent の lock と heap のまま）: frame ごとの task の queue、入力の routing、frame の上限、GC の stack の底の切り替え | in-progress（2026-11-02 21:40 P5 q1993） | p006、p008 |
| [ws345-p010](phase010/phase.md) | origin ごとの agent（段 B）: cross-origin の iframe を別の heap で並べて走らせる、遠くの WindowProxy、heap を越える postMessage、location・frames・parent、agent ごとの打ち切り | planned | p009 |
| [ws345-p011](phase011/phase.md) | 受け入れと回帰: 窓の event の遅れの上限（script の loop・connect の掛かり・document.write・resize・decode の間）、ThreadSanitizer、host の試験・js・test262 の部分集合・段 1・cross-build、zedBSD の QEMU、Windows の Wine、Linux の Keiland | planned | p007、p010、p014 |
| [ws345-p012](phase012/phase.md) | 規約の全文の見直し（AGENTS §2） | planned | p011 |
| [ws345-p014](phase014/phase.md) | 応答しない頁の問い（約 10 s 走り続ける script に「応答していません: 待つ／止める」、止めるは p004 の打ち切り）。各 shell の UI、窓の側の timer が script の thread の task の番号と始めの時刻を読む | planned | p004、p005、p007 |
| [ws345-p013](phase013/phase.md) | libkeiland の resize の口（Wayland の configure の ack を描画の側の present の直前に、Win32 の modal loop の間の大きさ）。design-reviewer の指摘 4 で足した（番号は表の後ろ、p007 の前にする） | cleared（2026-11-02 P4 q1974-i01: KL_VERSION 109 の `kl_window_set_configure_deferred`・`_configure_take`・`_ack` と `kl_window_on_resize`。host の configure-test・TSan・Wine の resize-wine PASS。cross-build の PASS の行は負荷の時間切れで未取得、[phase.md](phase013/phase.md) の結果） | p001 |

## Resume point

- 2026-11-02 14:03 Q1: 作成。P8 が p001（設計）に着手。設計の後に実装の Phase を立てる。
- 2026-11-02 P8（q1959-i01）: p001 の設計と計測、design-reviewer の反映、p002〜p013 の phase.md。次は Q1 が p002 の Queue を立てる（p004・p013 は p002 と並べてよい）。J2・J6・J7 は Q1 の答え待ち（実装の p005・p009・p006 の前に）。
- 2026-11-02 P8（q1968-i01）: p002 cleared（thread の表）。次は p003（通信を loader に）・p004（打ち切り）・p013（libkeiland の resize の口）が並べて始められる。
- 2026-11-02 P8（q1972-i01）: p003 cleared（通信を loader の client に）。p005 は p004（P5）の merge の後。

## Q1 の判断（2026-11-02 18:28、p005 の merge）

- p005 の _Thread_local で zedBSD amd64 の libbrowser.so が __tls_get_addr の未定義で link できなくなった（host と arm64 は通っていた）。Q1 が libvulkan と同じ形（amd64 は -mtls-dialect=gnu2 の TLS descriptor、riscv64 は -z undefs と provider の一覧）を libbrowser に当てて直した。amd64・riscv64 の libbrowser.so は warning 0。以後の Phase は zedBSD の amd64・arm64・riscv64 の libbrowser.so の build も確かめる。
- p005 の design.md の案（§6.1 の仮の CPU の絵と費用、§9 の agent・abort_waited・pagehide の 1 秒）は P8 が p006 で design.md に入れる。

## Q1 の判断（2026-11-02 20:23、p006 の merge）

- p006 の手順 6（iframe を別に layout して自分の frame として commit する）は p009 に移す。stage A では iframe の document は top の script の thread で動き、子の list は親の list に写されるので、commit は既に変わらず自足している。frame の item は iframe が自分の thread から commit する時に要る。
- 「1 つ先まで」の抑えは置かない（未描画の commit は新しい commit で置き換わり、script の thread は描画を待たない。present が止まれば rAF も止まる）。
- cross-build の FAIL（libbrowser.a の WebPDecodeBGRAInto・WebPGetInfo が allowed の表に無い）は p006 のせいでなく、cross-build.sh の archive の一覧に libwebp-compat.a が無かったため。Q1 が直した。

## Q1 の判断（2026-11-02 21:40、p008 の merge）

- WPT の reftest の見本は、host に WPT の checkout が無いので p008 では流さなかった。p011（受け入れ）で WPT を取得して流す（build/ws074-suites/wpt の壊れた symlink も p011 で直す）。
- OWN での貸しの費用（1000 回の変更と読みで 701〜757 ms、CALLER で 600 ms）は受け入れる。窓と描画は止まらない。spin の待ちなどの調整は p011 の計測の後に判断する。
- sheet の解析（page_update_styles）を script の thread に残したことは Future Work の候補として p012 で書く。
