# ベータ2 の image の license の一覧（ws129-p002、ベータ1 の一覧を作り直した物）

## ベータ2 の再生成（2026-10-09、P1、q920）

対象は release の config `config/release/config-amd64-beta2.mk`（CI の config に clang・libcxx・emacs・libavcodec・videoplayer、release の knob）。
生成物 [licenses-generated.md](licenses-generated.md)・[licenses-index.txt](licenses-index.txt) をこの config で作り直した:

    python3 tools/release/license-inventory.py --config config/release/config-amd64-beta2.mk \
        --distfiles /home/awe/zedBSD-claude1/build/distfiles \
        --markdown plan/ws129/licenses-generated.md --index plan/ws129/licenses-index.txt

→ `27 components, 0 open items`（250 の userland package、21 の kernel の option、`/usr/share/licenses` の 30 file。intelbt-firmware を足した後）。ベータ1 の一覧からの差:

| 差 | component | 理由 |
| --- | --- | --- |
| 増えた | FFmpeg 9.0.2（LGPL-2.1-or-later、status `decided`） | libavcodec・videoplayer（WS122、2026-10-05 ユーザーの判断）。build の `config.h` は `FFMPEG_LICENSE "LGPL version 2.1 or later"`、`CONFIG_GPL 0`・`CONFIG_NONFREE 0`・`CONFIG_VERSION3 0`（main の `build/packages/libavcodec/build/config.h` で確かめた）。共有 library で置き換えられる。release notes に source の在り処（tarball の URL と sha256、package の Makefile の configure の option）を書く（p005） |
| 増えた | Hershey fonts（hand-hershey）、SKK の入力の辞書（ime-dict-skk） | CI の config に入った（手書き・IME）。license の本文は在る |
| 変わった | desktop の font: Inter → Mahora（Zlib） | font の差し替え。`Mahora-LICENSE.txt` が入る |
| 減った | libvulkan の Venus の宣言（virglrenderer、MIT、旧 G4） | libvulkan が Venus の protocol をやめ Kei GPU の command protocol（uapi/gpu-op.h、ws167-p002）にした。第三者の文が無くなり、表からも外された（commit 4034e6b25） |
| 減った | Expat | ベータ1 の一覧はデモの config との和で、expat はデモの側だけ。release の config は選ばない |

firmware: i915・AX211 の WiFi・RTL8822B・**Intel の Bluetooth（`intelbt-firmware`）**が入り、本文も在る。intelbt は最初の再生成の時は release の
config に無く（UAT の config だけ）、2026-10-09 ユーザーの決定（Q1 の中継、クリック）「入れる、動かなければ既知」で release の config に足した
（`bluetoothd`・`bt`・`CONFIG_DRIVER_USB_BT` と `/etc/passwd` の `_bluetooth` は元から在る）。本文 `LICENCE.ibt_firmware`・`WHENCE`、変更しない binary だけ・
逆 engineering の禁止。

audit（`plan/tools/packages/audit-licenses.sh /home/awe/zedBSD-claude1/build/distfiles`、34 file）: FFmpeg の archive を package の判定に足した
（上の通り LGPL として build）。展開先を `build/tmp/audit-licenses`（`plan/tools/fresh-out.sh`、消すのは Q1）にし、script の `rm -rf` をやめた。
結果 `8944 GPL-bearing file(s)`、未知は `REmacs-1a72…/README.md` の 1 件だけ: main の distfiles に残った古い `remacs-1a724393053e.tar.gz`
（2026-10-04 から取得も参照もしない。tree のどの Makefile・script にも名前が無い）。2026-10-09 Q1 がこの archive を distfiles から消した（参照は plan/history だけ）ので all known: yes になる。

---

# ベータ1 の image の license の一覧（ws129-p002）

2026-10-04、P2（q668）。対象は release の image の当面の定義（`config/ci/config-amd64.mk` とデモの `plan/ws075/demo/config-demo-hdmi.mk` の和）。
release の config が p004 で決まったら、p006 の前に同じ command で作り直す。

- 生成物: [licenses-generated.md](licenses-generated.md)（component の表・非 Zlib の source・外部 archive の GPL の文言・残り）、
  [licenses-index.txt](licenses-index.txt)（`id<TAB>版<TAB>license<TAB>本文の path` の 1 行 1 component。image の `/usr/share/licenses/INDEX` の案）。
- 生成の script: [`tools/release/license-inventory.py`](../../tools/release/license-inventory.py) と component の表
  [`tools/release/license-components.json`](../../tools/release/license-components.json)。make に image の中身（依存を解いた package・kernel の
  option・各 package が `/usr/share/licenses/` に入れる file・外部 package の版・kernel と選んだ program の source）を聞き、表と照合する。
  `--rootfs BUILD/rootfs` で staging の上の本文の有無も確かめる。残りがあれば exit 1。
