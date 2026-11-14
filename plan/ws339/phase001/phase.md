<!-- awesome-plan project=zedbsd record=ws339-p001 -->

# ws339-p001: UEFI の loader の EDID による mode の選び方と試験

Status: in-progress（実装・host 試験・QEMU 済み。cleared は Q1）
Parent: [WS339](../ws.md)
Queue: q2119-i01（P5）

## 目的

amd64 の UEFI の loader（`bootloader/uefi/video.c`・`bootx64.c`）が、`video=` の無い起動で monitor の EDID の preferred timing の大きさを第一に GOP の mode を選ぶ。EDID が読めなければ今と同じ（logo の時 1920x1080 を望む、無ければ firmware の mode のまま）。

## 乖離の確かめ（2026-11-15）

- ws.md の `platform/amd64/zedbsd.cfg`・`zedbsd-native-uefi.cfg` は古い path。今の標準の設定は `mk/platform-x86_64/bootcfg/zedbsd-uefi.cfg`（UEFI）と `zedbsd-bios.cfg`（BIOS）で、どちらにも既に `video=` は無い（image.mk が `ZEDBSD_CONFIG_SRC` に使う）。
- `video=640x480` が残るのは `mk/platform-x86_64/bootcfg/zedbsd-loopback.cfg` だけ。make の規則・試験・docs のどこからも参照されない（grep）。
- bootloader/ に EDID の処理は無かった。`zbl_uefi_video_select` は video=、Ctrl の 640x480、logo の 1920x1080（縦の画面は 1080x1920）、今の mode の順。
- `docs/reference/kernel-boot-parameters.md` の「amd64 の zedbsd.cfg の video= の行」の記述は古かった（直した）。

## 設計

- 順: (1) `video=WIDTHxHEIGHT`（無ければ起動を止める、今と同じ）、(2) Ctrl を押した起動は 640x480 を望む（EDID は読まない。80 桁の console を読むための明示の操作なので）、(3) monitor の大きさ（EDID）に合う mode、(4) logo の時の 1920x1080（縦は 1080x1920）、(5) firmware の mode のまま。
- EDID の取得: `EFI_EDID_ACTIVE_PROTOCOL`、無ければ `EFI_EDID_DISCOVERED_PROTOCOL`（UEFI の仕様の GUID、`include/uefi.h`）。protocol を持つ handle を `LocateHandleBuffer(ByProtocol)` で列挙し、1 つならそれ（loader が使う GOP は ConSplitter の GOP で EDID を持たない事がある。OVMF の shell で GOP が 2 handle ある事を確かめた）、複数なら loader の GOP と同じ GOP を持つ handle、無ければ不明。size 0・NULL は無し。
- EDID の解析（純な関数 `zbl_edid_preferred_size`）: 128 byte 以上、header `00 FF FF FF FF FF FF 00`、128 byte の和が 256 の倍数（checksum）、version byte（0x12）が 1、0x36 の最初の 18 byte の descriptor の pixel clock が 0 でない（detailed timing）、interlace（byte 17 の bit 7）でない。幅 = byte 2 | (byte 4 の上 4 bit) << 8、高さ = byte 5 | (byte 7 の上 4 bit) << 8。EDID 1.3・1.4 とも最初の detailed timing が preferred timing。
- mode の選び方（`video_fit`）: 32 bit の RGB・BGR の linear な mode だけ。EDID の大きさと一致する mode があればそれ。無ければ monitor の大きさに収まる（幅・高さとも以下）mode の中で面積が最大の物、同じ面積なら monitor と同じ縦横比の物。切れて見えない事と、monitor の拡大を最小にする事を優先し、縦横比は同点の時だけ見る。収まる mode が無ければ (4) へ。
- `zbl_uefi_video_select` の引数に monitor の幅・高さを足した（呼ぶのは bootx64.c だけ。i386 の mk は video.c を挙げるが bootx32.c は無く build されない）。

## 試験

