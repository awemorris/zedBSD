<!-- awesome-plan project=zedbsd record=ws327-p007 -->

# ws327-p007 委譲の設計（標準廉価実装手順 §4.4〜§4.8）

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §13・§14。Queue: q1697 / q1697-i02（担当 P2、2026-10-30）。
- Status: in-progress（2026-10-30）
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
| 凍結の照合 | `tests/check-base.sh`・`tests/make-frozen.sh`・`tests/frozen.sha256` |
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

## 確かめ（2026-10-30）

- `sh plan/ws327/tests/build-win32.sh` → 骨格の 2 file が warning 0 で compile、`win32-link.exe` が link（`ws327-win32: PASS`）。
- `python3 -I plan/ws327/tests/check-layers.py` → T1 316・N1 2（移す前の今の数。N1 は Mailer の `sync.c` の poll）。許可の行は Wayland の fd の
  poll、instance.c、preview、ime/main.c、installer/admin、media-worker.c の `<sys/socket.h>`。
- `sh plan/ws327/tests/host-apps.sh build/ws327-linux ime picker` → `ws327-host-apps: PASS`（ime 230 件、picker）。
- `sh plan/ws314/tests/doc-examples.sh build/ws327-linux/lib` → `SUMMARY pass=71 fail=0`（p015 の基準）。
- `python3 -I plan/ws327/tests/doc-lines.py 08-worker.c` → PASS。
- Photos・Settings・picker・audio の source の host の `-fsyntax-only`（phase010 の command）が移す前の tree で通る。
- 6bb8c3c8d の Mailer の `conn.c`・`tls.c`・`imap.c` が host の gcc で `-Wall -Wextra -Werror` の compile を通る（link・試験は p008）。

## 結果

（design-reviewer の後に書く）
