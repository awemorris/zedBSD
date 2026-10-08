<!-- awesome-plan project=zedbsd record=ws177-p023 -->

# ws177-p023: 印刷の堅牢化の 2 — backend の printd の寿命の表（案 Q）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 夜 Q1: T1-457 apps.settings.printers を 2 回 fail なし、IPP・LPD・PDF Viewer の Ctrl+P が state=4、mock の受けた file が試料と一致。並行・名前解決・取り消し・寿命は host の試験）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q905（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 125 の backend 側（ws145-p003）、[案](../phasing-20261008.md) の Q。設計は [ws145 の design.md](../../ws145/design.md) §4 の状態遷移の表と §5.1。

## 実装（2026-10-08 P2、`userland/desktop/libkeiland-backend/print/print.c`）

- printd の EOF（`print_stopped`）: 終わっていない job を表のとおりに。取り消し中（CANCEL を送った）→ CANCELLED。JOB を送ったが ACCEPTED の前で document を持っている → 新しい printd に送り直す（`print_resend`・`print_send_job`、1 job 2 回まで）、超えたら FAILED daemon。ACCEPTED の後 → FAILED daemon。
- 起動の制限: BYE の後でない EOF を数え、10 秒に 3 回で 60 秒は起動しない（その間の依頼は FAILED daemon）。printd の `FATAL` も 60 秒休む。
- protocol の異常: printd から fd が来た（閉じる）・`MSG_CTRUNC`・1024 B を超える行 → `shutdown(SHUT_WR)` して以後の行を取らず、EOF を 10 秒待つ（来なければ socket を閉じる）。その後に送り直す（古い printd と新しい printd が同じ job を送らない）。受信は `recvmsg`（`MSG_CMSG_CLOEXEC`）。
- 取り消し: JOB の行の 1 byte も送っていない job だけを送信の queue から抜いて CANCELLED（前は一部を送った行も抜けて protocol が壊れうる不具合）。送った job は常に CANCEL（ACCEPTED の前でも）で CANCELLING に、REJECTED は CANCELLED。
- fd の所有: document の fd は job だけが持ち、送信の queue の行は借りるだけ（前は `kl_backend_print_close` と printd の EOF で同じ fd を 2 度閉じうる不具合）。

## 確認

- host（新規）: `sh plan/ws177/tests/host-print-life.sh` → PASS（ASan・UBSan、代わりの printd `fake-printd.py`）: 2 回の crash の後 3 回目で印刷、3 回 crash で FAILED daemon と 60 秒の休み、FATAL で休み、fd を返す printd を shut して送り直し、REJECTED と EOF での取り消し。直しの前の print.c では 7 項目とも FAIL。
- host（既存）: `sh plan/ws145/tests/run-host-print-backend.sh build/ws177-p023/host-print-backend` → PASS。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/wayland` exit 0・warning 0、`make -j16 keiland-linux` exit 0。style-check 指摘なし。
- QEMU: 未実施（p022〜p024 をまとめて T1 に）。
