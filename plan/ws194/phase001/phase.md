<!-- awesome-plan project=zedbsd record=ws194-p001 -->

# ws194-p001: keiland-linux・keiland-freebsd の package の確認と install の確認

Status: planned（2026-10-09 Q1、q919 P1）
Parent: [WS194](../ws.md)

## 手順

1. root の Makefile の keiland-linux・keiland-freebsd の規則、plan/tools/keiland-linux・keiland-freebsd の guest の build の手順（必要な package の既存の一覧）を読む。
2. package の一覧を一箇所に（apt・dnf/yum・pacman・pkg の名前の対応）。足りない物の検出（dpkg-query・rpm -q・pacman -Q・pkg info）。
3. 対話: 一覧を出し「install しますか [y/N]」、sudo で導入。build の成功の後に install の確認。端末でない時は止まって一覧を出す。
4. host 試験（検出と一覧の形、対話の分岐を模擬）。

## 受け入れ

- 設計と実装の記録、host 試験 PASS、T1 の依頼の行（p002）。

## 2026-10-09 P1 q919: 設計と実装（test-wait）

Q1 了承（2026-10-09）: 下の 1〜5 の形。

1. 新しい POSIX sh の `tools/build/keiland-prerequisites.sh`（Linux・FreeBSD の両方）:
   - `check linux|freebsd`: package manager を見つけ（apt＝dpkg-query、dnf・yum＝rpm -q、pacman -Q、FreeBSD＝pkg info -e）、要る package のうち入っていない物を一覧にする。端末（stdin と stdout が tty）なら「Install them now with: sudo … ?」を y/N で聞き、y で導入（root なら sudo 無し、sudo が無ければ doas）。N・端末でない・`KEILAND_ASK=n` の時は一覧と命令を出して止まる（exit 1、build を始めない）。全部あれば何も言わずに進む。package manager が分からない時は要る物を文で出して進む。
   - `offer-install linux|freebsd COMMAND…`: build の成功の後、端末なら「The build succeeded. Install Keiland now with: sudo … ?」を y/N で聞き、y で実行。端末でない・N・`KEILAND_ASK=n` の時は命令を出して成功で終わる。
2. 要る package の表（script の中の一箇所）: apt＝build-essential libvulkan-dev linux-libc-dev python3 curl（LINUX.md と同じ）、dnf・yum＝gcc make vulkan-headers vulkan-loader-devel kernel-headers python3 curl、pacman＝base-devel vulkan-headers vulkan-icd-loader linux-api-headers python curl、pkg＝gmake python3 meson ninja vulkan-headers vulkan-loader libdrm mesa-dri seatd（README.freebsd.md と同じ）。apt と pkg 以外は試験しない（ユーザー「テストはaptだけでいいです」）。
3. 配線: root の Makefile の `keiland-linux`・`keiland-freebsd`、GNUmakefile の FreeBSD の入口、BSDmakefile の `keiland-freebsd` を「check → build → offer-install」にした（install には make の command line の変数を渡す）。BSDmakefile では gmake の前に check が走るので、gmake 自体も pkg で入れられる。
4. `KEILAND_ASK`（make の変数、既定 y）で質問を省ける。.mk を直接呼ぶ既存の自動の build（tools/release/keiland-linux-deb、plan/tools/keiland-*）には影響しない。
5. `userland/desktop/LINUX.md`・`README.freebsd.md` を新しい流れに直した。

### 確かめ

- host 試験 `plan/ws194/tests/prerequisites-host-test.sh`（偽の dpkg-query・apt-get・rpm・dnf・pacman・pkg・sudo・id・make を PATH の先に、端末は script(1) の pty）: 11 項目 PASS（apt で全部ある時は何も聞かない、足りなくて端末が無い時は一覧と命令で止まる、端末で y なら sudo で導入、n なら止まる、KEILAND_ASK=n は聞かない、dnf・pacman の名前、pkg で gmake を y で導入、install は端末が無ければ出すだけ・y で sudo で実行・n でも成功）。`checkbashisms` 指摘なし、`sh -n` ok（Debian の /bin/sh＝dash で実行）。
- host（Debian、package は揃っている）で `make -j32 keiland-linux < /dev/null` rc 0: check は何も言わず、build の後に「The build succeeded. Install Keiland with: sudo make -f userland/desktop/keiland-linux.mk install」。
- `make -n keiland-linux`・`keiland-freebsd` で check と offer-install の行が入ること。
- 未実施: Debian 13 の guest（apt、足りない状態から）と FreeBSD 15 の guest（pkg）での対話の通し（T1、p002）。

## T1-498 の FAIL の直し（2026-10-09、P1）

T1-498: Debian 13 は PASS、FreeBSD 15 は端末つきの `make -j8 keiland-freebsd` が質問せずに「install them with: pkg install -y seatd」で止まった（2 回）。原因: script は標準入力と標準出力が端末かで判定していたが、BSD make の `-j` は job の出力を pipe で集め、入力も端末でない（GNU make の -j も標準入力は 1 つの job だけ）。
直し 7ff549ea9: 質問と答えを制御端末 `/dev/tty` で行い、開けるかで判定する（subshell で: 特別な組み込みの redirection の失敗は shell を終わらせる）。端末の無い CI（制御端末なし）は今まで通り質問しない。host 試験に「制御端末は在るが標準入出力は端末でない（make -j の job）」の場合を足し、前の script では T1-498 と同じ止まり方、今は y で導入を確かめた。端末なしの場合は setsid で制御端末を外して流す。`sh plan/ws194/tests/prerequisites-host-test.sh` → PASS（13 件）。
注: `ssh -t` で `< /dev/null` を付けても制御端末が在るので質問は出る。端末なしの確かめは制御端末の無い形（`-t` なしの ssh）で。再試験は T1 の行（Q1 が番号）。
