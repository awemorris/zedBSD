<!-- awesome-plan project=zedbsd record=ws177-p032 -->

# ws177-p032: printd の IPP の Print-Job の本体を chunked で（BUG-271 の続き）

Parent: [WS177](../ws.md)
Status: test-wait（T1-470、2026-10-08 夜 Q1。実の printer の IPP は未実施＝ユーザーの判断）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: Q1 の投入（2026-10-08 夜「printd の Print-Job の本体を chunked で送る直しを進めてよい（ws177 の印刷の Phase の新しい attempt …）」「chunked の直しは小さく mock の試験だけで済ませ（実機は「未実施」と記録）」）。ws177-p022 は cleared のため、新しい Phase にした。
Origin: [BUG-271](../../bugs/BUG-271.md)、T1-460・T1-466。

## 調べ（2026-10-08 P2）

- T1-466 の後、host から読み取りだけの IPP Get-Jobs で、実の printer（Brother MFC-L3770CDW）に job 213 が job-state=5 processing・reasons=job-incoming・job-printing・processing-to-stop-point（document の続きを待つ）、job 214 が pending で残っていた。ユーザーの許可で Cancel-Job（213 は受理、214 は既に aborted）。213 はその後も残ったが、ユーザーの確認では printer の queue には無かった（後に消えた）。
- printd は Print-Job を HTTP/1.1 の Content-Length（IPP の message＋file）で一度に送っていた。IPP（RFC 8010）は printer に chunked の受け取りを求め、CUPS も document を chunked で送る。

## 実装（2026-10-08 P2）

- `printd/ipp.c` `ipp_exchange`: document のある request は `Transfer-Encoding: chunked`（IPP の message を 1 つ目の chunk、file を `pd_send_file_chunked` で chunk ごと、最後に長さ 0 の chunk）。document の無い request（Get-Printer-Attributes・Get-Job-Attributes・Cancel-Job）は今までどおり Content-Length。
- `printd/net.c`・`printd.h`: `pd_send_file_chunked`（取り消しは chunk の間で、`pd_send_file` と同じ）。
- Get-Job-Attributes の `job-state-reasons` を読み（`ipp_add_reason`、keyword を comma で 128 byte まで）、job の state か reasons が変わるたびに syslog に `job N printer state S reasons R`。
- busy（0x0507）の答えでも job-id があれば、送り直さずにその job を見る（printer が job を作ったのに送り直して 2 つ目を作るのを防ぐ）。
- mock（`plan/ws145/tests/mock-printers.py`、AAT も使う）は chunked の本体も読み、Print-Job の本体の来方を `ipp-N.transfer` に。`printd-test.py` に `ipp-chunked`。p022 の mock（`plan/ws177/tests/mock-printers-q.py`）も chunked の本体を読む（読まないと printd の送信を待って止まった）。

## 確認

- host（mock）: `sh plan/ws145/tests/run-host-printd.sh` → printd-test PASS（ipp-document の SHA-256 一致、ipp-chunked、LPD・名前・取り消しも従来どおり）。`sh plan/ws177/tests/host-printd-q.sh` → PASS（同時の数・取り消し・応答の上限・octet-stream・名前解決の 15 項目）。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/keiland-printd` exit 0・warning 0。style-check（ipp.c・net.c）0。
- QEMU（mock の printer、AAT apps.settings.printers）: 未実施（T1 に依頼する時は T1-465 と同じ手順で IPP の印刷が state=4 になること）。
- 実機（Brother、IPP）: **未実施**（2026-10-08 ユーザー「ネットワークプリンタはこれしか持ってないので、今はIPPのテストは難しいかも？LPDが動く事実があればいいよ」→ Q1: 実機の IPP の再試験はしない）。
