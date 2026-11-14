<!-- awesome-plan project=zedbsd record=ws345-p014 -->

# ws345-p014 応答しない頁の問い

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md) §9。
- 状態: in-progress（2026-11-14 P3 q2090-i01: 残りの shell-wine の全 case と p010 の agent への広げを済ませた。実装済み・試験 PASS、cleared の判定は Q1。下の「結果（2026-11-14 P3）」。前: 2026-11-02 P8 q1996-i01 が実装と試験）
- 出所: 2026-11-02 ユーザーの決定（J2 の答え、ws.md）。

## 目的

script が決まった時間走り続けて頁が応答しない時に、Chrome・Firefox のように「このページは応答していません」の問い（待つ／止める）を出し、「止める」で p004 の打ち切りにより script を終わらせる。窓の thread は libbrowser の仕事をしない（決定 1）ので、問いの timer と UI は shell の窓の側にあり、libbrowser の front の snapshot を読むだけ。

## 決め事

- 閾値: 同じ task が 10 s 走り続けたら出す（`SHELL_UNRESPONSIVE_MS` 10000）。「待つ」の後は同じ task がさらに 10 s 走ったら再び出す。task が終われば問いは消える（利用者の操作は要らない）。
- 文言（英語の原文、libkeiland の翻訳の表 `kl_tr` に足す）: 題「This page isn't responding」、本文「A script on <頁の題、無ければ host> has been running for <N> seconds. You can wait for it or stop it.」、button「Wait」（既定）と「Stop script」。日本語: 「このページは応答していません」「<頁> の script が <N> 秒間動き続けています。待つか、止められます。」「待つ」「script を止める」。
- 「Stop script」は今の task だけを打ち切る（頁は残り、次の event・timer は普通に走る）。同じ頁が何度も止まらなくなる場合も毎回聞く。

## file

- `include/browser/browser.h`・`view/front.c`: snapshot に今の task の番号と始めた時刻（単調な時計、ms）。`browser_view_script_busy(view, &task, &milliseconds)`（窓の thread、snapshot を読むだけ）と `browser_view_stop_script(view, task)`（その task の番号を打ち切りに書く。task が変わっていれば何もしない）。
- shell（`userland/desktop/browser/shell/`、zedBSD・Linux の Keiland と Windows で共通）: 窓の loop の timer（`kl_app_dispatch` の待ちの期限に足す）、問いの表示。表示は libkeiland の窓の中の dialog（Keiland では libkeiland の ui の dialog、Windows でも同じ libkeiland の描画）。titlebar の操作と resize は問いの間も効く。
- libkeiland の翻訳の表（文言）。

## 試験

- host の試験（`plan/ws345/tests/host-threaded-view.c` に足す）: 12 s の loop の task で `script_busy` が 10 s を超えた値を返す、`stop_script` で 300 ms 以内に task が終わり頁は残る（次の timer が走る）、古い task の番号の `stop_script` は今の task を止めない、loop の無い頁で busy が 0。
- shell: zedBSD の QEMU と Windows の Wine で 30 s の loop の頁を開き、10 s 後の画面の PNG に問いが出ている、「Stop script」を入力の道具で押すと問いが消えて頁が応答する（画面の PNG）。Linux の Keiland の guest は p011 で。
- 回帰: host-build warning 0、host の試験、段 1、cross-build。

## 受け入れ

- 上の試験が通り、問いの間も窓の event（titlebar・resize・再描画）が止まらない。

## 結果（2026-11-02 P8 q1996-i01）

### 実装

- libbrowser（小さく、front_note・front_publish・view_front_process・paint/ には触れない）:
  - `vm/vm.h`・`vm/heap.c`: `struct vm_agent` に `since`（始めた時刻の ms を 32 bit に切った上位と task の番号の下位の 1 語、atomic）。`vm_agent_task_begin` が書き `_end` が 0 に戻す。新しい `vm_agent_task_since`（どの thread からも読むだけ）。
  - `view/front.c`・`front.h`: `view_front_script_busy`・`view_front_stop_script`（`view_front_request_image` の隣）。stop は番号が今の task の時だけ `vm_agent_request_termination` と、自分と iframe の client の `abort_waited`（同期の XHR を起こす）。
  - `view/view.c`・`include/browser/browser.h`: 公開の `browser_view_script_busy(view, &task, &ms)`（1/0）と `browser_view_stop_script(view, task)`（0・ESRCH・ENOTSUP）。段 A では top と iframe が一つの agent なので iframe の thread の task も同じ口で見えて止まる。
