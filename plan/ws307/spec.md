<!-- awesome-plan project=zedbsd record=ws307-spec -->

# WS307 インストーラの仕様

- 親: [WS307](ws.md)。作成: [ws307-p001](phase001/phase.md)（P4、2026-10-28）。
- 状態: **確定**（ユーザーの確認事項 U1〜U29・J1〜J5 は回答済み、§15 に原文。J1 は J5 で置き換えた。design-reviewer の 1〜3 回目の指摘は §17 で反映済み、J5 の反映は §17.4。実装の Phase の表は ws.md）。
- 正はユーザーの原文（[ws.md](ws.md) の「ユーザーの指示と希望の仕様」）。この文書は原文のブラッシュアップで、原文と食い違えば原文が正。
- 調査の出典は file:行（基点 main 5300ea917。3 回目のレビューの反映は main d483706d4 で確かめた）。

## 1. 範囲

- 対象: UEFI＋ACPI の amd64 の PC だけ（原文「インストーラはUEFI+ACPIなPC用で、他のプラットフォームは考慮しない」）。BIOS・i386・PC-98・aarch64・riscv64 などは扱わない。
- 形: Keiland のアプリ（Wayland の client、libkeiland の UI）。ウィザード形式で「次へ」で進む。
- 導入先: kernel が block device として登録する、書き込みのできる物理 disk。今の driver で NVMe（`nvme<C>n<N>`、`src/drivers/pci/pci-nvme.c:5630`）・SATA（AHCI、`sdX`、`pci-ahci.c:2281`）・USB Mass Storage（BOT・UAS、`sdX`、`usb-storage.c:1818`・`usb-uas-disk.c:412`）。Intel VMD の配下の NVMe も NVMe として出る（`pci-vmd.c`）。
- 入れる物: Base System（header 込み）・Keiland Desktop・Firmware・Packages（Clang・OpenSSH・OpenSSL＋CA 証明書・GTK4・Fonts・libavcodec・curl・Python と、その依存。package も header と `.pc` を入れる、U13）。
- 範囲外は §13。

## 2. 用語

| 語 | 意味 |
| --- | --- |
| live の system | installer が動いている system（導入元。`make installer-image` で作った USB の image、§11.2）。live の利用者は `user` で、password は公開の既定の `user`（U26・J5）。installer の image を運用に使ってはいけない。この image での起動は root での起動に相当と扱う（J5、§9.1） |
| 導入先 | 選ばれた disk と、その上の partition |
| unit | `/opt/installer` に置く tarball の単位。Base・Desktop・Firmware の 3 つと、`userland/packages/` の package ごと |
| helper | root の権限で disk の操作と展開を行う program（`install-admin`、§9） |
| 計画（plan） | 画面で決めた内容（disk、partition の表の変更、root・swap の割り当て、unit の一覧）。最終確認の後に helper に渡す |
| 公開（publish） | 導入先が起動できる状態にする最後の書き込み（loader と `zedbsd.cfg`）。§7 |

## 3. 全体の構成

```text
installer（Keiland のアプリ、live の session の利用者 `user` で動く、U26。起動は root 起動相当、J5）
  │ 画面・検査・計画の組み立て（disk には触らない）
  │ stdin/stdout の pipe（行の protocol、§9.2）
  ▼
install-admin（set-user-ID root の helper、1 つの仕事だけ）
  │ probe（読むだけ）／ install（計画を実行）
  ├─ /sbin/diskpart（GPT の書き込み、reload）
  ├─ /sbin/mkfs -t fat32 / -t ufs --profile=native（device の format）
  ├─ /sbin/mkswap（swap の header。partition の mode を足す、§11.3）
  └─ unit の検査（SHA-256）と展開（ustar＋deflate）、設定の書き込み
/opt/installer/（live の system の読み取り専用の payload、§6）
```

- installer は disk の device を開かない。disk の一覧・partition の表・ESP の空きは helper の `probe` から得る。
- disk に書くのは helper だけ。書く時は 2 つ（U2 の回答「ボタンでその場で書く」）:
  - S4（partition の編集）の「削除」「作成」: 押した時にその場で partition の表を書く（§4.4・§7.1）。キャンセルしても書いた分は残る。
  - それ以外（disk 全体を使う時の表、root・swap の型の印と format、展開、設定、公開）: 最終確認の後（§7.2）。root・swap の型の印と format は最終確認の後（U14 の回答）。

## 4. 画面

共通:

- 1 つの window。下の帯に「戻る」「次へ」（primary）、右上に「キャンセル」。Enter は「次へ」、Alt+←/→ は戻る・次へ（Settings の Welcome と同じ、`userland/desktop/settings/welcome.c:186-253`・`:308`）。
- 「次へ」は、その画面の検査を満たさない時は無効にし、理由を 1 行で出す。
- 「キャンセル」は、どの画面でも確認の dialog（`kl_dialog`）の後に S10 に移る（原文「キャンセルや失敗時にここに飛ぶ」）。S10 は disk に書いた変更の一覧を出す（何も書いていなければ「disk は変えていません」）。
- 文字は `kl_tr` で翻訳する（domain `installer`、§10）。
- 試験のため、Settings と同じ形で `INSTALLER READY`・画面の名前・control の矩形を session log に出す（`plan/tools/aat/scenarios/aatlib.py:71-77`、`settings/ui.c:1755` の形に倣う）。

### 4.1 S1 Welcome

- 表示: OS の logo、「<OS 名> をこのコンピュータに入れます」の説明（OS 名は `/etc/os-release` の `NAME`、今は zedBSD。U15・U28、§13a）、注意（導入先の disk の内容が消えうる、data の backup を勧める）、規則「この image は導入のためだけの物で、運用（普段の system としての常用）に使ってはいけない。この image で起動した system では、利用者の program が root と同じ力を持つ」（J5、§9.1）、言語（desktop の `ui.language` に従う、§10）。
- 検査: helper の `probe` が成功している（失敗なら理由と「再試行」を出し、次へは無効）。UEFI で起動していない時（boot provenance の `kern.boot.firmware_partition`、`docs/reference/boot-provenance.md`。`/boot/efi` の有無は MBR の USB からの UEFI の起動で出ないので使わない、`src/kern/vfs.c:1189-1285`）は「UEFI で起動した時だけ使える」と出して次へを無効にする。
- 遷移: 次へ → S2。

### 4.2 S2 導入先の disk の選択

- 表示: disk の一覧（1 行 1 台）: 名前（`nvme0n1`・`sda`）、種類（NVMe・SATA・USB、`hw.diskstats` の kind、`include/uapi/sysctl.h:104-170`。AHCI は kind が IDE なので（`src/drivers/pci/pci-ahci.c:2296`）「SATA/IDE」と出す）、大きさ（`BLKGETINFO` の sector の数×大きさ）、取り外し可の印、partition の要約（数・ESP の有無・partition table の種類）。型番は出さない（U9 の回答「今の口で出せる物だけ」。kernel は model・serial を userland に出していない）。
- 選べない disk は灰色で理由を出す:
  - live の system が使っている disk: 全ての mount の source、boot の slot（`boot0`〜`boot3`）、`/boot/efi`、有効な swap の source、loop の裏の file の disk（`volumed` の求め方は `/` だけで loop の root を辿らない、`userland/base/volumed/main.c:480-540`。helper はこれらを全て親の disk まで辿る）。
  - 読み取り専用（`KERN_BLOCK_READ_ONLY`）、file を裏に持つ物（`loopN`・ramdisk）。
  - 小さすぎる（§5.6 の最小）。
- disk の抜き差し: helper の `probe` を「再読み込み」ボタンで取り直す（自動の追従は Future）。
- 検査: 1 台を選んでいる。
- 遷移: 戻る → S1、次へ → S3。

### 4.3 S3 partition の扱い

- 表示: 2 つの選択肢（radio 相当。libkeiland に radio が無いので、選択の印の付いた大きな button 2 つ、`kl_button`）。
  - 「disk 全体を使う」: 既存の partition が全て消えることを、既存の partition の一覧（番号・大きさ・型・label）とともに出す。
  - 「partition を選ぶ」: 既存の partition を保ち、空きに作る。
- 「partition を選ぶ」が選べない disk（理由を出す。「disk 全体を使う」は選べる）:
  - GPT でない disk（空・MBR）。U10 の回答「GPT の disk だけ」。
  - diskpart が編集できない GPT: backup の header が最後の LBA に無い（小さい image を dd した USB に多い）、entry の大きさが 128 でない、header が 92 でない、hybrid MBR、protective MBR の大きさの不一致、片方の copy が壊れている（`userland/base/diskpart/table.c:364-397`。`dp_add`・`dp_delete` は EOPNOTSUPP、`:416`・`:449`）。
  - 使っている entry が 16（diskpart の上限、`userland/base/diskpart/main.c:401`）。17 以上は kernel が 1 つも出さない（`src/drivers/disklabel/gpt.c:1316-1319`）。
  - 別の zedBSD がある disk: disk のどれかの FAT の root に `/zedbsd.cfg` がある（loader は同じ disk の FAT を順に探し最初の `zedbsd.cfg` を使う、`bootloader/uefi/bootx64.c:1502`・`:1668-1673`・`:1815`。今の native の image は `zedbsd.cfg` を ESP でなく ZEDBSD の FAT に置く）。共存させると古い loader が新しい方を起動し、古い方が起動しなくなるため。
  - 既存の ESP があり、それが「十分な ESP」（§5.4）でない（U16 の回答「その disk では無効にする」）。
- 検査: どちらかを選んでいる。
- 遷移: 戻る → S2。全体 → S7。選ぶ → S4。

### 4.4 S4 簡易の partition の編集（「partition を選ぶ」の時）

- 表示: partition の一覧（slot・開始・大きさ・型の名前・label・「新規」「未使用」の印）と空き領域の行。下に「削除」「作成」。
- 操作は押した時にその場で disk の partition の表に書く（U2 の回答「ボタンでその場で書く」）。手順は §7.1:
  - 画面に入った時、1 回だけ「この画面の削除・作成はその場で disk に書きます。キャンセルしても元に戻りません」を出す。最初に書く操作の前に helper の session を始める（§9。helper は password を聞かない、U26。installer の image の起動は root 起動相当、J5）。
  - **削除**: 選んだ partition を消す。確認の dialog（partition の番号・大きさ・型・label と「中身は消えます」、`KL_BUTTON_DANGER`）の後に書く。mount 中の partition（live の system が使っている物、helper が外せない物）は削除の button を無効にし理由を出す。
  - **作成**: 大きさだけを聞く（`kl_field`、MiB・GiB の単位、既定は最大の空き。ESP を自動で作る時は最大の空きから ESP の 512 MiB と境界の分を引いた値）。置き場所は大きさが入る最初の空き（LBA の小さい方）、開始と大きさは 1 MiB に揃える。型は zedBSD 独自の「未割り当て」（U3 の回答、§5.3）、label は無し。確認の dialog は出さない（既存の data を消さないため）。書いた後、helper は新しい partition の先頭と末尾の 1 MiB を 0 で埋める（diskpart は data の sector を消さない、`docs/reference/block-administration.md:60`。前の image の FAT と `zedbsd.cfg` が残ると、loader の候補になり起動できるように見えるため）。
  - **ESP の自動作成**: 作成の時、disk に ESP（型 `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`）が無ければ、新しい partition の前に ESP（§5.4 の 512 MiB）を同じ空きに作り、`mkfs -t fat32` で format する。ESP と新しい partition の両方が入らない、または使っている entry が 15 以上（diskpart は使っている entry を 16 までしか持てない、`userland/base/diskpart/main.c:401`）の時は、何も書かずに作成の失敗を出す。表を書いた後に 0 埋めか ESP の format が失敗した時は、作った 2 つの entry を batch の削除で 1 回で消す書き込みを試み、作成の失敗として出す（消せなければ、その状態を出す）。
  - 書く前の条件（§7.1 の段 2）: 導入先の disk の全ての partition が mount されていない（外せる物は外す）こと、`BLKREREADPART` が成功すること。満たさなければ何も書かずに理由を出す（diskpart の `add` は書く前の reload の EBUSY を無視して書き、reload に失敗して exit 3 になる、`userland/base/diskpart/main.c:380-384`。押した button で「再起動まで使えない」disk を作らないため）。
  - 書いた後は helper が表を読み直し、一覧を disk の実際の表で描き直す（画面の想定でなく）。
  - S4 で 1 つでも書いたら、S5・S6 の選択と S8 の確認を消す（slot の番号は再使用されるので、前に選んだ root が別の partition を指しうる）。
  - 表を書いたが kernel の reload が失敗した時（diskpart の exit 3。導入先の partition が mount 中など）: 「表は書いたが、この disk の新しい partition は再起動まで使えない」を出し、次へを無効にする（S10 へ進む button を出す）。
- 検査（次へ）: disk に「十分な ESP」（§5.4）が 1 つ以上ある。無ければ次へを無効にし理由を出す。
- 遷移: 戻る → S3（書いた変更は残る。S3 で「disk 全体を使う」に変えると、全体の道の表で置き換わる）。次へ → S5。

### 4.5 S5 rootfs の partition の選択

- 表示: disk の partition のうち、root に選べる物（U12 の回答「ESP 以外の既存も選べる」）: ESP 以外の全て。ただし mount 中の物（live の system・Files が使っている物で helper が外せない物）は灰色。各々の大きさ・型・label・fs の型と、必要な最小（§5.6）。型が「未割り当て」の物（S4 で作った物）を先に並べる。
- 未割り当てでない partition を選んだ時は、確認の dialog（partition の番号・大きさ・型・label・fs と「中身は消えます」、`KL_BUTTON_DANGER`）を出す（U12 の回答）。他の OS の partition を消しうるので、型の名前（Microsoft Basic Data・Linux など）と fs の label を必ず出す。
- 選んだ partition は計画の上で型を UFS（`516E7CB6-6ECF-11D6-8FF8-00022D09712B`、`tools/build/zedimage-host.c:428-431` と同じ）、label を `zedBSD installed root` にし、最終確認の後に `mkfs -t ufs --profile=native` で初期化する（U14 の回答）。
- 検査: 1 つを選んでいる（U11 の回答「rootfs 未選択なら進めない」）。大きさが最小以上。
- 遷移: 戻る → S4、次へ → S6。

### 4.6 S6 swap の partition の選択

- 表示: root 以外の選べる partition（S5 と同じ規則と確認の dialog）と「swap の partition を使わない」の行。
- 選んだ partition は型を swap（`516E7CB5-6ECF-11D6-8FF8-00022D09712B`）、label を `zedBSD installed swap` にし、最終確認の後に swap の header を書く（`mkswap` の partition の mode、§11.3。U14 の回答）。
- 選ばない時: 次へで確認の dialog（「swap の partition がありません。root の UFS の上に swap file を作ります」）を出す（原文「swapパーティションは選択しなくてもいいが、確認はされる」）。swap file は §5.5。
- 検査: 最小の大きさ（64 MiB）以上。
- 遷移: 戻る → S5、次へ → S7。

### 4.7 S7 入れる項目の選択（S3 の両方の道の合流）

- 表示: check box 相当の行（`kl_switch` か選択の印の付いた行）。

  | 項目 | 中身（unit） | 既定（U8 の回答） |
  | --- | --- | --- |
  | Base System（header 込み） | `base`（kernel・loader・base の program・library・base の `/usr/include`） | 必須（外せない） |
  | Keiland Desktop | `desktop`（compositor・sessiond・標準の app・desktop の library・font） | 選択。外す時に警告 |
  | Firmware | `firmware`（`/lib/firmware` と license） | 選択。外す時に警告 |
  | Packages | 下位の項目 | 下位の既定に従う |
  | 　Clang | `lang/clang`（依存 `devel/libcxx`） | 無選択 |
  | 　OpenSSH | `network/openssh`（依存 `security/openssl`） | 選択 |
  | 　OpenSSL（＋CA 証明書） | `security/openssl`・`security/ca-certificates` | 選択（U8 の「OpenSSL」「ca-certificates」） |
  | 　GTK4 | `desktop/gtk4`（依存 glib・cairo・pango・harfbuzz など 15 個、`userland/packages/desktop/gtk4/Makefile:123-126`） | 無選択 |
  | 　Fonts | `fonts/*`（noto-color-emoji・hand-hershey） | 選択 |
  | 　libavcodec | `multimedia/libavcodec` | 無選択 |
  | 　curl | `network/curl`（依存 openssl・zlib・ca-certificates） | 無選択 |
  | 　Python | `lang/python3`（依存 zlib・openssl・ca-certificates、`userland/packages/lang/python3/Makefile:209-210`）。U8 の回答で追加 | 無選択 |
  | 　その他 | 上の項目に入らない `userland/packages/` の amd64 の全 package（glib・cairo・zlib・expat など）を 1 つずつの下位の行で（U22 の回答「9 項目＋「その他」で個別」）。依存で入る物は灰色の印 | 無選択 |

- Firmware の license（U18 の回答「一覧を出し同意で入れる」）: Firmware の行に「license を見る」を置き、配布の unit に入っている firmware ごとの名前と license の全文（`/usr/share/licenses/<pkg>/`、index の一覧）を出す。同意の check を入れないと Firmware を選んだまま次へ進めない（外せば進める）。Firmware の unit の中身は menuconfig の kernel の driver が選ぶ firmware（`config/drivers/firmware.list`。U24 の回答。今の CI の kernel の config では i915・intelax211・intelbt・nvidia-gsp・amd-navi・rtl8822b）。
- Desktop・Firmware を外す時の警告（U8 の回答「外そうとすると警告」）: `kl_dialog` で「外すと、入れた system は console の login だけになります（graphical の desktop がありません）」「外すと、Wi-Fi・GPU などの firmware が要る device が動かないことがあります」を出し、「外す」「やめる」を選ばせる。
- ca-certificates は原文で「OpenSSL (+cacertificate)」の 1 項目、U8 の回答では既定の名前に別に挙がる。1 つの項目（OpenSSL＋CA 証明書）のまま、既定で選択とした（§16 T13）。

