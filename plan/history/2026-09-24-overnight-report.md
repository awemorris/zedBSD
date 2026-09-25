<!-- awesome-plan project=zedbsd record=overnight-report -->

# 夜間の自律実行の報告（2026-09-23〜24）

ユーザーの指示「判断が要る Phase は uncleared にして先へ進める。キューを次々と組んで実行する」
（2026-09-23、`plan/master.md`「夜間の自律実行」）による実行の記録。commit は全部 `git commit -m WIP`、push はしていない。
HAL の変更は、夜間には無い（WS040 の pc98 PIT は、就寝前に承認を得たもの）。

## 実行した Queue

| Queue | Phase | 結果 | 要点 |
| --- | --- | --- | --- |
| q325 | ws035-p018 | cleared | networkd の購読: 上限がちょうど8、切れた購読者の枠が空く |
| q325 | ws035-p042 | cleared | `/lib/libzdesktop.so`（`zdesktop_version()` だけ）と `<zdesktop.h>` |
| q325 | ws034-p003 | cleared | `KERN_SYSTEM_GET_PCI_DEVICE` と `lspci [-Dkv]`。QEMU の `query-pci` と一致 |
| q327 | ws040-p006 | cleared | WS040 の漏れ: `ps` の TIME が amd64 で10倍だった。process 一覧の CPU 時間を固定単位に |
| q328 | ws034-p004 | cleared | `KERN_SYSTEM_GET_USB_DEVICE` と `lsusb [-tv]`。hot-plug 中の 300 回で失敗0 |
| q329 | ws034-p021 | cleared | zlib 1.3.2・expat 2.8.5 の package。SONAME の実体＋symlink を `/usr/lib` に（`ZEDBSD_PACKAGE_LINKS` を新設） |
| q330 | ws034-p019 | cleared | ca-certificates（curl の PEM を `/etc/ssl/cert.pem`）。Debian の certdata と121件一致。`external.mk` に1ファイルの取得 |
| q331 | ws034-p040（新） | cleared | libc の iconv（UTF-8・ASCII）。glibc と20万件で突き合わせ |
| q332 | ws035-p043（新） | cleared | **端末の mode 切替で先打ちが失われる kernel の不具合**。シリアル操作で「1文字抜けて shell が終わる」原因 |
| q333 | ws034-p041（新） | cleared | curses の termcap API。`/lib/libcurses.a` が PIE から link できなかったのも直した |
| q334 | ws034-p017 | uncleared | curl。TLS・CA は動くが、kernel の TCP が大きな write を届けなかった → p042 |
| q335 | ws034-p042（新） | cleared | **TCP の6つの不具合**（ACK して捨てる、固定 window、window update で起きない、回復が遅い、close で控えが消える、blocking write が1秒で失敗） |
| q336 | ws034-p017 | cleared | curl の再実行: HTTPS で頁を取得、既定の CA bundle を使う |
| q337 | ws034-p043（新） | cleared | **VM: fork した子の blocking write が、親の copy-on-write fault を止める**。pin された page は自分の pin の下で写す |
| q338 | ws034-p044（新） | cleared | TCP の throughput: 1 MiB が 20 秒で終わらない → 0.7〜7.6 秒 |
| q339 | ws034-p045（新） | cleared | fork した子の program 名（`ps` に `kernel` と出ていた）。`console-csi-test` を直した |
| q340 | ws035-p006 | cleared | audio フレームワーク（`/dev/dspN`・`/dev/mixerN`）。偽 driver の host fixture で再生・録音の byte 列、underrun・overrun、DRAIN・close の期限。実 hardware は p007 |
| q341 | ws035-p022 | cleared | hda ドライバの設計。p006 の DMA device を backend から渡す形へ変える必要を見つけた（p007 で実装） |
| q342 | ws035-p007 | cleared | **HD Audio driver**。QEMU で再生が bit 一致（ICH9・ICH6・INTx）、音量・mute、録音の速さ。QEMU の CORB 流量制御と、tick が進む前の attach で待ちが止まる問題を直した。実機は p008（人） |
| q343 | ws035-p039 | cleared | USB CDC-ECM の確認。**xHCI の割り込みが止まる競合**（IMAN の読み戻し）を直した: network や keyboard を boot disk と同じ controller に付けると約 1/3 で起動しなかった → 14/14。UHCI の取りこぼし（新 ws035-p044）と TCP の停止（新 ws034-p046）を発見 |
| q344 | ws034-p046 | cleared | **TCP**: 1 MiB が 246 秒で 600 KB → 0.13〜2 秒。MSS option、window update、close で FIN が出ない不具合、解放後の segment に RST。loopback も 8/8 ×2。ECM の送受信の取りこぼしは新 ws035-p045 |
| q345 | ws035-p045 | cleared | **USB CDC-ECM の送信の列**: 使用中の送信を捨てていた（1 MiB ×10 で 3697 回）→ 0。1 MiB が 0.15〜0.25 秒で 20 回連続、転送中の detach も通る。NCM は同じ作りだが実機でしか試せないので残した |
| q346 | ws035-p044 | cleared | **UHCI の短い packet（SPD）**: ECM の ping 0/3 → 5/5、1 MiB 2.3 秒。UHCI storage の読み出し（8 MiB 一致、速さ同じ）も退行なし |
| q347 | ws035-p038 | cleared | **SSH ハーネスが動く**: start→wait 8 秒、run・put・get・lldb・kgdb・screenshot。原因は networkd が USB の interface を UP にせず link を見られなかったこと（RAISE を追加）と `/root/.ssh` の mode。`net startup` の `invalid backend request` も直した |
| q348 | ws034-p037 | cleared | package を `/usr` へ: OpenSSL の library（`/lib` → `/usr/lib`）と remacs の辞書（`/home` → `/usr/share/remacs`）。sshd・curl HTTPS で確認 |
| q345（追記） | ws035-p045 | cleared | ユーザーが挿した実機 RTL8156 を QEMU passthrough で使い、**NCM にも送信の列**: TX dropped 55% → 0、1 MiB 0.14〜0.33 秒 ×20、8 MiB 1.4 秒、detach・attach も通る |
| q349 | ws034-p039 | cleared | menuconfig の「Install development files」と base の既定 ON。amd64 専用の 9 個の platform が `*` だったため既定の i386 が壊れていた（以前から）のを直した |

