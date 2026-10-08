<!-- awesome-plan project=zedbsd record=ws177-p022 -->

# ws177-p022: 印刷の堅牢化の 1 — printd の同時の数・名前解決・応答の上限・取り消し（案 Q）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 夜 Q1: T1-457 apps.settings.printers を 2 回 fail なし、IPP・LPD・PDF Viewer の Ctrl+P が state=4、mock の受けた file が試料と一致。並行・名前解決・取り消し・寿命は host の試験）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q905（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 124 と 125 の printd 側（ws145-p002）、[案](../phasing-20261008.md) の Q。設計は [ws145 の design.md](../../ws145/design.md) §5.3〜§5.5。

## 決めたこと（2026-10-08 P2、Q1 了解）

- **設計 §5.3 からの逸脱**: 「printd は 1 つの thread の poll の loop」は採らず、今の job ごとの thread（blocking I/O と timeout）のまま、main の thread が同時に送る数を数えて抑える。理由: 外から見える要件（同時の数、名前解決の 10 秒、応答の上限、取り消しの速さ）は thread のままで満たせ、IPP・LPD を非同期の状態機械に書き直す危険（printd の全部の書き直し）に見合う利点が無い。design.md §5.3 に注記した。
- 同時の送信: printer（protocol・host・port）ごとに 1、全体で 4。spool に受けた job は受けた順に、枠が空くまで待つ。IPP の job は printer が受け取った（Print-Job の応答）時点で枠を返し、見張り（Get-Job-Attributes）は数えない。待っている job の CANCEL は直ちに `STATE cancelled`（printer には届かない）。
- 名前解決: 数字の address は `AI_NUMERICHOST` でそのまま。名前は補助の thread（同時 4 つ、detach）で、場所の待ちを含めて 10 秒。答えが来なければ `failed timeout`（補助の thread は後で自分で片付ける）。名前が無ければ `unreachable`。
- 応答の上限: 行 1024 B、header 16 KiB（超えれば `failed protocol`）、1xx を読み飛ばす、本体は Content-Length・chunked・接続の終わりのどれでも、流しながら IPP を解き（値は 1024 B まで残し、長い値は読み捨て）、属性の名前は 256 個まで見る（後は読み捨て）、本体は 16 MiB まで。
- 取り消しの速さ: busy の 30 秒の待ちと見張りの 5 秒の待ちは 1 秒ごとに取り消しを見る（busy の待ちの間の取り消しは printer が受け取っていないので `cancelled`）。
- LPD（§5.5）: data file の応答の後の取り消しは subcommand `\001\n`（abort job）を送って閉じる → `cancelled`。data file の途中は閉じるだけ → `cancelled`。control file を送った後の取り消しは `failed unconfirmed`。
- protocol の異常（§5.1）: 知らない命令の語、fd の無い JOB の行で printd は終わる（backend が起動し直す、p023）。

## 実装（2026-10-08 P2）

- `userland/desktop/printd/printd.h`・`main.c`（`pd_schedule`・`pd_place_free`・`pd_start_job`、`pd_sent`・`pd_wait`、`pd_command`・`pd_job` が終わりを返す）、`net.c`（`net_resolve`・`net_lookup_thread`・`net_lookup_release`）、`ipp.c`（`ipp_read_header`・`ipp_read_body`・`ipp_line`・`ipp_feed_from`、流しの decoder `ipp_stream_*`、前の `ipp_parse` と 256 KiB の応答の buffer を除いた）、`lpd.c`。

## 確認

- host（新規）: `sh plan/ws177/tests/host-printd-q.sh` → PASS（ASan・UBSan の printd と、遅い resolver を preload する素の printd。`mock-printers-q.py` の 6 台の IPP と LPD）: 1 台に 2 つは 1 つずつ、6 台に 6 つは同時 4 つ、待っている job の取り消しは即座で printer に届かない、busy の待ちの取り消しは 5 秒以内、全部の属性の応答（100 Continue・chunked・400 個の 2000 B の値）で印刷できる、20 KiB の header は `failed protocol`、LPD の abort と control の後の `failed unconfirmed`、socket の終わり、知らない命令・fd の無い JOB で終わる、答えない名前は約 10 秒で `failed timeout`。
- host（既存）: `sh plan/ws145/tests/run-host-printd.sh build/ws177-p022/keiland-printd` → PASS。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/keiland-printd` exit 0・warning 0。style-check 指摘なし。
- QEMU: 未実施（p022〜p024 をまとめて T1 に）。
