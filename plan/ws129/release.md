<!-- awesome-plan project=zedbsd record=ws129-release -->
# ベータ1 の release の定義（案）

> **2026-10-06 夜 ユーザー: 最初の公開ベータはベータ2（10/17 に公開するのはベータ2）。** 版は `1.0.0-beta2`、release の config は `config/release/config-amd64-beta2.mk`、手引き・既知の問題は `docs/release/zedbsd-1.0.0-beta2-*.md`、tag は `zedbsd-1.0.0-beta2(-rcN)`（ws129 の ws.md、P2 q821）。下の表の beta1・0.1.0 の名前は当時の決定の記録として残す。

Parent: [WS129](ws.md) / Phase: [ws129-p001](phase001/phase.md)（q672、P2、2026-10-04）
Status: **案**（design-reviewer の review を反映した第 2 版、§11）。ユーザーの決めていない点は §9 に並べた。決まるまで、§1〜§8 の「案」を既定にしない。code は変えていない（読みだけ）。

## 0. 前提（決まっていること）

| 決定 | 出典 |
| --- | --- |
| 目標の日は 2026-10-17（土）。凍結はしない。できた所までをベータ1 にし、安定化は後のベータで | 2026-10-02 user「凍結はしません。できたところまででベータ1にします。…」、Master fg019 |
| 版と tag は `zedbsd-0.1.0-beta1` | 2026-10-02 user「zedbsd-0.1.0-beta1 にしましょう。」 |
| CI が Prerelease を作る。ユーザーが download して動作確認し、手で Latest Release に昇格する。エージェントは昇格・公開をしない | 2026-10-02 user「CIはPrereleaseを生成、それを…手動昇格します。」 |
| 配布物は USB の image（`zedbsd-0.1.0-beta1-amd64.img.gz` の形）と Windows の QEMU/Venus の zip（`zedbsd-0.1.0-beta1-windows.zip` の形）の 2 つ。名前の細部は p004 | 2026-10-02 user「両方載せる、でお願いします。」 |
| デモの image は CI の config を土台にする（p009 で実施済み） | 2026-10-02 user |
| push・tag・GitHub の release の公開はユーザーの指示でだけ（p008） | AGENTS.md「git」 |

## 1. 今の main の状態（2026-10-04、読んだ事実）

- **版の文字**: source に直書きの `0.0.1`（`userland/base/libc/posix.c:5324-5325` の uname、`src/hal/i386/cmain.c:42,46` の i386 の起動の表示）。一つの源は無い。Settings の About（2026-10-08 から `libkeiland-backend/machine/machine.c` が compositor 経由で読む、ws188-p002）は uname から kernel の名前と release を出す（About の版の文字は uname を直せば変わる）。
- **CI**（`.github/workflows/ci.yml`）: main への push で build → `nightly-<run_number>` の Prerelease に `zedbsd-amd64.img.gz`・`Kei-nightly.zip`。zip の段は `continue-on-error: true`。SHA-256 の file は無い。版のついた release の job・tag の trigger は無い。
  - 気づいた点（p004 で直す候補、今回は変えていない）: release の job は `keiland-linux-*` の artifact を download し、`release-source` を checkout するが、それを作る job・使う step は ci.yml に無い。本文の「Keiland Linux」の節も中身が無い。WS112 の package の job を外した名残と見える。pattern に合う artifact が無い時の `download-artifact@v4` の挙動は確かめていない。