- 依存は自動で選び、「依存で入る」と灰色の印で出す（外せない）。依存の元は §6.3 の index。例: Keiland Desktop は `fonts/noto-color-emoji`・`fonts/hand-hershey` を要る（`userland/desktop/wayland/Makefile:47`）ので、Desktop を選ぶと Fonts が依存で入る。GTK4 は `desktop/libwayland`（Desktop の unit）を要るので Desktop が依存で入る。
- 下に合計（展開後の大きさ）と導入先の root の空きを出す。
- 検査: root の大きさ ≥ 選んだ unit の展開後の合計＋swap file（作る時）＋余裕（§5.6）。足りなければ次へを無効にし、不足の量を出す。
- 遷移: 戻る → S6（選ぶ道）か S3（全体の道）。次へ → S7a。

### 4.7a S7a 利用者（U7 の回答で追加）

- 表示と入力（`kl_field`。password は `secret`）:
  - 利用者の名前（login の名前。小文字の英字で始まり、英小文字・数字・`-`・`_`、32 文字まで。`account-admin` の名前の規則に合わせる）と表示名。
  - 利用者の password と確認。
  - root の password と確認。
  - 自動 login（`kl_switch`、既定は入。Desktop を入れない時は無効）。
  - hostname（既定 `zedbsd`、U27 の回答。英小文字・数字・`-`、63 文字まで）。
- 作る利用者は管理者（wheel）と network の group に入れる（既定の利用者と同じ、`userland/base/etc/group:1-2`）。利用者の名前と同じ名前の group（gid 1000）も持つ（既定の利用者の `kei:x:1000:` の行、`userland/base/etc/group:12`。p002 で `user:x:1000:` になる）。
- 検査: 名前が規則に合い、`root` や base の system の利用者の名前でない。password が空でなく、確認と一致する。
- 導入先の設定（§7.2 の段 7）: base の unit の既定の利用者（p002 の後は `user`、uid 1000。今の tree では kei）を入力の利用者に置き換える。account の core は利用者の名前を変えられない（`userland/base/account-admin/main.c:1288-1290` の user-modify は表示名・shell・group・lock だけ）ので、名前が既定の利用者と違えば既定の利用者を消し、uid 1000 で新しい利用者を足し、`/etc/group` の wheel・network の行（`userland/base/etc/group:1-2`）と既定の利用者の名前の group（gid 1000、`:12`）の名前、`/home` の所有者を直す（同じ名前なら password と表示名だけを変える）。root の password を設定する。自動 login が入なら `/etc/keiland/autologin` に名前、切なら空にする（`userland/desktop/sessiond/main.c:201-217`）。password の hash は `account-admin` と同じ共有の account の core で作る。hostname は S7a の欄（既定 `zedbsd`、U27）の値を `/etc/rc.conf` の hostname に書く。
- 遷移: 戻る → S7、次へ → S8。

### 4.8 S8 最終確認

- 表示: 導入先の disk（名前・種類・大きさ）、partition の表（全体の道は前と後の表、消える partition を赤で。選ぶ道は S4 で書いた後の今の表）、root・swap・ESP の割り当て、format される partition、入れる項目、loader の置き場所（§5.4）。
- 「disk 全体を使う」の時は「disk の全ての data が消えます」を太字で出し、確認の check（「消してよい」）を入れないと開始できない。
- 開始の button（`KL_BUTTON_DANGER`）を押すと、確認の dialog（「disk に書きます」）の後に S9。password は聞かない（U26 の回答「インストーラはsetuidのヘルパーを使うので、パスワード認証を不要にします。これは特別なインストーライメージのみです。」。U6 の「最終確認で password」はこの回答で置き換わる）。
- 選ぶ道の S8 の表示は、helper の probe で取り直した今の表から作る。root・swap・ESP は (disk の登録番号, 開始, 大きさ, PARTUUID) で指す（§7.4）。
- 遷移: 戻る → S7。開始 → S9。

### 4.9 S9 progress

- 表示: 段の名前（§7 の手順の 1 行）、全体の進み（`kl_progress`、展開は byte で）、今の unit の名前、経過の時間、「電源を切らない・再起動しない」の注意（compositor の電源の menu を止める口は今無いので、止めるのは Future）。
- 「キャンセル」: 確認の dialog の後に helper に中止を送る。partition の表の書き込みと format の最中は、その段の終わりで止まる（途中の段を壊さない）。止まったら S10。
- 遷移: 成功 → S11。失敗・キャンセル → S10（原文「progressだけでなく、キャンセルや失敗時にここに飛ぶ」）。

### 4.10 S10 失敗

- 表示: 失敗かキャンセルか、失敗した段と理由（helper の message）、**導入先の今の状態**（S4 で書いた変更の一覧、変えた partition の表、format した partition、公開していないので導入先からは起動しない事）、「最初からやり直す」（S2 へ、probe を取り直す）と「閉じる」。
- 来る時: 全ての画面のキャンセル（確認の後）、S9 の失敗、S4 の書き込みの失敗で先へ進めない時、probe 以降の helper の失敗（原文「progressだけでなく、キャンセルや失敗時にここに飛ぶ」）。何も書いていなければ「disk は変えていません」と出す。§16 T1。

### 4.11 S11 終了

- 表示: 完了の message、入れた物、導入先から起動する方法（§5.8）: Boot#### を BootOrder の末尾に作るので（U15）、既定の起動の順は変わらない。そこで「再起動して firmware の boot menu で『zedBSD』を選ぶ（firmware の設定で順を変えれば既定にできる）」を出す。loader を `\EFI\ZEDBSD\` に置いた時は U5 の回答の文「firmware の boot menu で選ぶ」を必ず出す。Boot#### を作れなかった時は「boot menu で導入先の disk（または `\EFI\ZEDBSD\BOOTX64.EFI`）を選ぶ」、作った利用者の名前。規則「installer の image（USB）は導入のためだけの物。導入先から起動したら外し、運用に使わない」（J5、§9.1。Boot#### は末尾なので、USB を付けたままだと firmware が USB から起動しうる）。
- 「再起動」: `kl_system_power_action(…, KL_POWER_REBOOT, …)`（`userland/desktop/include/keiland/keiland.h:2293`。sessiond が root か wheel の session だけに許す、`sessiond/power-rules.c:8-16`。live の利用者 `user` は wheel、`userland/base/etc/group:1`（p002 の後））。
- 「閉じる」: installer を閉じ、live の desktop をそのまま使える（原文）。導入の続きとしての使用で、J5 の「運用」には当たらない（§9.1）。

## 5. disk と partition の規則

### 5.1 disk の列挙（helper の probe）

- `/dev` の block device を `BLKGETINFO`（`include/uapi/block.h:29-42`）で調べ、partition でない物（`KERN_BLOCK_PARTITION` の無い物）を disk とする。`diskpart --machine list` と同じ（`userland/base/diskpart/main.c:92-125`）。
- partition の表は diskpart の table の code（`table.h` の `dp_load`）で読む。GPT・MBR・空を区別する。
- 各 partition の fs の型・label・PARTUUID は `BLKGETIDENTITY`（`include/uapi/blkid.h:26-36`）、mount の有無は `KERN_SYSTEM_GET_MOUNTS`。
- 既存の ESP の空き: helper が ESP を読み取り専用で private の mount point に mount して `statvfs` で量り、すぐ unmount する（mount 中の ESP はその mount から量る）。
- disk の全ての FAT の root の `/zedbsd.cfg` の有無（別の zedBSD、§4.3）。各 FAT を同じく読み取り専用で mount して調べる。**分からない時は「有る」と扱う**（mount できない FAT（壊れている・FAT12・4Kn で p006 の前・既に読み書きで mount 中）は「別の zedBSD がある」「十分な ESP でない」とし、「partition を選ぶ」を無効にする。分からない物を「無い」とすると既存の system を乗っ取りうる）。
- GPT の制限（diskpart が編集できるか、degraded か、使っている entry の数、§4.3）。
- 新しい root の大きさごとの inode の数（`mkfs -t ufs --profile=native --check-size BYTES` の出力、§5.6）。

### 5.2 partition table

- GPT だけを書く。1 MiB 境界。protective MBR と primary・backup の GPT（diskpart の `init`・`add`・`delete` の既存の動作、`docs/reference/block-administration.md`）。
- 「disk 全体を使う」: `diskpart init` で表を置き換える（disk GUID・PARTUUID は helper が乱数で作る）。
- 「partition を選ぶ」: S4 の 1 つの操作ごとに書く（U2 の回答）。削除は `diskpart delete`、作成は `diskpart add`。ESP の自動作成は ESP と新しい partition の 2 つの entry を 1 回で書く（diskpart に複数の add を 1 回の確認・書き込みで行う batch の mode を足す、§11.3。2 回に分けると間で止まった時に ESP だけが残るため）。触らない entry は byte 単位で保つ（GPT の attribute を含む。diskpart の add・delete は他の entry を書き換えない）。型の印（root・swap）は最終確認の後に、型を変える batch で書く（U14 の回答）。
- 書いた後 `BLKREREADPART` で reload する。導入先の disk の partition が mount 中だと EBUSY になる（`src/kern/partition.c:316-377`）ので、helper は書く前に導入先の disk の partition の mount（Files などから `volumed` に頼んで mount された物。`volumed` は自分では mount しない、`userland/base/volumed/main.c:9-24`）を外し、外せなければ書かずに失敗する。
- 段と段の間（reload の後、mkfs の前など）に、利用者が Files から導入先の新しい partition を mount しうる（`volumed` は root の disk だけを除く）。helper は各々の子の program を起動する前に導入先の disk の mount を確かめ直し、有れば外すか失敗する。`volumed` に導入先の disk を一時に隠す要求を足すのは Future。
- 同時に 2 つの installer（helper）が動かないよう、helper は `/run/install-admin/lock` を排他で取る（`/run/install-admin` は mode 0700、symlink を辿らずに作る）。session は生きている間 lock を持ち、session がある間の probe（S2 の再読み込み、S10 の「最初からやり直す」）は session に頼む（別の probe の process を起動しない）。session の無い時の probe は lock を取ってから読む。
- session は 1 つの disk に結び付ける（始めた時の disk の登録番号）。S2 に戻って別の disk を選んだら、session を終えて新しく始める。

### 5.3 型と label

| 用途 | 型 GUID | label |
| --- | --- | --- |
| ESP | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | `EFI System` |
| root | `516E7CB6-6ECF-11D6-8FF8-00022D09712B`（今の image と同じ） | `zedBSD installed root` |
| swap | `516E7CB5-6ECF-11D6-8FF8-00022D09712B`（同） | `zedBSD installed swap` |
| 未割り当て（S4 で作った物。root・swap に選ばれるまで） | zedBSD 独自の GUID（U3 の回答）。値は p005 で乱数の v4 GUID を 1 つ決め、`include/uapi/` の header と diskpart の型の名前の表に置く | 無し |

- 既存の partition を root・swap にする時（U12）、型と label だけでなく PARTUUID も新しい乱数の値に替え、GPT の attribute（required・legacy BIOS bootable・Microsoft の bit 60・62・63 など）を 0 にする（古い PARTUUID は他の disk の写しと重なりうる、`src/kern/block-identity.c:184-190`）。diskpart の batch に PARTUUID と attribute の書き換えを含める（§11.3）。
- label は今の image の `zedBSD-root`・`zedBSD-swap` と違う名前にする。今の image の cfg は `PARTLABEL=zedBSD-root` で選ぶので、同じ label の導入先の disk が付いていると、今の image の方が曖昧で起動しなくなるため。
- 起動の設定の root・swap は PARTLABEL でなく PARTUUID で選ぶ（§7.3）。今の image は `PARTLABEL=zedBSD-root` で、同じ label の disk が 2 台あると曖昧で起動が失敗する（`src/kern/block-identity.c:184-190`、[WS062](../ws062/ws.md) の「制限・移管」）。live の USB を挿したまま導入先から起動しても曖昧にならないようにする。

### 5.4 ESP

- 新しく作る ESP の大きさ: 512 MiB（U4 の回答。FAT32 の最小は約 33 MiB、`userland/base/mkfs/fat32-format.c:66`）。
- ESP に置く物: loader（`BOOTX64.EFI`、今 29 KiB）と `zedbsd.cfg`（U19 の回答「ESP（FAT）のまま」）。kernel は root の UFS の `/boot/vmunix`、logo は `/boot/logo.ppm`（U20 の回答）に置き、loader が UFS から読む（§5.10、2026-10-28 ユーザーの追加の指示「ブートローダがUFSからカーネルをロードできるようにしたいです」）。
  - U4・U4b の「ESP に全部」は、この追加の指示で kernel（と logo）を除いた形に読み替える（Q1 の中継の解釈）。ESP の大きさ 512 MiB は U4 のまま。別の ZEDBSD の FAT は作らない（WS238 の今の image の形とは違う。WS238 は ESP に loader だけ、kernel・設定・logo は Windows・macOS で mount できる ZEDBSD の FAT32、`plan/ws238/ws.md`）。
  - loader は同じ disk の全ての FAT から root の `/zedbsd.cfg` を探す（`bootloader/uefi/bootx64.c:1815` 付近）ので ESP の root に置けば足りる。
- 「十分な ESP」（S4 の検査）: 型が ESP、loader が受け付ける FAT（FAT16 か FAT32、serial が 0 でない、block の大きさが loader の扱える物、`bootloader/uefi/bootx64.c:1555`。今の loader の FAT の探索は 512 byte の block だけを受け付ける、`bootloader/uefi/volume-discovery.c:854-862`。4096 は p008 の後、§5.9）、かつ
  - 計画で新しく作る ESP: 上の大きさ。
  - 既存の ESP: 空き ≥ 置く物の合計＋16 MiB（`/opt/installer` の index に置く物の大きさを書く。旧 zedinst の `max(64 MiB, loader＋kernel＋FAT の overhead＋16 MiB)` に倣う。kernel が UFS に移るので ESP に要る量は loader と cfg だけになる）。既に `zedbsd.cfg` がある disk は §4.3 で「partition を選ぶ」を無効にする（§13）。 ESP の root と `\EFI\ZEDBSD\` に、書く名前（`zedbsd.cfg`・loader）の file が既にあれば「十分な ESP」でない（他の OS の file を上書きしない）。
- 1 つの disk に ESP が複数ある時は、slot の番号の小さい方から最初の「十分な ESP」を使い、S8 にどの ESP かを出す。十分な ESP が 1 つも無ければ U16 の規則（「partition を選ぶ」を無効）。
- loader の path（U5 の回答）: `\EFI\BOOT\BOOTX64.EFI` が無ければそこに置く。有れば上書きせず `\EFI\ZEDBSD\BOOTX64.EFI` に置き、S11 で firmware の boot menu から選ぶよう案内する。UEFI の `Boot####` も作る（§5.8）。
- 新しい ESP は `mkfs -t fat32` で作る。今の mkfs は FAT の serial に disk の登録番号を使い（`userland/base/mkfs/block-command.c:157`、`fat32-format.c:156-158`）、他の disk と重なりうる。そこで mkfs に乱数の serial を使う形を足す（§11.3）。`zedbsd.cfg` に `boot0=` は書かず、loader が自動で足す `boot0=UUID=<serial>`（`bootloader/uefi/zedbsd-config.c:157-173`）に任せる。kernel は `boot0=UUID=` の時だけ、その FAT の disk を待って固定の disk を先に調べる（`src/kern/platform/pcat.c:147`・`:490-541`・`:830-900`）。`boot0=PARTUUID=` にするとこの待ちが無くなり、毎回 5 秒待つか、USB の disk が 2 台ある時に root が見つからないことがある（§16 T4）。

### 5.5 swap

- swap の partition を選んだ時: `mkswap` の partition の mode（§11.3）で ZEDSWAP2 の header を書き、`zedbsd.cfg` に `swap0=PARTUUID=<swap>`。kernel は header の無い partition を受け付けず、起動が止まる（`docs/reference/kernel-boot-parameters.md:219-252`、`src/kern/vm/swap-source.c:351-440`）ので、helper は書いた後に header を読み直して確かめる。partition の mode は ZEDSWAP2 の 64 bit の大きさを扱う（file の mode の INT32_MAX の上限、`userland/base/mkswap/swap-format.c:39-41` を partition には当てない）。
- 選ばない時: root の UFS に `/swapfile` を作る。大きさ = min(RAM, 2 GiB − 4 KiB)（`mkswap` の file の上限は INT32_MAX、`userland/base/mkswap/swap-format.c:31-44`）。穴の無い file として 0 を書いてから `mkswap`、`/etc/fstab` に `/swapfile none swap sw 0 0`（init が `swapon -a` で有効にする、`userland/base/init/main.c:310-311`）。旧 zedinst と同じ手順（`git show 2cf4b30e6^:userland/retro/zedinst/nativeinstall.noct`）。extent は 1024 まで（`swap-source.c:44`）。extent の数を userland から知る口は無いので、helper が導入先の file で `swapon` を試して `swapoff` する（旧 zedinst と同じ）。受け付けられなければ swap file を消して fstab に書かず、S11 で「swap 無し」と出す（失敗にしない。起動の時の `swapon -a` の失敗は起動を止めない、`init/main.c:310`）。 試しの `swapon` は live の kernel が導入先の file に page を出しうるので、time limit（30 秒）を付け、`swapoff` が失敗・時間切れなら段 8 の unmount の前に止まり、S10 に「導入先の swap file を live の system が使っている。再起動して入れ直す」と出す。
- 原文の「loopback swapfile」は、今の kernel が UFS の上の file を extent で直に swap に使う（loop device を通さない、`swap-source.c:145-200`）ので、その方式で満たす（§16 T2）。

