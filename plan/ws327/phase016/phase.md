<!-- awesome-plan project=zedbsd record=ws327-p016 -->

# ws327-p016 受け入れ: 境界の check、全 arch・Linux・FreeBSD の build、QEMU の回帰、Win32 の報告

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §1.3・§6.4・§8・§13。担当: P2（設計の担当）。依存: p008〜p015 が main に merge され cleared。
- Status: planned
- Disposition: normal

## 目的

実装の Phase の成果を一つの tree（main の merge の後）で通して確かめ、WS の目標（app が pthread・socket を直に呼ばず、POSIX と Win32 で
同じ interface）の達成を示す。直しが要れば小さな物は P2 が直し、大きな物は Q1 に返す。

## 確かめ

1. **境界**: `python3 -I plan/ws327/tests/check-layers.py`（範囲を絞らず）→ `ws327-check-layers: PASS`（T1・N1 が 0、許可の行だけ）。
   `sh plan/tools/keiland-os-boundary/check.sh` の FAIL の一覧を main の基点（WS327 の前）の一覧と比べ、増えた物が無い（B5 は
   `#include <keiland/keiland_*.h>` を全て拾う既存の誤検出で、`plan/ws327/tests/` の新しい試験の行が増える: Q1 に報告）。
   T1・N1 を check.sh へ入れる案（check-layers.py の中身と layers-allow.tsv を A1〜A5 の形で）を差分にして Q1 に出す（`plan/tools/` は
   Q1 の範囲）。
2. **build**: `sh plan/ws327/tests/build-zedbsd.sh p016 <arch> "mailer music videoplayer photos settings keiland-ime terminal"` を
   amd64・arm64・riscv64 → 各 `make=0 warnings=0`。Linux: libkeiland・settings・keiland-ime の build、warning 0。
   FreeBSD: P1 の guest（`plan/tools/keiland-freebsd/`、書き込まない overlay）で libkeiland・settings・keiland-ime の build と
   thread-test・socket-test（design §8）。
3. **host**: `sh plan/ws327/tests/host-test.sh build/ws327-p016-linux`（12 build）、`sh plan/ws327/tests/host-apps.sh build/ws327-p016-linux ime picker`、
   `sh plan/tools/mail/run-host-mail-backend.sh`、`sh plan/tools/music/host-music-play.sh`、`sh plan/tools/media/run-host-codec.sh`、
   `sh plan/ws327/tests/build-win32.sh`、`sh plan/ws314/tests/doc-examples.sh build/ws327-p016-linux/lib`。
4. **QEMU の回帰**（design §6.4、`plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-guest.mk build/ws327-p016-img`、1 本の script
   `plan/ws327/tests/guest-apps.sh` を P2 が書く: 配置 → 起動 → 実行 → 回収 → 要約。判定は画面の PNG と SSH での program の終了の status と
   app の log の行。console は読まない）:
   - Mailer: host で `plan/tools/mail/fake-mail-server.py`（host の 127.0.0.1 に bind、guest は slirp の 10.0.2.2 で届く、
     `--arrivals 1`。名前でなく address で指す: zedBSD に `/etc/hosts` が無い）、guest の Mailer が接続・sign-in・IDLE・新着・送信（`MAIL SIGNED-IN`・`MAIL REFRESHED`・
     `MAIL MESSAGE ... arrived=1`・`MAIL SENT` の行）。
   - Music・Video Player: `plan/tools/media/sample.mp4` の再生の開始と停止（窓の写真と、停止の後に process が残らない）。
   - Photos: 起動の直後（縮小の thread の start の前後）の縮小の要求と一覧の写真（静的な条件変数の once、design-reviewer #10）。
     Settings: storage の page（走査の終わりの log）と見た目の page。IME: 辞書の書きを 2 回（1 回目の flush で thread が終わった後に
     もう 1 回: once で作った条件変数の再使用）、`~/.config` の辞書の file の中身。Video Player の `kl_cond_wait_for` は再生の途切れの
     無い事（写真 2 枚の間の時間）で。Terminal の起動（回帰）。
   - 8 章の例: `sh plan/ws314/tests/doc-examples.sh guest build/ws327-p016-img 08-worker 08-language` の写真、08-network は同じ方法で撮り
     `docs/howto/keiland-examples/08-network.png` と本文の画像の行を足す（p015 が置かなかった物）。
5. **Win32**: p013・p014 の結果の「Q1 へ」をまとめ、interface を Windows で守れない所（例: key の destroy の回数、`FlsFree` の destroy、
   SetThreadDescription の無い Windows、`select` の FD の数の上限が無い事の確かめ）を design.md §5 に記録して Q1 に返す。

## 所見の引き継ぎ（p007 から）

- zedBSD の image に `/etc/hosts` が無く "localhost" が解けない（p003）。Q1 が Bug にするか決める。
- 閉じた WS の試験で、移した source を `-pthread` だけで build する物（`plan/ws095/tests/host-engine.sh`、`plan/ws089/tests/host-wallpaper.sh`・
  `host-build.sh`、`plan/ws191/tests/host-audio-stream.sh`（main で既に FAIL））: 削除の候補として Q1 に。
- `plan/tools/mail/host-mail-sync.c` は 6a53bce99 から build できなかった（p008 で直す）。

## 受け入れ

上の 1〜5 が全て済み、FAIL は 0 か Q1 が受け入れた物だけ。結果をこの phase.md に、ws.md の Resume point に要約。

## 結果

（P2 が書く）