- **CI の image の config**（`config/ci/config-amd64.mk`）: 216 の program。Settings・audiod・Files・Notes・Text Editor・PDF Viewer・Image Viewer・browser・Terminal・emacs（REmacs、今日 `/bin/emacs` に、ws129-p012）・IME（keiland-ime・ime-dict-ja）・clang・libcxx・openssh・curl・旧インストーラ `zedinst`。AX211（`CONFIG_DRIVER_PCI_INTEL_AX211 := y` と firmware）・i915・HDA・RTL8822BU・Venus は y。`ZEDBSD_ROOTFS_DEVELOPMENT := y`（sysroot の `/usr/include`・`/usr/lib` を入れる。clang・libcxx は program の一覧の側で入る）。System Monitor（`monitor`、WS134）は CI の一覧に無い。
- **利用者と login**: `userland/base/etc/passwd`・`shadow` に `root` と `kei`（uid 1000、network group）が**固定の password の hash** で入る。greeter（graphical login）が既定。greeter が root の login を受け付けるかは確かめていない。
- **sshd**: `rc.conf` で `sshd: enabled: true`、`sshd_config` は `PermitRootLogin yes`・`PasswordAuthentication yes`。→ 今の CI の image を公開の release にそのまま使うと、**固定の password で root に SSH で入れる** image を配ることになる（§4、§9 の U4・U10）。これらは base の固定の file で、config で変える knob は無い（`config/rootfs-options.list` は 4 項目だけ）。release で変える仕組みは p004 で作る。SSH の host key は初回の起動で作られ（`sshd-start`）、image に焼き込まれてはいない。
- **Windows の zip**: base は `rev-0` の release の `kei-nightly-base-winq-a10-1.zip`、`KEI_NIGHTLY_ALLOW_DRAFT ?= 1`、SHA-256 は draft の値（`tools/release/kei-nightly.mk:15-17,27`）。draft の base では `make-kei-nightly-base.py:147-156` が `THIRD-PARTY.txt` の QEMU（GPL-2.0-only）と virglrenderer の Source の commit を「(not yet confirmed; draft package)」で埋める。**つまり今の zip は GPL の対応する source の commit を示していない**（今の nightly も同じ物を配っている）。zip の中の `Kei-nightly/` の名前・README・THIRD-PARTY も base に焼き込まれ、`make-kei-nightly-zip.py` は base を変えずに複写するので、名前や文言を release 用にするにも base の作り直し（ws088-p002、rev-0 への upload はユーザーの公開の操作）が要る（§3、§9 の U6）。
- **license**（ws129-p002）: `tools/release/license-inventory.py` と `license-components.json` で image の全 component（25）の一覧と本文、open 0（CI と demo の config）。生成物 `plan/ws129/licenses-generated.md`・`licenses-index.txt`。image の `/usr/share/licenses/INDEX` は案のまま。CI の完全な rootfs（clang・libcxx 込み）の `--rootfs` の確かめは T 待ち。
- **今日の主な成果**（release notes の材料）: WS131（libkeiland-backend の分離、計画のユーザーのレビュー待ち）、WS134（System Monitor、incomplete）、WS135（設定の読み書きを libkeiland に一本化、completed）、WS127（Files、p002 は eject を除き済み）、WS128（Notes の Open・Save As、Text Editor の Replace・Open Recent）、WS099（タイトルバーの検索欄・menu の drag で窓の move、試験待ち）、ws129-p002（license の一覧）・p012（emacs）。
- **既知の問題の候補**（Bug Board の tracking から、利用者に見えるもの）: BUG-145（AX211 で 5GHz の DHCP が取れない、tracking）ほか。p005 で一覧にする。

## 2. 版の付け方（p003 の入力）

| 項目 | 案 | 理由 |
| --- | --- | --- |
| 一つの源 | repository の root に `VERSION`（1 行 `0.1.0-beta1`）。Makefile が読んで `ZEDBSD_RELEASE_VERSION` にし、生成の header（`build/<arch>/gen/zedbsd-version.h`）と `/etc/os-release` を作る | file 1 つなら CI（tag との照合）・Makefile・文書が同じものを読める。Makefile の変数の直書きより変更の差分が明らか |
| nightly の版 | `0.1.0-beta1+g<短い hash>`（CI の nightly と local の build、`.git` が無ければ `+unknown`）。release の build は `VERSION` そのまま。公開の後は `VERSION` を次の版の開発の名前（例 `0.1.0-beta2.dev`）に上げる（U15） | nightly と release を uname で区別できる。`+` は SemVer の build metadata で順序に効かないので、公開の後に上げる手順が要る |
| tag | `zedbsd-0.1.0-beta1`（決定）。rc は `zedbsd-0.1.0-beta1-rcN`（U7）。release の job は tag を `^zedbsd-<VERSION>(-rc[0-9]+)?$` で解析し、合わなければ失敗する | tag と image の中の版の食い違いを防ぐ。rc の tag も同じ VERSION で通る |
| uname | `sysname` = `zedBSD`、`release` = `0.1.0-beta1`、`version` = `zedBSD 0.1.0-beta1 (<短い hash> <build の日付>)` | 今の形（`release` に数字、`version` に名前つき）を保つ |
| `/etc/os-release` | `NAME="zedBSD"`、`VERSION="0.1.0 Beta 1"`、`VERSION_ID=0.1.0-beta1`、`PRETTY_NAME="Kei 0.1.0 Beta 1 (zedBSD)"`（Kei を前に出すかは U1） | 他の OS の慣例の file。About・release notes が同じ名前を使える |
| 表示の名前 | 利用者に見える所（About・greeter・release の題）は「Kei 0.1.0 Beta 1」、技術的な所（uname・tag・file の名前）は zedbsd | 今の nightly が「Kei」（`Kei-nightly.zip`、About「the version of Kei」）と「zedBSD」（img の名前、本文の「zedBSD OS」）を混ぜている。**U1 でユーザーが決める** |
| i386 の起動の表示 | 同じ header から（版の文字だけ。HAL の実装の修正で API は変えない） | — |

