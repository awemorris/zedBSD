<!-- awesome-plan project=zedbsd record=ws035p042 -->

# ws035-p042: libzdesktop

Phase ID: `ws035-p042`
Parent: [WS035](../ws.md)
Status: **cleared**（q325-i02、2026-09-23）
Phase disposition: normal
Queue: q325（q325-i02）
実行: メインセッション

## 役目（2026-09-23 ユーザー決定）

`userland/base/libzdesktop` に、**Vulkan 以外の OS 依存を閉じ込める**ライブラリを作り、
`/lib/libzdesktop.so` として出す。公開ヘッダは `include/libc/zdesktop.h`。

「zdesktop と話すためのライブラリ」ではない。**zdesktop が OS を触るときの唯一の入口**である。

### なぜ

1. **networkd の仕様は変わりうる。** zdesktop が networkd の protocol を直接呼んでいると、
   networkd を直すたびに zdesktop を直すことになる。間に1枚置けば直す場所が1か所で済む。
2. **zdesktop を他の OS へ移すとき**、OS に依存する部分がここに集まっていれば、
   そこだけを書き換えればよい。

### したがって

- **zdesktop は networkd・audiod などと直接話さない。** すべてこのライブラリを通す。
  WiFi の状態を networkd の購読（ws035-p018）から取るのも、このライブラリの中である。
- **Vulkan は例外。** 描画は標準の Vulkan/WSI をそのまま使い、ラッパーを挟まない。
  標準 API であり、移植先にも同じものがあるためである。
- Wayland の protocol も例外ではない。合成と入力は Wayland が標準なので
  `libwayland-client` をそのまま使い、このライブラリには入れない。

## この Phase で作るもの

2026-09-23 ユーザー指示「**まずは空でいい**」。**枠だけ**を作る。

- `userland/base/libzdesktop/`（Makefile、`exports.map`、最小の source）。
- `include/libc/zdesktop.h`: 版を問い合わせる1つだけ（例 `zdesktop_version()`）。
  中身が無いうちから ABI を主張しないため、公開するものは最小にする。
- `/lib/libzdesktop.so` が出来て、SONAME と依存が正しいこと。
- amd64 build warning 0。

## 後で入れるもの（この Phase の範囲外）

いずれも、その機能の Phase が**このライブラリに関数を足す**形になる。

| 機能 | 中で話す相手 | その機能の Phase |
| --- | --- | --- |
| WiFi の状態 | networkd の購読（p018） | p013（タスクバーの WiFi） |
| 音量 | audiod（p009）・互換 libpulse（p019） | p026（タスクバーの音量） |
| デスクトップ通知 | zdesktop 本体（表示するのは compositor） | 未計画 |
| 窓の一覧 | zdesktop 本体 | p013・p014 |

**移植のときに書き換える境界がここに来る**ので、関数を足すたびに
「これは OS に依存するか」を見る。依存しないもの（描画の計算など）はここに入れない。

## 結果（q325-i02）

| 作ったもの | 内容 |
| --- | --- |
| `include/libc/zdesktop.h` | `ZDESKTOP_VERSION`（1）と `zdesktop_version()` だけ。役目（OS に触る唯一の入口、Vulkan と Wayland は例外）を冒頭の注記に書いた |
| `userland/base/libzdesktop/` | `version.c`、`exports.map`（`zdesktop_version` だけを出す）、`Makefile`（package の既定は有効） |
| `platform/amd64/vmunix.mk` | `libzdesktop.so` の link と `check-dynamic-elf.py` による確認（libtruetype と同じ形） |
| `config/ci/config-amd64.mk`、`plan/ws035/tests/config-amd64-userland.mk` | `ZEDBSD_USER_PROGRAMS` に `libzdesktop` |

| 検証 | 結果 |
| --- | --- |
| amd64 `disk-image`（`build/p042`） | PASS。新しい warning なし（出たのは以前からある `-no-pie` と noct の `-Wreturn-type`） |
| `libzdesktop.so` | SONAME `libzdesktop.so`、NEEDED `libc.so` だけ、公開 symbol は `zdesktop_version` だけ。`/lib/libzdesktop.so` として image に入る。header は sysroot の `/usr/include/zdesktop.h` |
| `plan/tools/boot-test.sh` | PASS（login prompt） |

amd64 以外の platform には入れていない（zdesktop は amd64 から始めるため）。
他の platform で要るようになったら、その platform の `vmunix.mk` に同じ規則を足す。

## 未決定

無い。
