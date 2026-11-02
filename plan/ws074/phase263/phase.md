<!-- awesome-plan project=zedbsd record=ws074-p263 -->

# ws074-p263: 実際の Web サイトでの UAT の指摘への対処

Status: in-progress
Disposition: normal
Parent: [WS074](../ws.md)
Primary Milestone: MG006
Queue: [q2000](../../queue.md)（Q1 が自分で開発する。サブエージェントは使わない）
Origin: 2026-11-03 ユーザーから、WPT Interop 2025 の Phase（p260 ほか）とは別に、実際の Web サイトでの UAT の指摘に対処する Phase を立て、Q1 が直に開発する、という趣旨の指示を受けた。

## 目標

ユーザーが実際の Web サイトで見つけた libbrowser の表示・動作の不具合（UAT の指摘）を、指摘ごとに原因を調べて直す。
p260（Interop 2025 の Chromium が通る試験を全部通す）の目標・測り方は変えない。

## 範囲

- `userland/desktop/libbrowser/` と、必要なら `userland/desktop/browser/`（shell）。
- 指摘ごとの最小の再現（`plan/ws074/phase263/repro/`）と host の試験。
- 範囲の外: HAL・toolchain・compositor（`userland/desktop/wayland/`）。compositor の側の原因は根拠を記録して Q1 の別の作業にする。
  Windows 版の shell（WS334）の固有の不具合は WS334 の Bug にする。

## 受け入れ

- 指摘ごとに、host の browser（`plan/ws074/tests/host-build.sh plain`）でその site（または保存した頁・最小の再現）が直ったことを、
  Chromium との並べた画面（`live-compare.py`・`render-compare.py`）か動作の確かめで示す。
- host build の warning 0。p260 の測定で後退 0（全件の測り直しはまとまった単位で、細かい修正ごとには流さない、AGENTS §9）。
- 直せなかった指摘は理由と要る判断を記録する。

## 指摘の一覧

| # | 日付 | site | 指摘 | 状態 |
| --- | --- | --- | --- | --- |
| U1 | 2026-11-03 | https://www.amazon.co.jp/ | 画面が真っ白（Windows 版で確認） | host で直った（top が出る）。Windows の実機での確かめ待ち |

## 状態

2026-11-03 01:4x: Phase を立てた（Q1）。

2026-11-03 01:47: U1 を host で再現（main 010f487aa1 の host build、warning 0。`--render --async` 1280x900 で画面は白の 1 色）。
- amazon.co.jp は本当の頁ではなく **AWS WAF の bot の確認の頁**（`challenge.js` を読み、`AwsWafIntegration.getToken()` の後に
  `location.reload(true)` で開き直す）を返した。この頁は `<div id="challenge-container">` だけで、Chromium でも確認の間は白い。
- engine の誤り: `ReferenceError: FileReader is not defined` と `TypeError: Cannot read properties of undefined or null (at 1:646274)`
  （challenge.js の中）。token が取れず、開き直しに進まないので白のまま。
- AGENTS・master の規則「bot の確認に当たったら止めて報告、User-Agent の変更・別の取り方の再試行はしない」に当たるので、
  先へ進む前にユーザーに方針を聞く。

2026-11-03 01:53: ユーザーの決定（A）。2026-11-03 ユーザーから、標準の Web API と engine の誤りを正しく直して、普通のブラウザと同じ道筋で
確認に合格するのを目指してよい、という趣旨の答えを受けた。条件: User-Agent・ブラウザの特徴を偽らない、challenge.js は保存して主に
オフラインで試す、Amazon への取得は確かめに要る最小の回数。

2026-11-03 02:1x: U1 の直し（host、main bd90126dc5・90b305c177）。
- 原因 1: `document.scripts` が無かった（challenge.js が `document.scripts[i].innerHTML.includes(...)` で TypeError、token の取得が止まる）。
  `bind/collection.c` に Document の SameObject の live な HTMLCollection（HTML の script）を足した（images と同じ形）。
- 原因 2: `FileReader` が無かった（readAsDataURL）。`net.js` に FileReader（readAsArrayBuffer・readAsBinaryString・readAsText・
  readAsDataURL・abort、readyState・result・error、loadstart・progress・load・abort・loadend、task で非同期）を足した。あわせて Blob を
  byte で持つ形に直した（前は UTF-8 の文字列で持ち、binary の part が U+FFFD に化けた）。type の小文字化と非 ASCII の type の破棄も。
- これで AWS WAF の確認は通る（inputs → report → mp_verify が 200 で token、cookie `aws-waf-token`、`location.reload`）。
- 原因 3: 開き直した後に Akamai の interstitial（`/_sec/verify?provider=interstitial` へ **同期の** XHR で JSON を POST）が来る。
  同期の XHR は GET しかできず NetworkError。host の口 `request_sync`（`bind/bind.h`、`page/script.c` の `script_request_sync`、
  `page/network.c` の `page_send_response_now`）と native `syncSend` を足し、http・https の同期の XHR は method・header・body を送り、
  status・header を返すようにした。
- 道具: headless の `--follow=N`（`browser/main.c`、script が求めた navigation を N 回まで行い、ページごとに settle）。
  `BROWSER_NET_DUMP` の時に script の request・同期の request・top の navigation・document.cookie の設定を stderr に書く。
- 試験: [file-reader.html](repro/file-reader.html) 25/25、[sync-xhr.py](repro/sync-xhr.py) 11/11、run-fetch-tests 38/38、
  run-http-tests 15/15・`--async` 19/20（async-sheets は基点 010f487aa1 でも同じく FAIL、この変更による後退ではない）。host build warning 0。
- live（`--render --async --follow=4`）: 確認 → 開き直し → 本当の amazon.co.jp の top（nav の menu・検索の候補・lazy load の POST）まで進んだ。
  画面: build/review/q2000-amazon-after.png（1280x900、top の nav・検索・carousel・カードが出る）。headless では
  確認から描画まで約 4 分（CPU、BUG-411 の JS の速さと同じ系統、別に扱う）。残りの script の誤り:
  `TypeError: value is not a function (at 157:8)`、`Cannot read properties of undefined (reading 'getClientCapabilities')`。
- Windows の cross-build: `make browser-win64-zip LLVM_MINGW=$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64` PASS（warning 0、
  build/browser-win64/OmniGrammaton-0.1.0-win64.zip）。本物の Windows での確かめは未実施（ユーザーの UAT）。