## 3. 配布物（p004 の入力）

| asset | 中身 | 案 |
| --- | --- | --- |
| `zedbsd-0.1.0-beta1-amd64.img.gz` | release の config の image（UEFI、2 GiB）の gzip | 必須（決定） |
| `zedbsd-0.1.0-beta1-windows.zip` | WINQ-EMU（QEMU + Venus）と同じ image、`boot.bat` | 必須（決定）。**zip の段を release では失敗にする**（nightly の `continue-on-error` を引き継がない）。GitHub の asset は 1 つ 2 GiB 未満なので job で大きさを確かめる（2 GiB の raw image の deflate、今の大きさは確かめていない）。base の draft の扱いは U6 |
| `SHA256SUMS` | 上の 2 つの SHA-256 | 追加の案。利用者が download の破損を確かめられる（真正性は保証しないと notes に書く）。p008 の照合にも使う |
| `LICENSES.md` | p002 の生成物（image の全 component・license・本文の path） | 追加の案（U5）。本文そのものは image の `/usr/share/licenses/` と zip の `LICENSES/` |
| Keiland の deb（WS108/WS112） | — | **載せない案**（WS112 は fg019 で最下位、決定は img と zip の 2 つ）。nightly の本文の空の「Keiland Linux」の節は release の本文に入れない |

## 4. release の image の config（p004 の入力）

`config/release/config-amd64-beta1.mk` を新しく作り、`include config/ci/config-amd64.mk` の上に差分だけを重ねる（p009 のデモと同じ土台の形）。差分の案:

| 項目 | CI の今 | release の案 | 根拠・判断 |
| --- | --- | --- | --- |
| sshd | 有効、root の password の login 可 | **既定で無効**か、`PermitRootLogin no` と password の認証の禁止。base の `rc.conf`・`sshd_config` は固定の file なので、release の rootfs の option（例 `ZEDBSD_RELEASE_HARDENING` で rc.conf・sshd_config・shadow を差し替える）を p004 で作る | 固定の password の image の公開。U4 |
| 利用者と password | `root`・`kei` に固定の password | `kei`: 案 A は固定の password を release notes に書き、初回の変更を促す。案 B は初回の起動で利用者を作る（WS119 の範囲、ベータ1 に間に合う見込みは低い）。`root`: **lock（`root:*`）**して管理は `kei` から（su・doas の有無は p004 で確かめる） | U3・U10 |
| 旧インストーラ `zedinst` | 入る | 外す（WS119 の作り直しが planning。旧版を release で配ると手引きと既知の問題が増える） | U2 |
| System Monitor（`monitor`） | 入らない | WS134 が p003 の uncleared を直して合格なら入れる、だめなら入れない | WS134 の到達しだい |
| AX211 | y | y のまま。BUG-145（5GHz の DHCP）を既知の問題に | BUG-134 は resolved（2026-10-02 ユーザーの実機確認） |
| 開発の環境 | `ZEDBSD_ROOTFS_DEVELOPMENT := y`（header・lib）と program の clang・libcxx | 両方 y のまま（image の中で C を build できるのはベータの見せ所。2 GiB に入っている） | 外すなら 2 つは別々に外す（flag は clang・libcxx を外さない）。U2 |
| emacs・vim・python3 | emacs（REmacs）だけ | WS124（GNU Emacs）・WS125・WS126 は planning。ベータ1 の前に到達したものだけ足す | 「できた所まで」 |
| demo の利用者・自動 login・試験の道具（`test` は POSIX の test なので残す）・`CONFIG_INPUT_TEST_INJECT` | 入らない | 入らない（今と同じ。release の config の review で確かめる） | — |
| boot line | `ZEDBSD_GRAPHICAL_BOOT := y` | 同じ（graphical login） | — |

