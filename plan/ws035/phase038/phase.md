<!-- awesome-plan project=zedbsd record=ws035p038 -->

# ws035-p038: SSHハーネス

Phase ID: `ws035-p038`
Parent: [WS035](../ws.md)
Status: **cleared**（q347-i01、2026-09-24）
Phase disposition: normal
Queue: q347（q347-i01）
実行: メインセッション

## 範囲

ゲストへ SSH で入り、コマンドを実行し、ファイルを送受し、ゲストの `lldb` と
QEMU の gdbstub でデバッグする道具を仕上げる。

## 着手済みの分（q323-i07 の途中で作ったもの、未完成）

`plan/tools/guest/`（`guest.sh`・`guest.py`・`net.conf`）。
`keys`・`extra-files`・`start`・`wait`・`run`・`put`・`get`・`lldb`・`kgdb`・`screenshot`・`stop`。
鍵は `plan/tmp/guest/` に作り、ホスト鍵3種とクライアント鍵をイメージへ入れる。

**未達**: ゲストは起動し sshd も起きるが、**SSH が応答しない**。
`ue0`（USB CDC-ECM）が上がっているか、DHCP が取れているか、sshd が listen しているかは未確認。

## 依存

- **p037**（シリアルコンソールの受信）: 原因を追うのに、キー入力に頼らない観察の手段が要る。
- **p039**（USB CDC-ECM の実機確認）: ゲストのネットワークが上がらなければ SSH は成立しない。
  ECM は実績が無いまま入っているので、まずそれを確かめる。

## 受け入れ

- `guest.sh start` → `wait` が SSH の応答まで到達する。
- `run`・`put`・`get` が動く。
- `lldb` がゲストの program を止められる。`kgdb` が kernel の symbol を読める。
- 手順を `plan/master.md` から参照する。

## 結果（q347-i01、2026-09-24）

### SSH が応答しなかった原因

1. **ゲストの `ue0` が起動時に設定されない**（networkd）。networkd の有線の方針は「cable（carrier）のある interface を設定する」
   だが、**USB の adapter（ECM・NCM）は interface が UP になって初めて link を報告する**。誰も UP にしないので carrier が
   来ず、DHCP も走らない。起動時の `net startup` の `LAN_ENABLE` は受け付けられていたが、それだけでは何も起きなかった。
   - 直した: networkd の方針に **`NETWORKD_LAN_ACTION_RAISE`** を足した。設定に名前のある（DHCP・static の）interface で
     carrier の無いものを一度 UP にする（`ifconfig up`、設定はしない）。carrier が来れば既存の経路で設定される。
     daemon が DOWN にした interface は印を消して、次の cable を見るためにまた UP にする。
     （`userland/base/networkd/managed-lan.[ch]`・`main.c`。host 試験 `make managed-lan-host-test` は 35 項目 PASS、
     RAISE と「DOWN の後にまた RAISE」の項目を足した。）
   - 同時に、`net startup` が毎回出していた `net: invalid backend request` を直した。無線の開始を
     `backend("WIFI_ENABLE")`（表に無い名前）で送っていた。`wifi_backend(NETWORKD_OP_WIFI_ENABLE)` で送る
     （`userland/base/net/main.c`）。
2. **公開鍵が拒まれた**: `/root/.ssh` が group 書込み可（`drwxrwxr-x`）で、sshd の StrictModes が拒否した。
   ハーネスの `extra-files` に `--mode /root/.ssh=0700` を足した。

### ハーネスの直し（`plan/tools/guest/guest.py`）

- KVM が使えれば使う（`--no-kvm`）。シリアルを `build/guest/serial.sock` に出す（sshd の前の観察用）。
- 走行中の状態（disk の複製 800 MB、log、socket、session）を **git が追う `plan/tmp/guest/` から `build/guest/` へ移した**。
  `disk.img`・`qemu.log`・`uefi-vars.fd` の追跡を外した（鍵はそのまま `plan/tmp/guest/`）。
- `run` が argparse の名前の衝突（`command`）で落ちていたのを直した。
- `put`・`get` が scp に `-p`（時刻の保持）を port として渡していたのを `-P` にした。

### 検証（`plan/ws035/tests/config-amd64-guest.mk`: userland ＋ OpenSSH ＋ clang（lldb）＋ シリアル）

| 受け入れ | 結果 |
| --- | --- |
| `start` → `wait` が SSH の応答まで | 8 秒（KVM） |
| `run` | 出力と終了状態（`false` で 1）が返る |
| `put`・`get` | 1 MB を送って cksum 一致、`/etc/net.conf` を取り出して一致 |
| `lldb` | `/bin/ls` を `main` で止め、backtrace、kill |
| `kgdb` | `hal_cpu_idle` ← `sched_idle` ← `kernel_main` の symbol 付き backtrace、detach 後もゲストは動く |
| `screenshot`・`stop` | PNG を保存、QEMU が止まる |
| 手順 | `plan/master.md` の「ゲストの操作: SSH ハーネス」 |

### 気づいたこと

- `extra-files` の `--mode` だけを変えても rootfs の stamp が無効にならず、古い rootfs のまま image ができる
  （`build/<x>/rootfs/.stamp` を消して作り直した。master.md に注記）。
- ゲストに `which` が無い。`echo` は shell の builtin で `/bin/echo` は無い。
