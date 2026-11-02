<!-- awesome-plan project=zedbsd record=ws345-p003 -->

# ws345-p003 通信を全部 loader に

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared（2026-11-02 P8 q1972-i01、branch `agent/p8-ws345`、基点 d635cd1027）

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

## 結果（2026-11-02 P8、q1972-i01）

- 実装（`net/loader.c`・`net/net.h`）: loader の client（`struct net_loader_client`）。loader を作った thread の client は loader の中（`net_loader_own_client`、今までの `net_loader_*` の口はこれを使う）、他の thread は `net_loader_client_open`・`_close`（最大 96）。request は client に属し、client の live の一覧（loader の lock の下）に入る。終わった request は client ごとの completed の一覧へ、client ごとに notify（network の thread が notify を呼ぶ間は `telling` で close を待たせる）。waited の request（`net_loader_client_start_waited`・`net_request_wait`・`net_request_release`・`net_request_take_response`）は network の thread が finished の印を付けて condition を broadcast し、dispatch が要らない。`net_request_abort`（どの thread からも）と `net_loader_client_abort_before(client, epoch)`（`net_loader_client_set_epoch` で request の epoch）は印だけを付け、持ち主の thread が callback 無しで放す（waited は ECANCELED で起きる）。
- page（`page/network.c`）: `page_fetch_now`・`page_fetch_response_now`（loader のある頁の http・https は waited の request、それ以外は今の `page_fetch`）。替えた所: 同期の XHR（`script_fetch_sync`、`NET_FETCH_SCRIPT_API`）、先取りに無い parser の script（`script_fetch_file`）、iframe の `<script src>`（`frame-load.c`）、`BROWSER_FETCH_DEFAULT` の最初の頁（`page_load_location`）。`net/http.c` の同期の道は loader の無い view（`BROWSER_FETCH_AT_ONCE`）と loader の無い頁だけ。`page_wait_sheets` は前から loader の condition の待ちで socket に触れないので変えていない（design.md §7）。
- 確かめ（host）:
  - `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan`: exit 0、warning 0。
  - 新しい `sh plan/ws345/tests/host-loader-client.sh plain`・`tsan`: 23 checks、0 failed、tsan の報告 0。2 つの thread の client の callback がそれぞれの thread で走る、waited の request の response、別の thread の abort から waited の request が起きるまで 0.2 ms（abort は wait の始めから 200 ms 後、上限 100 ms）、epoch より前の request だけが callback 無しで消える、request の残る client の close がすぐ戻る。connect を `-Wl,--wrap=connect` で数えた: 同期の XHR・document.write の script・iframe の script・最初の頁の view で connect 7 回が全部 `browser-net`、他の thread 0（受け入れの「page の code が `net_http_fetch` を呼ばない」をこれで確かめた。対照: `BROWSER_FETCH_AT_ONCE` の view は自分の thread で connect する）。
  - `stall-probe`（40 s）: sync-xhr-connect 30027 ms、docwrite-connect 30098 ms、sync-xhr-read 30024 ms で loader の時間切れ（30 s）で終わる（前は connect が 40 s を超えても止まったまま）。窓の loop がこの間待つのは p005 まで残る。
  - 段 1（前 `git archive d635cd1027`、後 `git archive f18b46f437`、build/tmp/p8s3）: 前後とも host-build warning 0・102 本、host-* 82 exit 0・17 other で試験ごとに同じ、js 19/19、loader 11/11、http 15/15、http-async 19/20（同じ async-sheets）、fetch 38/38、component 83 PASS。
  - tsan: `run-loader-tests.py` で host-loader 11/11、host-back-forward・host-script-navigation exit 0、報告 0。
  - `sh plan/ws334/tests/cross-build.sh`（e15de54cc0）: PASS。
- 未実施: asan の variant、QEMU・実機・Wine（この Phase の変更は libbrowser の中で、cross-build は通した）。
