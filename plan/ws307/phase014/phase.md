<!-- awesome-plan project=zedbsd record=ws307-p014 -->

# ws307-p014 規約の全文の見直し（WS307 が変えた C）

- 親: [WS307](../ws.md)。Queue: q1665 / q1665-i09（担当 P4、2026-10-29、worktree p4-ws307m、基点 main 3f2370248）。
- Status: in-progress（直しを commit し build と host の試験まで。q1698-i01 で p011 以降の C を見直した（69f43f4c3）。p007 の C の後に残り。Q1 の判定待ち）
- 範囲（Q1）: WS307 が変えた C を `plan/coding-style.md` の全文で見直し、書き方だけを直す（動作は変えない）。conformance-reviewer に頼んでよい。QEMU は使わない（BUG-356）。
- 依存: p013 は cleared。p015 は cleared。p011（BUG-356）・p012・p004・p007 の残りの code は、入った時に別に見直す（この Phase の後に WS307 が変える C）。

## 対象の洗い出し

- WS307 の branch（agent/p4-ws307・p4-ws307b〜l、agent/p5-ws307lock・agent/p5-ws307p002）ごとに、main の first-parent の歴史に入る前の自分の commit（merge を除く）が変えた `.c`・`.h` を集めた: 181 file（`plan/` の試験の C 14 を除くと 167）。各 file の WS307 の差分（`git show`）を reviewer に渡し、差分の行だけを見させた（全体が新規の file は全体）。
- 4 つの組に分けて conformance-reviewer で並行に: 1 installer の app（7 file）、2 install-admin の session・target、3 install-admin の main・index・plan・probe・unpack、4 base の道具（diskpart・mkfs・mkswap）・loader（ufs-read・root-config・bootx64・volume-discovery）・kernel（fat・ufs・disk・file・backing-claim・swap-source）・zedimage-host・p015 と p002 の小さな差分（約 90 file）。

## 直し（commit 27c89c707、54 file、+1859 −470）

- §6 条件の中の呼び出しを名前の付いた変数へ（短絡の順序を保つ形）、条件演算子を if へ、複数の条件を 1 つずつの if へ。
- §5 閉じ括弧の後の空行、段落の comment。§8 引数の中の入れ子の呼び出し、複数行の本体の括弧。§11 return の形。
- §10・AGENTS.md §10 開発の管理情報: comment の `spec 7.1` などの設計文書の節の番号（installer・admin の約 28 か所）と、WS307 が書き換えた行に残っていた `ws035-p108`・`ws101-p016`・`ws095-p017`・`ws089-p022`・`ws175-p006`・`design.md [M4]` など（約 25 行）を外した。
- 外から見える名前・式の値・文字列の出力・log の文字列・error の値は変えていない。target.c に file の中の `UNUSED_PARAMETER` を定義、unpack.c の static な局所変数の名前 1 つ（`inflate_status`）。
- style-check（新規の file）: installer の app 216 → 0、admin 238 → 11（`target_accounts` の 1 つの cleanup への goto、§14 で許可）、ufs-read・root-config 5 → 0、swap-device 13 → 5（goto out、同じ）。base の道具・kernel の WS307 の外の既存の違反は直していない。

## 確かめ

- build（target の clang、`-Werror`）: `make -k -j32 ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p009.mk BUILD=build/p014 <変えた userland の 36 個の .o>` rc 0、warning 0。install-admin の main.o は index の hash の header を仮の値にして同じ command で compile（warning 0、基点との code の比較も同じ方法で: code は違い data は同じ）。`make … build/p014/vmunix build/p014/uefi/BOOTX64.EFI build/zedimage-host` rc 0、warning 0、`amd64 vmunix check: PASS`。
- 動作が変わらないことの確かめ: 基点（766f5584b を `git archive` した `build/tmp/p014-base.*`）で同じ object・vmunix・BOOTX64.EFI を build し、逆アセンブルを比べた。
  - userland の 36 個のうち 21 個（comment だけの p002 の file、diskpart の table、installer の catalog・disk・helper、settings の page-about など）は code と data が同じ。残り 15 は条件を分けた所の code の並びが違い、data は文字列の並びだけ（flow.c の `" autologin %s"` が `" autologin yes"`・`" autologin no"` の 2 つの文字列に。送る要求の byte は同じ）。
  - vmunix: 関数ごとに address・即値・comment を除いて比べ、違う関数は fat.c・ufs.c・file.c の 8 つ（fat_identify・parse_bpb・collect_extent・ufs_identify・ufs_mount_impl・ufs_file_extents・j3_replay・quota_import_config）だけ。その差分を読み、分けた条件の return の値が元と同じことを確かめた。
