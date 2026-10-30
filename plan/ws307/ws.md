<!-- awesome-plan project=zedbsd record=ws307 -->

# WS307 Keiland のアプリとしての zedBSD のグラフィカルなインストーラ

- Status: incomplete（2026-10-28 p001 cleared で実行が始まったので planned から incomplete に。仕様は spec.md で確定。実装 p002〜p014 はユーザーの指示の後）
- 仕様: [spec.md](spec.md)（確定。ユーザーの確認事項 U1〜U29・J1〜J5 は回答済み（J1 は J5 で置き換え）。design-reviewer の 1〜3 回目の指摘と J5 を反映済み、spec §17）。
- Resume point: 2026-10-29 q1665-i05（P4、worktree p4-ws307i）: p008-4k の FAIL の原因は FAT の identify（placeholder の label "NO NAME" で EINVAL、f553c82ba で直した）。再試験で p008-4k（4Kn の ESP＋UFS の root の起動）・p006-4k の NVMe と USB（UFS の上の swap file を含む）・512 の boot-test が PASS（[phase006/phase.md](phase006/phase.md)・[phase008/phase.md](phase008/phase.md)）。2026-10-29 Q1 の判定: p005・p006 cleared、p008 は 512 の j3 の sync の確かめを残して in-progress。q1665-i06（P4、worktree p4-ws307j）: p009（install-admin）: 段 1 の設計と段 2（index・unpack・plan・target）を host で確かめ、段 3 の probe と段 4 の main を書いた（target の compile が通る）。q1665-i07（P4、worktree p4-ws307k）: p009 の段 5 の session.c・段 6 の登録・段 7 の QEMU を済ませた。`p009-install.sh` PASS（NVMe 512・SATA・USB（選ぶ道）・4096 の導入、導入先だけの起動と SSH、512 の j3 の sync）、`p009-edit-cancel.sh` PASS、`diskpart batch` の既存の partition の delete の不具合を直して p005-batch PASS（[phase009/phase.md](phase009/phase.md)）。p009 は Q1 の判定待ち、p008 の残りの j3 の確かめも済み（[phase008/phase.md](phase008/phase.md)）。次: p010（installer の app）か p004（Q1 の割り当て）。p007 は HAL の差分の案（[proposed/hal-fw-variables.md](proposed/hal-fw-variables.md)）でユーザーの承認待ち。p002 の残りは phase002/phase.md。 p002（q1671-i01、P5）: compositor・H の段 2 の準備・試験まで済み。残り: asset の upload（ユーザーの指示）と upload の branch（agent/p5-nightly-upload、upload の前に merge しない、入れる時は cherry-pick）（phase002/phase.md）。p015（q1674、P5）: 画面の lock の既定を never。q1665-i08（P4、worktree p4-ws307l）: p009 cleared を記録。p010 の installer の app を実装し、QEMU で全画面を撮った（[phase010/phase.md](phase010/phase.md)、`build/ws307-test/p010s.liao5etg/shots/`）。p011（[phase011/phase.md](phase011/phase.md)）: 1 本の script `plan/ws307/tests/p011-install.py` を作ったが、試験の disk を付けた live の system が QEMU（Venus）で起動しない（BUG-356）ため通しは未達。BUG-356 の後に流し直す（p004 の分（GTK4 の app の build の確かめ）は後、2026-10-29 Q1 の判断）。 p015（q1674-i01、P5、worktree p5-ws307lock）: 画面の lock の既定を never に、never でも 10 分で lock された原因（compositor の固定の idle の lock）を直し QEMU で PASS、Q1 の判定で cleared（[phase015/phase.md](phase015/phase.md)）。sleep の既定（30・15 分）を Never にするかはユーザーの答え待ち。 q1688-i01（P4、2026-10-30、worktree p4-ws307n）: UFS の遅さ（[BUG-355](../bugs/BUG-355.md)・[BUG-358](../bugs/BUG-358.md)）を直した（6387b82a0: mkfs の native が tail journal を書かない、4096 の batched journal の sector の単位）。Q1 の判定待ち。q1665-i10（P4、同じ worktree）: p011 の `p011-install.py` が 12 回目で PASS。途中で installer の直しが 4 つあった（S4 の読めない大きさの log 67a3dd649、作った ESP の判定 01b12baa6、題名の訳 f703b3f28、訳の漏れ 4a587ed1f・c9662274b）。日本語の画面も PASS（`build/ws307-test/p010s-ja.7p9co8ca`）。p011 は Q1 の判定待ち。次: p012（実機）・p004・p007・p014 の残り（この WS で変えた C の見直し: ufs.c の包み、installer の flow.c・disk.c・main.c・screens.c）。 **q1698-i01（P4、2026-10-30、worktree `/home/awe/zedBSD-worktrees/p4-ws307o`、branch `agent/p4-ws307o`、基点 d3abf7c27、最後の SHA は phase の記録の commit の後の先頭）、09:13 に Q1 の指示でラップアップ**: p002: asset `zedbsd-nightly-base-winq-a10-1.zip` を rev-0 に upload（SHA-256 962f7926…cb149 を読み戻して一致）、f24bd97bf を cherry-pick（16e45a703）、新しい URL から make で取得・zip（Q1 の cleared の判定待ち、[phase002](phase002/phase.md)）。p015: sleep の既定を 0（211103282、host PASS）、QEMU は host の load で 2 回未達（[phase015](phase015/phase.md)）。p004: pkgconf の package、各 package の header・.pc・link、Clang の項目に devel-pkgconf、試験の script（2aad28963）。clang・libcxx・pkgconf の package は build 済み、gtk4 などは途中で止めた、installer-image と QEMU は未実施（[phase004](phase004/phase.md)）。p008: 512 の j3 の確かめ直しを p004 の試験に入れた（未実行）。p014: p011 以降の C を見直し 69f43f4c3。p007: HAL の案を取り下げ、driver の設計の下書き（3c14ef8f4、code 無し）。CI の別件 2 つ（ci.yml の pc98 の config の後の zedbsd-nightly-zip、release.yml の ALLOW_DRAFT=）は phase002。未 commit の物は無い（`plan/ws307/tests/__pycache__` は git に入れない）。次の 1 手: p004 の phase.md の 1〜3（installer-image を 1 本だけ build → host の pkgconf → `p004-gtk.sh`、p008 を兼ねる）、load の低い時に p015 の run3、その後 p007。
- Master: [plan/master.md](../master.md)。Primary Milestone: MG003。Related: MG006・MG007。
- 前身: [WS119](../ws119/ws.md)（インストーラの作り直し、2026-10-28 にこの WS の新設で canceled）。参考: [WS028](../ws028/ws.md)（canceled）、[WS019](../ws019/ws.md)・[WS062](../ws062/ws.md)（completed、disk image の構成）。

