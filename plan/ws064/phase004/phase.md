<!-- awesome-plan project=zedbsd record=ws064p004 -->

# ws064-p004: `/bin/sh` の process の生成を posix_spawn（vfork）に広げる

Phase ID: `ws064-p004`
Parent: [WS064](../ws.md)
Status: cleared
Queue: q450-i01（cleared）
Disposition: normal

## 目的

2026-09-26 ユーザー「/bin/shのプロセス生成も、vforkやposix_spawnになっているか確認して、最適化をお願いします。」

ws064-p002 の後、sh が posix_spawn を使うのは「前置の代入が無く、job control の無い、foreground の外部 command」だけで、pipeline の要素、command substitution、subshell、background、前置の代入のある command は fork する。どこで fork しているかを数え、subshell の意味（展開の副作用、エラーで終わるのは子）を変えずに posix_spawn に置き換えられる所を広げる。

## 受け入れ

sh の差分試験（host と guest）が前と同じ、make の差分試験 100/100。expat の configure・`make`・`make -j4` が前と同等以上。fork の数が減ったことを数えて示す。

## 進行の記録（q450-i01、2026-09-26）

### 確かめたこと

p002 の後の sh が posix_spawn を使うのは「前置の代入が無く job control の無い foreground の外部 command」だけだった。host で sh の複写に fork と spawn の記録を足し（commit しない）、expat の `make`（GNU make に `SHELL=` でこの sh を渡す）と configure で数えた。

| 構文 | expat の make | configure |
| --- | --- | --- |
| pipeline の要素（`$ECHO "$1" \| $SED ...` の両方） | 580 | 475 |
| subshell（libtool の `func_unset` の `(eval unset $1) >/dev/null 2>&1` が 345） | 429 | 203 |
| command substitution（`$($ECHO ... \| $SED ...)`、`$(eval \$ECHO ...)` が 132 など） | 385 | 284 |
| 前置の代入のある外部 command | 23 | 4 |
| fork の計 / posix_spawn | 1417 / 129 | 966 / ― |

### 変えたこと（`userland/base/sh/`）

| 内容 | 場所 |
| --- | --- |
| pipeline の要素のうち、代入も redirection も無く、語の展開が shell を変えず失敗しない（command substitution・算術・`${...}` の演算子を含まない。`set -u`・`-x`・`-n` の間は使わない）外部 command は、shell で展開して `posix_spawn`（前後の pipe は file action の dup2 と close） | `exec.c`（`pipeline_start`・`pipeline_spawn`・`word_is_pure`） |
| pipeline の最後でない `echo`・`printf`（builtin、関数でないもの）で出力の上限が分かり 4 KiB 以下（pipe は 16 KiB）のものは、shell が次の command を始める前にその pipe へ書く（子を作らない。`pipefail` の間は使わない） | `exec.c`（`pipeline_inline`・`output_builtin`・`output_bound`） |
| command substitution の本文が 1 つの pipeline なら、shell が pipeline を始めて最後の出力を substitution の pipe にし、読み終えてから待つ（間の subshell の fork を省く）。本文が単純 command なら: `echo`・`printf` は shell で pipe へ、外部 command は `posix_spawn`、`eval 語...` は 1 段だけ中身を同じように。どれでもなければ従来の fork。本文の parse は message を出さずに試し（`sh_error_quiet`）、失敗は fork の子が報告する。here-document と job control の間は従来どおり | `exec.c`（`substitution_in_shell`・`substitution_simple`・`parse_quietly`）、`util.c` |
| 本文が `unset 名前...`（直接か `eval` 1 段）だけの subshell は、shell で subshell の redirection の下で unset を走らせ、各変数を元の値と属性に戻す。読み取り専用・watch される変数（PATH・OPTIND）・関数の名前・option・trap の設定中は従来の fork | `exec.c`（`subshell_unset_trial`・`unset_trial`）、`vars.c`（`sh_var_hooked`） |
| subshell のための command の検索は hash に残さない（`SH_FIND_NO_REMEMBER`。fork の子で検索したときと同じく、親の `hash` に現れない） | `command.c`、`shell.h` |
| `sh_spawn` に file action を渡せるようにした | `jobs.c` |

### 結果

fork の数（host で数えた。expat の make / configure）: 1417 / 966 → **214 / 423**。残りは subshell の意味が要るもの（`$(cd "$dir" && pwd)`、変数に代入する機能の試し、`set -o` など）。

| 測定（QEMU 8 GiB 4 vCPU NVMe、各 2〜3 回） | p002 の後 | pipeline・substitution の後 | unset の試しの後 | host |
| --- | --- | --- | --- | --- |
| configure | 8.6〜8.9 秒 | 6.96〜7.21 秒 | 7.11〜7.25 秒 | 10.66〜10.73 秒 |
| `make`（直列） | 11.3〜11.4 秒 | 10.85〜10.90 秒 | 10.69〜10.78 秒 | 15.5〜15.7 秒 |
| `make -j4` | 4.58〜4.74 秒 | 4.13〜4.29 秒 | 4.18〜4.38 秒 | 5.14〜5.18 秒 |

（「p002 の後」の列は make と sh が posix_spawn を使うようになった直後の測定。）

### 回帰

| 確認 | 結果 |
| --- | --- |
| host の sh の差分試験（dash と） | 1417/1425（変更前の同じ試験 1414。差は `var-num` のばらつきのみ） |
| dash との比較（unset の試し: 普通の変数、export の付いた変数、読み取り専用、`func_unset`、`unset -v`） | 同じ出力 |
| guest の make の差分試験 | 100/100 |
| SMP 4/4、COW、vfork の試験、expat の `tests/runtests` 4932/4932 | OK |
| guest の sh の差分試験（NVMe の image） | 1390/1425（p002 の後 1389。差は順序に依存する `noclobber on &> >` の 1 件が通ったことだけ） |
| boot test（`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-ws064p004/login.png`） |

実機は未実施。

## 結果（2026-09-26、cleared）

sh の fork は expat の make で 1417 → 214、configure で 966 → 423。configure 8.6〜8.9 → 7.1〜7.25 秒、直列の make 11.3 → 10.7 秒、`make -j4` 4.58〜4.74 → 4.18〜4.38 秒（host 10.7・15.5・5.14 秒）。規約の見直しは ws064-p003。

