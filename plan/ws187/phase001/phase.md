<!-- awesome-plan project=zedbsd record=ws187-p001 -->
# ws187-p001: lock の画面の大きな時計

Status: in-progress（q864、P2、2026-10-08: 実装・build・host 試験まで。QEMU の PNG（縦長・横長）は T1、WS187 の p003 の後にまとめて依頼）
Disposition: normal
Parent: [WS187](../ws.md)
Queue: q864（ユーザー 2026-10-08「起動して作業開始してください」、P2）

## 範囲

- compositor の lock の画面（`userland/desktop/wayland/` の lock の描画、ws035-p102 の物）に時計（時:分、下に日付）を大きく出す。位置は画面の中央より上（例: 縦の 30〜35% の所に中心）。大きさは画面の短い辺に比例させ、論理 px の定数で上限・下限を持つ。
- 縦長（1080x1920）・横長（1920x1080）・小さい画面（1280x800）で、時計と認証の入力が重ならない。
- 規約: plan/coding-style.md の全文。

## 確認

- build（warning 0）、配置の計算の host 試験。QEMU の PNG（縦長・横長）は T1 に依頼（Q1 経由）。

## 記録

### 実装（2026-10-08、P2）

- `userland/desktop/wayland/lock-clock.c`・`.h`（新規、server を知らない配置の計算、host で単独に試験できる）: `kwl_lock_clock_layout(width, height, card_top, &clock)`。
  時刻の大きさ（em）= 短い辺 × 16%、96〜192 の間。2 行（時刻・日付）の中心を高さの 32% に置く。card の上に 32 の隙間を残すため、近ければ上へ寄せる。
  上端（余白 24）を越える時は、収まる大きさまで小さくする（最小 48）。数字の高さは em の 72%、日付の baseline は時刻の baseline の下 em×22% + 28、日付の下がり 8。全て論理 px の定数。
- `userland/desktop/wayland/glass.c`・`glass.h`: 大きな数字（0〜9 と `:`）を専用の image（2048×256）に、時計の求める大きさ（最大 240）で描く。
  `glass_large_prepare`（大きさが変わった時だけ描き直す。2 回目からは `vkDeviceWaitIdle` の後）、`glass_large_text_width`、`glass_draw_large_text`。
  atlas は 64 px までの glyph と cache の領域を共有しているため、大きな数字は atlas に入れない。拡大して描くとぼやけるので、等倍で描く。
- `userland/desktop/wayland/greeter.c`: `greeter_draw_clock` は `kwl_lock_clock_layout` の位置に、大きな数字で時刻を描き、下に日付（SIZE_SEARCH）を描く。
  淡い glow は 2 行を囲む大きさにした。数字が作れない時は atlas の SIZE_CLOCK（64）で描く。login の画面も同じ画面なので、同じ配置になる。
- `userland/desktop/wayland/Makefile`・`Makefile.linux`・`Makefile.freebsd`: `lock-clock.c` を追加。
- 試験: `plan/ws187/tests/host-lock-clock.c`・`run-host-lock-clock.sh`。

### 確認（2026-10-08）

- `sh plan/ws187/tests/run-host-lock-clock.sh <dir>`: ok（74 checks）。ASan・UBSan でも ok。配置: 1920x1080 は em 172・top 241・bottom 437（card 469）、1080x1920 は em 172・top 516・bottom 712（card 942、高さの約 32%）、1280x800 は em 128・top 124・bottom 280（card 312）、2560x1600 は em 192、1280x800 で user 8 人なら em 48。
- `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws181 build/ws181/bin/wayland`: rc 0、warning 0。`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws181-linux all`: rc 0、warning 0。
- `python3 plan/tools/style-check.py`（変えた 5 file と試験）: 0。`git diff --check`: 0。
- 未実施: QEMU の PNG（縦長・横長、T1）。大きな数字の glyph の見た目（Mahora の字形、glow の大きさ）は PNG で確かめる。FreeBSD の build は未実施（Makefile.freebsd に 1 file を足しただけ）。

