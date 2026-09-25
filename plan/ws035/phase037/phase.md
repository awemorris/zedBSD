<!-- awesome-plan project=zedbsd record=ws035p037 -->

# ws035-p037: シリアルコンソールの受信

Phase ID: `ws035-p037`
Parent: [WS035](../ws.md)
Status: cleared（q324-i01、2026-09-23）
Phase disposition: normal
Queue: q324（q324-i01）
実行: メインセッション

## なぜ要るか

これまでゲストを操作する手段は、QEMU の `send-key` でコンソールへキーを注入することだけだった。
これは**ゲストが忙しいとコマンドの途中から文字が落ちる**。q323 で実際に、
`pkill sshd` が `PILLSSHD` になり、`grep -c` が `grep c` になり、`net watch` が `et` になった。
自動で試験を回す土台にならない。

COM1 は出力のミラーだけで、**受信は実装されていなかった**
（`src/drivers/platform/pcat/serial-mirror.c` の「It is output only」）。

## 入れたもの

### `tty_console_input_byte()`（`include/kern/tty.h`、`src/kern/tty.c`）

キーボードは「どのキーが動いたか」を報告し、コンソールがそれを文字に直す。
シリアルは**文字そのもの**を運ぶ。後者の入口を1つ作り、
`tty_console_input_event()` の末尾（line discipline の実行、echo、signal）を共有させた。
ケーブルから来たものとキーボードから来たものが同じように読まれる。割り込みから呼べる。

### 受信（`src/drivers/platform/pcat/serial-mirror.c`）

- **割り込み駆動**（IRQ 4）。ポーリングではない。コンソールは機械が別の仕事をしている間も
  答えねばならず、コンソールへ書いたときだけ回るポーリングでは、コマンドの1文字目を取りこぼす。
- 1回の割り込みで取るのは最大 64 文字。止まらない回線がプロセッサを handler に居座らせないため。
- 受信した `\r` は `\n` へ、`0x7f` は `8` へ直す。ターミナルが返すものと line discipline が
  期待するものの差であって、同じキーの両端である。
- `drv_pcat_serial_mirror_start_input()` を `src/kern/platform/pcat.c` の PS/2 の隣で呼ぶ。
  **それ以前に届いた文字は捨てる**。電源投入時から繋がっている回線は、誰も聞いていない間に
  何を運んでいたか分からない。
- UART が無ければ（LSR が 0x00 か 0xff）何もしない。`CONFIG_PCAT_SERIAL_MIRROR=n` では
  関数は空のまま。

### ハーネス（`plan/tools/guest/serial.py`）

```
serial.py --socket S login
serial.py --socket S run 'net show'
serial.py --socket S expect 'login: '
```

`run` は**コマンドの出力をそのまま出し、コマンドの終了状態で終了する**。
状態は prompt からではなく `ZEDBSD-SERIAL-STATUS=$?` の行で運ぶ。
prompt は状態を示さないし、コンソールは system が書く他のものと共有されているためである。

## 検証（実QEMU、amd64 UEFI USB 起動、`-serial unix:...`）

| 見たもの | 結果 |
| --- | --- |
| シリアルに boot の出力と `login:` が出る | OK |
| `root` と空パスワードでログインできる | OK（`root@zedbsd:/root$`） |
| `echo SERIAL-INPUT-WORKS` が実行され、出力が返る | OK |
| `uname -a` | `zedBSD zedbsd 0.0.1 zedBSD 0.0.1 x86_64`、rc=0 |
| `ls /bin/which` | `/bin/which`、rc=0 |
| `false` | rc=**1**（終了状態が正しく運ばれる） |
| `net show` | `lo0 static online`、rc=0 |

**キー入力を1度も使っていない。** 文字落ちは起きなかった。

build: `CONFIG_PCAT_SERIAL_MIRROR=y` で amd64 `vmunix` warning 0、`amd64 vmunix check: PASS`、
`disk-image` エラー0。

## 制限

- pcat（と amd64 の PC/AT）だけ。pc98・rpi4・sun4u・x68k には無い。
- `CONFIG_PCAT_SERIAL_MIRROR=y` でビルドしたときだけ。既定は `n`。
- 115200 8N1 固定、COM1（0x3f8）固定、IRQ 4 固定。
- 受信は active な VT へ入る。シリアル専用の getty は無い。
- flow control は無い。64 文字を超えて一度に送られた分は次の割り込みで拾う。

## 受け入れ

- シリアルから文字を送るとコンソールの入力になる。
- ログインし、コマンドを実行し、終了状態を得られる。
- キー入力を使わずに上を行える。
- amd64 build warning 0。`git diff --check` PASS。