### 5.6 大きさの検査

- root の最小 = 選んだ unit の展開後の合計 × 1.1 ＋ swap file（作る時）＋ 1 GiB。S5 では Base だけの値、S7 で選んだ物の値で確かめる。
- inode: native の UFS は cylinder group（最大 63,936 KiB）ごとに 256 の inode で、1 MiB に約 4 つ（`userland/base/mkfs/ufs-format.c:25-28`・`:39`・`:523-536`。1 GiB で約 4,200）。index に unit ごとの file の数を書き、選んだ unit の file の数の合計 × 1.2 が、その大きさの root の inode の数（`--check-size` の出力）以下であることも確かめる。足りなければ必要な大きさを出す。小さな root でも足りるよう、mkfs に inode の密な形を足すかは p005 で量って決める（§11.3）。
- `mkfs -t ufs --profile=native --check-size BYTES` で、その大きさが UFS にできるかを helper の probe で確かめる（`docs/reference/image-formatters.md`）。
- 「disk 全体を使う」の disk の最小 = ESP ＋ swap（作る時）＋ root の最小。

### 5.7 「disk 全体を使う」の既定の配置（U4 の回答）

| # | partition | 大きさ |
| --- | --- | --- |
| 1 | ESP（FAT32） | 512 MiB |
| 2 | swap | RAM と同じ、1 GiB 以上 8 GiB 以下（RAM は sysctl の `HW_MEMORY_STATS`、`include/uapi/sysctl.h:43`）。disk が 32 GiB 未満なら 1 GiB |
| 3 | root（UFS） | 残り全部 |

### 5.8 UEFI の boot entry（U5 の回答「Boot####の作成もWSに入れる」）

- 形（2026-10-30 の p007 の改訂 5 で確定。経緯は [proposed/uefi-runtime-window.md](proposed/uefi-runtime-window.md) と [phase007/phase.md](phase007/phase.md)）: firmware の変数は kernel の driver が UEFI の runtime services で読み書きする。driver は HAL を知らず kernel の口（`kern_*`）だけを使う。`include/hal/hal.h` は変えない。
- 作る物:
  1. loader → HAL（G1・G3）: UEFI の loader は handoff を v8 にする（`bootloader/include/amd64-handoff.h` の `struct zbl6_handoff_v8_uefi`、v7 の後ろに 32 byte の runtime の部分）。ExitBootServices の直後、firmware の identity の map の上で、`EFI_MEMORY_RUNTIME` の記述子を low block（offset 0xb000、8 KiB まで）に写し、kernel の runtime の窓（`ZBL6_UEFI_RUNTIME_VIRTUAL_BASE` = `0xfffffffe80000000` から 64 MiB、PDPT の slot 506）の中に map の順で詰めた VirtualStart を付けて `SetVirtualAddressMap` を 1 回呼ぶ。成功した時だけ system table の物理 address・記述子の数・大きさ・版を handoff に入れ、flag `ZBL6_HANDOFF_FLAG_UEFI_RUNTIME` を立てる。窓・写しに入らない時と firmware が拒んだ時は渡さず、起動は続ける。
  2. HAL → kernel（G1、HAL の実装の補完）: amd64 の HAL は v8 の runtime の部分を検査して記述子を自分の領域に写し、名前 `uefi.system-table`（system table の物理 address）・`uefi.runtime-map`（記述子の列）・`uefi.runtime-map-count`・`uefi.runtime-map-entry-size`・`uefi.runtime-map-version` で返す（値は 64 bit の word への pointer）。BIOS の起動・v7 以前の loader・検査に通らない時は NULL。
  3. 窓の map（G2）: kernel の `kern_firmware_map(物理, 仮想, 大きさ, KERN_PROT_*, KERN_FIRMWARE_CACHED|UNCACHED)`（`include/kern/pmem.h`）。その下で amd64 の `hal_space_map(HAL_SPACE_SYS, …)` が runtime の窓の中だけで、HAL の RAM の map の外の頁を受け、`HAL_SPACE_EXEC` と `HAL_SPACE_NOCACHE` を扱う（hal.h は `HAL_SPACE_SYS` を禁じておらず、今まで amd64 の実装が拒んでいた。2026-10-30 のユーザーの決定で実装の補完として扱う）。他の port は今どおり拒む。map は kernel の生涯残る。
  4. driver（`src/drivers/platform/pcat/uefi-variables.c`）: 名前で記述子を受け、RuntimeServicesCode は実行可、他は NX、write-back の無い範囲は uncached で map し、system table と runtime services の表の signature と header の大きさ、3 つの関数の pointer が code の範囲にあることを確かめる（CRC32 は firmware により付け替えの後の値が揃わない事があるので見ない）。呼び出しは mutex で 1 つずつ、割り込みを止め、FP の状態を fxsave64 で退避して MXCSR 0x1F80・x87 0x037F にし、`ms_abi` の関数 pointer で GetVariable・SetVariable・GetNextVariableName を呼ぶ。名前は UTF-8 ↔ UCS-2（BMP だけ）、値は byte の列。
  5. UAPI: `/dev/system` の `KERN_SYSTEM_FIRMWARE_VARIABLE_GET`・`SET`・`NEXT`（ioctl 22〜24、`struct system_firmware_variable` 576 byte、版 1、space は vendor GUID の小文字の文字列 40 byte、name は UTF-8 512 byte、値は user の pointer と大きさ、上限 64 KiB）。root だけ（EPERM）。provider の無い platform と runtime services の無い起動は EOPNOTSUPP。GET の buffer が小さい時は ERANGE と要る大きさ。
  6. base の道具 `efiboot`（`userland/base/efiboot`）: `list`（BootCurrent・BootNext・BootOrder・各 Boot#### の label と device path）、`create [-l label] [-n] disk partition loader`（GPT の partition から `HD(番号,GPT,PARTUUID,開始,大きさ)/File(loader)` を作り、同じ device path の entry があれば使い、無ければ空いた最小の番号に書き、BootOrder の末尾に足す。`-n` は BootNext）、`delete`、`order`、`next`。今は amd64 の config（`config/ci/config-amd64.mk`）にだけ入れる。
- installer の使い方: §7.2 の段 11 で、導入先の ESP の PARTUUID・partition の番号・開始・大きさと loader の path から device path（`HD(番号, GPT, PARTUUID, 開始, 大きさ)/File(\EFI\…\BOOTX64.EFI)`）を作り、名前「zedBSD」の `Boot####` を書き、BootOrder の末尾に足す（U15 の回答）。同じ device path の既存の entry があれば作り直さずに使い、BootOrder に無ければ末尾に足す。名前が「zedBSD」で、指す partition（PARTUUID）がどの disk にも無い古い entry は消す（入れ直しで死んだ entry が増えないように）。書けなければ失敗にせず、S11 で「boot menu で選ぶ」を出す（loader は置いてあるので起動はできる）。

### 5.9 4096 byte の logical sector（U17 の回答「WS307 で 4096 byte に対応」）

- 導入先に logical sector が 4096 byte の disk（4Kn）も選べる。今は `mkfs` の device の mode が 512 byte だけ（`userland/base/mkfs/block-command.c:144-148`、4Kn は EOPNOTSUPP）。
- 対応する物（p005）:
  - `mkfs -t fat32`: sector 4096 byte の FAT32（cluster の数の下限 65525 から、最小は約 256 MiB。ESP 512 MiB は満たす）。
  - `mkfs -t ufs --profile=native`: fragment・block の大きさと sector の換算。
  - `mkswap` の partition の mode: header の page（4096 byte）と sector の整合。
  - `diskpart`: GPT の初期化は既に 512・4096 を扱う（`dp_initialize_gpt`、`userland/base/diskpart/table.c:235-310`）。add・delete・batch・1 MiB 境界の計算を 4096 で確かめる。
  - kernel: partition の reload は 4096 を扱う（`src/kern/partition.c:329-334`）。しかし FAT（`src/drivers/fs/fat.c:911`・`:963`・`:11842`）、UFS（`src/drivers/fs/ufs.c:60` の `UFS_SECTOR_SIZE 512`・`:2825`・`:5452`）、swap の partition（`src/kern/vm/swap-source.c:246`）、swap file の extent（`src/kern/file.c:2920`）は今 512 byte の sector だけを受け付ける。これらを直すのは確実な kernel の作業で、別の Phase（ws.md の p006）にする。HAL の API には触れない見込み。
  - native UFS の fragment は 1024 byte（`userland/base/mkfs/ufs-format.c:26`）で 4096 byte の sector より小さい。4096 の disk では fragment 4096 の profile を使う（driver の read-modify-write より単純で、loader の UFS の reader（§5.10）も sector と fragment の換算だけで済む。inode の密度は §5.6 の検査で確かめる）。技術の判断（§16 T19）。
  - **loader の FAT**（3 回目のレビューの blocker）: loader の FAT の探索は media の block が 512 byte の物だけを受け付け（`bootloader/uefi/volume-discovery.c:854-862` の `supported_fat_block_size`、`:409-411` の `ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE`）、BPB の sector は 512・1024 だけ（`:446-452`）。このままでは 4Kn の ESP の `zedbsd.cfg` が見つからず起動しない。p008 で、kernel の FAT（p006）と同じ契約で block・BPB の sector 4096 を受け付ける（`supported_fat_block_size` は「kernel の FAT の driver が受け付ける物」と揃える規則なので、p006 の後に変える）。読む buffer は今 4096（`bootloader/uefi/bootx64.c:41`・`:1425-1437`）。
  - **firmware の FAT**: 導入先の loader そのものは firmware の FAT の driver が 4Kn の ESP から読む。OVMF が 4096 byte の sector の FAT32 を読めるかは未確認（3 回目のレビュー）。p008 の 4096 の段の最初に OVMF で確かめ、読めなければ Q1 に報告する（firmware の制限なので、4Kn の対応の範囲をユーザーに聞く）。
  - **選べる時期**: probe は 4Kn の disk を、p005（道具）・p006（kernel）・p008（loader の FAT と UFS の reader の 4096）の全てが入るまで選べない disk として出す（灰色と理由）。
  - p008 の 4096 の試験は p005・p006 の後（mkfs の 4096 の FAT32・UFS と kernel の 4096 が要る）。
- 試験: QEMU の `-device nvme,logical_block_size=4096,physical_block_size=4096` と `usb-storage` の同じ設定の導入先に、全体・選ぶの両方で入れて起動する。

### 5.10 loader が UFS から kernel を読む（2026-10-28 ユーザーの追加の指示）

- 指示（原文、Q1 の中継）:「P4に、ESPにカーネルをインストールすると回答してしまいましたが、ブートローダがUFSからカーネルをロードできるようにしたいです。そのphaseを作ってほしいです。」
- 今: UEFI の loader は firmware の FAT の driver（`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`）で cfg・kernel・logo を開く（`bootloader/uefi/bootx64.c:1563-1599` の cfg の探索、`:2865-2960` の `load_kernel` は cfg の FAT の `root->Open`）。UFS は読めない。future-work の F-014（UFS からの直接の load、2026-09-25 ユーザー）の UEFI の部分に当たる。
- 作る物（実装の Phase、ws.md の p008）:
  1. **file の源の抽象**: loader の中で「開く・大きさ・offset からの読み」の 3 つの操作の口を作り、今の FAT（`EFI_FILE_PROTOCOL`）と新しい UFS の両方をその口で読む。`load_kernel`・`show_logo` はこの口を使う。
  2. **UFS の reader**（読み取り専用、freestanding、`bootloader/common/` に置き BIOS の loader でも後に使える形）: superblock（offset 65536、magic `0x19540119`、`userland/base/mkfs/ufs-disk.h`）、cylinder group の inode の位置、256 byte の dinode、直接の 12 block と 1・2・3 段の間接 block、fragment、directory（`UFS_DIRBLKSIZ` 512）の名前の照合、path の成分ごとの lookup。symlink は辿らない（kernel の path に symlink があれば失敗）。拡張属性・journal は読まない。block の大きさ・sector の大きさ（512・4096、§5.9）の換算。範囲外の block 番号・循環・大きすぎる値は失敗にする（壊れた disk で loader を暴走させない）。3 回目のレビュー #10 の規則:
     - 位置の換算: UFS の `fsbtodb` は 512 byte の単位（`userland/base/mkfs/ufs-disk.h`）。reader は fragment の番号から byte の offset を出し、それを media の `BlockSize` で割る（512 の単位のまま `BlockIo` の LBA に使わない）。fragment の大きさ（`fs_fsize`）が `BlockSize` より小さい volume は拒む（4096 の disk の fragment 1024 は読まない、§5.9 の fragment 4096 の profile）。
     - 範囲: superblock の `fs_size`（fragment の数）×`fs_fsize` が partition の大きさ（`Media->LastBlock`＋1 の block）以下であること。block の番号が `fs_size` 以上なら失敗。
     - 穴: block の番号 0（直接・間接のどれも）は穴で、0 の byte として読む（kernel の file の大きさの中の穴、sparse の kernel は無いが壊さない）。
     - byte order: superblock の magic が逆の byte order の volume は拒む（kernel は両方を読む、`src/drivers/fs/ufs.c:2524-2532`。loader は native（little endian）だけ）。
  3. **root の partition の見つけ方**: cfg の `rootpart=` を loader も読む（今は kernel に渡すだけ）。`PARTUUID=` だけを受け付け（`PARTLABEL=` などは曖昧さを避けるため loader では拒む）。
     - 列挙: 今の cfg の探索は `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` の handle だけを列挙する（`bootloader/uefi/bootx64.c:1735`）。firmware は UFS に SimpleFS を付けないので、`LocateHandleBuffer(ByProtocol, EFI_BLOCK_IO_PROTOCOL)` で全ての block の handle を列挙し、device path の末尾が `HD()` の node（partition）の物を候補にする（3 回目のレビュー #9）。firmware が型の分からない GPT の partition に BlockIo の handle を作るかは未確認で、p008 の最初に OVMF で確かめる。
     - 照合: cfg の PARTUUID の文字列（大文字・小文字を区別しない）を GPT の binary の GUID（先頭の 3 つの field は little endian の混ざった byte order）に直し、`HD()` の node の signature（16 byte）と byte で比べる。
     - 選び方: loader が起動した disk（自分の image の device path の disk の部分）と同じ物理 disk の候補で一致する物がちょうど 1 つなら、それを `EFI_BLOCK_IO_PROTOCOL`（と要れば `EFI_DISK_IO_PROTOCOL`）で読む。同じ disk で 0 個・2 個以上は失敗。
     - 他の disk の重なり: kernel は全ての disk から PARTUUID を探し、重なれば EEXIST で root を mount しない（`src/kern/block-identity.c:182-188`。導入先を dd で写した disk を付けた時など）。loader は他の disk の候補にも同じ PARTUUID があれば、kernel の失敗を待たずに `A64 KERN UFS duplicate PARTUUID` を出して止まる（§16 T21）。
  4. **cfg の書き方**: `kernel=rootpart:/boot/vmunix`（`rootpart:` の接頭辞は「`rootpart=` の partition の UFS の絶対 path」）。接頭辞の無い `kernel=vmunix` は今どおり cfg の FAT の相対 path（今の image・WS238 の形はそのまま動く）。`logo=` も同じ規則（U20）。今の disk image（hdd-image・installer の image）は WS238 の形のまま（U21 の回答「image は今のまま」。loader は両方の書き方を読む）。
     - 今の cfg の parser は kernel の path の `:` を拒む（`bootloader/uefi/zedbsd-config.c:620-624`、「the kernel is on the boot volume」）。p008 は `rootpart:` の接頭辞だけを先に切り出して別の種類の path として持ち、残りの path の検査（`..`・`:`・長さ）は今の規則のまま当てる。
     - `logo=` の path の検査は BIOS の loader と共有の `bootloader/common/logo-path.c` の `logo_path_character` が `:` を拒む。`rootpart:` の接頭辞は UEFI の loader の側で切り出し、共有の文字の規則は変えない。BIOS の loader は `rootpart:` を今どおり不正な path として拒み、画面に理由を出して止まる（UFS を読まない。p008 で BIOS の loader の host の試験か QEMU の BIOS で、拒み方が今の不正な path と同じことを確かめる）。
  5. **失敗**: 今の方針（隠れた fallback 無し、`docs/howto/boot-and-storage.md` の「a missing file does not select a hidden direct-kernel fallback」）どおり、`A64 KERN UFS <理由>` を画面に出して止まる。FAT の kernel に戻らない。
  6. **一貫性**: loader は journal を再生しない。UFS の上の kernel は、その内容が home の位置に書かれた状態でだけ正しく読める。installer は段 8 で unmount する（unmount で checkpoint し clean の印を付ける）。
     - clean の印で警告しない（3 回目のレビュー #8）: 今の kernel は clean の印を unmount の時だけ立て、mount で消し（`src/drivers/fs/ufs.c:18044`・`:18608`）、shutdown は unmount せずに sync する（`src/kern/shutdown.c:117-128`、`src/kern/mount.c:946`）。毎回の起動で印が無いので、印での警告は常に出てしまう。警告は出さない。
     - kernel の更新の規則（Future の更新の道具、と導入後に利用者が手で kernel を替える時の文書）: 新しい名前に書く → `fsync` → `rename` → `sync`。j3 の journal は封じた後に cache から home の位置を書く（`ufs.c:20719-20760`・`:20830`）ので、`sync` が home の位置まで書き終えることを p008 で確かめる（未確認。書き終えないなら、規則に unmount 相当の checkpoint を足す案を Q1 に出す）。
     - 最後の j3 の commit を loader が重ねて読む（journal の再生の読み取り専用の版）は Future（§13）。