## 5. CI の release の job（p004 の設計）

- **別の file** `.github/workflows/release.yml` にする。nightly の `ci.yml` は変えない（WS112 の package の job と同じ file の衝突を避け、nightly の失敗が release に、release の変更が nightly に影響しない）。
- trigger: `push: tags: ['zedbsd-*']` と `workflow_dispatch`（input: `tag`、promote なら `from_rc`）。main への push・PR では動かない。`concurrency: release-${{ tag }}`。書く権限のある job の action（`softprops/action-gh-release` ほか）は tag ではなく commit の SHA に固定する。
- 最初の job `classify`（`contents: read`）が tag を解析する: `^zedbsd-<VERSION>-rc[0-9]+$` なら **build の経路**、`^zedbsd-<VERSION>$` なら **promote の経路**、どちらでもなければ失敗。`workflow_dispatch` は input の tag が存在しなければ失敗し（softprops が `target_commitish` に新しい tag を作るのを防ぐ）、`ref: refs/tags/<tag>` で checkout する。
- **build の経路**（rc の tag、`contents: read`）:
  1. tag を checkout。tag と `VERSION` の照合（§2）。
  2. `make toolchain-cache`・`make toolchain`（nightly と同じ。top-level の `make download` は使わない: 2026-10-06 ユーザー、build は config の package の Makefile が個別に取得する）。
  3. `cp config/release/config-amd64-beta1.mk config.mk`、`make`。
  4. 門: `license-inventory.py --config config/release/config-amd64-beta1.mk --rootfs build/amd64/rootfs` の open が 0（open があれば release を作らない）。
  5. `gzip -9` で img.gz、`make kei-nightly-zip` を release の file の名前で（zip の中の名前・文言は base 次第、§1・U6）。**失敗なら job を失敗**。asset の大きさが 2 GiB 未満を確かめる。
  6. `sha256sum` → `SHA256SUMS`、`LICENSES.md`、release notes の file（§6）を artifact に入れる。
  7. publish の job（`needs`、`contents: write`）は artifact を download し、`actions/checkout` は要らない（本文は artifact の notes の file を `body_path` で）。`name` = 「Kei 0.1.0 Beta 1 RC N」（U1）、`prerelease: true`、`make_latest: false`。
- **promote の経路**（最終の tag、`contents: write`）: 作り直さない。
  1. 昇格する rc を決める: `workflow_dispatch` の `from_rc`、または最終の tag と同じ commit を指す rc の tag の最大の番号。
  2. 確かめる: 最終の tag の commit と rc の tag の commit の差が `tools/release/notes/` だけ（`git diff --name-only`）。それ以外の差があれば失敗（build し直すべき）。
  3. rc の Prerelease の asset を `gh release download` で取り、`SHA256SUMS` と照合して、最終の Prerelease（`name` = 「Kei 0.1.0 Beta 1」、`prerelease: true`、`make_latest: false`）に載せる。本文は**最終の tag の commit の** notes の file（回帰と実機で見つかった既知の問題を rc の後に書き足せる）。
  4. ユーザーが GitHub の画面で Latest に昇格する。
  - 理由: ユーザーが確かめた byte と公開の byte を同じにする（build は再現可能と確かめていないので、作り直すと別の byte になる）。
- **事前の試走**（U12）: 10/08 頃、ユーザーの指示で試験用の tag（例 `zedbsd-0.0.0-test1-rc1`、VERSION を試験の値にした branch）で build の経路を GitHub で 1 回流し、Prerelease を消す。本番の初回の実行が 10/14 だと、YAML や権限の誤りに残りが 3 日しかないため。
- local の確認（p004）: YAML の構文、job の shell の部分（tag の解析・照合・gzip・sha256sum・license の門・promote の diff の検査）の local での再現、release の config の `make`。image の起動（`boot-test.sh`）は T1 に依頼する（実装の担当は QEMU を起動しない、2026-10-03 の方針）。GitHub での実行は試走と p008（ユーザーの指示）。
- 同時に直す候補（p004、Q1 の許可で）: ci.yml の release の job の使われない `keiland-linux-*` の download と `release-source` の checkout（§1）。
- tag の付け直し（rc を使わない簡単な案）は、repository で immutable releases が有効だと使えない（設定は確かめていない）。