## ユーザーの指示と希望の仕様（原文、2026-10-28）

指示:「WSを新しく作成します。Keilandアプリとして、zedBSDのインストーラを新たに作成します。既存のインストーラ作成WSがあれば、それらはキャンセルを理由にcloseしておいてください。」「下記の仕様が私の希望なのですが、ブラッシュアップしたいです。まずは記録しておいてください。」

```
・グラフィカルなインストーラをKeilandアプリで作成する。インストーラはUEFI+ACPIなPC用で、他のプラットフォームは考慮しない。
・インストール先はNVMe, USB Mass Storage, SATA HDDなどのサポートされたディスクです。
・ウィザード形式で、次へで進める形式。画面は下記にします。
　・Welcome画面。
　・インストール先のディスク選択の画面。
　・ディスクのパーティションの扱いを選択する画面。ディスク全体を使う、パーティションを選ぶ、のどちらか。
　・パーティションを選ぶ場合に進む、簡易的なパーティション編集画面。パーティション一覧が表示される。選択されたパーティションを削除するボタンがある。パーティションを作成するボタンがある。パーティションの作成は、サイズのみ聞かれる。パーティションは未使用でマークされ、OSのIDはない状態で作成される。パーティションを作成するとき、EFIパーティションがなければ、自動で作成する。EFIを作成できなければパーティション作成も失敗する。次の画面に進むとき、EFIパーティションが存在することをバリデーションする。十分なサイズのEFIパーティションがないときは次の画面に進めない。
　・パーティションを選ぶ場合に、簡易的なパーティション編集画面のあとに表示される、rootfsパーティション選択画面。選択されたパーティションをUFSとマークして、mkfsして初期化する。
ィションがないときは次の画面に進めない。
　・パーティションを選ぶ場合に、rootfsパーティション選択画面のあとに表示される、swapパーティションの選択画面。選択されたパーティションをswapとマークして、mkswapして初期化する。swapパーティションは選択しなくてもいいが、確認はされる。swapパーティションがない場合、UFS上にloopback swapfileが作成される。
　・ここで、ディスク全体を使う、パーティションを選ぶ、の両方が合流する画面。インストールする項目の選択画面。選択項目は、Base System (ヘッダファイル込み）、Keiland Desktop、Firmware、Packages。Packagesには下位項目があり、Clang, OpenSSH, OpenSSL (+cacertificate), GTK4, Fonts, libavcodec, curlがある。依存ライブラリは自動選択で、libc++やcairoなども依存で入る。
　・最終確認の画面。
　・インストールのprogressの画面。
　・インストール失敗の画面。progressだけでなく、キャンセルや失敗時にここに飛ぶ。
　・インストール終了の画面。再起動ボタンがある。そのまま終了してデスクトップを使い続けられる。

base, desktop, firmwareはtarballにしておいて、/opt/installer以下に置く。packagesはuserland/packages/のパッケージ単位でtarballにしておき、選択項目＋依存関係のあるものをインストールする。
```

（原文の「ィションがないときは次の画面に進めない。」の行は、貼り付けの時の重複の断片と見られる。意味はユーザーのブラッシュアップの時に確かめる。）

## 目標（希望の仕様からの要約、Q1。正は上の原文）

UEFI＋ACPI の PC 向けに、Keiland のアプリとして動くウィザード形式のグラフィカルなインストーラを作る。対応する disk（NVMe・USB Mass Storage・SATA など）に、disk 全体か選んだ partition へ zedBSD を入れる。入れる物は base（header 込み）・Keiland Desktop・Firmware・Packages（clang・OpenSSH・OpenSSL＋CA 証明書・GTK4・Fonts・libavcodec・curl と依存）。base・desktop・firmware は tarball で `/opt/installer` の下、packages は `userland/packages/` の package ごとの tarball。

## 画面の流れ（原文の順）

1. Welcome
2. インストール先の disk の選択
3. partition の扱い: disk 全体を使う／partition を選ぶ
4. （選ぶ時）簡易の partition の編集: 一覧・削除・作成（大きさだけ、未使用の印・OS の ID 無し）。ESP が無ければ作成の時に自動で作る（作れなければ作成も失敗）。次へ進む時に十分な大きさの ESP があるかを検査
5. （選ぶ時）rootfs の partition の選択: UFS の印を付け mkfs
6. （選ぶ時）swap の partition の選択: swap の印を付け mkswap。選ばなくてもよいが確認する。無ければ UFS の上に loopback の swapfile
7. 入れる項目の選択（3 の両方の道が合流）
8. 最終確認
9. progress
10. 失敗（キャンセル・失敗の時もここ）
11. 終了: 再起動のボタン、または終えて desktop を使い続ける

## ブラッシュアップで決める点（Q1 の気付き、未決。ユーザーと詰める）

