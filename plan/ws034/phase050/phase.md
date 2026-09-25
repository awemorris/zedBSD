<!-- awesome-plan project=zedbsd record=ws034p050 -->

# ws034-p050: 調査: Wayland を POSIX（`poll`）の範囲で作れるか

Phase ID: `ws034-p050`
Parent: [WS034](../ws.md)
Status: **cleared**（q362-i01、2026-09-24）
Phase disposition: normal
Queue: q362（q362-i01）
実行: メインセッション

## 問い（2026-09-24 ユーザー）

「epoll, timerfd, signalfd については、我々は upstream を利用せずに、libwayland を独自実装しますので、これらの API は必須ではないと思います。
POSIX の範疇で wayland を構築できないか検討してみてください。できないか、難しい場合、epoll, timerfd, signalfd の実装を相談してください。」

## 結論

**POSIX の `poll` の範囲で作れる。epoll・timerfd・signalfd の実装は要らない。** 足りないのは libc の小さな関数 2 つ
（`mkostemp`、`posix_fallocate`。新 ws034-p053）だけで、どちらも Linux 固有ではない。

## 調べたもの（`build/distfiles` の版）

GLib 2.90.0、GTK 4.24.0、GTK 3.24.52、Qt 6.11.2（qtbase、qtwayland）、Qt 5.15.19（qtbase、qtwayland）、upstream wayland 1.26.0、
libxkbcommon 1.13.2、libepoxy 1.5.10。`epoll_*`・`timerfd_*`・`signalfd`・`eventfd`・`memfd_create` を探し、使っている箇所の代替の経路を読んだ
（test・example・3rdparty は除いた）。

| 対象 | 使っている所 | Linux の API が無いとき |
| --- | --- | --- |
| GLib の main loop | `poll()`（`gmain.c`） | そのまま |
| GLib `gwakeup.c` | eventfd | **pipe に落ちる**（`HAVE_EVENTFD` が無ければ最初から pipe） |
| GLib `giounix-private.c` | epoll（fd が poll できるかの判定） | `HAVE_KQUEUE` か、通常 file かどうかの判定に落ちる |
| GLib `gthreadedresolver.c` | （signalfd は comment だけ） | — |
| GTK 3 / GTK 4 の Wayland の共有メモリ | memfd_create | **`shm_open` に落ちる**（`HAVE_MEMFD_CREATE` が無ければ最初から） |
| GTK 4 `gdkdmabuf.c` | Linux の dma-buf | dma-buf を使わない（zedBSD は GPU の画像を OPAQUE_FD で共有する） |
| Qt 5 / Qt 6 の event dispatcher | eventfd（`__has_include(<sys/eventfd.h>)`） | **pipe に落ちる**。timer は `poll` の timeout で数える |
| Qt 6 / Qt 5 の Wayland の shm backing store | memfd（`SYS_memfd_create` があれば） | **`QTemporaryFile`（runtime dir の一時 file）に落ちる** |
| Qt 6 `qioring_linux.cpp` | io_uring | Linux だけの build 対象 |
| upstream wayland の client（`wayland-client.c`） | `poll()` | そのまま（今回は独自の libwayland を使うので参考） |
| upstream wayland の server（`event-loop.c`・`wayland-os.c`） | **epoll・timerfd・signalfd** | compositor 側だけ。zdesktop（zwl）は独自で、`poll` の loop |
| upstream wayland-cursor（`os-compatibility.c`） | memfd | **`mkostemp` ＋ `posix_fallocate`（無ければ `ftruncate`）** に落ちる |
| libxkbcommon、libepoxy | 無し | — |

**timerfd と signalfd を使っているのは upstream wayland の server の event loop だけ**で、client の toolkit はどれも使っていない。
toolkit の timer は main loop の `poll` の timeout、signal は自分の pipe で扱っている。

## 足りないもの

- libc の `mkostemp`（無い）、`posix_fallocate`（無い）: upstream の wayland-cursor の代替の経路が使う。GTK は wayland-cursor を使う
  （独自の libwayland に cursor の library を足すか、upstream の wayland-cursor を使う場合）。→ **ws034-p053**。
- 独自の libwayland は、toolkit が使う client の API（`wl_display_prepare_read`・`read_events`・`dispatch_pending`・`wl_event_queue`・
  `wl_display_get_fd` 等）を `poll` の前提で持つ必要がある。これは既存の p034（libwayland の拡張）の範囲。
- memfd の seal（`F_ADD_SEALS`）は、audiod の設計（[audiod-design.md §4.3](../../ws035/audiod-design.md)）で後から足す案として記録済み。
  toolkit は無くても動く（seal は任意）。

## 範囲外

- Chromium（ws035-p029〜）は別途。base の event loop が epoll を前提にする部分がある（libevent の epoll/poll backend、Mojo）ので、
  その時に改めて調べる。
