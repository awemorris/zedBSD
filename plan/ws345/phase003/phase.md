<!-- awesome-plan project=zedbsd record=ws345-p003 -->

# ws345-p003 通信を全部 loader に

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

窓・script の thread で socket を使う同期の道を無くす（D2、design.md §7）。connect・TLS・read・write は `browser-net` だけで行う。

## file

- `net/loader.c`・`net/net.h`: loader の client（`net_loader_client_open`・`_close`、request は client に属する、完了は client の list と notify、`net_loader_dispatch(client)`）。取り消しはどの thread からでも印を付けるだけで、放すのは持ち主の thread。取り消しの範囲は頁の epoch（design.md §7）。client は page でなく呼んだ thread の context から選ぶ。
- `page/network.c`・`page/script.c`（`script_fetch_sync`・`script_fetch_file`・`page_wait_sheets`・`script_settle_network`）・`page/frame-load.c`（iframe の `<script src>`）・`page/link.c`（`page_fetch` の http の道は loader の無い時だけ）・`view/view.c`。
- `net/http.c` の同期の道は `BROWSER_FETCH_AT_ONCE` と loader の無い headless にだけ残す。

## 手順

1. client を作り、今の view の一つの caller を「view の client」にする（動作は同じ）。
2. 同期の待ち `page_net_wait_request(context, request)`: request 自身の condition で、完了・取り消し・打ち切り（p004、この Phase では取り消しまで）を待つ。dispatch する者は要らない（net の thread が request に終わりを書いて signal する）。
3. 同期の XHR、漏れた parser の script、iframe の script、`page_wait_sheets` をこの待ちに替える。
4. 取り消し: navigation・stop・destroy で古い epoch の request を取り消すと、待っている thread がすぐ起き、新しい navigation の取得は残る。

## 試験

- 新しい host の試験 `plan/ws345/tests/host-loader-client.c`: 2 つの thread がそれぞれの client で取り、完了がそれぞれの thread の dispatch で走る。別の thread からの取り消しで同期の待ちが 100 ms 以内に起きる。同期の XHR が connect の掛かる listener（probe と同じ作り）に向かう間、呼んだ thread の中で `connect` が呼ばれない（`browser-net` の中だけ、thread の名前で確かめる）。
- `sh plan/ws345/tests/stall-probe.sh sync-xhr-connect docwrite-connect`: 窓の loop は（まだ同じ thread なので）止まるが、loader の時間切れ（30 s）で終わる事、socket は `browser-net` の中だけである事。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- `BROWSER_FETCH_DEFAULT`・`_BACKGROUND` の view で、page の code が `net_http_fetch` を呼ばない（呼び出しの数を試験の build で数える）。
- 上の試験と回帰が前と同じ。