- 「disk 全体を使う」の時の partition の配置（ESP・root・swap の大きさ）と、既存の data の消去の確認。
- ESP の「十分な大きさ」の値、既存の ESP を共有する時（他の OS と共存）の loader の置き方と UEFI の boot entry の作り方。
- インストーラ自身の起動の形（live の USB の image か、インストール済みの desktop から起動するのか）と、`/opt/installer` の tarball の作り方（build の target、署名・検査）。
- package の依存の解決の元（userland/packages の Makefile の依存か、別の manifest か）と、入れた後の package の管理（WS153 の Apps の頁との関係）。
- 権限（インストーラは root の操作が要る: sessiond・passkey との関係）、ロケール（WS158 の翻訳）。
- 試験の方法（QEMU の OVMF＋NVMe・USB・SATA、実機）。

## ユーザーの決定（p001 の確認事項の回答、Q1 の中継。問いと選択肢は [spec.md](spec.md) §15）

- 2026-10-28 U1「menuconfig で選び自動で開く (推し)」: installer と `/opt/installer` は menuconfig の選択（配布用の config で y）。その image で起動すると desktop で installer が自動で開き、閉じても desktop を使える。
- 2026-10-28 U2「ボタンでその場で書く」: partition の編集の画面の削除・作成は、押した時にその場で disk に書く（キャンセルしても書いた分は残る）。
- 2026-10-28 U3「zedBSD 独自の未割り当ての型 (推し)」: 作成した partition は zedBSD 独自の「未割り当て」の型 GUID で書く。
- 2026-10-28 U4「ESP 512M＋swap 区画＋root (推し)」: disk 全体を使う時は ESP 512 MiB（loader・kernel・設定を ESP に）・swap の partition・残りを root。
- 2026-10-28 U5「\EFI\BOOT\BOOTX64.EFIがなければそこに置く。あれば、上書きせず、\EFI\ZEDBSD\BOOTX64.EFIに置いて、完了の画面で、Frimware のboot menuで選ぶ、と出す。Boot####の作成もWSに入れる。」: UEFI の runtime services（SetVariable）と Boot#### の作成も WS307 の範囲。HAL の API に触る場合は、その差分の案を Phase に置き、別にユーザーの承認を得る（Q1 の補い）。
- 2026-10-28 U6「setuid の helper、自分で起動 (推し)」: 新しい setuid root の helper を installer が自分で起動する（compositor を通さない）。
- 2026-10-28 U7「「利用者」の画面を足す (推し)」: 最終確認の前に利用者の画面（利用者の名前・password、root の password、自動 login）。
- 2026-10-28 U8「Base必須、DesktopとFirmwareは規定で、外そうとすると警告、PackagesはOpenSSL, OpenSSH, ca-certificates, Fontsのみ規定。Pythonのパッケージも追記しておいてください。」: Packages の下位に Python を足す。
- 2026-10-28 U9「今の口で出せる物だけ」: disk の一覧に型番を出さない（kernel の作業を入れない）。
- 2026-10-28 U10「GPT の disk だけ」: 空・MBR の disk は「disk 全体を使う」だけ。
- 2026-10-28 U11「rootfs 未選択なら進めない (推し)」。
- 2026-10-28 U12「ESP 以外の既存も選べる」: rootfs・swap に ESP 以外の既存の partition も選べる（選んだ時に「中身を消す」確認）。
- 2026-10-28 U13「WS307 で header も入れる」: GTK4・cairo・pango・glib・libavcodec・libc++ などの package に header と .pc を入れ、入れた system で GTK4 の app を build できるようにする。
- 2026-10-28 U14「最終確認の後 (推し)」: root・swap の型の印と mkfs・mkswap は最終確認の後。
- 2026-10-28 U15「末尾に足します。また、KeiではなくzedBSDです。Keiの名前はプロジェクトから削除予定です。」: Boot#### は BootOrder の末尾、名前は「zedBSD」。spec と UI の文言で「Kei」の名を使わない。
- 2026-10-28 U4b「U4 のまま ESP に全部」: WS238 の決定（今の image は ESP に loader だけ、kernel・設定・logo は ZEDBSD の FAT32）との違いを示した上で、導入先は ESP 512 MiB に全部。導入先は今の image の形と違う。
- 2026-10-28 U16「その disk では無効にする (推し)」: 既存の ESP が小さい・使えない disk では「partition を選ぶ」を無効にする。
- 2026-10-28 U17「WS307 で 4096 byte に対応」: mkfs（FAT32・UFS）・mkswap・diskpart（と要れば kernel の FAT・UFS・swap）の 4096 byte の sector の対応を WS307 に含める。
- 2026-10-28 U18「一覧を出し同意で入れる (推し)」: Firmware の license の一覧を S7 で出し、同意しないと入れない。
- 2026-10-28 追加の指示「P4に、ESPにカーネルをインストールすると回答してしまいましたが、ブートローダがUFSからカーネルをロードできるようにしたいです。そのphaseを作ってほしいです。」: loader が root の UFS から kernel を読む Phase（p007）を足す。導入先の kernel は UFS の上（`/boot/vmunix`）、ESP は loader（と cfg）。U4・U4b の「ESP に全部」は kernel を除いた形に読み替える（ESP 512 MiB は U4 のまま、Q1 の解釈）。
- 2026-10-28 U19「ESP（FAT）のまま (推し)」: `zedbsd.cfg` は ESP。loader の探し方を変えない。
- 2026-10-28 U20「root の UFS（/boot/logo.ppm） (推し)」: 起動の logo は root の UFS。
- 2026-10-28 U21「image は今のまま (推し)」: 今の disk image（hdd-image・installer の image）は WS238 の形のまま。新しい形は導入先だけ。
- 2026-10-28 指示「P4に伝えて下さい。make menuconfigは、インストーラ作成の場合、カーネルのコンフィグには使いますが、make installer-imageでは、すべてのパッケージをビルドしますし、イメージの形式（kernelのパーティション）が異なります。」: installer の image は専用の target `make installer-image`。menuconfig は kernel の config にだけ使い、userland・package は全部を build して unit にする。image の形は `hdd-image.img` と違う。U1 の「menuconfig で選ぶ」はこの形に読み替える（自動で開くのは残る）。U21 の「image は今のまま」は `hdd-image.img` の事（Q1 の解釈）。
- 2026-10-28 U22「9 項目＋「その他」で個別 (推し)」: 全 package を unit にし、S7 は 9 項目と「その他」の下位に他の package を個別に。
- 2026-10-28 U23「インストーライメージでは、ESPにブートローダ、BOOT FAT32にカーネルとzedbsd.cfg、rootfs, swapのパーティションを置きます。」: installer の image は ESP（loader）＋BOOT の FAT32（kernel・cfg）＋root＋swap。
- 2026-10-28 U24「FW は driver の選択、tests・X11 無し (推し)」: Firmware は kernel の driver が選ぶ firmware。tests・X11 の package は入れない。
- 2026-10-28 U25「今の hdd-image と同じ (推し)」: installer の image の BOOT の FAT32 は Basic Data の型・label「ZEDBSD」、logo.ppm もその FAT32。
- 2026-10-28 U26「すべてのイメージで、ユーザ名をuser, パスワードをuserに変更します。インストーラはsetuidのヘルパーを使うので、パスワード認証を不要にします。これは特別なインストーライメージのみです。」: 全 image の既定の利用者を user/user に（p002）。installer の image では helper は password を聞かない（呼べる者の制限は spec §9.1）。
- 2026-10-28 U27「今後keiの名前は削除しますので、zedbsdにします。」: 導入先の hostname の既定は zedbsd（S7a の欄は技術の判断で足す）。base の既定の hostname も zedbsd に（p002）。
- 2026-10-28 指示「P4に伝えてください。起動ロゴ画像を含めて、KeiというOS名を削除して、zedBSDに統一します。インストーラだけでなくすべてに影響します。そういうphaseをこのWSに追加してください。」: p002 を足した（spec §13a）。git mv・大量の rename は Phase の中で Q1 に一覧を出してから（Q1 の補い）。
- 2026-10-28 U28「Keilandの名前はそのままです。libkeilandもそのままです。KeiというOS名は、予定ではzedBSDに変更ですが、OSディストリビューション名が変更になる可能性があります。zedBSDはカーネル名です。OS名にするかは未定ですが、現状ではOS名もzedBSDに仮決めしておきます。libkeiland以外にlibkei*があるなら、変更を検討します。」: Keiland・libkeiland と内部名は残す。OS 名は zedBSD に仮決めし、1 か所（`ZEDBSD_OS_NAME` → os-release）で定義する。libkeiland の系の外の libkei* は無い（旧名 libkeiui は統合済み、spec §13a）。
- 2026-10-28 U29「推し」: 担当が今の logo と同じ様式で「zedBSD」の文字の画像を作り、PNG を Q1 経由でユーザーに見せてから入れる。
- 2026-10-28 J1「live の password を非公開に」（3 回目のレビューの推し A「受け入れ、理由の文を直す」と違う）: installer の image の live の利用者の password を公開しない。U26 の「user/user」は installer の image では password が変わる（名前は user のまま）。helper の password 無し（U26）は残る。→ J5 で取り消し・置き換え（下）。
- 2026-10-28 J2「userland/base の外へ移す (推し)」: install-admin は `userland/desktop/installer/admin/`（spec §11.1）。
- 2026-10-28 J3「これらも変える」（推し「残す」と違う）: GPU の command の protocol の名前（UAPI の識別子に Kei を含む物は今無い、comment の名前）、mark の絵（U29 と同じ手順でユーザーに見せる）と Files の文字、license の記録の文、GitHub の release の asset の名前も zedBSD に変える。asset の名前を変えると取得が壊れるので、取得の道（CI・script）を同じ Phase で直し、移行の手順を p002 に（Q1 の中継、spec §13a）。
- 2026-10-28 J4「許す (推し)」: `plan/ws307/tests/` の config だけで live の tree に openssh・aat-input・keiland-shot を足す（unit・index・CI には入れない、spec §11.2）。
- 2026-10-28 J5「規定値を作ります。非公開である必要はないです。インストーライメージを運用に使ってはいけないというルールにして、このイメージでの起動は規定ユーザでの起動ではあるが、root起動相当、という扱いにします。インストーラがrootで動くことは一般的であり、セキュリティ上の懸念は存在しません。」（推し A「build ごとの乱数の password」は採らない）: J1 は取り消し。installer の image の live の利用者は公開の既定の `user`/`user`（U26 と同じ値、spec T24）。「installer の image を運用に使ってはいけない」規則を S1・S11・文書（p013）に示す（spec T25）。この image での起動は root 起動相当と扱い、helper の password 無しの root の操作はその前提で受け入れる。乱数の password と live の session の lock をしない設定は外した（spec §9.1・§17.4）。

