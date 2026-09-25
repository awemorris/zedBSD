<!-- awesome-plan project=zedbsd record=ws064p001 -->

# ws064-p001: make の `-j` の設計と実装

Phase ID: `ws064-p001`
Parent: [WS064](../ws.md)
Status: cleared
Queue: q448-i01（cleared）

## 目的

`make -j N` で独立した target の command を N 個まで同時に走らせる。再帰の make とは GNU make 互換の jobserver（`MAKEFLAGS` の `-jN --jobserver-auth=R,W`、pipe の token）で job の数を分け合う。

## 受け入れ

make の差分試験 91 件と新しい `-j` の case。expat の `make -j4` が正しい成果物を作る（`make check` の結果が直列と同じ）。

## 設計（2026-09-26）

| 項目 | 内容 |
| --- | --- |
| 更新の進め方 | `update.c` を「歩き」にした。goal の graph を歩き、前提が済んだ target の recipe を job の空きがある限り始め、前提か recipe が走っている target は pending（`TARGET_PENDING`・`TARGET_RUNNING`）。recipe が 1 つ終わるたびに goal から歩き直す（済んだ target はすぐ返る）。job の空きが無い target に出会ったらその先の兄弟を見ない。これで `-j1` の時の target を見る順番と時点（`stat`・implicit rule の探索）が直列の make と同じになる |
| scope | target の変数の scope は初めて見た時に target に持たせる（`target->scope`・`own_scope`）。歩き直しでも同じ scope を使う |
| 循環 | 歩いている path 上の target（`in_walk`）に再び出会ったら `Circular a <- b dependency dropped.` を出し、その依存に `dropped` を付けて以後は辿らない（1 度だけ出す） |
| job | `job.c` の `struct job` が recipe の行・展開・今の command・子・token を持つ。行は着いた時に展開し（今までどおり）、1 command ずつ子の shell で走らせる。子の終わり（`waitpid(-1)`）で次の command か recipe の終わりへ進む。recipe が終わると `update_recipe_done` が target の時刻を読み直して done にする。割り込みは走っている全 job の target を消す |
| jobserver | GNU make と同じ形。`-jN`（N > 1）の make は pipe を作り N−1 個の token（`+`）を入れ、`MAKEFLAGS` に ` -jN --jobserver-auth=R,W` を書く。make は 1 つの job を token 無しで持ち、2 つ目からは token を取る。token は job の終わりに pipe へ返す。walk で token が要った時だけ待ちで token を読み、要らなかった token は待つ前に返す（持ち続けない）。読みは pipe の複製（`dup`）に対して行い、`SIGCHLD` の handler がその複製を閉じる（子が終わったら読みから戻る）。handler は読みの間だけ置く（stdio の書き込みを `EINTR` にしない） |
| 継承 | `MAKEFLAGS` の `--jobserver-auth=R,W`（と古い名前の `--jobserver-fds`）を読み、descriptor が開いていれば使う。無ければ `warning: jobserver unavailable: using -j1.  Add '+' to parent make rule.`。自分の command line に `-jN` がある make は新しい jobserver を作る（`warning: -jN forced in submake: resetting jobserver mode.`）。`-j` だけ（数無し）は上限無し、jobserver 無し |
| 失敗 | `-k` 無しで recipe が失敗したら、goal はその時点で失敗し、走っている job の終わりを待つ（`make: *** Waiting for unfinished jobs....`）。`-k` なら作れるものを作る |
| `.WAIT`・`.NOTPARALLEL` | 前提の並びの `.WAIT` は target にせず、次の前提に「前が済むまで待つ」印を付ける。`.NOTPARALLEL:`（前提無し）は全体を直列に、前提付きはその target の前提を 1 つずつ |
| MAKEFLAGS の読み | `-` で始まる語は `=` を含んでも option として読む（`--jobserver-auth=3,4` を変数と取り違えていた） |

## 進行の記録（q448-i01、2026-09-26）

| 確認 | 結果 |
| --- | --- |
| host の差分試験（GNU make 4.4 と比べる、host で build した make） | 直列の既存 91 件 91/91。新しい `cases/parallel.sh` 9 件（`-j4` の独立な target、chain の順、`-j`・`--jobs=N`、再帰の make の jobserver、`.WAIT`、`.NOTPARALLEL`、失敗の後の待ち、`-k -j`、`-j1`）9/9 |
| host の時間（sleep 1 の target 4 つ、再帰の make を含む） | `-j1` 4.0 秒、`-j2` 2.0 秒、`-j4` 1.0 秒。再帰の make 3 つを `.WAIT` で順に: 3.04 秒（GNU make 3.04 秒）、並べて 2.02 秒（GNU 2.02 秒）。token は失われない |
| host の expat の build | この make の `-j4` 5.08 秒、GNU make の `-j4` 5.06 秒、この make の `-j1` 15.3 秒 |
| guest の差分試験（QEMU 8 GiB 4 vCPU NVMe、`/root` に置いて） | 100/100（parallel 9/9 を含む） |
| guest の expat | `make -j4` が通り、`tests/runtests` は `-j4` の build も直列の build も 4932 検査すべて通る（`make check` 自体は test driver が bash を要り、両方とも走らない。ws046-p014 の既知の制限） |

## 結果（2026-09-26、cleared）

base の make が `-j` で並列に recipe を走らせ、再帰の make と GNU make 互換の jobserver で job の数を分け合う。guest の expat の `make -j4` は 7.3〜7.8 秒（直列 12.3 秒、host `-j4` 5.06 秒）で、性能は [ws064-p002](../phase002/phase.md)。実機は未実施。

