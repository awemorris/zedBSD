<!-- awesome-plan project=zedbsd record=ws327-p007 -->

# ws327-p007 委譲の設計（標準廉価実装手順 §4.4〜§4.8）

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §13・§14。Queue: q1697 / q1697-i02（担当 P2、2026-10-30）。
- Status: in-progress（2026-10-30。成果は揃った、cleared の判定は Q1）
- Disposition: normal
- ユーザーの指示（2026-10-30、原文）: 「P2をいったんラップアップして、標準廉価実装手順を適用するようにWS237のphaseを整理しなおして、再起動してほしいです。」
  （Q1 の解釈: WS327 の事。手順は main の `docs/agent/spec-module-delegation.md`。）

## 目的・受け入れ

- 実装の Phase（p008〜p015、p017）を実装の担当（Sonnet 5.5 high）が phase.md と design.md だけで迷わず実装できる形にする:
  触ってよい file、design の節、前後の対応表、build と host 試験の command、突然変異の確かめ、受け入れ、凍結の物。
- Win32 の `thread-win32.c`・`socket-win32.c` の骨格（未実装の印、design §5 の写しを comment に）と mingw の build の script。
- app の移行の host 試験の道（既存の試験の link の直し方）と境界の check の script。
- p016（P2 の受け入れ）の phase.md。design-reviewer 1 本と反映。

## 成果（2026-10-30）

| 物 | path |
| --- | --- |
| 実装の Phase の共通の約束（凍結・pthread → kl の写しの表・起動の形・静的な条件変数の形・command・突然変異・終わりの条件） | design.md §13 |
| 各 phase.md | `phase008/`〜`phase017/` |
| Win32 の骨格（mingw で compile・link が通る、全関数は未実装の印） | `userland/desktop/libkeiland/platform/thread-win32.c`・`socket-win32.c` |
| mingw の build と全関数の link の確かめ | `tests/build-win32.sh`・`tests/win32-link.c` |
| 境界の check（design §1.3 の T1・N1） | `tests/check-layers.py`・`tests/layers-allow.tsv` |
| 凍結と Phase の範囲の照合（branch の分岐点との diff） | `tests/check-base.sh <phase>`（`make-frozen.sh`・`frozen.sha256` は review #4 で使わなくなった。削除を Q1 に頼む） |
| 他の WS の試験を層と一緒に build する host 試験（IME・picker） | `tests/host-apps.sh` |
| 解説の行の参照の一覧 | `tests/doc-lines.py` |
| p003 の guest の試験（p007 の前に） | `tests/guest-layers.sh`・`tests/config-amd64-layers.mk` |

## 決めた事

- Mailer の `sync.c` は `conn.socket` を直に見る（design §6.2 の「ml_conn の関数に」を変えた。`imap.c` が既にその形）。
- Win32 の thread ごとの値: FLS の callback は key を知らないので、値を record（destroy と値）に包む（design §5.1 の誤りを直した）。
- p015（解説）の依存を p007 に（API は凍結済み。app の移行を待たない）。
- `plan/tools/mail/host-mail-sync.c` は main で既に build できない（6a53bce99 で `ml_sync_start` の引数が変わり `ml_sync_fd` が無くなった）。
  p008 で直す（`plan/tools/mail/` の最小の変更、Q1 の範囲の追加の内と解した）。
- 閉じた WS の試験（ws095 の host-engine、ws089 の host-wallpaper、ws191 の host-audio-stream）は変えず、`host-apps.sh` が ime・picker を
  層と一緒に build する。ws191 の host-audio-stream は WS327 の変更の前の audio.c で既に FAIL（EOPNOTSUPP）なので使わない。

- `plan/tools/music/host-music-play.c` は main で既に link できなかった（decoder の API が `media_decoder_*` から `app_decoder_*` に変わり、
  backend・frame_us・trim が増えた）。p007 で stand-in を直した（`plan/tools/music/` の最小の変更）。

## 確かめ（2026-10-30）

- `sh plan/ws327/tests/build-win32.sh` → 骨格の 2 file が warning 0 で compile、`win32-link.exe` が link（`ws327-win32: PASS`）。
- `python3 -I plan/ws327/tests/check-layers.py` → T1 316・N1 2（移す前の今の数。N1 は Mailer の `sync.c` の poll）。許可の行は Wayland の fd の
  poll、instance.c、preview、ime/main.c、installer/admin、media-worker.c の `<sys/socket.h>`。
- `sh plan/ws327/tests/host-apps.sh build/ws327-linux ime picker` → `ws327-host-apps: PASS`（ime 230 件、picker）。
- `sh plan/ws314/tests/doc-examples.sh build/ws327-linux/lib` → `SUMMARY pass=71 fail=0`（p015 の基準）。
- `python3 -I plan/ws327/tests/doc-lines.py 08-worker.c` → PASS。
- `sh plan/tools/music/host-music-play.sh build/tmp/p007/music/host-music-play` → `host-music-play: PASS`（stand-in を直した後、移す前の play.c）。
- `sh plan/tools/media/run-host-codec.sh` → `run-host-codec: PASS`（移す前、p009 の基準）。
- Photos・Settings・picker・audio の source の host の `-fsyntax-only`（phase010 の command）が移す前の tree で通る。
- 6bb8c3c8d の Mailer の `conn.c`・`tls.c`・`imap.c` が host の gcc で `-Wall -Wextra -Werror` の compile を通る。IDLE の待ちだけ仮に直した
  host-mail-backend.c の写し（build/tmp/p007/mailprobe）で `host-mail-backend: PASS`（50 件）。sync の試験は p008。

- sync.c を移した後（design-reviewer #1）: `sh plan/ws327/tests/build-zedbsd.sh p007m amd64 "mailer"` → `make=0 warnings=0`。
  基点の 3 arch（#8）: amd64 `p007 "music videoplayer photos settings keiland-ime"`、arm64・riscv64 `p007b "mailer music videoplayer photos
  settings keiland-ime"` → 全て `make=0 warnings=0`（移す前の app、sync.c だけ移した後）。Linux の libkeiland・settings・keiland-ime も
  status 0・warning 0。仮に直した host-mail-sync の写しで `host-mail-sync: PASS`（10 件）。
- `sh plan/ws327/tests/build-win32.sh` → x86_64・i686 の両方で compile（warning 0）と link が PASS。
- `python3 -I plan/ws327/tests/check-layers.py` → T1 298・N1 0（残りは p009〜p012 の file だけ）。

## 結果

- 2026-10-30 P2: design-reviewer 1 本（21 件: high 1・medium 7・medium-low 1・low-medium 1・low 11）。全てを design.md §14 のとおり反映。
  high の 1 は P2 が `sync.c` を移して解いた（p008 は確かめと host 試験の直しに）。
- Q1 へ: `plan/ws327/tests/make-frozen.sh`・`frozen.sha256` は使わなくなった（削除の候補）。Sonnet の worktree の `build/<arch>` は
  共有の build への link にしない（build-zedbsd.sh が sysroot を `build/<arch>/sysroot` に書くので、link なら止まる）。
- Status: 成果は揃った。cleared の判定は Q1。