- 2026-10-29 HAL の案（p007）へのユーザーの回答（原文、Q1 の中継）:「UEFIに限らないファームウェア環境変数という概念で作り直してみてください。もう一度レビューして承認します。」→ UEFI に限らない firmware の環境変数の抽象で案を作り直し、design-reviewer を通して Q1 に送る（[proposed/hal-fw-variables.md](proposed/hal-fw-variables.md)）。
- 2026-10-30 p007 の値の形（原文）:「ご確認の、バイト列にするのはOKです。」→ kernel の firmware の変数の driver の口は「名前は文字列、値は byte の列」の汎用の形。Boot####（EFI_LOAD_OPTION）・BootOrder の組み立てと読み解きは userland の efiboot。UEFI の system table と memory map は既存の HAL の口 `hal_get_arch_handoff`（新しい名前）で受け取り、HAL の API は変えない（Q1 の設計、2026-10-30）。
- 2026-10-30 ユーザーの回答（原文）:「GitHub releaseのアップロードはご提案の通り許可します。」「clangとlibcxxのビルドは許可しますが、導入先というのはどういう意味なのかわかりません。zedBSDホストでのセルフビルドのことですか？」「実機への導入はどこかのリリースのUATで行うので、WSの完了条件に入れなくていいです。」「sleep の既定もneverにします。」
  - 解釈（Q1）: p002 の nightly の asset の upload と、名前を変える f24bd97bf の cherry-pick を行う。p004 は toolchain（`lang/clang`・`devel/libcxx` の package）の build を許可、「導入先」= インストーラで入れた zedBSD の上（zedBSD の host の self build）で GTK4 の app を build する確かめ（ユーザーに確認の返事）。p012（実機への導入）は WS307 の完了の条件から外し、release の UAT へ（canceled）。p015 の sleep の既定（30・15 分）も Never に。
