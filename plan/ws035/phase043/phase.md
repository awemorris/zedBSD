<!-- awesome-plan project=zedbsd record=ws035p043 -->

# ws035-p043: 端末のモード切替で先行入力が失われる

Phase ID: `ws035-p043`
Parent: [WS035](../ws.md)
Status: **cleared**（q332-i01、2026-09-23）
Phase disposition: normal
Queue: q332（q332-i01）
実行: メインセッション

## きっかけ

シリアルコンソール（ws035-p037）でゲストを操作していると、コマンドの**1文字が抜けて改行になり、shell が終了して
login に戻る**ことが繰り返し起きた（WS040 p005 で3回、p003・p040 でも）。どれも shell がプロンプトを出す前後に
コマンドが届いたときだった。

## 原因（kernel の tty）

`src/kern/tty.c` は入力を3か所に分けて持つ。canonical mode の編集中の行（`edit`）、読まれるのを待つ完成した行
（`records`）、non-canonical の文字の列（`input`）。`TCSETS` で ICANON を切り替えても、**溜まっている入力を
もう一方へ移していなかった**。

- canonical で打たれたものは、non-canonical の read からは見えない（`input` しか読まない）。
- non-canonical で打たれたものは、canonical の read からは見えない。

shell の行編集（libedit）はコマンドを読むたびに non-canonical にし、コマンドを走らせる間は canonical に戻す。
その切り替えの間に届いた入力は取り残され、あるいは前後が分かれる。シリアルでの「1文字抜けて shell が終わる」は
これの現れで、**キーボードの先打ちでも同じことが起きる**（シリアル固有ではない）。

## 直したもの

`tty_switch_mode_locked()`（`src/kern/tty.c`）。`TCSETS`・`TCSETSW`・`TCSETSF` が新しい設定を入れた直後に呼ぶ。
- canonical を**やめる**とき: 読まれていない行（読みかけの行は残りだけ）、続いて編集中の行を、打たれた順に
  `input` へ移す。
- canonical に**入る**とき: `input` の文字を行にする。改行か VEOL で行を閉じ、残りを編集中の行にする。
- `TCSETSF` は先に入力を捨てるので、移すものは無い。mode が変わらない設定では何もしない。

POSIX は non-canonical へ切り替えたときに溜まった入力を読めるようにすることを求めており、Linux も同じ扱いをする。

## 検証

| 検証 | 結果 |
| --- | --- |
| host 試験 `plan/ws035/tests/tty-mode-switch-test.py` | PASS。`tty.c` の関数をそのまま取り出して build（ASan・UBSan）。1行半を打った状態で canonical をやめる、読みかけの行、戻す方向（改行と VEOL で行に分ける）、mode が変わらない設定、`input` があふれない。**関数の中身を空にすると失敗する**ことも確かめた |
| ゲスト（amd64、KVM）: `sleep 2` の最中に次のコマンドを送る（5回） | **修正後 5/5 が実行された。修正前の kernel では 0/5**（`echo` の行が表示されるだけで実行されない） |
| amd64 `disk-image`、pcat `vmunix` | PASS |
| `plan/ws019/tests/console-csi-test.py` | **FAIL（以前から）**。取り出した `tty_render` が今は `tty_console_puts` を呼ぶのに、試験の stand-in にそれが無く compile できない。修正前の `tty.c` でも同じく失敗するので、この Phase とは関係が無い。試験の更新は別の作業 |

## 残したこと

- 行の数（`TTY_RECORDS` = 8）や行の長さ（256）を超える先打ちは、これまでどおり捨てる。
- ゲストの `kill %1` が `Unknown error` になる件（ws040-p006 で観察）は別の問題で、ここでは扱っていない。