- HAL・kernel: kernel は今どおり `rootpart=` で root を mount する。loader → HAL の handoff は変えない（kernel の位置と大きさは今の handoff のまま）。HAL の API に触れない。
- 試験: host で、`mkfs`（`mkfs -t ufs --profile=native` を file に）・`zedimage-host` で作った UFS に大きな kernel（間接 block を使う 20 MiB 以上）・fragment・深い directory を置き、reader で読んで byte を照らす。壊した superblock・inode・間接 block で失敗すること。QEMU の OVMF で、ESP（loader・cfg）＋ root の UFS（`/boot/vmunix`）の disk を 512・4096 の sector で起動して login prompt（boot-test.sh）。

## 6. 入れる物（payload）

### 6.1 置き場所と形

- live の system の `/opt/installer/`（原文）:

  ```text
  /opt/installer/index              unit の一覧（§6.3）
  /opt/installer/boot/BOOTX64.EFI   loader（導入先の ESP へ）
  /opt/installer/boot/vmunix        kernel（導入先の root の /boot/vmunix へ）
  /opt/installer/boot/logo.ppm      起動の logo（導入先の root の /boot/logo.ppm へ、U20）
  /opt/installer/units/base.tar.gz
  /opt/installer/units/desktop.tar.gz
  /opt/installer/units/firmware.tar.gz
  /opt/installer/units/<category>-<name>.tar.gz   package ごと（例 lang-clang.tar.gz）
  ```

- tarball は ustar（`pax` が読み書きする形、`userland/base/pax/main.c:239`）を deflate（gzip の形）で圧縮した物。guest の展開は helper が `libz-compat`（base の library、inflate がある、`userland/base/libz-compat/Makefile:3-8`）で行う。zstd・xz は guest に無いので使わない（§16 T3）。
- 所有者と mode: 今の image の build に所有者・mode の manifest は無い（`tools/build/zedimage-host.c` は inode の uid・gid を 0 のままにし（`:59-60`）、mode は staged の tree の `st_mode & 07777`（`:72`）で、build する人の umask で group-writable になる）。tarball では:
  - 所有者は全て 0:0。helper は tar の header の所有者を使わず 0:0 で作る（既定の利用者の home など 0 以外の所有者は §7.2 の段 7 で helper が設定する）。
  - mode は正規化する: 明示の `--mode`（package の登録の第 11 引数と `ZEDBSD_ROOTFS_TREE_RULE` の `--mode`）が無い file は group・other の書き込みを落とす。
  - set-user-ID・set-group-ID の file は build の許可の一覧（`account-admin`・`su`・`sudo`・`passwd`・`newgrp`・`ping` など、各 Makefile の mode から作る）と照らし、一覧に無い物があれば build を失敗させる。helper も展開の時に同じ一覧（index にある）と照らす。
- 展開の安全: 絶対 path・`..` の成分・hard link・device の node・FIFO は拒む。symlink は作るだけで辿らない。file は path の成分ごとに `openat(…, O_NOFOLLOW)` で開き、先に作った symlink の下に書かせない。ustar の名前・link 先の 100 byte の上限は pax の `x` header で越えるか、build の検査で拒む。
- 改ざんの検出: `/opt/installer` は live の root（読み書きのできる UFS）にある。そこで:
  - index の SHA-256 を install-admin の binary に build の時に埋め込む（`-D` の定数）。index は install-admin を含まない全部入りの tree から作るので循環しない（build の順は §6.2）。helper は index を 1 回だけ memory に読み、その写しで hash を照らしてから解釈する。
  - tarball は、展開に使うのと同じ fd から読みながら hash を取り、終わりで index の値と照らす（照らしてから別に読み直さない。読み直すと間に書き換えられうる）。合わなければ失敗にする。展開した物はまだ公開していない（段 9 の前）ので起動できる状態にならない。
  - boot の file（loader・kernel・logo）は、hash を取りながら一時の名前に写し、合えば rename する。
  - `/opt/installer` の file と親の directory は、`fstat` で所有者 0・group と other の書き込み無しを確かめる。
  - live の image の tree の mode は p003 で manifest から正規化する（今の image の group-writable の不具合は Q1 が Bug として扱う）。
- boot の file（`BOOTX64.EFI`・`vmunix`・`logo.ppm`）も index に名前・大きさ・SHA-256 を持ち、Base を選ぶと必ず入る（S7 の Base の中身）。

### 6.2 unit の分け方（build）

- 今の root の tree の rule（`Makefile:1415-1459` の `ZEDBSD_ROOTFS_TREE_RULE`）は、全ての package の file を 1 つの `ZEDBSD_PACKAGE_FILES` の一覧・`--subtree`・`ZEDBSD_PACKAGE_LINKS` で写し、その後に sysroot の `/usr/include`・`/usr/lib` を丸ごと写す（`Makefile:1520-1528`）。どの file がどの package の物かが残らない。sysroot の header は `include/libc`・`include/uapi`・`userland/desktop/include` から来る（`toolchain/llvm/sysroot.mk:137-147`）ので、source の directory で持ち主を機械的に付けられる（X11 の header は明示で除く）。curl・OpenSSL の物は sysroot に入らない（`userland/packages/network/curl/Makefile:36-39`。1 回目のレビューの観察は main の古い build の tree の物だった）。clang は sysroot の header・`libc.so`・`crt1.o` を自分でも入れる（`userland/packages/lang/clang/Makefile:272-289`）。
- **toolchain の許可**: sysroot の持ち主の付け方（`toolchain/llvm/sysroot.mk`）、clang・libcxx の package（`userland/packages/lang/clang`・`devel/libcxx`）に触れる所は AGENTS.md §7 の toolchain で、Q1 の許可が要る（p003・p004 の Queue の時に）。
- そこで build を変える（p003）:
  1. **file の manifest**: 登録の macro が package ごとの変数（写す file・subtree・link）を持ち、root の tree を作る時に「導入先の path → 持ち主の package → mode」の manifest（`$(BUILD)/rootfs.manifest`）を書く。sysroot の開発の file は、それを sysroot に入れた package（libc・base の library は base、curl の header は `network/curl` など）に付ける。
  2. **重なりの規則**: 同じ path を 2 つの package が入れる時、中身が同じなら依存される側（例 clang と base の libc の header は base）に付け、違えば build を失敗させる。
  3. **unit**: manifest の持ち主の package の menu（第 8 引数）で振り分ける: base → `base`、desktop → `desktop`、firmware → `firmware`、`userland/packages/<category>/<name>` → `<category>-<name>`。platform の `--file`（`rc.conf`・`os-release` など）は `base`。menu が tests・x11-applications・x11-servers の package と `ZEDBSD_EXTRA_FILES`・試験の上書きは、`installer-image` の build で選ばない（U24 の回答「tests・X11 無し」）。
  4. **検査**: 全ての file がちょうど 1 つの unit に属し、unit の和が全部入りの tree（installer と install-admin を含まない、下の順の 1）と一致する。live の tree には当てない（live の tree は installer・install-admin・`/opt/installer` と live だけの上書きを持つ）。
- package の header（U13 の回答「WS307 で header も入れる」）: 今 header を入れない package（gtk4・cairo・pango・glib・harfbuzz・fribidi・fontconfig・freetype・gdk-pixbuf・graphene・libepoxy・libxkbcommon・pixman・libpng・libtiff・libjpeg-turbo・libavcodec・libcxx の `c++/v1` など。`userland/packages/libs/fontconfig/Makefile:115-117`、`desktop/gtk4/Makefile` の「development packages later」の注記、`multimedia/libavcodec/Makefile:117-118`）にも、stage の `usr/include`・`usr/lib/pkgconfig/*.pc`・開発の symlink（`lib*.so`）をその package の file として入れる。`.pc` の `prefix` は導入先の `/usr`。受け入れ: 導入先で GTK4 の小さな app を clang と `pkg-config gtk4` で build して動く。guest に pkg-config は無い（`userland/packages/` に無い）ので、外部の package として pkgconf（ISC の license）を `devel/pkgconf` に足し、Clang の項目の依存にする（U13 の「GTK4 の app を build できる」に要る手段、§16 T18）。
- 1 つの BUILD で作る（§11.2 の `make installer-image`）: menuconfig の userland・package の選択に依らず、`installer-image` の決まった選択（base の全部、desktop の menu の全部、kernel の driver が選ぶ firmware、`userland/packages/` の amd64 の全 package。U22・U24 の回答）で root の tree（全部入り）を作り、その manifest から unit を作る。live の image の root は、manifest から `base`・`desktop`・`firmware` の unit の file と installer・install-admin・`/opt/installer` だけを取った別の tree から作る（package は live の system に入れない。tree を 2 回 build しない）。
- build の順（3 回目のレビュー #2。index の hash を install-admin に埋め込むと「tree → manifest → unit → index → install-admin → tree」の循環になるため、全部入りの tree から installer・install-admin を除く）:
  1. 全部入りの tree `$(BUILD)/installer/full`（installer の menu の package（installer・install-admin）を含まない）。
  2. manifest（`$(BUILD)/installer/full.manifest`）。
  3. unit（`$(BUILD)/installer/units/*.tar.gz`）と boot の file。
  4. index（§6.3）と、その SHA-256。
  5. install-admin を index の SHA-256 の `-D` で build、installer（app）を build。
  6. live の tree `$(BUILD)/installer/live`: manifest の `base`・`desktop`・`firmware` の file を mode を正規化して写し、installer・install-admin・`/opt/installer`（index・unit・boot の file）と live だけの上書き（§11.2 の `/etc/shadow`・自動 login・session の設定）を足す。
  - 検査 4 は 1 の tree にだけ当てる。live の tree には「1 の部分集合＋足した物の一覧」と一致することを当てる。
- installer 自身（app と helper）と `/opt/installer` は、どの unit にも入れない（導入先に installer を残さない）。
- base の全部を入れると fidoctl・passkey-fido2（base、menuconfig の既定は n）も入り、Base が `security/openssl` を要る（`userland/base/fidoctl/Makefile:12`、`userland/base/passkey-fido2/Makefile:15`）。その時 OpenSSL は依存で必ず入る（S7 で灰色）。

### 6.3 index と依存

- `/opt/installer/index` は build が作る text（1 行 1 unit、TAB 区切り）: unit 名、表示名、group（base・desktop・firmware・package）、版、tarball の大きさ、展開後の大きさ、file の数、SHA-256、依存の unit（空白区切り）。ほかに boot の file の行、set-user-ID の許可の一覧、firmware の license の一覧（U18、§4.7）。S7 の項目と下位の項目の対応（項目 → unit の一覧）も index に書く（項目を増やす時に installer を変えない）。
- 依存の元は `ZEDBSD_USERLAND_PACKAGE` の第 10 引数（REQUIRE、`Makefile:521-532` が展開する物）。group を跨ぐ依存（desktop の compositor → `fonts/*`、`gtk4` → `desktop/libwayland`）は unit の依存に写す（package の依存が desktop の unit の中の package を指せば `desktop` に、base の中を指せば `base` に）。
- installer は選んだ項目の unit の閉包を index から求める（循環は build の検査で拒む）。

### 6.4 導入の記録

- 入れた unit の一覧と版を `/var/db/installer/units`、unit ごとの file の一覧を `/var/db/installer/<unit>.files` に書く。今の guest に package の database は無い（WS153 の設計は `.kapp` で別物、`plan/ws153/ws.md:20-22`）。後の package の管理（削除・更新）はこの記録を読む前提で Future に置く。

## 7. disk への書き込み（helper）

### 7.1 S4 の編集（その場で書く、U2）

1. **再確認**: disk の登録番号・大きさ・partition の表が、画面が表示している表と同じか（`BLKGETINFO` と表の読み直し。要求に画面の表の digest を付ける）。違えば書かずに失敗（「disk が変わった」）し、画面の表を描き直す。
2. **mount を外す**: 導入先の disk の partition の mount（§5.2）を外す。消す partition が外せなければ書かずに失敗。
3. **書く**: 削除 → `diskpart delete`、作成 → `diskpart add`、ESP の自動作成 → batch で 2 つの add、続けて新しい partition の先頭と末尾の 1 MiB を 0 で埋め、ESP を `mkfs -t fat32`（失敗の扱いは §4.4）。確認の phrase（`WRITE name:registration`、`FORMAT name:registration`）は、helper が直前に取り直した `BLKGETINFO` から作って送る（子の program の表示を読んで写さない）。新しい partition は計画の時に登録番号が無いので、reload の後に親の disk の登録番号・開始の LBA・PARTUUID で見つける。
4. **reload の結果**を返す（exit 3 は「書いたが reload の失敗」、§4.4）。
5. 表を読み直して返す。

### 7.2 インストール（最終確認の後）

順序は「起動できない途中の状態を、起動できるように見せない」ことを守る。loader と `zedbsd.cfg` は最後に置く（旧 zedinst の「設定を最後に公開する」と同じ）。

1. **再確認**: 計画の disk の登録番号・大きさ・partition の表が、最終確認の画面が表示した表と同じか（`BLKGETINFO` と表の読み直し）。違えば何も書かずに失敗（「disk が変わった」）。
2. **disk の確保**: `/run/install-admin/lock` を取り、導入先の disk の mount を外す（§5.2）。`BLKRESERVE` の前提（mount・open が無い）を満たさなければ失敗。helper は diskpart・mkfs が自分で reserve するので reserve を持ち続けられない。代わりに各々の子の前に mount を確かめ直す。
3. **partition の表**: §5.2。全体: `diskpart init`。選ぶ: 表は S4 で書き済み。root・swap の型と label を batch の mode で変える（U14 の回答）。reload まで。
4. **format**: 新しい ESP を `mkfs -t fat32`（乱数の serial、§11.3）、root を `mkfs -t ufs --profile=native`、swap の partition に `mkswap`（§11.3）。各々の確認の phrase（`FORMAT name:registration` など、`docs/reference/image-formatters.md`）は helper が最終確認の後に、直前の `BLKGETINFO` から作って送る（diskpart・mkfs の文書が installer に許す使い方）。全体の道でも新しい partition の先頭と末尾の 1 MiB を format の前に 0 で埋める。
5. **root の mount**: root を `/run/install-admin/target`（`/run/install-admin` は mode 0700）に nosuid・nodev で mount（展開する set-user-ID の file を live の system から使わせない）。
6. **展開**: 計画の unit を依存の順（base が先）に、SHA-256 を確かめてから展開。進みを byte で報告。中止の要求は file の間で受ける。
7. **設定**: `/etc/fstab`（swap file の時）、`/var/db/installer/`、swap file（§5.5）、利用者と root の password と自動 login（§4.7a）。live の system の `/etc` は写さない（base の unit の既定）。Desktop を入れない時は graphical の login を設定しない（`zedbsd.cfg` に `login=graphical` を書かない）。
   - 既知の password を残さない: base の unit の `/etc/shadow` は root と既定の利用者（今 kei、p002 の後 user）に固定の hash を持つ（`userland/base/etc/shadow:1,5`）。helper は S7a の入力で root と利用者の hash を必ず書き換え、既定の利用者を入力の利用者に置き換える（同じ名前の時も password は入力の物）。書き換えた後、導入先の shadow の root・利用者の行が base の物と違うことを確かめる。
   - OpenSSH を入れた時: package の `sshd_config` は `PermitRootLogin yes`・`PasswordAuthentication yes`・全ての address で待つ（`userland/packages/network/openssh/files/sshd_config:6,11,12`）。導入先では `PermitRootLogin no` に書き換える（§16 T14）。
   - 利用者の file の編集: 共有の account の core は `/etc` の path を固定で持つ（`userland/base/common/account.h:48-57`）ので、導入先の root を前提に開く形を足す（p009）。hostname は S7a の値（既定 `zedbsd`）。
8. **root の unmount と sync**。
9. **ESP の公開**: kernel（`/boot/vmunix`）と logo（`/boot/logo.ppm`）は段 6 で root の UFS に置き、段 8 で sync 済み。ESP を mount し、loader（§5.4 の path）を書き、最後に `zedbsd.cfg` を一時の名前で書いて rename。`zedbsd.cfg` の中身（kernel・logo の書き方は §5.10）:

   ```text
   kernel=rootpart:/boot/vmunix
   rootpart=PARTUUID=<root>
   swap0=PARTUUID=<swap>        （swap の partition の時だけ）
   logo=rootpart:/boot/logo.ppm （live の system の設定に倣う）
   login=graphical              （Desktop を入れた時だけ）
   kmsg=quiet
   ```

10. **ESP の unmount と sync**。
11. **boot entry**: `Boot####` を書く（§5.8）。失敗は警告にとどめる。成功を報告。

### 7.3 起動の識別

- root・swap は PARTUUID で選ぶ（§5.3）。PARTUUID は helper が乱数で作り（既存の partition を使う時も替える、§5.3）、disk の中と既存の entry と重ならないようにする。boot0 は書かず、loader が新しい ESP の乱数の serial から足す `boot0=UUID=` に任せる（§5.4、§16 T4）。

### 7.4 partition の照合（書く直前）