## 6. 文書の置き場所（p005）

| 文書 | 置き場所の案 | 使われ方 |
| --- | --- | --- |
| release notes | `tools/release/notes/zedbsd-0.1.0-beta1.md` | CI の release の本文（`body_path`）。新機能・対象 platform・確認の範囲（QEMU と実機を分ける）・既知の問題への link・license の一覧への link |
| 既知の問題 | 同じ file の節（短い表、Bug の番号と回避）。詳しくは release notes から Bug Board へは link しない（plan は利用者向けでない） | — |
| 利用の手引き | `tools/release/notes/zedbsd-0.1.0-beta1-guide.md`（USB への書き方 Windows・Linux・macOS、UEFI・Secure Boot、初回の login、WiFi、使える adapter、Windows の zip の使い方）。release notes から repository の URL で link | — |
| license の一覧 | image の `/usr/share/licenses/INDEX`（p002 の index、p004 で image に入れる）、release の asset `LICENSES.md`（U5） | — |

理由: `docs/` はこのプロジェクトでは目標の設計の文書（未実装を含む）で、release の事実の記録と混ぜない。`plan/` は利用者向けでない。release の入力（`kei-nightly/`・`license-components.json`）が既に `tools/release/` にあるので並べる。**U8 でユーザーが決める**。

## 7. 日程の案（凍結なし）

| 日 | 内容 | Phase |
| --- | --- | --- |
| 10/05（月）〜10/07（水） | U1〜U15 の判断。p003（版の一つの源）、p004 の config・release の rootfs の option・release.yml（local の確認まで）。U6 で zip を載せるなら ws088-p002 の base の作り直しを並行で始める（担当と期限は U6） | p003・p004・ws088 |
| 10/08（木） | 試験用の tag で release.yml の試走（U12、ユーザーの指示）。直しは 10/09 まで | p004 |
| 10/08〜10/12（月） | 各 WS の作業を続ける（凍結なし）。10/12 に release notes の草稿の材料を ws.md から集める。U6 の新しい base の rev-0 への upload（ユーザー）と SHA の更新 | p005・ws088 |
| **10/13（火）** | **RC の commit を Q1 が選ぶ**。release notes の草稿をユーザーに | p005・p006 |
| 10/13〜10/14（水） | RC の image で最終回帰（§8 の (a)）。FAIL は直すか既知の問題にするかを Q1 とユーザー | p006 |
| **10/14（水）** | ユーザーの指示で rc1 の tag → CI の Prerelease | p008 の一部 |
| **10/15（木）〜10/16（金）** | rc1 の asset で実機の確認（§8 の (b)）。必要なら rc2。見つかった既知の問題を notes に書き足す（promote の経路で最終の本文に入る） | p007・p005 |
| **10/17（土）** | ユーザーの指示で最終の tag → promote → Prerelease。ユーザーが確かめて Latest に昇格 | p008 |

注意（U9）: 「10/12 の main までを release notes の新機能に、10/13 からは RC に入れる差分を Q1 が選ぶ（bug の直しだけ）」は、ユーザーの「凍結はしません」とぶつかりうる。**事実上の機能の締切を 10/13 に置いてよいか**をユーザーに問う。置かないなら、rc を作り直すたびに最終回帰と実機の確認をやり直す日程が要る。

## 8. 最終回帰と実機の確認の中身の案

(a) **最終回帰（p006、RC の release の config の image、QEMU）**。集約の `make check` は使わない。T1・T2 に 1 回にまとめて依頼:

