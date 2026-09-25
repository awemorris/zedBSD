<!-- awesome-plan project=zedbsd record=ws034p047 -->

# ws034-p047: shell の job 表と `kill %N`

Phase ID: `ws034-p047`
Parent: [WS034](../ws.md)
Status: **cleared**（q352-i01、2026-09-24）
Phase disposition: normal
Queue: q352（q352-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー指示「シェルの kill %N は修正してください」。shell は最後の job 1 つ（`last_job`）しか覚えておらず、
`jobs` は `[pid] active or stopped` と出すだけ、`kill` は builtin でないので `%N` を解決できなかった。

## 変更

- `userland/base/sh/jobs.c`・`jobs.h`（新規）: job の表（32 件、1 件に 16 process）。
  - 番号は 1 から。消えた job の番号だけを再利用する（既存の最大＋1）。
  - 前面で止まった job を再び止めても番号は変わらない（process group で同じ job を探す）。
  - `%N`・`%`・`%%`・`%+`・`%-` を解決する。current job は、止まっている job を優先し、次に最も新しく始めた／止まった job（POSIX の規則）。
  - `sh_job_poll()` が `waitpid(WNOHANG | WUNTRACED)` で終わった・止まった process を集め、全部終わった job を消す。
    job の status は pipeline の最後の process のもの。
  - `jobs [-l | -p] [job ...]`。
- `userland/base/sh/main.c`:
  - `last_job` を表に置き換えた。`$!` は `last_background` が持つ。
  - 背景で始めると `[N] pgid`、前面で止まると `[N]+  Stopped    pgid`。対話 shell は prompt の前に、終わった job を `[N]+  Done`・`Exit n`・`Signal n` と報告する。
  - `fg [job]`、`bg [job ...]`、`wait [pid | job ...]`（引数なしは止まっていない全 job。知らない process・job は 127）。
  - **`kill` を builtin にした**: `kill [-s sig | -sig] pid | job ...`、`kill -l [status]`、`kill -0`。job には process group へ送る。
    止まっている job に TERM・HUP を送るときは CONT も送る（止まったままでは受け取れない）。
  - signal 名の表を関数の外へ出し、`kill -l` と共用した。

## 検証（SSH ハーネスのゲスト、amd64）

| 試験 | 結果 |
| --- | --- |
| 非対話（`sh /tmp/a.sh`）: `jobs` の一覧と `+`・`-`、`kill %1` → `wait %1` が 143、`kill %%`、`wait`、`kill -l`、`kill -l 143` → TERM、`kill -0 $$`、無い `%5` は失敗、`kill -s KILL %-` → 137、`jobs -p` | 全部期待どおり |
| 対話（serial の console、`plan/ws034/tests/sh-jobs-interactive.py`）: Ctrl-Z で `[1]+  Stopped`、`jobs`、`bg` で `[1]+ pid &`、`Running`、`sleep 1 &` が終わると次の prompt で `[2]+  Done`、`fg %1` と Ctrl-C で終わる、表が空 | **11/11**（1 回目は test の正規表現が `[2]+` の `+` を許さず 10/11。shell の出力は正しかった） |
| i386 の既定 rootfs | build できる（warning 0） |

## 削除した試験

`plan/ws001/tests/qemu-shell-job-control.sh` は旧出力（`[pid] stopped`、`[pid] active or stopped`）と古い起動手順
（`build/amd64/hdd-image.img`、`rg`）に依存していた。2026-09-24 のユーザー指示（動かない試験は書き直さず削除）に従い削除した。
同じ内容は `sh-jobs-interactive.py` が確かめる。
あわせて `plan/ws001/tests/shell-job-control-test.sh` と補助の `sh-job-control-{hooks,probe,pty}.c` も削除した。
この host 試験は変更前の source でも compile できなかった（`uapi/hosted.h` が見つからない。refactor で header の置き場所が変わった）。
README の記述も直した。

## 残り

- `%string`（command 名での指定）と、`jobs` での command の文字列の表示はしていない（pipeline の source 文字列を持っていない）。