- 計画の partition は全て (disk の登録番号, 開始の LBA, sector の数, PARTUUID) の 4 つで指す。slot の番号・device の名前だけで指さない（delete の後の add は同じ slot を使い、`sda3` が別の範囲を指しうる。kernel の partition の名前は GPT の slot の番号、`src/drivers/disklabel/gpt.c:683`・`src/kern/partition.c:599-601`）。
- 0 埋め・`mkfs`・`mkswap` の直前ごとに、その device を `BLKGETINFO`・`BLKGETIDENTITY` で調べ、`parent_device` が計画の disk の登録番号、`parent_offset`・`sector_count` が計画の開始・大きさ、PARTUUID が計画の値と一致することを確かめる。kernel の partition の表が disk の表と一致すること（diskpart の `live_extents_match` と同じ検査、`userland/base/diskpart/main.c:191-216`）も確かめる。reload が失敗して kernel が古い表を持っている時はここで止まる。
- 確認の phrase（`FORMAT name:registration`）は、いま確かめた partition の登録番号で作る。
- helper は、計画の 4 つ組が今の表に無い計画を拒む。

## 8. 失敗とキャンセル

| 時 | 導入先の状態 | 画面 |
| --- | --- | --- |
| S4 で何も書く前のキャンセル | 無傷 | S10 に「disk は変えていません」 |
| S4 で書いた後のキャンセル（S5〜S8） | S4 の変更（削除・作成・ESP）が残る | S10 に書いた変更の一覧 |
| S4 の 1 つの書き込みの失敗 | §4.4（作成の失敗は ESP の entry を消す試み、reload の失敗は再起動まで使えない） | S4 に理由。先へ進めなければ S10 |
| §7.2 の段 1〜2 の失敗 | S4 の変更だけ | S10 |
| 段 3 の途中・後 | partition の表が変わったかもしれない（diskpart の exit 3 は「書いたが reload の失敗」） | S10 に表の状態と「再起動が要るかもしれない」 |
| 段 4〜8 の失敗・キャンセル | 表は新しく、format・展開が途中。`zedbsd.cfg` は無いので導入先からは起動しない | S10 に状態 |
| 段 9 の失敗 | ESP に一部の file。`zedbsd.cfg` は最後なので起動の設定は無い | S10 に状態 |

- 巻き戻しはしない（diskpart と mkfs は crash に原子的でない、`docs/reference/block-administration.md`。§4.4 の ESP の自動作成の失敗だけは作った entry を消す）。「最初からやり直す」で同じ disk に入れ直せる（全体なら表を作り直す。選ぶなら S4 で作った partition は型が「未割り当て」のまま残り、S5 で選べる）。
- 段 3・4 の最中のキャンセルはその段の終わりまで待つ。段 6 は file の間で止まる。

## 9. 権限（U6 の回答「setuid の helper、自分で起動 (推し)」）

### 9.1 形

- installer は利用者（live の session の `user`）で動き、権限を持たない（`docs/architecture/security.md:11-30` の原則。U6 の UI と helper の分け方は構造として保つ）。
- **installer の image の扱い**（J5 の回答、2026-10-28、原文は §15。J1「live の password を非公開に」は J5 で取り消した）:
  - live の利用者は U26 の既定と同じ `user`/`user`（公開の既定の値、T24）。live の tree の `/etc/passwd`・`/etc/shadow`・`/etc/group` は base の既定（p002 の後）をそのまま使い、installer の image だけの password の上書きをしない。root の行は release の image と同じ（`ZEDBSD_ROOT_LOCKED` の lock、`Makefile:1183-1193`・`:1268-1272`）。
  - **installer の image での起動は、既定の利用者での起動だが root での起動に相当する**と扱う。live の利用者の全ての process（browser の中の web の内容など遠隔の内容を含む）が root と同じ力を持つ前提で設計し、live の system の中で利用者と root を分ける守りは求めない（J5「インストーラがrootで動くことは一般的であり、セキュリティ上の懸念は存在しません。」）。
  - **installer の image を運用に使ってはいけない**（規則）。運用は、導入の外の目的で live の system を使い続けること（USB の image を常用の system として起動し続ける、installer の image を内蔵の disk に dd して使う、network の service を載せる、利用者の data を置く）。S11 の「閉じる」の後に live の desktop をそのまま使えること（原文）は、その導入の続きとして許す。利用者に示す所（T25）: S1（§4.1）、S11（§4.11）、文書（p013 の `docs/howto/boot-and-storage.md` の installer の節と `docs/reference` の install-admin）。
  - live の session の lock（compositor の 10 分の idle の lock、`userland/desktop/wayland/main.c:33`・`:153-155`）・log out・sleep の後の lock は普通の image と同じ（password `user` で戻れる）。installer の image のための lock の設定は足さない。`sudo`・`su` も base の既定のとおり（`user` は wheel）。
  - 配布の installer の image は network の login の service を持たない（openssh は package の unit で live の tree に入らない、§11.2。試験の build の hook（J4）は `plan/ws307/tests/` だけ）。password が公開なので、配布の live の tree に sshd などの network の login を足す変更は J5 の前提の見直しとして扱う。
- helper は **password を聞かない**（U26 の回答「インストーラはsetuidのヘルパーを使うので、パスワード認証を不要にします。これは特別なインストーライメージのみです。」）: security.md の「管理者は変更の時に自分の password を入れる」の例外。installer の image の起動は root 起動相当（J5）なので、password 無しの root の操作はその前提で受け入れる。次の確かめは helper が installer の image の外（普通の image・導入先）と他の利用者で動かないための物（live の system の中の守りの境界ではない）:
  - helper は `make installer-image` の live の tree にだけ入れる（§11.1）。普通の image・導入先には無い。
  - file の mode は 4550、所有者 0:0（root、group wheel。wheel は gid 0、`userland/base/etc/group:1`）。wheel の利用者だけが実行できる（3 回目のレビュー #11。`account-admin` は 4555、`userland/base/account-admin/Makefile:8`）。kernel は実行の許可を egid と補助の group で見る（`src/kern/cred.c:122-140`、sessiond は `initgroups` で補助の group を付ける、`userland/desktop/sessiond/session.c:646`）。
  - helper の中でも、`getgroups()` か egid に 0（wheel）があり、real uid が `/etc/keiland/autologin` の名前の利用者の uid と同じことを確かめる。
  - helper は起動の時に、自分に埋め込んだ index の hash と `/opt/installer/index` が合うこと（installer の image で動いていること）を確かめ、合わなければ何もしない。
  - 危険の扱い（J5 の前提）: live の利用者の process は画面を通さず password 無しで helper を呼び、全ての disk を書ける。installer の image の起動を root の起動に相当と扱う（J5）ので、これは root で動く installer の system で root の process が disk を書けるのと同じで、新しい危険として扱わない。残る守りは 2 つ: helper が普通の image・導入先に入らないこと（§11.1 の SELECTABLE n と build の error、上の index の hash の確かめ。普通の image は root 起動相当でないので、ここは守りの境界）と、installer の image を運用に使わない規則（上、S1・S11・文書で示す）。helper の呼び出し元を installer の process に絞る形は J5 の前提で要らない（以前の Future から外した）。
- root の仕事は新しい set-user-ID root の helper `/usr/libexec/install-admin`（mode 4550、所有者 0:0。`account-admin` と同じ形の 1 つの仕事だけの program、`userland/base/account-admin/main.c:8-45`。呼び出し元が wheel かは helper が自分でも確かめる）。source は `userland/desktop/installer/admin/`（J2、§11.1）。
- installer が helper を子として起動する（U6 の回答。zedBSD の UEFI PC だけの app なので compositor を通さない。Guardrail の「app 自身の機能のための OS の依存」、D14）。
- 要求の種類:
  - `probe`: 読むだけ（disk・partition・ESP の空き・RAM・UEFI の起動か）。
  - `session`: 書き込みの session。上の呼び出し元の確かめの後、installer が終わるまで stdin の要求を順に受ける: `edit`（§7.1 の 1 つの操作）と `install`（§7.2）。S4 の最初の書き込みの前（選ぶ道）か最終確認の開始（全体の道）で始める。
- 全ての要求を syslog の `auth` に記録する（S7a の password は記録しない）。
- S7a の password: installer は送った後に `kl_field` の文字を消す。helper は hash を作った後に memory を消す。
- signal と権限: helper の real uid が live の利用者のままだと、その利用者は SIGKILL・SIGSTOP を helper と子の diskpart・mkfs に送れる（`src/kern/signal.c:1659-1667`。SIGTERM を無視しても防げない）。そこで helper は呼び出し元を確かめた後に `setresuid(0,0,0)`・`setresgid(0,0,0)`・`setsid()` をし、子を完全な root として起動する。`clearenv()`（`account-admin` と同じ、`userland/base/account-admin/main.c:266`）、他の fd を閉じる、umask と rlimit（`RLIMIT_FSIZE`・`RLIMIT_NOFILE`）を既定に戻す。SIGPIPE・SIGHUP・SIGINT・SIGTERM は無視し（`account-admin/main.c:41-42`）、installer が先に終わっても（stdout の pipe が閉じても）その段の終わりまで進め、記録して終わる。子の diskpart・mkfs を途中で残さない。

### 9.2 protocol

- `session` の stdin: 要求を 1 行ずつ: `edit delete`・`edit create`・`edit create-with-esp`（各々に disk の識別と、画面が表示した表の digest、対象の partition の 4 つ組（§7.4）を付ける）、`install <計画>`。`install` の計画は（disk の登録番号と識別、root・swap・ESP の 4 つ組、unit の一覧、利用者の設定、hostname）。利用者の入れた文字（表示名・password・hostname）を含む field は長さを前に付けた形で送り、空白・改行で要求を壊せないようにする。syslog には password を書かない（計画の中の password も伏せる）。stdout: 1 行ずつ `STEP <n> <総数> <名前>`、`BYTES <済み> <合計>`、`UNIT <名前>`、`DONE`、`FAIL <段> <errno> <message>`、`STATE <導入先の状態の要約>`。
- 中止: installer が stdin に `cancel` の行を送る（signal でなく。set-user-ID の process への signal の可否に頼らない）。installer が終わった（stdin の EOF）時も中止と扱う。
- helper は計画を全て自分で検査し直す（installer を信用しない）: 選んだ disk が live の system の使う disk でない（§4.2）、partition の重なり・境界、触らない entry を変えていない、unit が index にあり SHA-256 が合い、index の SHA-256 が helper に埋め込んだ値と合う（§6.1）、利用者の名前と password の規則。

## 10. 翻訳と表示

- 文字は `kl_tr`（domain `installer`）。英語が原文、日本語の catalog `userland/desktop/locale/ja/installer.tr` を作る（`tools/i18n/tr.py`、WS158 の仕組み）。desktop の `ui.language` に `kl_tr_follow` で従う（`keiland.h:2849`）。
- 寸法は論理 px（Guardrail「寸法の単位」）。
- App Home の entry を `userland/desktop/wayland/apps.conf` に足す（command の path が無ければ隠れる、`wayland/home.c:41-50`。installer の無い image では出ない）。icon は `wayland/icons.c:596-619` の一覧に足す。

## 11. build と image

### 11.1 置き場所

| 物 | source |
| --- | --- |
| installer（app） | `userland/desktop/installer/`（`ZEDBSD_USERLAND_PACKAGE`。menu が base・desktop などの時、platform は `*` に固定される（`Makefile:455`）ので、専用の menu の値（installer）にし、`make installer-image` だけが選ぶ。普通の image・Linux・FreeBSD の Keiland の build には入れない） |
| install-admin（helper） | `userland/desktop/installer/admin/`（J2 の回答「userland/base の外へ移す (推し)」。`userland/base` の program は arch・platform で有無を分けず全部を root に入れる規則（`plan/guardrail.md` の「userland/base の program の有無」）なので、installer の image だけに入れる set-user-ID root の helper は base に置かない）。menu は installer、platform は amd64。account の core（`userland/base/common/account.*`）・format の共有の code は `account-admin` と同じく source として使う。UEFI でない system では `probe` が「対象外」を返す |
| unit と index を作る道具 | `tools/build/`（host） |
| image の target | `make installer-image`（2026-10-28 ユーザーの指示、§11.2）。menuconfig は kernel の config にだけ使う |

- installer の menu の package（installer・install-admin）は SELECTABLE を n にし（menuconfig に出さない）、`make installer-image` の外の build で `ZEDBSD_USER_PROGRAMS` に入っていれば（手で書いた config.mk、`Makefile:513-519` の依存の展開は登録された package を何でも選ぶ）Makefile が error で止める（3 回目のレビュー #12。普通の image に password 無しの set-user-ID root の helper を入れない）。helper の index の hash の確かめ（§9.1）は 2 つ目の守り。

### 11.2 image

- 指示（2026-10-28 ユーザー、原文、Q1 の中継）:「make menuconfigは、インストーラ作成の場合、カーネルのコンフィグには使いますが、make installer-imageでは、すべてのパッケージをビルドしますし、イメージの形式（kernelのパーティション）が異なります。」U1 の「menuconfig で選ぶ」はこの形に読み替える（Q1 の中継の解釈）。
- `make installer-image`:
  - kernel（`vmunix`）と loader は menuconfig の config（kernel の driver など）で build する。導入先に入る kernel と同じ物。
  - userland・package は menuconfig の選択に依らず、決まった選択で全部を build する（§6.2、U22・U24）。
  - 全 unit・index・`/opt/installer` を作り（§6）、live の root（base・desktop・firmware の unit と installer・install-admin・`/opt/installer`）を作る。
  - **make の形**（3 回目のレビュー #3）: 今の Makefile は package の選択を parse の時に決め（`Makefile:504-532`）、root の tree は 1 つの `$(BUILD)/rootfs`（`Makefile:1422` 付近）で、native の image の rule は `VARIANT=native` の時だけある（`platform/amd64/vmunix.mk:1302`、`Makefile:213`）。同じ make の中で選択を変えられず、`installer-image` と `hdd-image` が同じ tree を書き換え合う。そこで:
    - `installer-image` は再帰の `$(MAKE) ZEDBSD_IMAGE_PROFILE=installer …` を起動し、その make は parse の時に選択（`ZEDBSD_USER_PROGRAMS`）を installer の決まった選択で置き換える。
    - tree は `$(BUILD)/installer/{full,live,units}` に置き、`$(BUILD)/rootfs` を使わない。`vmunix`・loader は普通の build と共有する（同じ config から同じ物）。
    - amd64 で `VARIANT=native` でなければ error。`installer-image` を他の image の goal（`disk-image`・`hdd-image` など）と同じ make の command line に並べたら error。
    - 起動の設定: graphical の login と graphical の boot は installer の image では常に入（installer の自動起動に要る）。`kmsg` と追加の boot の行（extra の行）は menuconfig の値を使う。
  - 出力は `$(BUILD)/installer-image.img`。普通の `hdd-image.img`（`make disk-image`）とは別の target で、`hdd-image.img` の形・大きさ（WS238 の形、`AMD64_NATIVE_ROOT_MIB` 1024、CI の 2 GiB、`platform/amd64/vmunix.mk:1333`・`:1344`）は変えない（U21 の「image は今のまま」は `hdd-image.img` の事）。
- installer の image の形（U23 の回答「インストーライメージでは、ESPにブートローダ、BOOT FAT32にカーネルとzedbsd.cfg、rootfs, swapのパーティションを置きます。」）: ESP（loader）、BOOT の FAT32（`vmunix`・`zedbsd.cfg`）、root の UFS（`/opt/installer` を含む）、swap の 4 つ。loader の UFS の読み（p008）を待たずに作れる。BOOT の FAT32 は今の `hdd-image.img` と同じく Basic Data の型・volume label「ZEDBSD」で、`logo.ppm` もその FAT32 に置く（U25 の回答）。`hdd-image.img` との違いは root の大きさ・`/opt/installer`・build ごとの PARTUUID の cfg。root の大きさは unit の合計に合わせる。導入先の形（ESP に loader・cfg、kernel は root の UFS、§5.4）とは違う。
- live の image の起動の選び方: 今の native の image は `rootpart=PARTLABEL=zedBSD-root`・`swap0=PARTLABEL=zedBSD-swap`（`platform/amd64/zedbsd-native-uefi.cfg`）で、同じ image を dd した disk が付いていると EEXIST で起動しない（`src/kern/block-identity.c:184-190`）。zedimage-host は今も build ごとに disk と partition の GUID を乱数で作る（`tools/build/zedimage-host.c:749-760`、3 回目のレビュー #17）。違いは cfg がその PARTUUID を名指すこと: installer の image では zedimage-host に PARTUUID を外から渡す（または作った値を出力する）形を足し、その値を `rootpart=PARTUUID=`・`swap0=PARTUUID=` として BOOT の FAT32 の cfg に書く。同じ image の file を 2 台に dd した時の曖昧さは残る（loader の disk を優先する kernel の規則は Future）。
- root の大きさと zedimage-host の上限（3 回目のレビュー #17）: zedimage-host は image 全体を host の memory に取り（`calloc`）、上限は 16 GiB（`tools/build/zedimage-host.c:27`）、inode は 65536 まで（`:18` の `MAX_INODES`）。root の大きさは「live の tree ＋ `/opt/installer`（全 unit）＋空き」で、2 段で量る（live の tree と unit を作った後に合計を出し、空きを足して image を作る）。上限を越えれば build を失敗させ、量った値を出す。今の image の検査 `platform/amd64/tools/check-amd64-native-image.py:190-202` は今の cfg（PARTLABEL）を前提にするので、p003 で installer の image の形（PARTUUID の cfg）を検査する形を足す。
- live だけの上書き: `/etc/keiland/autologin`（`user`）だけ。unit（導入先に入る物）には入れない。password・shadow・session の lock は base の既定のまま（J5、§9.1。J1 のための乱数の password と lock をしない設定は J5 で外した）。
- 試験の build（3 回目のレビュー #4、J4 の回答「許す (推し)」）: live の image には試験の手段が無い（openssh は package で live に入らない、`aat-input`・`keiland-shot` は tests の menu、`login=graphical` は getty の代わりに graphical の login を出す、`userland/base/etc/rc.conf:33-35`）。そこで、`plan/ws307/tests/` の config.mk だけが使う試験の hook（`ZEDBSD_INSTALLER_TEST_EXTRAS` などの変数）で、`network/openssh`・`aat-input`・`keiland-shot` と試験の SSH の鍵（`user` の `authorized_keys`）を **live の tree にだけ** 足す。unit・index・CI・配布の image には入れない（U24 の「tests・X11 無し」は導入先と配布の image に当てる、と J4 で確かめた）。index の hash は試験の build でも同じ（unit が同じなので）。
- 起動時に自動で開く（U1 の回答）: compositor の Welcome の自動起動（`userland/desktop/wayland/desktop.c:857-880`）と同じ仕組みで、`/opt/installer/index` がある時に 1 回 `installer` を起動する。Settings の Welcome と重ならないよう、installer がある時は Welcome を出さない。S1 と S11 は「installer の image を運用に使ってはいけない」規則を出す（J5、§4.1・§4.11・§9.1）。