- 2026-10-30 HAL の案（p007、改訂 2）へのユーザーの回答（原文）:「WS307ですが、ファームウェアの環境変数は文字列でいいのではないかと思ったのと、HALでアクセスする必要性が感じられませんでした。UEFIの起動エントリーは、UEFIランタイムサービスを経由すると思うのですが、それはカーネルのドライバで実装するものではないでしょうか。カーネルにファームウェア変数書き込みのドライバを作るのがいいと思います。」→ HAL の差分の案（proposed/hal-fw-variables.md）は取り下げ、kernel の driver で UEFI の runtime services を呼ぶ形に作り直す（Q1 の解釈、値の形はユーザーに確認中）。
- 2026-10-29 形式の識別子（原文、ユーザーのクリック、Q1 の中継）:「全部置き換え」→ `/KeiNotesImage`・`kei-notes.bin`・`application/x-kei-notes`・libpdf の `Kei` の接頭辞・`~/.config/kei/ime` を zedBSD の名に置き換え、古い名は読まない（移行の code は書かない、Q1 の中継の解釈）。
- 2026-10-29 F（U29・J3 の案へのユーザーの回答、原文、クリック、Q1 の中継）: mark「案1にします。このマークは尖石遺跡の縄文時代の子孫繁栄の神様の石の棒や石の舟のご神体をモチーフにしています。」、字体「b Lato Light＋tagline」→ mark は今のまま（mark.c は描き直さない）、「zedBSD」は Lato Light。案 b の絵の tagline は「operating system」（字間を空けた Lato Regular）で、P4 はその絵のとおりに入れた。Q1 の中継の文の「tagline「powered by zedBSD」を残す」は Q1 の誤りで、絵の「operating system」が正しい（2026-10-29 Q1）。Lato の OFL の記録を絵の出典（`userland/desktop/artwork/README.md`）に。
- 2026-10-29 ユーザーの要望（原文、Q1 の中継）:「インストーラはスクリーンショットをぜひ見せてほしいです。楽しみです。」→ p010 の installer の app ができたら S1〜S11・S7a の各画面を QEMU の screendump で撮り、`build/` の下に置いて path を Q1 に送る（Q1 がユーザーに見せる）。p011 の AAT でも各画面の PNG を残す（Q1 の中継の解釈）。
- 2026-10-29 Q1 の判断（q1665-i02）: 試験の data の SSID `Kei Lab` と mail の試験の人名 `Kei`・`kei@example.net` は OS 名でないので残す（p002 の「残す物の一覧」）。
- 2026-10-29 Q1 の判断（q1665）: p002 の git mv の一覧（`tools/release/kei-nightly*` の 4 件と `kei-boot-splash.png`）は OK（splash の中身は F の新しい絵をユーザーに見せた後）。`docs/release/zedbsd-1.0.0-beta2*.md` は履歴として残す。compositor の 4 file は最後に小さく（P1 に知らせてから）。nightly の asset の upload はユーザーの指示まで止め、URL を変える commit は main に merge しない印を付ける。

## 制限

- loader（p008）: cfg に `video=640x480` がある時、root の partition の失敗（`A64 KERN UFS …`）の文字が画面に出ない（debug port には出て、loader は `halt()` で止まっている）。導入先の cfg は `video=` を持たないので、その形では出る。原因（GOP の mode を変えた後の firmware の console の行の位置と見られる）は未確定（2026-10-29、P4）。
- swap file（p009、2026-10-29 P4）: installer が UFS の上に作る 2 GiB の swap file の 0 書きが QEMU（KVM、SATA）で約 1 MB/s（約 30 分）。下の native UFS の書き込みの遅さと同じと見られる（未確認）。p010・p011 の前に Q1 の判断。→ 原因は mkfs の native の tail journal（[BUG-355](../bugs/BUG-355.md)）。q1688-i01 で直した（6387b82a0、Q1 の判定待ち、QEMU だけ）。
- native UFS（p006）: 4096 byte の logical sector の disk では書き込みが 512 の disk より遅い。QEMU（KVM、NVMe、whole disk の native UFS、`plan/ws307/tests/p006-ufs-speed.sh`）で 16 MiB の dd が 512 で 23 秒、4096 で 63 秒（約 2.7 倍）。見立ては 512 byte の journal の record の read-modify-write（未確認）。正しさに関わらないので直していない（2026-10-29 Q1 の判断）。→ [BUG-358](../bugs/BUG-358.md)（2026-10-29 ユーザー「P4は、先にUFSが遅いバグを修正してください。…」）で直した（q1688-i01、6387b82a0。tail journal をやめ、4096 の batched journal の sector の単位の誤りを直した。Q1 の判定待ち、QEMU だけ）。

## Phase

| Phase | 内容 | 依存 | 状態 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 仕様の確定（調査、spec.md、ユーザーの確認事項、design-reviewer、実装の Phase の案） | — | cleared（2026-10-28、q1649、spec.md で確定） |

### 実装の Phase の案（p001 の成果、[spec.md](spec.md) による。ユーザーの回答で変わりうる。Phase の directory はまだ作らない）

