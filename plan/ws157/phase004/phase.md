<!-- awesome-plan project=zedbsd record=ws157-p004 -->

# ws157-p004: 写真の library・データベース・取り込み

Status: cleared（2026-10-07 Q1 の判定: T1-331 で photos.browse の fail が消え（2026-08.tsv の sea.jpg の行にお気に入りと回転）、timeline の PNG を Q1 が目視）（旧: in-progress（2026-10-07 q835 P2: 実装と host の試験 PASS。QEMU は p005 の T1 にまとめる、判定は Q1））
Disposition: normal
Parent: [WS157](../ws.md)
Queue: q835（2026-10-07、P2）
依存: [p001](../phase001/phase.md)（D1〜D7）

## 範囲（正常系）

`userland/desktop/photos/` の `photos.h`・`library.c`・`db.c`（旧 `store.c` を `git mv`）・`import.c`・`exif.c`。

## 実装（2026-10-07 P2）

- `library.c`: 写真（id・SHA-256・path（root ＋ 相対）・元の名前・大きさ・撮影日・取り込み日・幅・高さ・お気に入り・回転・変わったか）と album（id・名前・写真の id の
  sorted の一覧）を memory に。写真は新しい順、album は名前の順。一覧（Timeline・Favorites・album の写真）、id・hash で探す、album を作る（16 byte の乱数の id、
  tab・改行の名前は EINVAL）・写真を足す（同じ写真は 1 度）。
- `db.c`: `~/Pictures/Library/db/photos/YYYY-MM.tsv`（撮影の月ごと）と `db/albums/<id>.album`（1 album 1 file）を読む・書く。変わった月・album だけを一時 file と rename で書く。
  行の時刻は `YYYY-MM-DDTHH:MM:SS`。読めない行・path が `/` で始まる・`..` を含む行は飛ばす。
- `import.c`: file か folder（深さ 4、隠しは見ない、library の中は見ない）の JPEG・PNG・GIF を SHA-256 → 重複は数えて飛ばす → 撮影日（EXIF、無ければ mtime の地方時）→
  `img/YYYY/MM/DD/<元の名前>`（同じ名前があれば `名前-1.ext`…）へ複写（元は残す）→ 行（変わった印）。tab・改行の名前は取り込まない。
- id は SHA-256 の先頭 32 字。SHA-256 は libc の `<sha2.h>`。

## 確認（2026-10-07）

- host: `sh plan/ws157/tests/run-host-photos-db.sh` → PASS（29 の check、ASan・UBSan、TZ=UTC）: 時刻の文字列の往復、8 枚の取り込み（text・偽の JPEG・隠し・深さ 4 より下は入れない）、
  EXIF の日・file の日・深い folder の置き場所、複写の大きさ、元の file が残る、id、新しい順、2 度目は重複 8、同じ日の別の中身の beach.jpg は `beach-1.jpg`（元の名前は beach.jpg）、
  album の作成と追加（同じ写真は 1 度、tab の名前は EINVAL）、月の file・印の行・album の file、空の library への読み戻し（印・順・album・Favorites）、1 か月だけ変えると
  その月の file だけが書き直される（inode）、img に手で置いた file は一覧に出ない。
- style-check 0（photos.h・library.c・db.c・import.c・試験の C）。

## 2026-10-10 後続設計

ユーザーのメディア管理指示でDB取得/追加/metadata更新は後続[p006](../phase006/phase.md)のCLIとcompositor APIへ移す。本Phaseの過去のcleared結果は保存し、変更後の検証を[p007](../phase007/phase.md)へ依存させる。Photosの再起動/監視更新とPhone利用は新Phaseが検証する。