- `plan/tools/boot-test.sh`（framebuffer の login prompt、PNG をユーザーに）。
- `plan/ws099/tests/criteria.sh` の C1〜C5・C7〜C10（Venus）。
- Settings の `plan/ws089/tests/settings-regress.sh`、Files の主な試験（WS127 の回帰の組）、Notes・Text Editor（WS128）、`plan/ws129/tests/remacs-guest.sh`（emacs）、IME（WS095 の ime の試験）。
- network: `plan/ws033/tests/managed-lan-host-test.c`（host）と QEMU の有線の DHCP。
- `license-inventory.py --rootfs`（open 0）と `license-inventory-test.sh`。
- 5330 の passthrough の smoke（`plan/ws099/tests/c5-hw.sh`、`/tmp/i915-hw.lock`）は、Master の「iGPU は i915 の driver の改善の Phase だけ」（master:blocked）に触れるので、**U11 でユーザーが許したときだけ**。許さないなら実機の確認（b）で代える。
- Windows の zip: 中身の一覧と `boot.bat` の存在（Windows での起動は実機のユーザー）。

(b) **実機の確認（p007、ユーザー、`plan/ws129/phase007/checklist.md` を p006 の前に作る）**: USB から起動（5330・5320）→ graphical login → desktop → Files・Settings・Terminal・Notes・Text Editor・emacs・browser → 音 → WiFi（AX211、2.4GHz と 5GHz、BUG-145）・USB の LAN（ws033-p002）→ 5320 の LCD（ws118-p004）→ Shut Down で電源が切れる。Windows の zip を Windows の PC で起動。インストーラ（ws119-p006）は release の image に入るときだけ（U2 で外すなら p007 から外す）。安定版 S2 の実機試験（WS133）と同じ回にまとめられるかは Q1 とユーザーが決める。

## 9. ユーザーが決める点

| # | 問い | 選択肢（案は太字） | 何が待つか |
| --- | --- | --- | --- |
| U1 | 利用者に見える名前 | **「Kei 0.1.0 Beta 1」（技術の名前は zedbsd）** / 「zedBSD 0.1.0 Beta 1」/ 「Kei (zedBSD) 0.1.0 Beta 1」 | p003（os-release・About）、release の題 |
| U2 | release の image に入れる物 | **旧インストーラ `zedinst` を外す、開発の環境（header・clang・libcxx）は残す**、System Monitor は WS134 の合格しだい | p004、p007 の範囲 |
| U3 | `kei` の初回の login | **案 A: 既知の password を release notes に書き、変更を促す** / 案 B: 初回の起動で利用者を作る（WS119、間に合わない見込み） | p004・p005 |
| U4 | SSH | **sshd を release では既定で無効** / 有効のまま root と password の login を禁止 / 今のまま | p004（**公開の前に要る判断**） |
| U5 | asset の追加 | **`SHA256SUMS` と `LICENSES.md` を載せる** / img と zip だけ | p004 |
| U6 | Windows の zip | **base を fork の commit の確定した版に作り直し（ws088-p002、担当と期限を決め、rev-0 への upload はユーザー）、間に合えば載せる。間に合わなければベータ1 は zip を載せない** / draft のまま載せる（GPL の対応する source の commit を示さないので**推さない**）。あわせて今の nightly の zip も同じ状態であることの扱い | p004、日程 §7 |
| U7 | RC の作り方 | **rc の tag ごとの Prerelease、合格した rc の asset をそのまま最終の tag に（§5 の promote）** / 最終の tag 1 つ（だめなら付け直す。immutable releases なら不可） | p004 の job の形、p008 |
| U8 | 文書の置き場所 | **`tools/release/notes/`** / `docs/release/` / repository の root | p005 |
| U9 | 日程と事実上の締切 | **RC の commit 10/13（その日を事実上の機能の締切とする）、rc1 10/14、実機 10/15〜16、公開 10/17** / 締切を置かず rc ごとにやり直す / ほかの日 | p006〜p008、実機のユーザーの時間 |
| U10 | root | **lock（`root:*`）して管理は `kei` から** / 既知の password のまま | p004 |
| U11 | 5330 の passthrough の smoke を最終回帰に入れるか | 入れる（master の iGPU の制限の例外） / **入れず実機の確認で代える** | p006 |
| U12 | release.yml の試走 | **10/08 頃に試験用の tag で 1 回（ユーザーの指示）** / 本番の rc1 が初回 | p004 |
| U13 | release notes と手引きの言語 | **英語（GitHub の release の本文）、日本語の手引きは後** / 日本語 / 両方 | p005 |
| U15 | 公開の後の nightly の版 | **公開の直後に `VERSION` を `0.1.0-beta2.dev` に** / ほかの名前 | p003・p008 |

