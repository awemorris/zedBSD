<!-- awesome-plan project=zedbsd record=ws193-p001 -->

# ws193-p001: menuconfig の新しい階層と Build boot image

Status: planned（2026-10-09 Q1、q918 P1）
Parent: [WS193](../ws.md)

## 手順

1. tools/menuconfig.py・root の Makefile の menuconfig の規則・config.mk の変数・menuconfig-target-host-test.py を読む。
2. ws.md の階層を実装する。Base・Desktop の All / Select、Packages の階層は userland/packages の directory の分類から作る。
3. Build boot image: menu から image の build を `-j$(nproc)` で走らせ、進捗の bar と今の対象の名前を出す（make の出力から対象を拾う形など、設計を phase.md に）。失敗の時は log の場所と末尾を出す。
4. host 試験（menuconfig-host-test の追従、階層・既定値・保存の形）。build warning 0 に相当する確認。

## 受け入れ

- 設計と実装の記録、host 試験 PASS、T1 の依頼の行（p002）。

## 2026-10-09 P1 q918: 設計・ユーザーの決定・実装（test-wait）

### ユーザーの決定（Q1 経由）

- (a) 原文に無い今の項目について「menu から外す」→ Variant（disk の形）・kernel option・driver の選択・試験の hook・Noct の GPU accel は menu に出さない。config.mk の直接の記述だけにし、読んだ config.mk の値は保って書き戻す（消さない）。
- (b)「Firmwareはトップレベルに階層を作る。X11とTestsはメニューから削除し、直接記述のみにする。」→ toplevel に Firmware（置き場は Q1 の判断で Packages の次、空行の前）。X11 と Tests は menu から外し、値は保つ。

### 形

- toplevel: CPU / Board、Boot Option、Development、Base、Desktop、Packages、Firmware、空行、Build boot image、空行、Exit（Exit と Esc/q は保存して終わる）。
- CPU は x86_64・arm64、Board は CPU に従う（x86_64→UEFI + ACPI、arm64→Raspberry Pi 4）。他の platform の config.mk は値を保って現在の値として表示。
- Boot Option: Graphical boot（ZEDBSD_GRAPHICAL_BOOT と ZEDBSD_BOOT_KERNEL_MESSAGES を逆に揃えて書く）、Graphical login（新しい ZEDBSD_GRAPHICAL_LOGIN、Makefile の既定は ZEDBSD_GRAPHICAL_BOOT に従う。amd64 の vmunix.mk で login=graphical を logo と分け、cfg の名前に login の値も入れた）、Mirror kernel messages to serial port（CONFIG_PCAT_SERIAL_MIRROR）。
- Development: Install development files だけ。
- Base・Desktop: `[*] All`（全部が選ばれていれば [*]、押すと全部（依存も）を選び、もう一度で既定の選択に戻す）と Select（個別、依存の自動選択と外す時の警告は今のまま）。Firmware は個別の一覧。
- Packages: userland/packages の分類の directory から自動で作る（名前は lang→Languages ほかの表、無い物は先頭を大文字）。
- Build boot image: 確認の後に保存し、`make -n --trace disk-image` で作り直す target を数え、`make -j$(nproc) --trace disk-image`（ZEDBSD_JOBS で変えられる、今までどおり）の trace の行で進捗の bar（済み/全体と %）と今の target を curses に出す。出力は build/<platform の dir>/menuconfig-build.log、失敗の時は status・log の場所・末尾 20 行。
- 今の Build all・toolchain・kernel・rootfs、Select target・kernel option・drivers・rootfs option の項目と、使わなくなった code（ARCHITECTURES、DRIVER_CATEGORIES、option 編集）を消した。`make list-user-programs` は 1 回だけ読む（今は描き直しのたびに make を起動していた）。README の Variant の説明を config.mk の直接の記述に直した。

### 確かめ

- `make menuconfig-host-test`（plan/tools/menuconfig-target-host-test.py、Q1 の許可で追従）PASS: 既存の round-trip・依存・Fonts の walk に加え、toplevel の項目の並び、Boot Option の 3 つの切り替え（Graphical boot が kernel の message を逆にする）、Base の All の全選択と 2 回目の既定への戻り、menu に出さない値（X11 の zterm の選択、CONFIG_BUF_CACHE_KIB）の保存と読み直し、ZEDBSD_GRAPHICAL_LOGIN の round-trip、Build boot image を stand-in の tree（3 段の disk-image と失敗する disk-image）で走らせて進捗が 3/3、失敗の時の log の末尾。
- pty で実際の curses の menu を動かした（Base → All、Desktop → All、Exit）: 保存した config.mk で base 189/189・desktop 33/33 が選ばれた。
- boot の cfg: `ZEDBSD_GRAPHICAL_LOGIN=n` で login=graphical が無く、既定（y）は前と同じ行の並び（logo、login、kmsg）。
- 未実施: 実際の image を menu の Build boot image で作ること（P は disk-image を流さない、T1）、進捗の画面の撮影、boot-test。
