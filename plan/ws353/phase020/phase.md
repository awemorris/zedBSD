<!-- awesome-plan project=zedbsd record=ws353-p020 -->

# ws353-p020: sessiond が入力の device の node を利用者に chown するのをやめ、1 s ごとの giving を事象だけに

Status: in-progress（2026-11-14 Q1 起票、P1 q2083）
Parent: [WS353](../ws.md)

## 目的

2026-11-14 ユーザーの決定（ws.md）: sessiond が入力の device を開き fd を SCM_RIGHTS で compositor に渡す（p018）ので、入力の device の node を利用者に chown する必要は無い。sessiond を通さず compositor が node を直に開くのは Linux（logind がある）と FreeBSD だけで、FreeBSD では /dev/input が chown されている前提で進める。

今の sessiond（`userland/desktop/sessiond/seat.c` の `sessiond_seat_give`）は、greeter と session の間、`/dev/input/event*` を利用者に chown・chmod し、greeter は 1 s ごと（`GREETER_SEAT_MS`）、session は loop の毎回、それを繰り返して新しい device を拾う（node ごとの stat）。

## 範囲

1. `sessiond_seat_give`・`sessiond_seat_restore` から `/dev/input/event*` の chown・chmod を外す。event の node は devfs の既定（root・wheel の mode）のまま。compositor は p018 の `OPEN eventN` で受け取る。sessiond を通さない時の compositor の hotplug の thread が node を直に開く道（FreeBSD 向け）は変えない。
2. 残る giving（GPU・backlight・hidraw（security key）・smart card・camera）を点検し、hotplug で増えうる物（hidraw・camera・smart card など）は `/dev/system` の事象（`sessiond_seat_events_collect`）が来た時だけ与える形にする。greeter の `GREETER_SEAT_MS` の 1 s の見回りと、session の loop の毎回の giving を消す。事象の届かない device の種類があれば、根拠（kernel の事象の class）を書いて Q1 へ（勝手に見回りを残さない・kernel を変えない）。
3. sessiond の loop の poll の timeout・`sleep(1)` が giving のためだけにある所は無くす。他の締め切り（greeter の起動の待ちなど）のための timeout は残してよいが、一覧を結果に書く。
4. host 試験（sessiond の既存の試験があれば追従）、build 3 機種（x86_64・arm64・riscv64）warning 0（`-Wall -Wextra -Werror` を足した `-fsyntax-only` でも 0 を確かめる。package の compile は今それを付けていないため）。QEMU: login（greeter → session）・入力（keyboard・pointer）が効く・USB の入力 device の後からの挿し込みが効く（QEMU の monitor の device_add）、ls -l /dev/input で event の node が root のままである事。

## 規則

WS353 の design.md と p018 の phase.md に従う。kernel・HAL・Makefile は変えない。

## 結果

（担当が書く）
