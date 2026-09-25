<!-- awesome-plan project=zedbsd record=ws034p002 -->

# ws034-p002: `which`（base独自実装）

Phase ID: `ws034-p002`
Parent: [WS034](../ws.md)
Status: cleared（q323-i02、2026-09-23）
Phase disposition: normal
Queue: q323（q323-i02）
実行: メインセッション

## 範囲

`userland/base/which` に `which` を新規実装する。GNUのwhichは使わず、Zlibの独自実装とする。

## 実装

`userland/base/which/main.c`（Zlib）と `Makefile`。`ZEDBSD_USERLAND_PACKAGE(which,which,*,y,basic,...)` で
既定ONのbasicクラスとして登録し、`/bin/which` に入る。

動作は `execvp()` の探索に合わせた。

- PATHの要素を順に見て、名前をつないだものが**通常ファイルで、実行できる**最初のものを報告する。
- 実行できるかの判定は `faccessat(AT_FDCWD, path, X_OK, AT_EACCESS)`。`access()` は実uidで答えるが、
  実際にexecするのは実効uidなので、kernelがexecで行う判定と同じにした
  （kernelは `sys_faccessat_call` で `AT_EACCESS` を解釈する）。
  `stat()` で `S_ISREG` も見る。ディレクトリは実行bitを持ちうるがコマンドではない。
- 名前にスラッシュが入っていれば探索しない。そのファイルが実行できるときだけ報告する。
- PATHの空要素はカレントディレクトリ（先頭・末尾・連続するコロン）。シェルと同じ。
- `PATH` が**無い**ときは空のPATHとして扱わず、`/bin:/usr/bin` へ落とす
  （`userland/base/libc/posix.c` の `confstr(_CS_PATH)` と同じ値）。
- オプションは `-a`（全部）、`-s`（黙る）、`--`。
- 終了状態は 0（全部見つかった）、1（見つからないものがあった）、2（コマンドを実行できない、
  または出力が書けない）。

見つからない報告は `fflush(stdout)` の後に標準エラーへ書く。標準出力は端末でないとき
ブロックバッファなので、そうしないと両方を混ぜて読む相手に、答えより先に苦情が届く。

## 検証

### hostでの動作（`cc -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE`、15ケース）

| # | 確かめたこと | 結果 |
| --- | --- | --- |
| 1 | PATHの最初の一致を報告 | `/tmp/whichtest/a/cmd`、rc=0 |
| 2 | `-a` で全部 | a/cmd と b/cmd の2行、rc=0 |
| 3 | 見つからない | `which: nosuch: not found`、rc=1 |
| 4 | `-s` は黙る | 出力なし、rc=1 |
| 5 | 実行bitの無いファイルは飛ばす | not found、rc=1 |
| 6 | ディレクトリは飛ばす | not found、rc=1 |
| 7 | スラッシュ入りの名前 | そのまま報告、rc=0 |
| 8 | スラッシュ入りで実行できない | not found、rc=1 |
| 9 | 複数の名前で成否が混ざる | 見つかった分を出し、rc=1 |
| 10 | PATHの空要素がカレント | `./cmd` が先、rc=0 |
| 11 | 引数なし | usage、rc=2 |
| 12 | 知らないオプション | usage、rc=2 |
| 13 | `--` | 以降を名前として扱う、rc=0 |
| 14 | `--` の後のダッシュ始まりの名前 | 名前として探す、rc=1 |
| 15 | PATH未設定 | `/bin/sh`、rc=0 |

ケース8は最初メッセージが出ず、ケース9は出力の順が入れ替わった。どちらも直して再確認した
（`not_found()` を切り出し、スラッシュ経路からも呼び、`fflush(stdout)` を先に行う）。

### target build

- amd64 `world`・`disk-image`: warning 0、`build/which-chk/rootfs/bin/which` を確認。
- pcat・pc98 `disk-image`: エラー0。

### 実QEMU（amd64、UEFI USB起動）

`plan/ws035/tests/guest-console.py`（新規）でloginし、QMPの `send-key` で入力して画面を撮った。
serial・console logは読んでいない。

```
/bin/sh
which: nosuchcmd: not found
rc=1
silent=1
/bin/ls
abs=0
```

`which -a sh` → `/bin/sh`、`which nosuchcmd` → not found・rc=1、`-s` は黙って rc=1、
`which /bin/ls` → `/bin/ls`・rc=0。証拠は [which-guest.png](which-guest.png)・[which-guest.txt](which-guest.txt)。
別に `which ls` → `/bin/ls` も確認した。

## 途中で直した別の問題

`include/kern/boot.h` の `enum bootfat_type;`（前方参照）が、UEFIブートローダのbuildで
`-Werror,-Wmicrosoft-enum-forward-reference` を起こし、amd64の `disk-image` が通らなかった。
enumの前方参照はC標準にない。定義を `include/kern/fat.h` から `include/kern/boot.h` へ移し、
`fat.h` が `boot.h` をincludeする形にした。`fat.h` はVFS全体を引き込むので、
ブートローダが読む `boot.h` からは参照できない。

これは2026-09-23のboot header統合で入り込んだもので、そのときはamd64の `vmunix` と
pcat・pc98の `disk-image` しか作っておらず、amd64の `disk-image` を作っていなかったため見つからなかった。

## 新しい道具

`plan/ws035/tests/guest-console.py`: 起動したguestの画面を `plan/tools/boot-test.py` と同じ
フォント照合で読み、QMPの `send-key` で入力する。`--script expect:PATTERN` と
`--script send:TEXT` を順に適用する。userlandのコマンドを実機で確かめるのに使う。

## 制限

- `-p`（PATH指定）や tcsh の alias 展開は実装していない。POSIXにwhichは無く、
  必要が出たときに足す。
- 画面のカーソルはkernelのフォントに無いグリフなので、`guest-console.py` の読み取りでは
  プロンプト行の末尾が未知文字になる。期待パターンを行末に固定しないこと。

## 受け入れ

- `/bin/which` が入る。amd64・pcat・pc98 の build が warning 0。
- host 15ケースと実QEMUの4ケースが通る。
- `git diff --check` PASS。
