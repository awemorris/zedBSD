<!-- awesome-plan project=zedbsd record=ws332-p004 -->

# ws332-p004: 規約の全文の見直し

Status: cleared（2026-10-31 P6）
Parent: [WS332](../ws.md)
Queue: q1738-i01（P6、p003 の後）

## 範囲

WS332 が変えた C（p002 の fe1d64379..b3b05b2be の .c・.h 93 file と p003 の 13 file）を plan/coding-style.md の全文と照らし、
WS332 が書いた・書き換えた文と、その段落・関数の形だけを直す。動作は変えない。file の他の所の既存の不適合は直さない（WS332 の範囲の外）。

## 結果（2026-10-31 P6）

見直した物: p002 の diff（`git diff fe1d64379 b3b05b2be -- '*.c' '*.h'`、約 2,800 行）と p003 の diff を全部読み、coding-style.md の §2〜§11・§14 と照らした。
直した物（書き方だけ。値・評価の順・副作用は同じ）:

| 規則 | 所 |
| --- | --- |
| §11 意味のある関数の結果を直に return しない | clock.c の `clock_milliseconds()` と変換の 7 関数（`kern_ms/us/ns_to_ticks`・`kern_ticks_to_ms/us/ns`・`kern_ticks_to_rate`）、powervr render-queue.c・render-target.c の `ms_ticks()`、ghci display-scan.c の `ghci_scan_now_ns()`、usb.c の `drv_usb_trace_now_ns()`・disk.c の `disk_stats_now_ns()`（counter の無い時の分岐）、pci-nvme.c の `nvme_timeout_ticks()`（条件演算子と「Returns the computed result.」も） |
| §4 初期化子で関数を呼ばない | pci-nvme.c の `nvme_timeout_ticks()` |
| §6 条件の中で関数を呼ばない・条件演算子 | net/core.c の `wlan_retirement_worker()`（複数行の条件演算子）、usb-rtl8822bu.c の `rtl8822bu_log_error()`（同）・`rtl8822bu_h2c_packet_send()`、intel-ax211-pci.c の `ax211_pci_log_rejection()`、venus transport.c の `venus_worker_stop()`、powervr display.c の `flip_report()`・render-queue.c の `timing_report()`、random.c の `kern_random_read()`（呼び出しは副作用の無い時計と変換なので先に読んでも同じ）、vm/reclaim.c の `vm_reclaim_frame()`、userland/base/wifi/main.c の `wait_for_connection()` |
| §8 引数の中に入れ子の呼び出し | powervr firmware-boot.c の `watch_thread()` の log の秒 |
| §2 公開の関数は static の前 | clock.c の `kern_usleep_range()` を static の `realtime_write_begin()` の前へ移した |
| §3・§11 公開の関数の comment と return の comment | plan/ws220/tests/tcp-model/shim.c の変換の stub 9 個 |
| §10 comment の誤り | i915 display.c の `I915_DISPLAY_DMC_FINI_TICKS` の「(500 ticks)」→「(five seconds)」、tcp-model model.h の `MODEL_TICK_HZ` が「Host A …」の comment と定義の間に入っていたのを前へ |

直さずに残した物（理由）:

- 名前が `*_TICKS` の macro（58 個）は p002 で `kern_ms_to_ticks()` の呼び出しに展開する形になり、33 か所で `if`・`while` の条件の中に現れる
  （例 `if (ticks < WLAN_BEACON_WATCH_MIN_TICKS)`）。§6 の「条件の中で関数を呼ばない」の字面には当たるが、名前は p002 で意図して残しており（design.md §3）、
  直すと 33 か所に局所変数を足すことになる。可変の tick（design.md §5）でこれらの macro を見直す時に合わせて扱う。
- `kern_deadline_after(now, kern_ms_to_ticks(...), &deadline)`・`sched_sleep(sched_ticks() + kern_ms_to_ticks(...))` の 1 段の入れ子は、tree で確立した
  書き方で §8 の「深い入れ子」に当たらないとした。
- WS332 が触れていない行の既存の不適合（usb-rtl8822bu.c の「Handles the … condition.」の comment、writeback.c の comment の無い段落など）は範囲の外。

### 確かめ（P6、p003 の追加の 24 か所を含む tree）

- 全 port の vmunix 19 個（p003 と同じ command、BUILD=/tmp/p6-ws332/ports2/<port>）: rc=0、warning 0（要約 /tmp/p6-ws332/ports2/summary.txt）。
- host 試験: `OUT=/tmp/p6-ws332/tcp-model sh plan/ws220/tests/tcp-model/run.sh` → `tcp-model: PASS`、
  `sh plan/tools/i915-display/run-xd-host-tests.sh /tmp/p6-ws332/i915-xd2` → `run-xd-host-tests: all passed`。`git diff --check` は空。
- QEMU（amd64、48219a2e9 の tree）: `sh plan/ws332/tests/boot.sh /tmp/p6-ws332/boot2 amd64`（config-amd64 の disk-image、boot-test.sh の画面の判定）→ `amd64: boot PASS`・
  `ws332-boot: PASS`。画面 /tmp/p6-ws332/boot2/amd64/login.png に `login:` と `init: system running`。image の build の log の warning は openssh の package と
  perl の locale だけで、kernel と userland/base/wifi には無い。
- 未実施: 100 Hz の port（pcat・m68k）と rpi4 の QEMU の起動（p003 の VM の待ちの変更が効く所。今回は amd64 だけを割り当てられた）、実機。