- 再生成: `python3 tools/release/license-inventory.py --config config/ci/config-amd64.mk --config plan/ws075/demo/config-demo-hdmi.mk --distfiles <main>/build/distfiles --markdown plan/ws129/licenses-generated.md --index plan/ws129/licenses-index.txt`
- host 試験: `sh plan/ws129/tests/license-inventory-test.sh` → PASS（本物の表で既知の 5 件だけ、component の無い source・SPDX の無い第三者の文・
  判断の印の無い GPL・disk に無い本文の検出、index の形）。T1 の `build/t1-full/rootfs`（CI の config の image）に `--rootfs` で当て、入れている本文は全て disk にあった。

## 結果（2026-10-04）

image に入る component は 25（自作の zedBSD・LLVM の runtime・kernel の取り込み 3・Khronos の header・libvulkan・browser の表・font・noct・remacs・
IME の辞書・clang・libcxx・openssl・openssh・curl・expat・zlib・CA 証明書・emoji・firmware 3）。227 の userland package は全て自作か表の component。
外部 package の版は make の `ZEDBSD_EXT_*_VERSION` から。

### 足りない本文（2026-10-04 に見つけ、同日 P2 が直した。Q1 の委任）

| # | component | license | 足りない物 | 直し方の案 | 持ち主 |
| --- | --- | --- | --- | --- | --- |
| G1 | zedBSD 自身 | Zlib | `/usr/share/licenses/zedbsd/LICENSE` | repo の `LICENSE` を base の package（`userland/base/licenses/` に `zedbsd` を足し、常に選ぶ）で入れる。zlib の license は binary に表示を求めないが、一覧の最初の行として入れる | main（base） |
| G2 | libc の regex（TRE・musl） | BSD-2-Clause AND MIT | `/usr/share/licenses/libc-regex/LICENSE` | `src/libc/regex/LICENSE` は TRE の BSD-2 の全文があるが musl の MIT は名前だけ。musl の COPYRIGHT の MIT の全文を足して libc の package で入れる。TRE の BSD-2 は binary の配布に表示を求める | libc の持ち主 |
| G3 | i915 の driver（Linux i915 由来、Intel） | MIT | `/usr/share/licenses/i915-driver/LICENSE` | `src/drivers/gpu/i915/display`・`intel` の 17 file が Intel の MIT。Intel の MIT の本文を `userland/base/licenses/i915-driver/` に置き、AX211 と同じく `CONFIG_DRIVER_PCI_I915=y` で自動で選ぶ | main（kernel の GPU） |
| G4 | libvulkan の Venus の宣言（virglrenderer 1.1.0、Google） | MIT | `/usr/share/licenses/libvulkan/LICENSE-PROTOCOL` | tree の `userland/desktop/libvulkan/LICENSE-PROTOCOL` を libvulkan の package の DATA で入れる | libvulkan の持ち主 |

直し（commit は phase.md）: G1 `userland/base/licenses/zedbsd/`（package `zedbsd-license`、全ての image で自動で選ぶ、Makefile の選択の行）。
G2 `src/libc/regex/LICENSE` に musl の MIT の全文を足し、libc は全ての image に在るので `zedbsd-license` が `/usr/share/licenses/libc-regex/LICENSE` として入れる。
G3 `userland/base/licenses/i915-driver/`（Linux i915・DRM の display の派生部分の著作権表示 50 行と MIT の許諾文、package `i915-driver-license`、
`CONFIG_DRIVER_PCI_I915=y` で自動、AX211 と同じ形）。G4 libvulkan の package の DATA に `LICENSE-PROTOCOL`。
確かめ: CI の config から clang・libcxx を除いた config（`plan/ws129/tests/config-amd64-ci-noclang.mk`、subagent は target の clang を build しない）で
`build/p2-ci/rootfs` を作り（rc=0、新しい warning なし）、`license-inventory.py --config plan/ws129/tests/config-amd64-ci-noclang.mk --rootfs build/p2-ci/rootfs`
→ 残りは remacs の判断だけ。CI の config そのものに当てると clang・libcxx の本文が disk に無いと出る（build していないため。T1 の `t1-full` の CI の image では在った）。

### ユーザーの判断が要る物

| # | component | 事実 | 選択肢 |
| --- | --- | --- | --- |
| D1（閉じた） | **remacs**（`/usr/bin/remacs.nap`、CI の config が選ぶ） | GNU Emacs の再実装。README は「Copyright (C) 2025 Free Software Foundation, Inc.、Copyright (C) 2026 Awe Morris」「GNU General Public License」、COPYING は無く版も書いていない。GPL の component が image に入っている。辞書（ime-dict-ja）は ws095 D1 で著作権者が zlib に再 license 済みで、本体の扱いの記録は無い | (a) 著作権者が FSF の部分を含めて license を確かめ、許されるなら再 license／(b) GPL の本文と source の入手方法を image と release に付けて配る／(c) ベータ1 の image から外す（ime-dict-ja は remacs に依らず残せる） |