- shell（zedBSD の Keiland と Windows で共通）:
  - `shell/unresponsive.c`・`.h`（新）: 規則だけ（10 s で一度、Wait の後は同じ task がさらに 10 s で再び、task が終われば消す、別の task は数え直し、次に見る時刻）。
  - `shell/prompt.c`（新）: 窓の thread が kl_ui・kl_text・kl_button で card（題・説明・[Stop script] [Wait]、Wait が既定、Enter と Esc も Wait）を自分の memory に描き、描画の thread に渡す。表示中は内容の上の pointer は card へ（外は捨てる）、鍵は Enter・Esc の他は shell の shortcut だけ、titlebar・resize・描画は止まらない。頁の名は title、無ければ host、無ければ URL。行 `ZBROWSER PROMPT shown|wait|stop|hidden`。
  - `shell/render.c`: `shell_render_overlay`（窓の thread が card を渡す）、描画の thread が自分の写しを毎 frame の中央に重ねる（CPU は blit）。`shell/present.c`: swapchain に TRANSFER_DST（surface が許す時）、host 可視の buffer から `vkCmdCopyBufferToImage`（`browser_view_render` の後、PRESENT_SRC → TRANSFER_DST → PRESENT_SRC）。card は不透明の角の四角（GPU の copy は混ぜられないため。veil は無い）。
  - `shell/shell.c`・`window.c`・`internal.h`・`sources.mk`: loop の timeout に問いの次の時刻、入力の門、resize・外観・`KL_APP_LANGUAGE` で描き直し。
- 文言: libkeiland の翻訳（domain `browser`）。`userland/desktop/locale/browser.keys`（kl_tr に表から渡す 5 語）、`locale/ja/browser.tr`（「このページは応答していません」「{1} のスクリプトが {2} 秒間動き続けています。待つか、スクリプトを止めることができます。」「待つ」「スクリプトを止める」「このページ」、tr.py check --strict 5/5）、`locale/ja/glossary.md` に 4 語。phase の案の「script」は日本語の UI に合わせ「スクリプト」にした（P8 の判断）。Windows の kl_tr は stub（NULL）なので英語、`{1}`・`{2}` は shell の中で埋める。

### 試験（2026-11-02、全て P8 が流した）

| 試験 | 結果 |
| --- | --- |
| host-build plain・tsan（`BROWSER_HOST_BUILD=build/ws345-host`） | 両方 exit 0、warning 0 |
| `host-threaded-view.sh plain`（新しい (r)） | 55 checks 0 failed。(r): caller の view は busy 0・stop ENOTSUP、短い timer の頁は 200 ms を超える task なし、timer の spin・接続の来ない同期の XHR・iframe の spin の各々で busy と 1.5 s の値、古い番号は ESRCH で止めない、番号の stop で 0〜1.1 ms に終わる、`spin end`・finally は走らず頁の tick と iframe の tick が再び走り、URL は同じ |
| `host-threaded-view.sh tsan` | 55 checks、ThreadSanitizer の報告 0、失敗 2 は前からの tsan の遅さの時間の上限（(h) 1726 ms、caller の navigation-wait 988 ms）。(r) だけ（`HOST_THREADED_VIEW_ONLY=r`）は tsan で 15/15 |
| host-terminate・host-frames（plain） | 39/39、15 checks 0 failed |
| `host-prompt-rule.sh`（新） | 13 checks 0 failed（10 s で一度、Wait で再び、終われば消す、別の task、14 s で初めて見えた task、時刻を返す loop の 12 s と 35 s の task） |
| `STALL_MODE=own stall-probe.sh` | 9 場面とも窓の event の最長 0.1〜0.2 ms、over100 0 |
| zedBSD amd64（config-amd64-browser.mk）・arm64（config-rpi5.mk）・riscv64（config-visionfive2-keiland.mk）の `bin/browser`・`libbrowser.so` | 3 つとも exit 0、warning 0（最後の直しの後） |
| `sh plan/ws345/tests/shell-guest.sh`（zedBSD の QEMU、Venus、image は今の tree から） | **27 passed、0 failed、PASS**。p007 の busy・xhr・busy-cpu 15/15 と新しい prompt・prompt-cpu: spin start の 8〜9 s 後に問い（行の seconds=10）、触らなければ script の終わりで reason=ended で消える、reload の後に Stop script をクリックで `stop error=0`・3 s に tick 7・spin end なし、Enter で wait・その後 script が自分で終わる、窓の待ち最長 20 / 16 ms・over_100ms 0、ERROR 0。画面: `build/review/ws345-p014/prompt*.png` |
| `sh plan/ws345/tests/shell-wine.sh`（Wine・lavapipe） | 1 回目（全 case）: busy・xhr・busy-cpu 15/15、prompt・prompt-cpu の stop は PASS、shown の時間の測り方が間違い（script の 1 枚目の画面の後から測った 7 s、直した）と GPU の prompt で窓の待ち 124 ms が 1 回（同時に image の build と arm64・riscv64 の build と stall-probe が走っていた）。2 回目（`SHELL_WINE_CASES="prompt prompt-cpu"`、測り方を直した後）: **8 passed、0 failed、PASS**（問い 8〜9 s、Stop で tick 1→8・spin end 0、待ち最長 23.5 / 13.1 ms、over_100ms 0）。画面: `build/review/ws345-p014/wine-*.png` |
| `sh plan/ws334/tests/cross-build.sh` | 1 回目 FAIL（prompt.c の `strpbrk` が allowed-imports に無い）→ `strcspn` に替えて **PASS** |