途中で直した libc・kernel の不足（各 Phase に記録）: `<sys/time.h>` の `fd_set`、`<netinet/in.h>` の `IN6_IS_ADDR_*`、
`FIONBIO`。

## あなたの判断が要る点

1. **autotools の package の共通の対応**（inventory §10 の6）。`config.sub` が zedbsd を知らない件と、libtool が
   共有 library を作らない件。(a) package ごとの patch（OpenSSH の今のやり方）、(b) `external.mk` が展開後に
   共通の script を当てる、(c) GNU config へ登録。**wget（ws034-p020）、bash、coreutils など autotools の package は
   これが決まるまで進められない**ので、今夜は手を付けていない。
2. **scheduler の起床 preempt**（2026-09-23 の会話で提案、未回答）。tick で起きた thread が quantum を待たずに
   走れるようにし、quantum をミリ秒で決める。新しい WS として計画するか。
3. inventory §10 の残り: 本家 libwayland の epoll・timerfd・signalfd（GTK・Qt の前提）、git の `NO_RUST=1`。

## 気づいたが扱っていないこと

- `FD_SETSIZE` が 32（`select()` が fd 32 以上を扱えない）。
- shell の `kill %1` が使えない。`kill` は builtin でなく、shell は最後の job しか覚えていない（job の表が要る）。
- VM の host 試験（`plan/ws025/tests/run-user-lease-host.py` など）は、もう無い API を使っていて build できない。
  今の VM の設計に合わせた書き直しが要る。
- rpi4 の `vmunix` は HEAD から build できない（kernel の include path に `include/libc` が入っている）。
- TCP の重複再送・512 byte の segment（p044 に記録）。
- `plan/ws004/tests/` の xHCI・zero-packet・ECM の host fixture は変更前の source でも失敗する（`pc98-auto.c` の section、`kern_malloc` 未定義）。
- ゲストの `dd bs=65536` が 32768 byte しか書かなかった（`dd` か tmpfs）。
- **USB hub の driver が無い**。QEMU の UHCI（root port 2 つ）に 3 つ目の device を付けると hub が挟まり、その先が見えない。実機の hub・dock でも同じはず。
- `net dhcp ue0` の直後に networkd がもう一度 DHCP をやり直し、その数秒間の `connect()` がすぐ失敗する（実機 NCM で観察）。

## 2026-09-24 のユーザーの回答

上の「判断が要る点」と「気づいたが扱っていないこと」への回答を `plan/master.md`「2026-09-24 ユーザーの判断」節に
表でまとめ、それぞれを Phase にした（ws034-p047〜p050、ws035-p046〜p051、WS041、WS036 の rpi4）。
