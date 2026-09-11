# ws025-p003: RAM 専用 direct map と bootstrap

日付: 2026-09-07

Phase ID: `ws025-p003`

Status: completed; Queue q090, 2026-09-07. [results](results.md)

Parent: [WS025](../ws.md)

依存: ws025-p002

## 目的と境界

kernel image/MMIO から RAM 窓を分離し、全 RAM を map できる土台を作る。高位の一般貸出しはまだ有効化しない。

## 変更対象

- `src/hal/amd64/defs.h`
- `src/hal/amd64/space.c`
- `src/hal/amd64/space.h`
- `src/hal/amd64/cmain.c`
- `src/hal/amd64/bsp-pcat/cons.c`
- `src/hal/amd64/page.c`
- `src/hal/amd64/smp.c`
- `src/hal/amd64/ap-trampoline.S`
- `bootloader/uefi/elf64.c`
- `platform/amd64/vmunix.ld`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. memory-design.md §2 の仮想配置と image/direct/generic VA の変換を分離する。全変換 caller を分類し、kernel symbol に RAM base 減算を使わない。
2. early arena とその予約一覧を設ける。初期 map で触れる RAM から開始し、管理用 table を通常 heap/reclaim に頼らず作る。
3. RAM extent ごとに 2 MiB/4 KiB table を構築する。hole/MMIO/cache 属性混在を large page で跨がない。arena 拡張は実際に到達可能になった RAM だけを使う。
4. CR3 切替え後も image/stack/console/handoff が到達可能で、kernel text/rodata の別 alias が writable にならないことを確認する。AP 用 root は 4 GiB 未満に残す。
5. 全 process が kernel upper-half table を共有するよう修正する。shared table の free 禁止、TLB、AP identity の寿命を定義し、p004 へ必要な map/early allocation 情報を渡す。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- MEM06–MEM09。PA 1 GiB/4 GiB 境界、hole、窓上端、page 属性、image/direct round trip と初期 arena 不足を検証する。
- 低 RAM の BIOS/UEFI boot と SMP が従来どおり通る。高位 map の存在と通常 allocator 公開量を別々に表示する。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

map 構築不能は不足量と段階を記録する。到達性未確認の page を allocator へ先に公開しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
