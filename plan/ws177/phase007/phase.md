<!-- awesome-plan project=zedbsd record=ws177-p007 -->

# ws177-p007: Welcome の準正常系（案 B）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 判定、T1-410 PASS）（旧: test-wait（T1-410）（2026-10-08 P1 q884 の 4: 実装・host PASS・build warning 0、main に統合））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q884 の 4（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 26・27・28・30・31（WS164 ws164-p002）、[案](../phasing-20261008.md) の B

## 設計と変更（2026-10-08 P1、`userland/desktop/settings/welcome.c`・`main.c`・`ui.c`・`settings.h`）

| 行 | 形 |
| --- | --- |
| 26 welcome.done を付けられない | `welcome_finish`: `se_look_set` が失敗したら（Keiland の拡張の無い desktop・送れない）、窓を閉じずに bar の中に「印を付けられず次の login でまた出る、もう一度押すと進む」と理由（strerror）を赤で出す。次の Start・Skip は従来どおり閉じる。限界: 設定の store への書き込みの失敗は非同期の result で、ここでは見ない（拡張の無さと送りの失敗だけ） |
| 27 Files の起動の失敗 | `main_open_files` が posix_spawn の誤り（zedBSD の libc は exec の失敗を返す）を返し、失敗なら窓を閉じずに `se_welcome_files_failed`: Done の段に戻し「Files を開けなかった、App Home から開く」と理由を bar に、Start のボタンは Close に（もう Files を起動しない） |
| 28 Network の段の失敗 | 段の header の下に: Wi-Fi の join の失敗（Wi-Fi の頁の message_bad の message）なら理由と「鍵をもう一度、または Next で後で Settings から」、radio も cable の入った有線も無ければ「見つからない、Next で進み後で Settings から」 |
| 30 小さい窓・長い文 | 段の header（題と要約）を自前で描き、要約を `kl_text_break` で最大 4 行に折り返す（Welcome の段の挨拶の 2 行目も）。bar は Back・Next の幅を測り、点の列が重なるなら点を描かない。失敗の言葉は点の代わりに 2 行まで |
| 31 keyboard | `se_welcome_key`（page の key の後、ui の key の前）: Enter で Next（Done では Start）、Esc で閉じる（印を付けない、`WELCOME closed step=N`）、Alt+Left・Alt+Right で Back・Next。page の field が key を取る時（Wi-Fi の鍵）はそちらが先 |

- 試験（WS164・WS089 は未完了）: `plan/ws164/tests/run-host-welcome.sh` に key（Enter・Alt+Left・Alt+Right・Esc で印無し）、560x620 の窓、Network の段の `nonet`・`joinfail` を足し、`plan/ws089/tests/host-network.c` に scenario `nonet`・`joinfail` を足した。QEMU: `plan/ws177/tests/welcome-p007.sh`（Files の起動の失敗）。

## 確認（host・build、2026-10-08）

- `sh plan/ws164/tests/run-host-welcome.sh` → PASS（11 + 7 の ok）。絵（host の renderer）: 560x620 の Network（点は入る）、420x800 の Done（要約が 3 行に折り返し、点は消える）、`nonet`・`joinfail` の段の文。
- build（warning 0）: `make BUILD=build/p1-d ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk build/p1-d/bin/settings`。style-check: welcome.c・main.c 0、ui.c 5 → 5（既存）。

## 未実施

- QEMU（T1）: `plan/ws177/tests/welcome-p007.sh`（guest の中で /bin/files を退けて Files の起動の失敗、Close で閉じる）。welcome.done が書けない場合は QEMU・実機とも未確認（拡張の無い desktop で起こる）。日本語の UI の長い文での折り返しは絵で未確認（翻訳の catalog がある時）。

## Event

2026-10-08 / q884-i04（P1）: 実装と host・build。


## Q1 の判定（2026-10-08）

T1-410: welcome-p007 PASS（2 回とも）。