| D2（閉じた） | `src/drivers/gpu/i915-old/parity/` の GPL-2.0 の file | `lcd/intel_acpi_port.c` に `SPDX-License-Identifier: GPL-2.0`（Linux の intel_acpi.c の生成物）。i915-old は kernel の build に入らない（image には無い）が tree に在る | 消す／GPL の file として tree に残す記録だけにする |

2026-10-04 Q1 がユーザーに上げた（q668 の P2 の報告から）。ユーザーの判断（同日、原文）:「i915-oldはもう使っていないので削除です。remacsはuserland/base/emacsとして
コピーを取り込み、作者としてzlibライセンスにします。特別扱いは不要です。」→ q669（P2）: `src/drivers/gpu/i915-old/` を削除。REmacs（1a72439）を
`userland/base/emacs/` に取り込み（.git を除く。noct2 の patch は今の source には不要だった）、README の FSF と GPL の文を zlib の表示に、code の各 file の
header に `SPDX-License-Identifier: Zlib`、`LICENSE` を置いた。取り込みの前の grep で FSF の著作権・GPL の文は README だけ（辞書と tools の GPL は
「SKK-JISYO.L（GPL）を使わない」という説明）。package は `userland/base/emacs/Makefile`（名前 `remacs`・install の path は今のまま、REmacs 自身の
Makefile は `Makefile.remacs`）、`userland/packages/editors/remacs` を削除。作った remacs.nap は git から取った前の build と byte で同じ。
CI の config（clang・libcxx 抜き）の rootfs で `license-inventory.py --rootfs` の open は 0。ime-dict-ja はまだ REmacs の archive から辞書を取る
（tree の `userland/base/emacs/dict` に替えるのは WS095 の持ち主の判断、残り）。
2026-10-04 追記（ws129-p012、user「remacs のコマンド名は/bin/emacsでお願いします」）: package の名前は `emacs`、install は `/bin/emacs`（同じ nap、`#!/usr/bin/noct`）と link `/usr/bin/noct -> /bin/noct`。表の id も `emacs`（生成物を再生成、open 0）。
2026-10-04 追記（ユーザー「IME の辞書はコピーして取り込んでください。」）: ime-dict-ja の辞書を `userland/desktop/ime/dict/SKK-JISYO.X` に写し、REmacs の archive の取得をやめた（zedBSD・Linux・FreeBSD）。生成物を再生成、open 0。

### 確かめて問題の無い物

- 自作の source: kernel の 381 file と選んだ program の source の SPDX は Zlib。非 Zlib の行は全て表の component の範囲（i915 の MIT 17、AX211 の
  ISC AND BSD-3-Clause 8、RTL8822B の BSD-3-Clause 1、regex の MIT 1、libvulkan の MIT 1）。SPDX の無い第三者の文（AX211 の OpenBSD・Intel の
  header、TRE、Unicode・WHATWG の生成表）も全て component の範囲。SPDX も他の著作権も無い自作の file は 1（`userland/base/curses/curses.h`、空の file）。
- i915-old の GPL-2.0 の file は上の D2（kernel の build に入らない）。
- Khronos の header（EGL・GLES・Vulkan、Apache-2.0 か MIT）は各 header の中に本文があり、development の rootfs にそのまま入る。
- noct（NoctLang、Zlib、著作権者は同じ）: binary に表示を求めない。tree の clang の resource の header は install されない。

## audit（`plan/tools/packages/audit-licenses.sh`）

ws115-p005 で分かった「openssl・openssh の tarball だけを見ている」を直し、distfiles の全ての tar の archive を見る形にした（引数で distfiles の
場所を渡せる。main の `build/distfiles` を読み取り専用で）。既知の扱い:

- autotools・libtool の補助（config.guess・ltmain.sh・m4 など）と試験・contrib・文書の directory: build の時だけで image に入らない。
- GPL・LGPL の package そのもの（gtk・glib・pango・cairo・fribidi・gdk-pixbuf・gperf・remacs の archive）: package ごとの判定。CI の image は選ばない
  （gtk4 の stack は WS114 の GTK の移植用で、image に入れるならユーザーの判断。表に `decision` で載せた）。remacs の archive は ime-dict-ja が辞書だけを使う。
- 名前で判定した 8 件（fontconfig の ABOUT-NLS、freetype の builds/unix の m4 2、libffi の ChangeLog.old・LICENSE-BUILDTOOLS・libtool-ldflags・
  msvcc.sh、zlib の FAQ）: 文書と build の補助。

結果: `sh plan/tools/packages/audit-licenses.sh /home/awe/zedBSD-claude1/build/distfiles` → `3919 GPL-bearing file(s), all known: yes`（29 archive）。
自分の worktree の distfiles（4 archive）でも `6 ..., all known: yes`。inventory の script も image の外部 package の archive の GPL の文言を数える
（[licenses-generated.md](licenses-generated.md) の節、全て build の補助・文書）。
