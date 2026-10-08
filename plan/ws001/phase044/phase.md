<!-- awesome-plan project=zedbsd record=ws001-p044 -->

# ws001-p044: ps の XCU の形（台帳 #98）

Status: test-wait（2026-10-09 P1: 実装、host 15/15、zedBSD の build warning 0。guest は T1）
Parent: [WS001](../ws.md)
Queue: Q1 の dispatch（P1、2026-10-09、ベータ3 の合間の仕事。Q1「ws001-p044 として ps の XCU の適合（-o の field=header、既定の選択、-f の args、field の幅）を足して進めて。WS001 は合計 4 LW の上限」）
前提: [BUG-274](../../bugs/BUG-274.md)（kernel の command line と `KERN_SYSTEM_GET_PROCESS_ARGUMENTS`）。

## 範囲

1. -o: 区切りは comma か blank。`name=header` の header は引数の終わりまで（comma・blank を含む）。-o を何度でも。全部の header が空なら header の行を出さない。
2. XCU の field の名前と header: ruser RUSER、user USER、rgroup RGROUP、group GROUP、pid PID、ppid PPID、pgid PGID、pcpu %CPU、vsz VSZ、nice NI、etime ELAPSED、time TIME、tty TT、comm COMMAND、args COMMAND。前からの uid・gid・sid・state・stat・pri・ni・command も残す。
3. 幅: 列は header と一番広い値の幅。数は右、文字は左に揃え、最後の列は詰めない。time は `[dd-]hh:mm:ss`。
4. 選択: -A・-e は全部。-a は端末のある process（session の長を除く）。-d は session の長を除く全部。-p・-g・-u・-U・-G の list（user・group は名前も可）。これらは足し合わせる（どれかが選べば出す。前は AND）。どれも無い時は、呼び手の実効 user で、呼び手の session（端末の代わり）の process。
5. -f は `UID PID PPID TTY TIME CMD`（CMD は command line）、-l は `S UID PID PPID PRI NI SZ TTY TIME CMD`、両方なら CMD が command line。-n は受けて無視する。

## kernel に無い物（記録、直していない）

- pcpu と etime: kernel は process の始まりの時刻を持たないので `-` を書く。
- ruser・rgroup・-U・-G: process_info は実効の uid・gid だけなので、実の物の代わりに実効の物を使う。
- tty: kernel は端末の有無しか言わないので、`tty` か `?`。-t（端末の名前の list）は受けない（usage で 1）。
- XSI の -f・-l の F・C・STIME・ADDR・WCHAN の列は出さない。

## 実装

`userland/base/ps/main.c` を書き直した（coding-style の全文、style-check 0）。

## 確かめ

- host: `sh plan/ws001/tests/ps-host-test.sh` → 15 の case が全部 ok、`ps-host: PASS`。`plan/ws001/tests/ps-host-fake.c` は偽の /dev/system（GET_PROCESS・GET_PROCESS_ARGUMENTS）で、main.c の open・close・ioctl・geteuid・getsid を名前の置き換えで差し替える。5 つの process（init、呼び手の shell と sleep（session 100）、1 日を超えた root の daemon、command の無い kernel の process）で、次を確かめる。
  - 既定の選択
  - -A の args
  - `pid=`・`comm=` で header の行が無い
  - header の中の comma
  - blank の区切り
  - -f
  - -u の名前
  - -d と -a
  - list の和
  - 日のある time
  - pcpu・etime の `-`
  - 知らない field と operand が 1
- zedBSD: `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p1-ws177 build/p1-ws177/bin/ps` は -Werror で warning 0。
- guest: 未実施（T1）。