（U14 は U6 にまとめた。）

### ユーザーの決定（2026-10-04、AskUserQuestion）

| # | 決定 | 影響 |
| --- | --- | --- |
| U1 | **「Kei/zedBSD 1.0.0 Beta 1」**（ユーザーの記入。案の 0.1.0 ではなく **1.0.0**） | p003: `VERSION` は 1.0.0 の系（`1.0.0-beta1`）、os-release・About・release の題。U15 の案も `1.0.0-beta2.dev` に読み替える（U15 は未決） |
| U3 | 案 A: 既知の password を release notes に書き、変更を促す | p004・p005 |
| U4 | **今のまま**（sshd は有効、password の login も今の通り） | p004: release の rootfs で sshd・shadow を変えない。release notes に既知の password と sshd が有効であることを明記（p005） |
| U6 | base を fork の確定した commit で作り直し（ws088-p002）、間に合えば載せる。間に合わなければベータ1 は zip を載せない。今の nightly の zip も同じ扱い | p004、日程 §7 |
| U2 | 旧インストーラ `zedinst` を外し、開発の環境（header・clang・libcxx）は残す。System Monitor は WS134 の合格しだい | p004・p007 |
| U5 | `SHA256SUMS` と `LICENSES.md` を載せる | p004 |
| U7 | rc の tag ごとの Prerelease、合格した rc の asset をそのまま最終の tag に（§5 の promote） | p004・p008 |
| U9 | RC の commit 10/13（事実上の機能の締切）、rc1 10/14、実機 10/15〜16、公開 10/17 | p006〜p008 |
| U8 | **`docs/release/`**（案の tools/release/notes/ ではなく） | p005 |
| U10 | root を lock（`root:*`）して管理は `kei` から | p004 |
| U11 | 入れず実機の確認で代える | p006 |
| U12 | **試走は不要**（ユーザー「すでに動作しており不要です。」） | p004 |
| U13 | 英語（GitHub の release の本文）、日本語の手引きは後 | p005 |
| U15 | 公開の直後に `VERSION` を `1.0.0-beta2.dev` に | p003・p008 |

## 10. 次の Phase への引き渡し

- p003: U1・U15 の後。`VERSION`・生成の header・os-release・uname・i386 の表示・`.git` の無い build の fallback。
- p004: U2〜U7・U10・U12 の後。`config/release/config-amd64-beta1.mk`・release の rootfs の option（sshd・shadow）・`release.yml`（rc の build と promote）・ci.yml の名残の掃除（Q1 の許可）。zip の名前と README は ws088 の base 次第。
- p005: U3・U8・U13 の後。release notes・手引き・既知の問題（rc の後の書き足しを含む）。
- p006・p007・p008: §7・§8 の通り。

## 11. review の記録（2026-10-04、design-reviewer）

第 1 版への指摘と反映: H1 rc の tag と VERSION の照合の矛盾、build と promote の経路の分け方 → §2・§5。H2 最終の tag の commit の notes が古くなる、publish の job に checkout が無い → §5（notes を artifact に、promote は notes だけの差分を許す）。H3 zip が GPL の source の commit を示さないことは code で確定（`make-kei-nightly-base.py:147-156`）→ §1・U6（draft は推さない、今の nightly も同じ）。H4 sshd・password を release で変える仕組みが無い、root も固定 → §1・§4・U10。M1 RC の日が事実上の凍結 → §7・U9。M2 本番の初回の実行が遅い → §5・U12。M3 zip の名前・README は base に焼き込み → §1・§5。M4 passthrough の smoke は master の制限 → §8・U11。M5 workflow_dispatch の意味 → §5。M6 ws119 の実機とインストーラを外す案の矛盾 → §8。L1 program の数（216）と開発の flag の意味 → §1・§4。L2 公開の後の版・`.git` の無い build → §2・U15。L3 実装の担当は QEMU を起動しない → §5。L4 action の SHA の固定・asset の 2 GiB・SHA256SUMS は真正性でない → §3・§5。L5 immutable releases → §5・U7。
