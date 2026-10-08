<!-- awesome-plan project=zedbsd record=ws177-p024 -->

# ws177-p024: 印刷の堅牢化の 3 — 設定の writer の thread、title の検め、dispatch の層の試験（案 Q）

Parent: [WS177](../ws.md)
Status: test-wait（T1-457、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q905（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 126・143（ws145-p003）、[案](../phasing-20261008.md) の Q。設計は [ws145 の design.md](../../ws145/design.md) §3・§4・§7。

## 実装（2026-10-08 P2）

- **設定の file の writer の thread**（`libkeiland-backend/print/print.c`）: add・remove・set_default と printd の PATH・NAMED は変更（`struct print_change`）として writer の thread に渡す（最初の変更で起動、待ちは 16 まで、超えれば BUSY）。thread は file の lock（flock）を取り、読み直し、変え、書き、表ごと返す。compositor の thread は `kl_backend_print_update` で受け取って表を差し替え、result を出し、IPP の printer なら NAME を printd に、remove ならその printer の終わっていない job を取り消す。別の session が lock を持っていても compositor の thread は止まらない。表は `struct print_table` にまとめた（`print_load`・`print_save` は表を引数に）。close は待ちの変更を書き終えてから thread を止める。
- **job の title の厳密な検め**（`wayland/printers-shell.c` の `printers_title_ok`）: UTF-8 として正しい（overlong・surrogate・U+10FFFF 超・途中で切れた列を断る）、C0・DEL・C1 が無い、127 byte まで。前は C2 80〜9F だけを見る緩い検めだった。
- **dispatch の層の試験**（backlog 143）: `plan/ws177/tests/host-dispatch-kinds.py` が source を読み、`enum kwl_kind` の全ての kind に protocol.c の `kwl_dispatch` の case があること（wl_callback を除く）、group で渡す先の handler（`kwl_system_request` など）の `switch (object->kind)` が渡された kind を全部持つことを確かめる。T1-306 の型（KWL_SYSTEM_PRINTERS の case の欠け）を捕まえる（protocol.c の写しから case を消すと FAIL を確かめた）。
- **printers の object の wire の試験**（backlog 126 の host の protocol の試験）: `plan/ws177/tests/host-printers-shell.c` が printers-shell.c と print.c を代わりの printd（`fake-printd.py`）で動かし、wire の byte で要求する。libkeiland の client の側は既存の T1 の QEMU（printtest・Settings）で通る。

## 確認

- host（新規）: `sh plan/ws177/tests/host-printers-shell.sh` → PASS（作った時の done、add の OK と printer の event、title の断り（途中で切れた UTF-8・overlong・surrogate・C1）で INVALID と fd が閉じる、128 byte の title は wire の上限で EPROTO、fd がまだ無い print は EAGAIN、日本語と絵文字の title は queued の後に OK、知らない opcode は EPROTO）と `host-dispatch-kinds.py` PASS。`sh plan/ws177/tests/host-print-life.sh` に「別の session が lock を持つ間の add は呼び手を止めず、lock が外れてから答える」を足して PASS（8 項目）。
- host（既存を追随）: `plan/ws145/tests/host-print-backend.c` の `take` を update しながら待つ形に（結果が writer の thread から来るため）、next-id の前に答えを待つ → `run-host-print-backend.sh` PASS。`plan/ws131/tests/host-system.sh` PASS。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/wayland` exit 0・warning 0、`make -j16 keiland-linux` exit 0。style-check 指摘なし。
- QEMU: 未実施（p022〜p024 をまとめて T1 に）。