| Phase | 内容 | 依存 | 見積もり | 状態 |
| --- | --- | --- | --- | --- |
| [p002](phase002/phase.md) | OS 名「Kei」を削除して zedBSD に統一する（tree 全体、起動の logo と mark の絵を含む。既定の利用者 user/user（U26、group の `kei:x:1000:` も）と hostname zedbsd（U27）も）。`ZEDBSD_OS_NAME`・`ZEDBSD_OS_ID` の make の断片（Linux・FreeBSD の Keiland の build も include）。J3 の物（GPU の protocol の名前、mark と Files の文字、license の記録の文、nightly の asset の名前と取得の道（mk・script・CI・release の title）と移行の手順）。棚卸しは `git grep -I -i kei` から keiland を除き、`plan/tools` を含む（Q1 が範囲を明示し、前に道具を user → kei の順に）→ 一覧を Q1 に出して git mv・rename の所有と衝突を調整 → 変更 → 試験（古い kei と新しい user の両方の image で道具の回帰）。新しい asset の upload はユーザーの指示で Q1（spec §13a） | p001 | 16 LW（upload 待ちを除く） | in-progress（q1665 P4・q1671 P5・q1698-i01 P4。A〜H を commit。2026-10-30 asset `zedbsd-nightly-base-winq-a10-1.zip` を rev-0 に upload（SHA-256 962f7926…cb149 を読み戻して一致）、f24bd97bf を cherry-pick（16e45a703）、新しい URL から make で取得。cleared は Q1 の判定、残りは push の後の CI の 1 回） | cleared（2026-10-30 Q1、main fbdd143a8。asset を rev-0 に upload し読み戻しの SHA-256 が一致、空の build/releases から新しい URL で取得して pin 一致。push の後の CI は未確かめ）。旧:  
| [p003](phase003/phase.md) | build: `make installer-image`（menuconfig は kernel の config だけ、userland・package は決まった選択で全部）、file の manifest（持ち主の package・mode）、重なりの規則、unit の tarball（ustar＋deflate、所有者 0:0、mode の正規化、set-user-ID の許可の一覧）、index（SHA-256・file の数・依存・項目・firmware の license）、manifest から mode を正規化した live の tree、installer の image（ESP＋ZEDBSD の FAT32＋root＋swap、cfg が build ごとの PARTUUID を名指す）、漏れと重複の検査（spec §6・§11.2）。make の形: 再帰の `$(MAKE) ZEDBSD_IMAGE_PROFILE=installer`、`$(BUILD)/installer/{full,live,units}`、build の順（全部入りの tree（installer 無し）→ manifest → unit → index → install-admin（`-D` の hash）→ live の tree）、amd64＋native 以外と他の image の goal との併用は error、installer の menu の package は SELECTABLE n で外の build は error。live だけの上書き（自動 login だけ。password は base の既定の user/user、J5）、試験の hook（J4）、root の大きさを 2 段で量り zedimage-host の上限（16 GiB・65536 inode）で失敗、`check-amd64-native-image.py` の installer の形。sysroot の持ち主の付け方は toolchain（`toolchain/llvm/sysroot.mk`）に触れるので Q1 の許可が要る | p001 | 16 LW | cleared（2026-10-29、q1665-i03、P4。`make installer-image` の全 build と QEMU（OVMF、USB）で login と `/opt/installer`・PARTUUID の root を確かめた。Q1 の判定） |
| [p004](phase004/phase.md) | package の header と `.pc`（gtk4・cairo・pango・glib・libavcodec・libcxx など）と pkgconf の package。導入先で GTK4 の app を build する確認（spec §6.2、U13）。`lang/clang`・`devel/libcxx` は toolchain なので Q1 の許可が要る | p003 | 8 LW | in-progress（q1698-i01、P4。pkgconf の package・各 package の header と .pc と link・試験の script を commit（2aad28963）。gtk4 の build・installer-image・QEMU は未実施、phase.md の「次の 1 手」） |
| [p005](phase005/phase.md) | base の道具: `mkswap` の partition の mode（ZEDSWAP2 64 bit）、`diskpart` の batch（add・delete、型・名前・PARTUUID・attribute の変更）と「未割り当て」の型 GUID、`mkfs -t fat32` の乱数の serial、UFS の inode の密度、道具の側の 4096 byte の sector（mkfs の FAT32・UFS の fragment 4096 の profile、mkswap、diskpart）（spec §5.9・§11.3）。host の試験と QEMU の短い確認 | p001 | 12 LW | cleared（2026-10-29、q1665-i05、P4。4096 の NVMe・USB の FAT・swap・UFS と 512 の回帰が QEMU で PASS。Q1 の判定） |
| [p006](phase006/phase.md) | kernel の 4096 byte の sector: FAT・UFS の driver、swap の partition、swap file の extent、fs の識別（spec §5.9、U17。`fat.c:911`・`ufs.c:60`・`swap-source.c:246`・`file.c:2920`）。QEMU の 4096 の NVMe・USB で mount・swap・起動 | p005 | 10 LW | cleared（2026-10-29、q1665-i05、P4。QEMU の 4096 の NVMe・USB で FAT・swap の partition と file・native UFS と UFS の上の swap file・host の読み戻し、512 の回帰が PASS。Q1 の判定） |
| [p007](phase007/phase.md) | UEFI の runtime services と Boot####（spec §5.8、U5・U15）。**最初の段で HAL の差分の案（handoff の名前の追加、runtime の呼び出しの関数）を plan に置き、ユーザーの承認を待つ**。承認の後: loader の handoff、HAL、`/dev/system` の UAPI（形・版・上限を先に決める）、`efiboot`。QEMU の OVMF で Boot#### を書いて読み戻す | p001 | 12 LW（承認待ちを除く） | in-progress（q1665、P4。2026-10-30 ユーザーの回答で HAL の差分の案 2 つは取り下げ、kernel の firmware 変数の driver（既存の `hal_get_arch_handoff` の新しい名前）で作り直す。設計の下書きは phase.md の改訂 3、code はまだ無い） |
| [p008](phase008/phase.md) | boot loader が UFS から kernel を読む（spec §5.10）: file の源の抽象、`bootloader/common/` の UFS の reader（換算・範囲・穴・byte order の規則）、BlockIo の handle の列挙と `rootpart=PARTUUID=` の binary の照合（他の disk の重なりで止まる）、`kernel=rootpart:/boot/vmunix`・`logo=rootpart:…`（cfg の parser の `rootpart:` の切り出し、共有の `logo-path.c` は変えず BIOS の loader の拒み方を試験）、失敗は画面に止める、sync が j3 の home まで書くかの確かめ。4096 の段（p005・p006 の後）: 最初に OVMF が 4Kn の FAT32 を読むかと BlockIo の handle を確かめ、loader の FAT の探索（`volume-discovery.c` の block と BPB の sector）を 4096 に（3 回目のレビューの blocker）。host の試験と QEMU の OVMF の起動（512、4096）。HAL の API・handoff は変えない | p001（4096 の段は p005・p006） | 14 LW | in-progress（q1665-i05、P4。512 と 4096 の段は QEMU で受け入れを満たした（4Kn の ESP＋UFS の root で login prompt、Q1 の判定）。残り: 512 の disk の j3 の sync の確かめ（p009 の段 7 の QEMU で）） |
| [p009](phase009/phase.md) | helper `install-admin`（`userland/desktop/installer/admin/`、J2）: probe・session（edit・install、lock、disk への結び付け。password は聞かない、U26）、mode 4550・0:0 と呼び出し元（wheel・自動 login の利用者の uid）の確かめ、4 つ組の照合、signal と `setresuid`、index の hash（同じ fd）、展開（安全な path、nosuid の mount）、利用者（既定の利用者と gid 1000 の group の置き換え）・hostname・sshd の設定、kernel と logo を root の /boot に、ESP の公開（loader・cfg）、Boot####（p007 の承認の後、名前は導入先の os-release）。host の敵対的な計画の試験と、QEMU の SSH での導入（NVMe・SATA・USB、導入先だけの起動）（spec §5・§7〜§9） | p003・p005・p008（4096 は p006、Boot#### は p007） | 18 LW | cleared（2026-10-29、q1665-i07、P4。QEMU の p009-install・p009-edit-cancel・p005-batch の回帰が PASS、Q1 の判定。実機は未実施。Boot#### は p007 の後）|
| [p010](phase010/phase.md) | installer の app: S1〜S11 と S7a、検査、確認の dialog、選択の無効化、翻訳（ja）、App Home、自動起動、AAT の log（spec §4・§10・§11.2）。S1・S11 に「installer の image を運用に使ってはいけない」規則と理由（J5、spec §9.1・T25。live の session の lock などは普通の image のまま）。できたら S1〜S11・S7a の各画面を QEMU の screendump で撮り `build/` の下の path を Q1 へ（2026-10-29 ユーザーの要望） | p009 の protocol | 15 LW | cleared（2026-10-29 Q1 の判定: QEMU の p010-screens.py PASS、全画面の PNG 21 枚、host の installer-model-test PASS。日本語の画面と実機は未実施） |
| [p011](phase011/phase.md) | 通しの試験: installer の image（試験の hook、J4）、AAT で GUI の導入（全体・選ぶ・共存・既存の partition を root に・swap 無し・キャンセル・失敗・拒む disk・dd した disk との同居・4096 の disk）、導入先の起動（自動 login を切った導入で boot-test.sh、中は OpenSSH の SSH）、`plan/ws307/tests/` の 1 本の script（spec §12）。AAT でも各画面の PNG を残す（2026-10-29 ユーザーの要望）。p007 の承認が無い間は Boot#### の確認を「未実施（承認待ち）」と記録 | p002・p004・p010（4096 は p006 と p008 の 4096 の段。p004 の分（GTK4 の app の build の確かめ）は p004 の後、2026-10-29 Q1 の判断） | 12 LW | cleared（2026-10-30 Q1 の判定。q1665-i10、P4。q1688 の UFS の直しの後、`p011-install.py` が 12 回目で PASS（25 STEP）、日本語の画面 `p010-screens.py --language ja` が PASS。Boot#### は p007・GTK4 は p004 の後で未実施） |
| p012 | 実機の導入（Q1 の割り当て、ユーザーの試験の一覧） | p011 | 2 LW | canceled（2026-10-30 ユーザー「実機への導入はどこかのリリースのUATで行うので、WSの完了条件に入れなくていいです。」） |
| [p013](phase013/phase.md) | 文書: `docs/howto/boot-and-storage.md` の古い zedinst の節を新しい installer に置き換え（「installer の image を運用に使ってはいけない」規則と root 起動相当の扱い、J5）、`docs/reference` に install-admin・efiboot・mkswap・diskpart・mkfs・loader の UFS の追加 | p012 | 3 LW | cleared（2026-10-29 Q1 の判定、q1665-i09、P4。how-to の置き換えと reference の installer.md・diskpart・mkfs・mkswap・loader の UFS。links 0 broken・anchor 0 error。手順を実機で辿るのは未実施） |
| p015 | 画面の lock（idle の後の lock）の既定を never にし、never でも実際には lock される不具合を直す（2026-10-29 ユーザー「WS307でついでに行ってほしいのですが、デスクトップがロックされるデフォルトの時間の設定を、neverをデフォルトにしてほしいです。また、neverにしても、現在は実際にはロックされてしまうみたいなので、直しておいてほしいです。」）。P5、q1674。[phase015/phase.md](phase015/phase.md): 原因は compositor の固定の 10 分の idle の lock（設定に従わない）。`power.lock`（既定 0 = never）と Settings の Power の Screen Lock の slider を足し、QEMU の試験 PASS（Q1 の判定待ち） | — | 3 LW | in-progress（lock は 2026-10-29 cleared。2026-10-30 ユーザー「sleep の既定もneverにします。」で q1698-i01 が sleep の既定を 0 に（211103282、host の試験 PASS）。QEMU は load で 2 回未達、phase.md の次の 1 手） |
| [p014](phase014/phase.md) | 規約の全文の見直し（WS307 が変えた C）。WS の最後 | p013・p015 | 5 LW | in-progress（q1665-i09、P4。4 つの conformance-reviewer で 167 file の WS307 の行を直し 27c89c707。build warning 0、基点との逆アセンブルの比較、host の 13 試験 PASS。q1698-i01: p011 以降の C（installer の 4 file・ufs.c の包み・ufs-format.c）を見直し screens.c を 69f43f4c3。Q1 の判定待ち。p007 の C は別に） |

