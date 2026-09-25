<!-- awesome-plan project=zedbsd record=ws034p045 -->

# ws034-p045: fork した子の program 名

Phase ID: `ws034-p045`
Parent: [WS034](../ws.md)
Status: **cleared**（q339-i01、2026-09-24）
Phase disposition: normal
Queue: q339（q339-i01）
実行: メインセッション

## 問題

`fork()` した子は、exec するまで親と同じ program を走らせるのに、process の名前（`command`）が空のままだった。
`ps` はそれを `kernel` と表示した（ws040-p006・ws034-p042 で観察）。exec は名前を入れるが、`process_fork()` は
写していなかった。

## 修正

`src/kern/process.c` の `process_fork()` で、`command` と `command_initial` を親から写す。

## 検証

ゲストで、fork だけして exec しない子を持つ `tcp-bulk` を背景で走らせ `ps -o pid,ppid,comm`:
子（pid 62）は `/root/tcp-bulk`、背景 job の subshell（pid 59）は `-sh`。修正前はどちらも `kernel` だった。
amd64 `disk-image` PASS、`cow-stress` PASS。

## あわせて直した試験

`plan/ws019/tests/console-csi-test.py` が build できなくなっていた（`tty_render()` が `hal_cons_write_n()` ではなく
`tty_console_puts()` を呼ぶようになったのに、試験の stand-in が古いままだった）。stand-in を合わせて PASS（77 case）。

VM の host 試験（`plan/ws025/tests/run-user-lease-host.py`・`private-upgrade-host.c`・`run-file-cache-host.sh`）は、
もう無い API（`vmspace_user_lease_acquire`、`kern_pmem.vaddr` など）を使っており、今の VM の設計に合わせた
書き直しが要る。ここでは直していない。