- host の試験（基点と直した後の両方、scratchpad の 1 本の runner）: diskpart-table-test・mkswap-partition-test・mkfs-fat32-test・root-config-test・fat-bpb-4k-test・install-admin-plan-test・install-admin-unpack-test・installer-model-test（p4-ws307l の build の index）・ufs-read-test・mkfs-ufs-native-test・native-partuuid-test・installer-manifest-test・installer-units-test の 13 個が両方で PASS。
- `git diff --check` clean。追加の行に WS・Phase・plan/ の参照が無いことを grep で確かめた。
- 未実施: QEMU・実機（BUG-356 のため Q1 の指示で行わない。p011 の通しの試験でまとめて確かめる）。

## 直さなかった物・報告

- 1 段の `kl_tr("…")` を引数に入れた呼び出し（screens.c などの約 40 か所）は、翻訳の呼び出しの慣例として残した（reviewer の判断を担当が採った）。
- installer の flow.c・screens.c に短い一語の段落の comment が残る（checker の外）。
- reviewer 4 が見つけた既存の疑い（WS307 の行ではない）: `src/drivers/fs/ufs.c` の `j3_extents_read` が `return EINVAL;` で確保した `j3->extents` を解放せずに抜ける。触っていない。Q1 に報告した。

## q1698-i01（2026-10-30、P4、worktree p4-ws307o、基点 d3abf7c27）: p011 以降の WS307 の C

- 対象（27c89c707 の後に WS307 が変えた C、branch agent/p4-ws307n の自分の commit）: installer の `flow.c`（67a3dd649、S4 の拒みの log）・`disk.c`（01b12baa6、作った ESP の判定）・`main.c`（f703b3f28、題名の訳）・`screens.c`（4a587ed1f・c9662274b、訳の漏れ）、`src/drivers/fs/ufs.c` の cache の包み（6387b82a0: `ufs_buf_write_pinned`・`ufs_buf_unpin`・`ufs_buf_writeback_range`・`ufs_buf_writeback_cached`・`ufs_buf_prefetch` と呼び手の名前の置き換え）、`userland/base/mkfs/ufs-format.c`（6387b82a0、native の tail）。この Queue で変えた C（p015 の sleep の既定: `sleep-rules.h`・`look.c`・`settings-keys.c` の定数と comment、p004 の試験の `plan/ws307/tests/p004-hello.c`）も見た。
- 全文（`plan/coding-style.md`）と差分の行を照らした結果: 直したのは `screens.c` の 1 か所（69f43f4c3）: else の本体の段落に comment が無く、`strcmp` の結果の名前 `none_diff` が何を持つかを言わない → `none_differs` に、表の名前の段落と行の段落に comment。動作は同じ（同じ式・同じ文字列）。他は規約どおり（前方宣言 1 行、各段落の comment、分けた呼び出しは 1 引数 1 行の揃え、成功の return が最後で `Succeeded`、条件の中に呼び出し無し）。
- 確かめ: `make ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p009.mk ZEDBSD_IMAGE_PROFILE=installer BUILD=build/p014b build/p014b/dynamic/obj/userland/desktop/installer/screens.o` rc 0、warning 0。`plan/tools/style-check.py` は installer の 4 file で 0、ufs.c・ufs-format.c の新しい行の範囲で 0。`git diff --check` clean。QEMU は p004 の image の試験（installer の画面は通らない）の他は未実施。
- 残り: p007（UEFI の変数の driver・efiboot）の C は入った時に見直す。