- host: `plan/ws339/tests/edid-test.c`（`cc -std=c11 -Wall -Wextra -Werror -I. plan/ws339/tests/edid-test.c bootloader/uefi/video.c`）。期待の値は EDID 1.4 の byte の並びと手の計算: 1920x1080@60（VIC 16）の block を byte ごとに書き、checksum を手で 0xF9（header 0xFF×6=1530、version 1・revision 4、descriptor 02 3A 80 18 71 38 2D 40 … 1E の和 520、計 2055 = 8×256+7、256−7=249）と計算し、生成の関数の出力と一致・decode が 1920x1080。2560x1440（下位 byte 0、上位 nibble だけ）、4095x4095、extension つき 256 byte、短い・NULL・checksum 違い・header 違い・version 2・display descriptor・interlace・幅 0 の拒否。偽の firmware で active 優先・空の active から discovered・1 handle（他の GOP の上）・2 handle で自分の GOP・2 handle で他人の GOP（不明）・EDID 無し・LocateHandleBuffer 無し、pool の解放の数。偽の GOP で一致・収まる最大（1680x1050 → 1280x800）・Blt only の除外（2560x1440 → 1920x1080）・同面積の縦横比（1920x1280 → 1200x800）・収まらない時の preferred・何も無い時の現状維持（SetMode を呼ばない）・video= の優先・無い video= の失敗。
- QEMU（OVMF、KVM、q35、NVMe、QEMU 名 p5-ws339）: `plan/ws339/tests/p001-qemu.sh OUT`（image を 2 つ作って boot-test.sh で起動し、login の画面の大きさを比べる）。display は `qemu-display.sh` が boot-test.sh の `-vga std` を置き換える。
- OVMF の EDID の protocol の有無: `plan/ws339/tests/p001-shell.sh OUT DISPLAY`（disk 無しで UEFI shell、`dh -p <GUID>` の画面を保存）。

## 結果（2026-11-15）

- host: `edid-test: PASS`（最後の行）。
- build: `make -j4 CONFIGMK=plan/ws339/tests/config-x86_64-edid.mk disk` と `…-edid-logo.mk disk` が rc=0、warning 0（loader と kernel を含む）。riscv64 の loader（同じ uefi.h）も -Werror で compile が通る。`config/config.x86_64.mk` の disk は amd-navi の firmware の license の確認（対話）で止まるので、試験の image は gnu-utils の base の config（firmware と外部の package 無し）で作った。
- OVMF（Debian の edk2、UEFI v2.70）: std VGA（edid=on）・bochs-display・virtio-vga・virtio-gpu-pci のどれも EDIDDiscovered・EDIDActive の handle は 0（shell が GUID をこの名前で示したので GUID は正しい）。GraphicsOutput は 2 handle（PCI の device と ConSplitter）。QemuVideo は EDID を自分で読み、その大きさで起動する（std VGA・bochs）。
- `p001-qemu.sh build/ws339-p001/qemu3` → 最後の行 `p001-qemu: PASS`、7 件とも login PASS:
  - 標準の cfg（logo 無し）: VGA edid 1600x900 → 1600x900（OVMF の初期の mode のまま）、VGA edid=off → 1280x800、virtio-vga → 1280x800、bochs edid 1600x900 → 1600x900。
  - logo あり: VGA edid 1600x900 → 1920x1080、VGA edid=off → 1920x1080、virtio-vga → 1920x1080。
  - 旧 loader（基点 1a97451059 の source から build した BOOTX64.EFI を image の複写に mcopy）でも VGA edid 1600x900・bochs edid 1600x900 は 1600x900（OVMF 自身が EDID の大きさで起動するため）。
- よって QEMU で確かめられたのは「EDID の protocol が無い firmware で今と同じ」だけ。EDID の protocol から大きさを選ぶ道は host 試験だけで、実機（GOP が EDID の protocol を出す物）は後の Phase。
- 別の所見（今回の変更と無関係）: VGA の EDID を 1500x1000 にすると OVMF は 1496x1000 で起動し、kernel の画面が 1 行ごとに約 4 pixel ずれて斜めに崩れ login が読めない。旧 loader でも同じ。未解析（stride の不一致の疑い）。

## zedbsd-loopback.cfg の案（mk の下の data file なので案だけ）

- 使われていない: make の規則・試験・docs のどれも参照しない。`overlay-root=`・`overlay-data=` の書き方は `docs/howto/boot-and-storage.md` に例がある。
- 推し: `git rm mk/platform-x86_64/bootcfg/zedbsd-loopback.cfg`（参照の無い雛形で、docs に同じ例がある）。残すなら `video=640x480` の行を消し、今の標準の cfg と同じ形（`kernel=vmunix`・`overlay-root=rootfs.img`・`overlay-data=data.img`・`swap0=swapfile`）にする。どちらもユーザーの判断（Q1 経由）。

## 未実施と残り

- 実機（5330）の受け入れ: 後の Phase（p002）。5330 の GOP が EDID の protocol を出すか、出せば内蔵の panel の preferred の大きさで splash と console が出る事、外部の monitor の時の handle の選び方。
- Ctrl を押した起動の QEMU の確かめ（key の時機が要る）は未実施。host 試験は loader の関数の単位までで、bootx64.c の分岐（Ctrl の時は EDID を読まない）は読んで確かめただけ。
- 1496x1000 の崩れの解析（範囲の外、Q1 へ報告済み）。
