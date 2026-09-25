<!-- awesome-plan project=zedbsd record=guardrail -->

# Guardrail

zedBSD の貢献の規則と標準の索引。Queue・backlog・実行許可ではない。より新しく具体的なユーザーの決定が優先する。
規則の本文（エージェントの守ること）はリポジトリ直下の [AGENTS.md](../AGENTS.md) の「プロジェクトの規則」節にもある。

## 範囲と構造

- 作業は承認された Phase の範囲の中で行う。kernel（`src/`・`include/`）、bootloader、libc（`src/libc/`・`include/libc/`）、
  userland、platform、build/config、tools は所有が違う。file の追加や module の境界の変更の前に、現行のコードと Phase を確かめる。
- secondary queue は 2026-09-26 ユーザー指示で削除した（うまく実行できず、作業中のデータや成果は無い）。
- HAL: **API の変更（`include/hal/hal.h` の宣言・契約・HAL の責務）は具体的な差分ごとの事前承認**が要る。`src/hal/` の実装の変更
  （既存宣言の実装の修正・補完・最適化、arch 内部の header と struct）は承認なしで行ってよい
  （2026-09-25 ユーザー「HALの実装は勝手に修正してください。APIの変更のみ許可が必要です」。2026-09-12 の「HALの改変には許可が必要です」を
  この範囲に狭めた）。実装の変更は Phase の記録に差分の所在と検証を書く。hal.h に触れる差分はレビューできる形で plan に置き、承認まで適用しない。
  承認済みの差分は下の表。
- driver の配置と `drv_` の global symbol の方針を保つ。検証済みの refactor を古い試験の前提より信頼し、試験の側を直す。
- RTL8822B の `.inc` はライセンスを分けるために独立させており、**別の file のまま保つ**。
- kernel と libc（2026-09-23 ユーザー明確化）: kernel と libc はモノリシック。kernel・driver・HAL が include してよい libc の header は
  `libc/vulkan/*` だけ。ioctl・errno などの ABI は UAPI に分ける。kernel は標準 C の header 名を暗黙に読まず、libc の object を link
  しない（kcrt を使う）。`userland/base/libvulkan` は必須の構成要素。SPIR-V の compile は kernel 空間の driver が行う。この構成は変えない。
- kernel の実装を userland の build の依存へ写さない。`mkfs` などの tool は単独で使える形を保つ。
- base system の実装とライセンスの境界: [設計方針](master-design-policy.md)。
- 外部 package（`userland/packages/`）はソースツリーへ取り込まず、tarball を取得・検証して patch する。ライセンスは
  [provenance](ws032/provenance.md) と `plan/tools/packages/audit-licenses.sh` で監査する。
- WS は一つの具体的な到達目標を持つ（2026-09-12 ユーザー指示）。完了・終了した WS を再利用して別の目標を足さない。
  WS の終了時は子 Phase を全件照合し、未完了は完了にせず、指定の保留先か別の WS へ引き継いで元の Phase を閉じる。

## 標準と検証

- C のコーディング規約: [coding-style.md](coding-style.md)（全文が正本。簡約版は置いていない）。新しいコードには全文を適用する。
  規約の変更で評価順・所有・振る舞いを変えない。tool の対応: [standards/automation.md](standards/automation.md)。
- 集約の `make check` は走らせない。Phase に意味のある絞った確認を行う。
- build の関門: 選んだ platform の構成で `make -j16`（warning 0）。
- 起動の確認は `plan/tools/boot-test.sh`（画面の login prompt）。QEMU の console log・serial log を読んで判定しない。
  機能の回帰は guest を起動しない host の試験で行い、guest の操作はシリアル（`plan/tools/guest/serial.py`）か SSH
  （`plan/tools/guest/guest.sh`）で対話する。不具合の解析は QEMU の gdbstub・monitor・QMP で行う。詳細は [Master](master.md) の Tools 節。
- 回帰の範囲は Phase の性質で決める。コードの意味を変えない refactor は build（warning 0）と最後の boot test だけ。
- ユーザーの受け入れの範囲を守る。取り下げられた網羅的な異常系試験・繰り返しの実機起動・免除された実機関門を戻さない。
  QEMU の証拠と実機の証拠を分けて書く。

## 承認済みの HAL 差分