- 変えた試験の file: `plan/ws345/tests/host-threaded-view.c`（(r) と `HOST_THREADED_VIEW_ONLY`）、`shell-guest.sh`（`start_session`・`reload`・`finish` に分け、prompt・prompt-cpu の case と `SHELL_GUEST_CASES`）、`shell-wine.sh`（prompt の mode と case、`SHELL_WINE_CASES`）、新しい `host-prompt-rule.c`・`.sh`・`pages/spin.html`。
- shell-guest は worktree に `build/ws035-sq-venus` が無いと compositor が Vulkan の device を作れず全 case が READY 無しで落ちる（1 回目）。`VENUS_RENDERER=/home/awe/zedBSD-claude1/build/ws035-sq-venus/install` を付けて流した。

### 未実施・残り

- Linux の Keiland は browser の build が無く未実施（p007 と同じ）。本物の Windows・実機は未実施（ユーザーが拾える試験の一覧に、Q1）。日本語の文言の表示（desktop の言語を日本語にした画面）は未確認（catalog の check だけ）。
- touch の入力は問いの表示中も今までどおり頁に行く（card への tap は未対応）。
- p010（段 B、origin ごとの agent）: busy・stop は今 view の一つの agent（top と iframe）だけを見る。agent が増えたら全ての agent の task を見て止めるよう広げる（P5 へ、Q1 経由）。
- 最後の直し（`strcspn`、shell-wine の測り方）の後の shell-wine の全 case（busy・xhr・busy-cpu）は流していない（prompt の 2 case だけ）。

## 結果（2026-11-14 P3 q2090-i01、残りの確かめ）

- main c5b8a10d24 に p014 の実装（`browser_view_script_busy`・`_stop_script`、`shell/unresponsive.c`・`prompt.c`、`locale/ja/browser.tr`）がある事を確かめた。
- 「未実施・残り」の片付け:
  - p010 への広げ: 済み（p010 の結果の (6)、commit 7073d0c19c）。task の番号を process で一つにし、busy は全 agent の一番長い task、stop はその task の agent だけ。host-frames (k) の 4 check と host-threaded-view (r) で確かめた（p010 の表）。
  - 最後の直しの後の shell-wine の全 case: `sh plan/ws345/tests/shell-wine.sh build/tmp/p3/shell-wine`（7073d0c19c の tree、make は `-j4` に直した後）→ `shell-wine: 23 passed, 0 failed`、`shell-wine: PASS`（busy・xhr・busy-cpu・prompt・prompt-cpu）。
  - 回帰（p010 の表と同じ run）: host-build plain・tsan warning 0、host-threaded-view plain 55/55、host-prompt-rule 13/13、cross-build PASS、zedBSD x86_64・rpi5・vf2 の browser warning 0。
- 受け入れの根拠: host の (r)（busy・stop・古い番号・loop の無い頁）、QEMU の shell-guest 27/27（2026-11-02、上の表）、Wine の全 case 23/23（今回）。問いの間の窓の待ち最長は Wine で 20.7〜22.4 ms・over_100ms 0。
- 未実施のまま（理由）: Linux の Keiland は今も browser の build が無い（`userland/desktop/keiland-linux.mk` に browser が無い）。本物の Windows・実機（ユーザーが拾える試験の一覧へ、Q1）。日本語の文言の画面。touch の問いへの tap。shell-guest は image の config の乖離（p010 の結果）で今回は流していない（2026-11-02 の 27/27 の後に変えた所は libbrowser の busy・stop の agent の扱いだけで、shell の側は変えていない）。