- 2026-10-28 ユーザー「P4を立てて、WS307の仕様検討を行ってもらってください。ユーザ確認事項があれば回答させていただきます。仕様を確定させたいです。そのためのphaseをQ1が作り、P4に実行させてください。」
- 2026-10-28 ユーザー「P4に、ESPにカーネルをインストールすると回答してしまいましたが、ブートローダがUFSからカーネルをロードできるようにしたいです。そのphaseを作ってほしいです。」→ boot loader が UFS から kernel を読む Phase を足す（P4 が spec と Phase の表に、q1643）。
- 2026-10-28 U19「ESP（FAT）のまま (推し)」、U20「root の UFS（/boot/logo.ppm） (推し)」、U21「image は今のまま (推し)」（ユーザーのクリック、Q1 経由）。
- 2026-10-28 ユーザー「P4に伝えて下さい。make menuconfigは、インストーラ作成の場合、カーネルのコンフィグには使いますが、make installer-imageでは、すべてのパッケージをビルドしますし、イメージの形式（kernelのパーティション）が異なります。」→ installer の image は `make installer-image`（全 package を build、image の形は hdd-image.img と違う）。menuconfig は kernel の config だけ。
- 2026-10-28 U22「9 項目＋「その他」で個別 (推し)」、U23「インストーライメージでは、ESPにブートローダ、BOOT FAT32にカーネルとzedbsd.cfg、rootfs, swapのパーティションを置きます。」、U24「FW は driver の選択、tests・X11 無し (推し)」（ユーザー、Q1 経由）。
- 2026-10-28 U25「今の hdd-image と同じ (推し)」（ユーザー、Q1 経由）。
- 2026-10-28 U26「すべてのイメージで、ユーザ名をuser, パスワードをuserに変更します。インストーラはsetuidのヘルパーを使うので、パスワード認証を不要にします。これは特別なインストーライメージのみです。」、U27「今後keiの名前は削除しますので、zedbsdにします。」（ユーザー、Q1 経由。全体の決定は master-design-policy.md §11 にも）。
- 2026-10-28 ユーザー「P4に伝えてください。起動ロゴ画像を含めて、KeiというOS名を削除して、zedBSDに統一します。インストーラだけでなくすべてに影響します。そういうphaseをこのWSに追加してください。」 → WS307 に OS 名を zedBSD に統一する Phase（tree 全体、起動の logo を含む）を足す（P4、q1643）。
- U28: 2026-10-28 ユーザー「Keilandの名前はそのままです。libkeilandもそのままです。KeiというOS名は、予定ではzedBSDに変更ですが、OSディストリビューション名が変更になる可能性があります。zedBSDはカーネル名です。OS名にするかは未定ですが、現状ではOS名もzedBSDに仮決めしておきます。libkeiland以外にlibkei*があるなら、変更を検討します。」
- U29「推し」（担当が同じ様式で zedBSD の logo を作り、PNG をユーザーに見せてから入れる）。
- 2026-10-28 3 回目の design-reviewer の結果（[design-review3.md](design-review3.md)、blocker 1・major 6・minor 10）の J1〜J4 へのユーザーのクリック（原文）: J1「live の password を非公開に」（推しと違う: installer の image の live の利用者の password を公開しない。U26 の「user/user」は installer の image では変わる）、J2「userland/base の外へ移す (推し)」、J3「これらも変える」（推しと違う: GPU の protocol の UAPI の識別子・mark の絵・license の記録の文・GitHub の release の asset の名前も zedBSD に変える。取得の道も直す）、J4「許す (推し)」。
- 2026-10-28 q1648（P4）: 3 回目のレビューの 17 件と J1〜J4 を spec に反映（spec §17.3）。J1 の方法を J5 として Q1 に送った（推し A）。
- J5: 2026-10-28 ユーザー（J5 への回答、原文）「規定値を作ります。非公開である必要はないです。インストーライメージを運用に使ってはいけないというルールにして、このイメージでの起動は規定ユーザでの起動ではあるが、root起動相当、という扱いにします。インストーラがrootで動くことは一般的であり、セキュリティ上の懸念は存在しません。」 → J1 の「非公開」は取り消し。installer の image の live の利用者は既定の password（公開の既定値）。この image は運用に使ってはいけない規則とし、この image での起動は「既定の利用者の起動だが root 起動に相当」と扱う。installer（helper）が root の権限で password 無しに動く事は受け入れる。
- 2026-10-28 J5 の回答（原文は上の「ユーザーの決定」）。q1649（P4）: spec の §2・§3・§4.1・§4.4・§4.11・§9.1・§11.2・§12・§13・§13a・§15〜§18 と p003・p010・p013 の行に反映（spec §17.4）。
- 2026-10-28 Q1 の判定: p001 cleared（spec.md を確定。U1〜U29・J1〜J5 は回答済み、design-reviewer 3 回の指摘を反映。J5 の反映は J1 で入れた物を外す簡単化なので再レビューはしない）。実装の Queue（p002〜）はユーザーの指示の後。