| 日付 | 範囲 | 出典 |
| --- | --- | --- |
| 2026-09-12 | amd64 の MMIO read/write accessor 8 個（hal.h・責務は不変） | WS014 p003（q306） |
| 2026-09-13 | amd64 の device mapping の補完（patch SHA256 `e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d`） | WS030 p002（q308）、[承認記録](https://github.com/awemorris/zedBSD/issues/390#issuecomment-5647471812) |
| 2026-09-23 | HAL 配下の `#include` path の変更と、kernel/HAL 共通の compile flag（`-nostdlibinc`・`-fno-builtin`）。宣言・実装・責務は不変 | WS035 p004・p023・p034・p035 |
| 2026-09-23 | `HAL_TIMER_FREQUENCY` を arch ごとに（`include/hal/arch/<arch>.h`）、pc98 の PIT 入力 clock を BIOS `0:0501` bit 7 で選ぶ | WS040 p001・p005 |
| 2026-09-23 | i386 の `hal_mmio_read8`/`hal_mmio_write8`（既存宣言の補完） | WS036 |
| 2026-09-24 | rpi4 の framebuffer の console の font を PC/AT の 8x16 の複製へ（`src/hal/arm64/bsp-rpi4/` への font の追加と描画の置換。hal.h は不変） | WS044 p001 |
| 2026-09-24 | rpi4 の framebuffer に描いた後（文字の枠・カーソル・初回のクリア）に data cache をメモリへ書き出す（`src/hal/arm64/bsp-rpi4/framebuffer.c`、`hal_dcache_clean_range` を使う。hal.h は不変）。実機で画面に出ない不具合の修正。差分 `plan/ws044/proposed/rpi4-framebuffer-cache.diff`、承認「承認、すぐ進める」 | WS044 p006 |
| 2026-09-24 | rpi4 の framebuffer の大きさを firmware の設定（`TAG_GET_PHYSICAL`、config.txt の 1920x1080）から取り、cache の書き出しの後に `dsb sy`（`src/hal/arm64/bsp-rpi4/framebuffer.c`。hal.h は不変）。差分 `plan/ws044/proposed/rpi4-framebuffer-size.diff`、承認「承認、すぐ進める」 | WS044 p007 |
| 2026-09-24 | rpi4 の起動の診断: ACT LED（GPIO42）の点滅で段階を示す `led.c`/`led.h`、framebuffer の 3 秒のテスト模様と値の表示、`locore.S` で `SCTLR_EL1` を確定した値にする（MMU・cache 無効、little-endian）。hal.h は不変。差分 `plan/ws044/proposed/rpi4-boot-diagnostics.diff`、承認「承認、すぐ進める」 | WS044 p008 |
| 2026-09-24 | arm64 の命令 cache の同期: user の page を実行可能に対応付けるとき（`hal_space_map`）と実行可能に変えるとき（`hal_space_prot`）に `hal_sync_instruction_stream`、MMU の有効化で EL0 に UCT・DZE・UCI・nTWI・nTWE（`src/hal/arm64/space.c`・`locore.S`。hal.h は不変）。実機の init の SIGILL の修正。差分 `plan/ws044/proposed/arm64-icache-sync.diff`、承認「承認、すぐ進める」 | WS044 p009 |
| 2026-09-25 | amd64 の `amd64_percpu_current()` を `rdmsr IA32_GS_BASE` から `%gs:0` の `self` の load に（`src/hal/amd64/percpu.c`、`percpu.h` に `self` が先頭の field である `_Static_assert`。hal.h は不変）。lock と `thread_current()` のたびの rdmsr が kernel の時間の約 24% だった。差分 `plan/ws046/phase009/hal-percpu-gs.md`、承認「承認待ちのHALの変更を許可します」 | WS046 p009 |

| 2026-09-25 | amd64 の page table の owner PTE に子 table の present entry の数を bit 52〜62 に持ち、`hal_space_unmap` の空 table の切り離しを全 table の走査（`detach_empty_tables`）から O(1) の判定に（`src/hal/amd64/space.c`。hal.h は不変）。差分 `plan/ws061/phase002/hal-amd64-table-counts.diff`。2026-09-25 の規則の変更（実装は承認不要）により適用 | WS061 p002 |

2026-09-25 以降、hal.h を変えない `src/hal/` の実装の変更は承認を要しない（上の規則）。hal.h の変更はこの表の承認が要る。

## 決定の出典

ユーザーの指示（HAL の制限と rollback の review、RTL8822B のライセンスの例外、refactor と試験の信頼、WS025 の規約の柔軟性、
インストーラの受け入れの範囲、QEMU だけの UAS の受け入れ）。新しい規則はこの索引、該当する規約の全文、tool の対応、影響する計画を
更新する。合意した構造や範囲を黙って置き換えない。