### 11.3 base の道具に足す物

- `mkswap` の partition の mode: `mkswap DEVICE`（block device、`BLKRESERVE`、`FORMAT name:registration` の phrase、書いた後の検証、ZEDSWAP2 の 64 bit の大きさの検査）。今は regular file だけ（`userland/base/common/format-file.c:48`）。
- `mkfs -t fat32` の乱数の serial（今は disk の登録番号、`userland/base/mkfs/block-command.c:157`）。既定を乱数にするか option にするかは p005 で決める（他の利用者が今の値に頼っていないかを確かめる）。
- （§5.6 の量りの結果で）`mkfs -t ufs --profile=native` の inode の密度。
- `diskpart` の batch の mode: 複数の操作（add・delete、型・名前・PARTUUID・attribute の変更）を 1 回の確認（`WRITE name:registration`）で 1 回書き、触らない entry を保つ。型と名前の変更は今の diskpart に無い（add・delete・init だけ、`userland/base/diskpart/main.c:73-90`）。

## 12. 試験

| 層 | 方法 |
| --- | --- |
| host | 計画の組み立て（配置・ESP の自動作成・検査）、index と依存の閉包、ustar＋deflate の展開（path の安全）、`zedbsd.cfg` の生成、diskpart の batch と mkswap の partition の mode の table・codec。host の C の試験 |
| QEMU の helper | live の image（`make installer-image` を `plan/ws307/tests/` の config.mk の試験の hook で、§11.2）を USB で起動、導入先の空の disk を `guest.py start --qemu-extra` で足す（NVMe `-device nvme`、SATA は q35 の ich9-ahci に `-device ide-hd`、USB `-device usb-storage`）。試験の鍵の SSH で `install-admin` を直に流す（GUI 無し） |
| QEMU の GUI | AAT（`plan/tools/aat/`、`aat-input`・`keiland-shot`）で S1〜S11 を操作。installer の READY・画面・control の log で操作する。S1・S11 に「運用に使わない」規則の文が出ること（J5） |
| 導入先の起動 | 導入先の disk の file だけで `plan/tools/boot-test.sh`（`BOOT_MODE=uefi-nvme`）の login prompt。boot-test.sh が login の画面を読めるよう、起動を確かめる導入は S7a の自動 login を切って行う（自動 login が入だと desktop が出て login prompt が無い）。導入先の中の確かめは OpenSSH を選んで入れ、SSH で行う。USB の image を付けたままの起動でも root が導入先になる（PARTUUID） |
| 共存 | 前もって「他の OS」の GPT（ESP に `\EFI\BOOT\BOOTX64.EFI` と別の file、Basic Data の partition に file）を作った disk に「partition を選ぶ」で入れ、触らない partition の byte の hash が変わらないこと、既存の fallback の loader が残り `\EFI\ZEDBSD\` に置かれること、Boot#### が書かれ OVMF の変数に残ること（p007 の後） |
| 「partition を選ぶ」を拒む場合 | 古い native の zedBSD（ZEDBSD の FAT に `zedbsd.cfg`）がある disk、未割り当ての partition に古い FAT が残る disk（0 埋めの確認）、diskpart が編集できない GPT（backup が途中にある・degraded）、使っている entry が 16 の disk、空・MBR の disk（U10） |
| 起動の識別 | 同じ image を dd した disk を 2 台目に付けて live の installer の image を起動、USB の導入先と別の USB の disk を付けての導入先の起動（boot0 の待ち） |
| helper の堅さ | 敵対的な計画（live の disk・範囲外・重なり・触らない entry の変更）を helper に与える host の試験、改ざんした `/opt/installer`（index と tarball）、段の途中で installer を kill、2 つの installer を同時に、inode の不足 |
| 失敗 | 容量の不足、展開の途中のキャンセル、SHA-256 の不一致（壊した unit）、導入先の partition の mount が外れない。S10 の表示と、導入先から起動しないこと |
| 実機 | Q1 が割り当てる。導入先は空の USB の disk か空の NVMe（ユーザーの判断）。ユーザーの試験の一覧に置く |

- boot-test.sh の UEFI の mode は disk を 1 台しか取らない（`plan/tools/boot-test.sh:316-335`）ので、導入の試験は guest.py の `--qemu-extra` を使う 1 本の script を `plan/ws307/tests/` に作る（配置 → 起動 → 導入 → 回収 → 導入先だけの起動 → 要約）。

## 13. 範囲外（Future の候補）

- partition の大きさの変更・移動、MBR から GPT への変換、空の disk に GPT を作って partition を選ぶ（U10 の回答で「disk 全体を使う」だけ）。
- disk の型番・serial の表示（U9 の回答）。
- network からの導入、package の後からの追加・削除・更新（§6.4 の記録だけを残す）。
- 同じ disk に 2 つ目の zedBSD を入れる（disk のどれかの FAT に `zedbsd.cfg` があれば「partition を選ぶ」を無効にする、§4.3）。
- 導入の間の電源の操作（再起動・電源断・suspend）を止める口。
- loader が UFS の最後の j3 の commit を重ねて読む（journal の読み取り専用の再生、§5.10）。
- package の後からの更新に合わせた kernel の更新の道具（§5.10 の規則に従う）。
- `volumed` に導入先の disk を隠す要求。
- PARTLABEL・UUID の選び方で loader の disk を優先する kernel の規則。
- 暗号化、RAID、LVM 相当。
- BIOS・他の arch。

## 13a. OS 名「Kei」を zedBSD に統一する（p002）

- 指示（2026-10-28 ユーザー、原文、Q1 の中継）:「P4に伝えてください。起動ロゴ画像を含めて、KeiというOS名を削除して、zedBSDに統一します。インストーラだけでなくすべてに影響します。そういうphaseをこのWSに追加してください。」と U15「Keiの名前はプロジェクトから削除予定です」。
- 範囲: OS 名の「Kei」（画面の文言・起動の logo の画像・`/etc/os-release`・UEFI の名前・文書・release の道具の名前）、J3 の回答「これらも変える」の 4 つ（GPU の command の protocol の名前、mark の絵と Files の文字、license の記録の文、GitHub の release の asset の名前と取得の道。下の「J3 の物」）、既定の利用者 `kei`/`kei` → `user`/`user`（U26。`/etc/passwd`・`/etc/shadow`・`/etc/group` の wheel・network と利用者の group `kei:x:1000:`（`userland/base/etc/group:12`）→ `user:x:1000:`・`/etc/keiland/autologin`・`/home/kei`。installer の image の live の利用者も同じ `user`/`user`、J5・§9.1）、既定の hostname `kei` → `zedbsd`（U27、`userland/base/etc/rc.conf:2`）。desktop の名前 Keiland と libkeiland（と Keiland の内部名: libkeiland-backend-*・libkeiland-compat・kl_*・`/etc/keiland`・`/usr/share/keiland` など）は残す（U28 の回答）。logo の画像は U29 の回答（担当が今の logo と同じ様式で「zedBSD」の文字の画像を作り、PNG を Q1 経由でユーザーに見せて確かめてから入れる）。
- U28 の回答（原文）:「Keilandの名前はそのままです。libkeilandもそのままです。KeiというOS名は、予定ではzedBSDに変更ですが、OSディストリビューション名が変更になる可能性があります。zedBSDはカーネル名です。OS名にするかは未定ですが、現状ではOS名もzedBSDに仮決めしておきます。libkeiland以外にlibkei*があるなら、変更を検討します。」
- **OS 名を 1 か所で定義する**（distribution 名が後で変わり得るため）: build の変数 `ZEDBSD_OS_NAME`（今の仮の値 `zedBSD`）を 1 つ置き、`/etc/os-release` の `NAME`・`PRETTY_NAME` をそこから作る（今は `NAME="zedBSD"`・`PRETTY_NAME="Kei/zedBSD …"` を Makefile が直に書く、`Makefile:1259-1266`）。画面に OS 名を出す program は source に OS 名の文字列を書かない。kernel の名前（`uname` の zedBSD、`ZEDBSD_VERSION` の行）は kernel 名なので別に保つ。logo の画像は絵なので変数にできない（名前が変われば作り直す）。
  - 定義の置き場（3 回目のレビュー #16）: `ZEDBSD_OS_NAME`（今 `zedBSD`）と `ZEDBSD_OS_ID`（今 `zedbsd`、os-release の `ID`）を 1 つの make の断片に置き、`Makefile` と Linux・FreeBSD の Keiland の build（`userland/desktop/keiland-linux.mk`・`keiland-freebsd.mk`）が include する。置き場の path は p002 で決める。os-release は `NAME="$(ZEDBSD_OS_NAME)"`・`ID=$(ZEDBSD_OS_ID)`・`PRETTY_NAME="$(ZEDBSD_OS_NAME) <版の名前>"`（今の `Kei/zedBSD` を外す）。
  - 走っている system を説明する文（Settings の About、installer の S1・S11 など zedBSD の上だけで動く物）は `/etc/os-release` の `NAME` を読む。
  - project の名前として出す文（browser の開始の頁「The Web browser of Kei」`userland/desktop/browser/data/start.html:27`、Files の hero の文字など、Linux・FreeBSD の Keiland の build でも動く物）は os-release を読まない（Linux・FreeBSD では host の名前になる）。build の定数（`ZEDBSD_OS_NAME` を `-D` か data の file の置き換えで）を使う。
  - UEFI の Boot#### の名前（§5.8）は install-admin が導入先の `/etc/os-release` の `NAME` から作る。loader の文言は os-release を読めないので build の定数。
- libkei* の棚卸し（U28 の「libkeiland以外にlibkei*があるなら」）: 今の tree の library は libkeiland の系（libkeiland・libkeiland-backend・-backend-zedbsd・-linux・-freebsd・libkeiland-compat）だけ。旧名 `libkeiui` は libkeiland に統合済みで、残るのは comment の経緯（`userland/desktop/libkeiland/ui/window.h:316`・`:474` など）と、Linux・FreeBSD の host から古い file を消す一覧（`userland/desktop/keiland-linux.mk:171`・`keiland-freebsd.mk:176` の `lib/libkeiui.so`・`include/keiui.h`）だけ。別の libkei* は無いので、変える物は無い。
- 棚卸し: `git grep -I -i -w kei`（`plan/` を除く）で、2026-10-28 の基点で約 460 行・約 200 file（`userland/desktop` 73・`tests/scenarios` 59・`userland/base` 37・`src/drivers` 9・`tools/release` 7 など）。`-w` は `KEI_NIGHTLY_*`・`kei-nightly`・`SKK-JISYO.kei` のような語の一部を落とす（3 回目のレビュー #15）ので、棚卸しは `git grep -I -i kei` から `keiland`（大文字・小文字を問わず）を除いた物で行う。ほかに binary（logo の画像、`bootloader/uefi/logo.c`・`bootloader/bios/logo.c` の参照する絵、`platform/amd64` の boot-logo、`kei-boot-splash.png`）、file の名前・directory の名前（`git ls-files | grep -i kei | grep -vi keiland`）。分類: 文言、利用者名・home の path、hostname、logo と mark、file の名前、試験（scenarios・AAT・guest の login）、文書、release の道具、J3 の物。`plan/` の履歴は直さない（Q1 が今も有効な規則の文だけを直す。master-design-policy §11 の「logo は必ず Kei」も）。
- **`plan/tools` も棚卸しに入れる**（3 回目のレビュー #6）: 共有の試験の道具が既定の利用者 kei を持つ（`git grep -i -w kei -- plan/tools` で 285 行。例 `plan/tools/aat/scenarios/aatlib.py:48-50` の `AAT_USER`・`AAT_USER_PASSWORD` の既定 kei、`plan/tools/guest/serial.py:93`、`plan/tools/hw5330/`）。`plan/tools` は WS307 の所有の外で、他の担当の branch の image（kei/kei）もこの道具で試験する。そこで:
  - p002 の前に（Q1 か、Q1 が p002 の Queue で範囲を明示して）、道具が `user` を先に試し、だめなら `kei` を試す形にする（login・SSH・AAT の既定）。古い image と新しい image の両方で動く。
  - p002 の Queue は `plan/tools` の変更を範囲に含めることを Q1 が明示する（所有の調整）。
  - p002 の受け入れに、他の branch の image（kei/kei のまま）と p002 の後の image（user/user）の両方で、道具の回帰（boot-test.sh・guest の SSH・AAT の login）が通ることを入れる。
- **J3 の物**（J3 の回答「これらも変える」。3 回目のレビューの推しは「残す」だったが、ユーザーは変えるを選んだ）:
  - GPU の command の protocol: 名前は `include/uapi/gpu-op.h:9` の comment「The Kei GPU command protocol, version 1」と driver の comment（`src/drivers/gpu/*/vk/*.h`・`vk-stream.c` など）だけにある。UAPI の識別子（macro・struct・enum の名前）に Kei を含む物は今の tree に無い（2026-10-28 に `git grep -i kei -- include/uapi src/drivers/gpu` で確かめた）。名前を「zedBSD GPU command protocol」に変える。番号・struct の形・版は変えない（ABI は同じ）。p002 の棚卸しで GPU の道（libvulkan など）に Kei を含む識別子が見つかれば、番号と形を保って名前だけを変え、使う側を同じ commit で直す。
  - mark の絵: `userland/desktop/artwork/mark.c` の「the Kei mark」（縦の棒と葉、`kei-boot-splash.png` と同じ意匠。compositor の greeter・lock screen と Files が使う、`artwork/mark.h:10`）を zedBSD の mark に描き直す。U29 と同じ手順（担当が今の様式で案を作り、PNG を Q1 経由でユーザーに見せて確かめてから入れる）。Files の hero の文字「Kei」（`userland/desktop/files/ui-home.c:545-567`）は build の定数の OS 名にする（上の「project の名前として出す文」）。
  - license の記録の文: `userland/desktop/ime/dict/Makefile:15` の「relicensed under zlib for Kei on 2026-09-29」と `userland/desktop/ime/dict/SKK-JISYO.ja:2`・`:9-10` の header の Kei。著作権者はユーザー自身で、ユーザーが変えると決めた。「for zedBSD」（同じ project の今の名前）に変え、日付とユーザーの発言の原文の引用（Kei を含まない）はそのまま保つ。「formerly SKK-JISYO.kei」のような旧い file の名前の注記は、下の「残す物の一覧」に入れて Q1 が確かめる。
  - GitHub の release の asset の名前と取得の道: `tools/release/kei-nightly.mk`（rev-0 の asset `kei-nightly-base-winq-a10-1.zip` を名前と SHA-256 で固定、`:13-21`）、出力 `Kei-nightly.zip`、target `kei-nightly-base`・`kei-nightly-zip`、変数 `KEI_NIGHTLY_*`、`tools/release/make-kei-nightly-base.py`・`make-kei-nightly-zip.py`・`tools/release/kei-nightly/`、archive の中の root の directory `Kei-nightly/`（`make-kei-nightly-base.py:25-27`）、`.github/workflows/ci.yml:85-86`、`release.yml:4`・`:6`・`:110-111`・`:162`・`:207`（release の title「Kei/zedBSD」）。asset の名前を変えると取得が壊れるので、取得の道（mk・script・workflow）を同じ Phase で直す。移行の手順（p002）:
    1. 棚卸し: archive の中の `Kei-nightly/` の名前に、中の launcher・script（WINQ-EMU）が頼るかを確かめる。頼れば中の名前は変えないか、頼る file も直す（どちらかを Q1 に報告して決める）。
    2. 新しい asset: 外の file の名前だけを変えるなら同じ byte（SHA-256 は同じ）を新しい名前で用意する。中の名前を変えるなら、決まった出力の base の作り方（`make-kei-nightly-base.py` の後継）で作り直し、新しい SHA-256 を量る。
    3. rev-0 への新しい asset の upload は GitHub への書き込みなので、ユーザーの指示で Q1 が行う（push と同じ扱い）。upload の前に、URL を変える commit を main に merge しない（CI の取得が壊れる）。
    4. 1 つの commit で、mk（asset の名前・要れば SHA-256・target・変数）・script の名前・workflow（`zedbsd-nightly-*` の target、release の title「zedBSD <名前>」）・文書を変える。確かめ: 新しい URL からの取得と SHA-256 の照合（`make` の取得の target）と、CI の 1 回の実行（ユーザーの push の後）。
    5. 旧い asset は rev-0 に残す（旧い tag の workflow の再実行・他の branch がまだ旧い名前を使う）。消すかはユーザーの判断（ユーザーの判断の一覧に置く）。手元の `build/releases/` の旧い名前の cache は消さない（共有の build、要れば Q1）。
  - 最終の release の asset（`zedbsd-$VERSION-*`）は既に zedBSD の名前で、変えない。
- **残す物の一覧**: p002 の後に `git grep -I -i kei`（keiland を除く）に残ってよい物は、Keiland・libkeiland と内部名（U28）、`plan/` の履歴、旧い名前の互換の code（Linux・FreeBSD の host から古い `libkeiui` を消す一覧、`userland/desktop/keiland-linux.mk:171`・`keiland-freebsd.mk:176`）、旧い file の名前の注記。p002 はこの一覧を phase.md に書き、Q1 が確かめる。
- 手順: (1) 棚卸しの一覧と、git mv・大量の rename の一覧を Q1 に出し、所有と他の担当の作業との衝突を調整してから変える（Q1 の補い）。(2) 文言・設定・試験の道具を変える。(3) logo（U29）。(4) build と試験。
- 順: installer の実装の前（p002）。installer の画面・live の image の利用者・hostname がこの Phase の後の名前を前提にする。ただし installer の helper は既定の利用者の名前に依らない（`/etc/keiland/autologin` と uid 1000 で見つける）ので、p003 以降はこの Phase の完了を待たずに進めてよい（通しの試験 p011 は待つ）。
- 試験: build（warning 0）、変えた所の host の試験、boot-test.sh の login prompt の PNG（新しい logo と login の画面）、AAT の login（user/user）と既存の scenario の利用者名の追従、`plan/tools` の道具の回帰を古い（kei）・新しい（user）の両方の image で、nightly の取得（新しい URL と SHA-256）、`git grep -I -i kei`（keiland を除く）の残りが「残す物の一覧」だけであること。
- 見積もり: 16 LW（logo と mark の絵の作り直し、`ZEDBSD_OS_NAME`、`plan/tools`、asset の移行を含む。upload を待つ時間を除く）。

## 14. 原文との対応

| 原文の項目 | この文書 |
| --- | --- |
| グラフィカル、Keiland のアプリ、UEFI＋ACPI の PC だけ | §1・§3 |
| 導入先は NVMe・USB Mass Storage・SATA など | §1・§4.2 |
| ウィザード形式、次へで進む | §4 共通 |
| Welcome | §4.1 |
| disk の選択 | §4.2・§5.1 |
| partition の扱い（全体／選ぶ） | §4.3・§5.7 |
| 簡易の partition の編集（一覧・削除・作成は大きさだけ・未使用・OS の ID 無し・ESP の自動作成と失敗・次へで ESP の検査） | §4.4・§5.3・§5.4 |
| rootfs の選択（UFS の印・mkfs） | §4.5 |
| 「ィションがないときは次の画面に進めない。」 | §4.5（U11 の回答） |
| swap の選択（swap の印・mkswap・選ばなくてよいが確認・無ければ UFS の上の swapfile） | §4.6・§5.5 |
| 入れる項目（Base（header 込み）・Desktop・Firmware・Packages と 7 つの下位・依存の自動選択） | §4.7・§6.3 |
| 最終確認 | §4.8 |
| progress | §4.9・§7 |
| 失敗（キャンセル・失敗の時も） | §4.10・§8 |
| 終了（再起動、終えて desktop を使い続ける） | §4.11 |
| base・desktop・firmware は tarball で `/opt/installer`、packages は package ごとの tarball、選択＋依存を入れる | §6 |

## 15. ユーザーの確認事項

2026-10-28 に Q1 へ送った（第 1 回 U1〜U8、第 2 回 U9〜U13、U14 は U2 の回答から。J1〜J4 は 3 回目のレビューから Q1 が聞いた物、J5 は J1 の回答から）。回答の原文（ユーザーのクリックで選んだ選択肢の文、Q1 の中継）をここに引用し、本文を直す。

| # | 問い | 推し | 回答（原文） |
| --- | --- | --- | --- |
| U1 | installer の載る媒体と起動 | A: menuconfig の選択、配布用の config で y、起動で自動で開く | 2026-10-28「menuconfig で選び自動で開く (推し)」→ A。後の指示（§11.2）で「menuconfig は kernel の config だけ、image は `make installer-image`」に読み替え。自動で開くのは残る |
| U2 | partition の編集を disk に書く時期 | A: 最終確認の後にまとめて | 2026-10-28「ボタンでその場で書く」→ B（原文どおり、S4 の button でその場で書く。キャンセルしても書いた分は残る）。§3・§4.4・§7.1・§8・§9 に反映 |
| U3 | 作成した「未使用」の partition の型 | A: zedBSD 独自の GUID | 2026-10-28「zedBSD 独自の未割り当ての型 (推し)」→ A |
| U4 | disk 全体の配置 | A: ESP 512 MiB（kernel も ESP）・swap partition・root | 2026-10-28「ESP 512M＋swap 区画＋root (推し)」→ A |
| U5 | 既存の ESP を共有する時の loader と boot entry | A: fallback が無ければそこ、有れば `\EFI\kei\`、Boot#### は Future | 2026-10-28「\EFI\BOOT\BOOTX64.EFIがなければそこに置く。あれば、上書きせず、\EFI\ZEDBSD\BOOTX64.EFIに置いて、完了の画面で、Frimware のboot menuで選ぶ、と出す。Boot####の作成もWSに入れる。」→ §5.4・§5.8・§4.11 |
| U6 | root の権限 | A: setuid の helper を installer が直に起動、最終確認で password | 2026-10-28「setuid の helper、自分で起動 (推し)」→ A。password は後の U26 の回答で聞かない形に置き換わった（§9.1） |
| U7 | 利用者のアカウント | B: 最終確認の前に利用者の画面 | 2026-10-28「「利用者」の画面を足す (推し)」→ B、§4.7a |
| U8 | 入れる項目の必須と既定 | A: Base 必須、Desktop・Firmware 既定で選択、Packages 無選択 | 2026-10-28「Base必須、DesktopとFirmwareは規定で、外そうとすると警告、PackagesはOpenSSL, OpenSSH, ca-certificates, Fontsのみ規定。Pythonのパッケージも追記しておいてください。」→ §4.7 |
| U9 | disk の一覧の型番 | B: kernel に model を足す | 2026-10-28「今の口で出せる物だけ」→ A |
| U10 | GPT でない disk で「partition を選ぶ」 | A: 空は新しい GPT、MBR は全体だけ | 2026-10-28「GPT の disk だけ」→ B（空・MBR は「disk 全体を使う」だけ） |
| U11 | 「ィションがないときは…」の行 | A: rootfs を選ばないと進めない | 2026-10-28「rootfs 未選択なら進めない (推し)」→ A |
| U12 | rootfs・swap に選べる partition（U2・U3 の後に書き直した問い） | A: 型が「未割り当て」の物だけ | 2026-10-28「ESP 以外の既存も選べる」→ B（選んだ時に「中身を消す」確認） |
| U13 | package の header | A: 今の package のまま | 2026-10-28「WS307 で header も入れる」→ B（GTK4 などの package に header と `.pc`、入れた system で GTK4 の app を build できる） |
| U15 | （U5 の回答から）作った Boot#### の BootOrder の位置 | A: 先頭に入れる（再起動で導入先から起動）。B: 末尾に足す（firmware の menu に名前で出るだけ） | 2026-10-28「末尾に足します。また、KeiではなくzedBSDです。Keiの名前はプロジェクトから削除予定です。」→ B、名前は「zedBSD」。spec と UI の文言で「Kei」の名を使わない |
| U22 | （installer-image の指示から）「すべてのパッケージ」と S7 の項目 | A: 全 package を unit に、S7 は 9 項目＋「その他」の下位に他の package | 2026-10-28「9 項目＋「その他」で個別 (推し)」→ A |
| U23 | （installer-image の指示から）installer の image の形（kernel の partition） | A: 導入先と同じ形（kernel は root の UFS） | 2026-10-28「インストーライメージでは、ESPにブートローダ、BOOT FAT32にカーネルとzedbsd.cfg、rootfs, swapのパーティションを置きます。」→ 別の形（§11.2） |
| U25 | （U23 の回答から）installer の image の logo の置き場と BOOT の FAT32 の型・label | A: 今の hdd-image の ZEDBSD の FAT32 と同じ（Basic Data、logo も FAT32） | 2026-10-28「今の hdd-image と同じ (推し)」→ A |
| U24 | （installer-image の指示から）Base・Desktop・Firmware の中身 | A: base の全部、desktop の menu の全部、firmware は kernel の driver が選ぶ物。tests・X11 は入れない | 2026-10-28「FW は driver の選択、tests・X11 無し (推し)」→ A |
| U19 | （追加の指示から）`zedbsd.cfg` の置き場 | A: ESP（loader の探し方を変えない） | 2026-10-28「ESP（FAT）のまま (推し)」→ A |
| U20 | （追加の指示から）起動の logo の置き場 | A: root の UFS の `/boot/logo.ppm` | 2026-10-28「root の UFS（/boot/logo.ppm） (推し)」→ A |
| U21 | （追加の指示から）今の disk image（hdd-image・installer の image）の形 | A: 導入先だけ新しい形、image は WS238 の形のまま | 2026-10-28「image は今のまま (推し)」→ A |
| U26 | （2 回目のレビュー #9）live の system の password の案内 | A: dialog に既定の password（kei）を書く | 2026-10-28「すべてのイメージで、ユーザ名をuser, パスワードをuserに変更します。インストーラはsetuidのヘルパーを使うので、パスワード認証を不要にします。これは特別なインストーライメージのみです。」→ installer の image では helper は password 無し（§9.1）。全 image の既定の利用者を user/user に（§13a、p002）。installer の image の live の利用者の password は後の J1 で非公開にしたが、J5 で公開の既定（`user`）に戻した |
| U27 | （2 回目のレビュー #23）導入先の hostname | A: S7a に欄、既定 `zedbsd` | 2026-10-28「今後keiの名前は削除しますので、zedbsdにします。」→ 既定 `zedbsd`。S7a の欄は技術の判断で足す（Q1） |
| U28 | （OS 名の統一の指示から）名前を変える範囲 | A: OS 名の Kei だけ。Keiland と内部名は残す | 2026-10-28「Keilandの名前はそのままです。libkeilandもそのままです。KeiというOS名は、予定ではzedBSDに変更ですが、OSディストリビューション名が変更になる可能性があります。zedBSDはカーネル名です。OS名にするかは未定ですが、現状ではOS名もzedBSDに仮決めしておきます。libkeiland以外にlibkei*があるなら、変更を検討します。」→ A。OS 名は 1 か所（`ZEDBSD_OS_NAME`）で定義（§13a） |
| U29 | （OS 名の統一の指示から）起動の logo の画像 | A: 担当が同じ様式で zedBSD の画像を作り、ユーザーに見せて確かめる | 2026-10-28「推し」→ A |
| U4b | （design-reviewer の指摘 14）U4 の答えと WS238 の決定（ESP は loader だけ、kernel・設定・logo は Windows で mount できる ZEDBSD の FAT32）の食い違い | B: 導入先も WS238 の形（ESP＋ZEDBSD の FAT32＋swap＋root） | 2026-10-28「U4 のまま ESP に全部」→ A（導入先は WS238 の image の形と違う、§5.4） |
| U16 | 既存の ESP が小さい・使えない disk で「partition を選ぶ」 | A: その disk では「partition を選ぶ」を無効 | 2026-10-28「その disk では無効にする (推し)」→ A |
| U17 | 4Kn の disk | A: 選べない（Future） | 2026-10-28「WS307 で 4096 byte に対応」→ B（§5.9） |
| U18 | Firmware の license | A: 配布の config の firmware、S7 で license を表示し同意を求める | 2026-10-28「一覧を出し同意で入れる (推し)」→ A（§4.7） |
| U14 | （U2 の回答から）S5・S6 の root・swap の型の印と mkfs・mkswap の時期 | A: 最終確認の後（progress の中）。B: S5・S6 の次へでその場で | 2026-10-28「最終確認の後 (推し)」→ A |
| J1 | （3 回目のレビュー、U26 の危険）live の利用者の process が helper で画面無しに全ての disk を書ける | A: 受け入れ、理由の文を直す。B: setgid の `_installer` で helper を 4510（弱い）。C: live の password を非公開に（U26 を変える） | 2026-10-28「live の password を非公開に」→ C（推しと違う）。installer の image の live の利用者の password を公開しない。**J5 の回答で取り消し・置き換え**（公開の既定の password、運用に使わない規則、root 起動相当、§9.1） |
| J2 | （3 回目のレビュー #5）install-admin の置き場 | A: `userland/base` の外へ移す。B: Guardrail の例外 | 2026-10-28「userland/base の外へ移す (推し)」→ A（`userland/desktop/installer/admin/`、§11.1） |
| J3 | （3 回目のレビュー #7・#15）OS 名の範囲: GPU の protocol の UAPI の識別子・mark の絵・license の記録の文・GitHub の release の asset の名前 | A: 残す（comment だけ、Files の文字は os-release、記録は変えない、asset の名前は保つ） | 2026-10-28「これらも変える」→ 変える（推しと違う）。取得の道（CI・script）も同じ Phase で直し、移行の手順を p002 に（§13a の「J3 の物」） |
| J4 | （3 回目のレビュー #4）試験だけの config で live の tree に openssh・aat-input・keiland-shot を足すのを U24 の下で許すか | A: 許す（`plan/ws307/tests/` だけ、unit・index・CI には入れない） | 2026-10-28「許す (推し)」→ A（§11.2） |
| J5 | （J1 の回答から）live の password を非公開にする方法 | A: build ごとの乱数の password（平文は捨てる）、root は lock、live の session は lock しない、sudo・su は使えない | 2026-10-28「規定値を作ります。非公開である必要はないです。インストーライメージを運用に使ってはいけないというルールにして、このイメージでの起動は規定ユーザでの起動ではあるが、root起動相当、という扱いにします。インストーラがrootで動くことは一般的であり、セキュリティ上の懸念は存在しません。」→ A は採らない。live の password は公開の既定の値（`user`、T24）、J1 は取り消し。installer の image を運用に使ってはいけない規則（S1・S11・文書、T25）、起動は root 起動相当（§9.1、§17.4） |

## 16. 担当が決めた技術の判断（理由）

| # | 判断 | 理由 |
| --- | --- | --- |
| T1 | 全てのキャンセルは確認の後に S10 へ。何も書いていなければ S10 は「disk は変えていません」 | 原文「progressだけでなく、キャンセルや失敗時にここに飛ぶ」に合わせる（1 回目のレビュー #12、2 回目 #15） |
| T2 | 「loopback swapfile」を UFS の上の file を extent で使う今の方式で満たす | kernel は file の swap を loop device を通さず extent で直に扱う（`src/kern/vm/swap-source.c:145-200`）。loop を足す利点が無い |
| T3 | tarball は ustar＋deflate（gzip の形） | guest に zstd・xz が無く、deflate は base の `libz-compat` にある。外部の圧縮の library を base に入れない（設計方針 §2.1） |
| T4 | root・swap は PARTUUID、boot0 は loader の自動の `boot0=UUID=`（mkfs の乱数の serial） | PARTLABEL は live の USB と重なりうる（`block-identity.c:184-190`）。`boot0=PARTUUID=` は kernel の boot の disk の待ちを無くす（`src/kern/platform/pcat.c:147`・`:490-541`）。FAT の serial を乱数にすれば UUID の重なりは実用上起きない（design-reviewer の指摘 5） |
| T5 | loader と `zedbsd.cfg` を最後に書く | 途中で止まった導入先が起動できるように見えない（§8） |
| T6 | helper は diskpart・mkfs・mkswap を子として起動し、確認の phrase を送る | 文書の決めた使い方（`docs/reference/block-administration.md` の diskpart init、image-formatters の block-device modes）。format の code を二重に持たない |
| T7 | （取り下げ。4Kn は U17 でユーザーに聞く） | design-reviewer の指摘 23: 原文の USB Mass Storage を狭めるので技術の判断でない |
| T8 | 既存の ESP の空きは helper が読み取り専用で mount して量る | installer は device を開かない（§3）。FAT の空きは fs を読まないと分からない |
| T9 | 中止は stdin の `cancel` の行 | set-user-ID の process への signal の可否に頼らない。EOF も中止 |
| T10 | 展開の元は build の unit で、live の root を写さない | live の system の状態（log・設定・利用者の file）を導入先に持ち込まない。旧 zedinst も live の upper を写さなかった |
| T12 | （取り下げ。U26 で installer の image は password 無し） | — |
| T13 | 「OpenSSL（＋CA 証明書）」は 1 つの項目のまま、既定で選択 | 原文が 1 項目。U8 の回答の既定の名前（OpenSSL・ca-certificates）はどちらも選択になり食い違わない |
| T14 | 導入先の OpenSSH は `PermitRootLogin no` | 入れた system が network から root で login されないように。root は利用者から `su`・`sudo` で使う |
| T15 | 新しい partition の先頭と末尾の 1 MiB を 0 で埋める | 前の FAT・`zedbsd.cfg` が残ると loader の候補になる（design-reviewer の指摘 4） |
| T16 | index の SHA-256 を install-admin に埋め込み、tarball は展開と同じ fd で hash を取る | `/opt/installer` は live の system で書き換えうる。照らしてから読み直すと間に書き換えられる（1 回目 #2、2 回目 #7） |
| T19 | 4096 byte の sector の disk の UFS は fragment 4096 の profile | driver の read-modify-write より単純で、loader の reader も換算だけで済む（2 回目 #8） |
| T17 | 1 つの BUILD で全部入りの tree を作り、manifest から unit と live の tree を作る | tree を 2 回 build せず、unit と live の system の file が同じ build から来る（指摘 8・9） |
| T18 | pkgconf の package を足す | U13 の「入れた system で GTK4 の app を build できる」に pkg-config が要り、guest に無い |
| T11 | 日本語の catalog も作る | Keiland の app は `kl_tr` で翻訳する（WS158）。install の画面だけ英語にする理由が無い |
| T20 | install-admin は `userland/desktop/installer/admin/`（installer の app と同じ directory の下） | J2 の回答で `userland/base` の外。installer の menu の 2 つの package を 1 つの所に置き、menu の外の build での error（§11.1）を 1 つの規則で当てる |
| T21 | loader は他の disk に同じ PARTUUID があれば止まる（3 回目のレビュー #9 の推しは警告） | kernel は全ての disk を探して EEXIST で root を mount しない（`block-identity.c:182-188`）。loader が先に理由を出して止まる方が、kernel の後の失敗より分かりやすい。隠れた fallback を作らない方針（§5.10 の 5）とも合う |
| T22 | loader の UFS の reader は native の byte order だけ、fragment が media の block より小さい volume を拒む | 3 回目のレビュー #10。導入先は installer が作る UFS だけで、両方とも満たす。reader を小さく保つ |
| T23 | kernel の更新の規則は「新しい名前 → fsync → rename → sync」、clean の印では警告しない | 3 回目のレビュー #8。clean の印は毎回の起動で無い（unmount の時だけ立つ） |
| T24 | installer の image の live の password は U26 の全 image の既定と同じ `user`（別の値にしない） | J5「規定値を作ります。非公開である必要はないです」。root 起動相当（J5）なので password は守りでなく、別の値にする理由が無い。同じ値なら live の tree は base の shadow をそのまま使え（p003 の live だけの上書きが autologin だけになる）、試験の道具（`aatlib.py` の `AAT_USER_PASSWORD` など）も全 image で同じ値で済む。base の既定が変われば live も従う |
| T25 | 「運用に使わない」規則は S1・S11・文書（p013）に出す。login の画面・毎回の通知には出さない | S1 は disk に書く前に必ず通る最初の画面、S11 は導入の後に使い続けうる時点、文書は image を配る時と dd する前に読まれる。毎回の通知は導入の邪魔になる |

## 17. design-reviewer の指摘と反映

### 17.1 1 回目（2026-10-28、opus、基点 082c46884 の案に対して）

| # | 重さ | 要点 | 反映 |
| --- | --- | --- | --- |
| 1 | blocker | 導入先に固定の root・kei の password が残る、sshd が root の login を許す | §7.2 段 7（必ず書き換え・確かめ）、T14。U7 の利用者の画面 |
| 2 | blocker | `/opt/installer` は書き換えうるので SHA-256 の照合が無意味、mode が group-writable | §6.1（index の hash を helper に埋め込む、mode の正規化、所有者 0:0、set-user-ID の許可の一覧）、T16。image の mode の不具合は Q1 へ報告 |
| 3 | major | 古い native の zedBSD（ZEDBSD の FAT の `zedbsd.cfg`）を乗っ取る | §4.3（disk のどの FAT に `zedbsd.cfg` があっても「partition を選ぶ」を無効）、§5.1、§12 |
| 4 | major | 新しい partition に古い FS が残る | §4.4・§7.1・§7.2（先頭と末尾の 1 MiB を 0 埋め）、T15 |
| 5 | major | `boot0=PARTUUID=` が boot の disk の待ちを無くす | §5.4・§7.3（mkfs の乱数の serial と自動の `boot0=UUID=`）、§11.3、T4 |
| 6 | major | Boot#### 無しの `\EFI\kei\` は起動できない | U5 の回答で Boot#### を範囲に（§5.8）、U15 |
| 7 | major | live の installer の image が dd した disk と曖昧で起動しない | §11.2（build ごとの PARTUUID）、§12、§13 |
| 8 | major | 今の rootfs の rule では unit に分けられない | §6.2（file の manifest・重なりの規則・除く menu）、T17 |
| 9 | major | 2 つの tree が要る | §6.2（1 つの BUILD、manifest から live の tree）、§11.2、T17 |
| 10 | major | 所有者・mode の manifest は無い | §6.1 |
| 11 | major | inode の数を見ていない | §5.6、§6.3、§11.3 |
| 12 | major | T1 が原文と違う | §4 共通・§4.10・T1（全てのキャンセルを S10 へ） |
| 13 | major | U3（独自の型）は「OS の ID 無し」と食い違う | U3 は選択肢の文「zedBSD 独自の未割り当ての型」でユーザーが選んだ。GPT は型 0 の使用中の entry を持てない事は問いに書いた。追加の確認はしない |
| 14 | major | U4 は WS238 の決定と違う | U4b でユーザーに聞き「U4 のまま」。§5.4 に明記 |
| 15 | major | diskpart が編集できない GPT | §4.3、§5.1、§12 |
| 16 | major | 既存の ESP が小さい disk で行き詰まる | U16 の回答で「partition を選ぶ」を無効、§4.3 |
| 17 | major | 段の間に disk を守れない、2 つの installer | §5.2（子の前の mount の確かめ直し、lock）、§7.2 段 2、§13（volumed の隠す要求は Future） |
| 18 | major | 段の途中で helper が死ぬ | §9.1（signal を無視し段の終わりまで） |
| 19 | major | 実装の Phase の表、BLKGETINFO の版 | ws.md の表を並べ直し、§11.3 |
| 20 | minor | `/boot/efi` で UEFI を判定できない | §4.1（boot provenance） |
| 21 | minor | AHCI の kind は IDE | §4.2 |
| 22 | minor | live の disk の除外が `/` だけ | §4.2 |
| 23 | minor | 4Kn を技術の判断で狭めた | U17 の回答で 4096 byte に対応（§5.9）、T7 を取り下げ |
| 24 | minor | slot と使用中の entry の混同 | §4.4 |
| 25 | minor | kernel・loader が unit の外で検査が無い | §6.1（boot の file を index に） |
| 26 | minor | 十分な ESP は loader の受け入れの規則で | §5.4 |
| 27 | minor | swap file の extent の数を知る口が無い | §5.5（試しの swapon・swapoff） |
| 28 | minor | mkswap の上限は INT32_MAX | §5.5・§11.3 |
| 29 | minor | 確認の phrase の作り方、新しい partition の識別 | §7.1・§7.2 段 4 |
| 30 | minor | account の core の path が固定、hostname | §7.2 段 7、§13 |
| 31 | minor | password の消去、失敗の待ち | §9.1 |
| 32 | minor | menu と platform の固定 | §11.1 |
| 33 | minor | 既定の image の大きさを変える | §11.2（別の target） |
| 34 | minor | fidoctl などで OpenSSL が必ず要る | §6.2 |
| 35 | minor | 展開の symlink・hard link・名前の長さ | §6.1 |
| 36 | minor | firmware の license | U18 の回答、§4.7 |
| 37 | minor | 導入の間の電源の操作 | §4.9、§13 |
| 38 | minor | 古い文書 | ws.md の p013 |
| 39 | minor | 試験の抜け | §12 |

### 17.2 2 回目（2026-10-28、opus、ea2d011cc の版に対して）

| # | 重さ | 要点 | 反映 |
| --- | --- | --- | --- |
| 1 | blocker | 書く直前に partition の範囲を確かめていない（slot の再使用、reload の失敗） | §7.4（4 つ組と `BLKGETINFO`・`BLKGETIDENTITY`・`live_extents_match` の照合） |
| 2 | major | 作成で表を書いて reload に失敗し S4 が行き止まり | §4.4（書く前に全ての partition の unmount と reload の成功を求める） |
| 3 | major | 選択が無効にならない、計画の partition の指し方 | §4.4（S4 で書いたら S5・S6・S8 を消す）、§4.8・§7.4・§9.2（4 つ組） |
| 4 | major | 既存の partition の古い PARTUUID・attribute | §5.3（PARTUUID を替え attribute を 0 に）、§11.3 |
| 5 | major | HAL の承認が確実に要る | §5.8（名前の追加と runtime の呼び出しは承認が要る、p007 の最初の段で案） |
| 6 | major | kei が helper を SIGKILL できる | §9.1（`setresuid(0,0,0)`・`setsid`・`clearenv`・rlimit） |
| 7 | major | hash の照合と展開の間の書き換え | §6.1（同じ fd で hash、index は memory に 1 回、`fstat` の確かめ、mode の正規化）、T16 |
| 8 | major | 4Kn は確実な kernel の作業 | §5.9、ws.md の p006、T19 |
| 9 | major | password の時期と live の password | 当時は §4.8（S8 で聞き直す）。後の U26 の回答で helper は password を聞かない形に置き換わり（§4.8・§9.1）、J1 で live の password を非公開にし、J5 で公開の既定・root 起動相当に置き換えた |
| 10 | major | lock の範囲 | §5.2（session が lock を持ち、probe は session に頼む） |
| 11 | major | 普通の image に set-user-ID の helper | §11.1（installer の image だけに入れる） |
| 12 | major | toolchain の許可、sysroot の観察の誤り | §6.2、ws.md の p003・p004 |
| 13 | minor | probe は分からない時に閉じる側へ | §5.1 |
| 14 | minor | ESP が複数 | §5.4 |
| 15 | minor | T1 と §8 の食い違い | §8・T1（全てのキャンセルを S10 へ） |
| 16 | minor | Phase の参照の誤り | HEAD で直し、全てを確かめ直した |
| 17 | minor | ESP の自動作成の失敗の巻き戻し | §4.4（batch の delete）、§11.3 |
| 18 | minor | 試しの swapon が live の system で | §5.5（time limit と止め方） |
| 19 | minor | 要求の行の利用者の文字 | §9.2（長さを前に付ける、password を log に出さない） |
| 20 | minor | session が disk に結び付かない | §5.2 |
| 21 | minor | 展開の mount の権限 | §7.2 段 5（nosuid・nodev、0700） |
| 22 | minor | kei の名前の変更ができない | §4.7a |
| 23 | minor | hostname | U27 |
| 24 | minor | 古い Boot####、UAPI の形 | §5.8 |
| 25 | minor | Phase の順（規約の見直しを最後に） | ws.md（実機の p012 を規約の見直しの p014 の前に、通しの試験の p011 の Boot#### の扱い） |
| 26 | minor | 共有の ESP の名前の衝突 | §5.4 |
| 27 | minor | 作成の既定の大きさ | §4.4 |

### 17.3 3 回目（2026-10-28、opus、2ac2eeaad の版に対して。全文 [design-review3.md](design-review3.md)。blocker 1・major 6・minor 10、ユーザーの判断 J1〜J4）

| # | 重さ | 要点 | 反映 |
| --- | --- | --- | --- |
| 1 | blocker | loader の FAT の探索は 512 byte の block だけ（`volume-discovery.c:854-862`・`:409-411`・`:446-452`）。4Kn の ESP は見つからず起動しない、受け持つ Phase が無い | §5.9（loader の FAT の 4096 を p008 に、kernel の FAT（p006）の後。OVMF の FAT の確かめ。4Kn を選べるのは p005・p006・p008 の後）、§5.4、ws.md の p008（依存に p005・p006） |
| 2 | major | index の hash を install-admin に埋めると build が循環、検査 4 が installer を含む tree と食い違う | §6.1、§6.2（build の順: 全部入りの tree（installer 無し）→ manifest → unit → index → install-admin → live の tree。検査 4 は全部入りの tree だけ） |
| 3 | major | package の選択は parse の時、`$(BUILD)/rootfs` が 1 つ、native の rule は VARIANT=native だけ。installer-image と hdd-image が書き換え合う、起動の設定の元が無い | §11.2 の「make の形」（再帰の make と `ZEDBSD_IMAGE_PROFILE=installer`、`$(BUILD)/installer/{full,live,units}`、vmunix・loader の共有、amd64＋native 以外と他の image の goal との併用は error、graphical の login・boot は入、kmsg・extra は menuconfig） |
| 4 | major | live の image に試験の手段が無い | J4 の回答で §11.2 の試験の hook、§12（導入先の起動の試験は自動 login を切る、中は OpenSSH の SSH） |
| 5 | major | install-admin を `userland/base` に置くのは Guardrail と食い違う | J2 の回答で `userland/desktop/installer/admin/`（§11.1、§9.1、T20） |
| 6 | major | 棚卸しが `plan/tools` を除く（285 行）、他の担当の kei の image が壊れる | §13a（`plan/tools` を入れる、p002 の前に道具が user → kei の順で試す、両方の image で回帰、Q1 が範囲を明示） |
| 7 | major | rev-0 の asset の名前と SHA-256 を固定している、名前を変えると取得が壊れる | J3 の回答で asset の名前も変える。§13a の移行の手順（新しい asset の upload はユーザーの指示で Q1、URL の変更は 1 つの commit、旧い asset は残す） |
| 8 | minor | clean の印は unmount の時だけ、警告が常に出る。j3 の home の書き込み | §5.10 の 6（警告を出さない、kernel の更新の規則、sync が home まで書くかを p008 で確かめる、j3 の重ね読みは Future）、T23、§13 |
| 9 | minor | 今の探索は SimpleFS の handle だけ、`HD()` の GUID は混ざった byte order、kernel は全 disk で EEXIST | §5.10 の 3（BlockIo の列挙、文字列 → binary の照合、他の disk の重なりで止まる）、T21 |
| 10 | minor | `fsbtodb` は 512 の単位、逆の byte order、cfg の `:` の拒み、共有の `logo-path.c` | §5.10 の 2（換算・範囲・穴・byte order の規則）と 4（`rootpart:` の切り出し、BIOS の loader の拒み方の試験）、T22 |
| 11 | minor | mode 4550 と 4555 の食い違い、wheel は gid 0、kernel は補助の group を見る | §9.1（4550・0:0、`getgroups()`・egid と自動 login の利用者の uid の確かめ） |
| 12 | minor | 手で書いた config.mk で普通の image に helper が入る | §11.1（SELECTABLE n、installer-image の外は error、hash の確かめは 2 つ目の守り） |
| 13 | minor | U26 の後の古い文（S8 の聞き直し、§17.2 #9、§15 U6、§4.11 の kei、ws.md の p009、§4.4） | §4.4・§4.11・§15 の U6・§17.2 の #9、ws.md の p009 を直した |
| 14 | minor | 利用者の group `kei:x:1000:` の名前の変更が無い | §4.7a、§13a |
| 15 | minor | `git grep -w` が `KEI_NIGHTLY_*` を落とす。GPU の protocol・mark・license の記録の分類 | §13a（`-i kei` から keiland を除く、J3 の物、残す物の一覧） |
| 16 | minor | Boot#### は os-release を読める。Linux・FreeBSD では os-release が host の物。`ZEDBSD_OS_NAME` の置き場と `ID` | §13a（make の断片を Makefile と keiland-linux.mk・keiland-freebsd.mk が include、`ZEDBSD_OS_ID`、system の説明は os-release・project の名前は build の定数、Boot#### は導入先の os-release） |
| 17 | minor | zedimage-host は既に PARTUUID を乱数で作る、image は memory に全部、16 GiB・65536 inode、検査の script | §11.2（違いは cfg が PARTUUID を名指すこと、2 段で量る、上限で失敗、`check-amd64-native-image.py` を p003 で） |

- レビューの「未確認」（reviewer の推測）: OVMF の FAT が 4096 byte の sector の FAT32 を読むか（§5.9、p008 の最初）、firmware が型の分からない GPT の partition に BlockIo の handle を作るか（§5.10 の 3、p008 の最初）、j3 の解放の追跡と home の書き込みの前の block の再使用（§5.10 の 6、p008）。

### 17.4 J5 の反映（2026-10-28、q1649、P4）

J5 の回答（原文は §15）で、J1「live の password を非公開に」を取り消して置き換えた。変えた所:

| 所 | 前（J1・J5 の推し A） | 後（J5 の回答） |
| --- | --- | --- |
| §2・§3・§4.4 | live の password は非公開 | 公開の既定の `user`、起動は root 起動相当 |
| §4.1 S1・§4.11 S11 | S1 に「普段の system として使わない」 | S1・S11 に「installer の image を運用に使ってはいけない」規則と理由（T25）。S11 の「閉じる」は導入の続きとして許す |
| §9.1 | build ごとの乱数の password、root は lock、live の session は lock しない（`--lock-idle=0`・sleep の後・System Menu の lock・log out）、sudo・su は使えない。危険は U26・J1 の帰結として受け入れ | live は base の既定の `user`/`user`（T24）、root の行は release と同じ。lock・log out・sudo・su は普通の image と同じ。起動は root 起動相当で、helper の password 無しの root の操作はその前提で受け入れる。守りの境界は helper を普通の image に入れない事と運用に使わない規則。配布の live に network の login を足さない前提 |
| §11.2 | live だけの上書きに乱数の password の shadow と lock をしない設定 | live だけの上書きは autologin だけ |
| §12 | — | QEMU の GUI の試験で S1・S11 の規則の文を確かめる |
| §13 | Future に「helper の呼び出し元を installer の process に絞る」 | 外した（J5 の前提で要らない） |
| §13a・§15・§16・§17.2 #9 | J1 の非公開 | J5 の形（T24・T25 を足した） |
| ws.md の p003・p010・p013 | p003 に乱数の password の shadow、p010 に live の session の lock をしない口 | p003 は autologin だけ、p010 は S1・S11 の規則の表示、p013 の文書に規則 |

## 18. 未決・残り

- 実装の Phase で決める技術の点: 「未割り当て」の型 GUID の値（p005）、mkfs の FAT の serial を既定で乱数にするか（p005）、UFS の inode の密度（p005）、`/dev/system` の EFI の変数の UAPI の形（p007）、HAL の差分の案（p007、ユーザーの承認）。
- toolchain の許可（p003・p004、Q1）。
- rev-0 への新しい nightly の asset の upload（ユーザーの指示で Q1、p002）と旧い asset を消すか（ユーザーの判断）。
- OVMF の 4Kn の FAT（p008 の最初）。読めなければ 4Kn の対応の範囲をユーザーに聞く。
