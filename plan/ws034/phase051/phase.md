<!-- awesome-plan project=zedbsd record=ws034p051 -->

# ws034-p051: 1 process の descriptor を 1024 まで

Phase ID: `ws034-p051`
Parent: [WS034](../ws.md)
Status: **cleared**（q367-i01、2026-09-24）
Phase disposition: normal
Queue: q367（q367-i01）
実行: メインセッション

## 経緯

p048（`FD_SETSIZE` を 1024 に）で、kernel の descriptor の表が 32 固定（`KERN_OPEN_MAX`）なので、select の 1024 の集合の大半が
開けない番号になることが分かった。

## 決定

- `KERN_OPEN_MAX` を **1024**（`FD_SETSIZE` と同じ。select がすべての descriptor を指せる）にし、`RLIMIT_NOFILE` の既定と上限も 1024。
- 表は固定の配列ではなく、**32 slot で始まり、空きが無いと倍に伸びる**（`filedesc_grow`）。伸ばすときは lock の外で新しい配列を確保し、
  lock の中で差し替える（他の thread が先に伸ばしていればそれを使う）。slot は lock を持っている間だけ触るので、差し替えで古い配列を
  指したまま残るものは無い。縮めない。1024 slot で 32 KB。
- 1024 の配列を kernel の stack に置かない:
  - reservation（`SCM_RIGHTS` の受け取り・pair）は `FILEDESC_RESERVE_MAX`（16、`KERN_MSG_FD_MAX` 以上を static assert）で上限を別にした。
  - close-on-exec は 32 本ずつ外して閉じる。
  - 表の破棄は、表全体を lock の中で外してから（callback が表に戻っても空に見える）walk する。
  - `ppoll`・`pselect` の `struct pollfd` は 32 本まで stack、それより多いときは heap。
- devfs: `/dev/fd` の 4 桁の名前（`/dev/fd/1000`）を引けるように。一覧の領域の予約は `/dev/fd` だけにした（他の directory の readdir
  が descriptor 1024 本分の領域を確保していた）。
- libc: `getdtablesize()`（`sysconf(_SC_OPEN_MAX)`、つまり `RLIMIT_NOFILE`）。`sysconf(_SC_OPEN_MAX)` は元から `RLIMIT_NOFILE` を返す。
- ついで: sysroot の smoke link の `-static -no-pie` で clang が `-no-pie` 未使用の warning を出していた（sysroot を作り直すたびに 1 件）。
  `-no-pie` を外した。

## 検証

| 試験 | 結果 |
| --- | --- |
| guest の `plan/ws034/tests/openmax-test.c`（`plan/ws034/tests/config-amd64-openmax.mk` の image） | **20/20**（[openmax-guest.txt](evidence/openmax-guest.txt)）: `RLIMIT_NOFILE`・`sysconf`・`getdtablesize` が 1024、1023 まで開いて EMFILE、最小の空きの再利用、`dup2` 1023 と 1024（EBADF）、満杯の `F_DUPFD`、`/dev/fd/1000`、fd 1000 の select、1001 本の poll（heap の経路）、fork で fd 900 を引き継ぐ、500 本の close-on-exec（複数の batch）で 700 は残る |
| `plan/ws014/tests/run-handle-fd-test.sh`（host、`filedesc.c` を直接 compile。reservation・`SCM_RIGHTS`・abort・破棄） | PASS |
| CI の kernel（amd64・pcat・pc98・rpi4） | warning 0 |
| sysroot（amd64・i386）の作り直し | warning 0 |

初回の guest 試験は 19/20 だった。「最初の open が 3」を仮定していたが、試験を起動した shell が fd 3 を開いていて 4 になった（試験の誤り）。
`base >= 3` に直して 20/20。
