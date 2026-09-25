<!-- awesome-plan project=zedbsd record=ws046p010 -->

# ws046-p010: file の page の fault の複写を減らす（設計から）

Phase ID: `ws046-p010`
Parent: [WS046](../ws.md)
Status: cleared（q419-i01、2026-09-25）
Queue: q419（q419-i01）

## きっかけ

ws046-p009（q418-i01）: guest の configure の遅さの残りの原因の一つ。file を裏付けにした page の fault（`fill_file_page()`）は、buffer cache から VM object の page へ、
そこから process の private の page へ複写し、prefetch でも複写する。1 page 約 25 µs（Linux は 1〜2 µs）。clang・ld.lld の起動は数万の page を fault する。

## 目的

read-only（または書かれるまでの MAP_PRIVATE）の file の mapping で、VM object の page（page cache）をそのまま map し、書き込みで COW にする設計を作り、実装の Phase に分ける。
既存の exec の object cache（`vmspace_exec_cache_fault()`、`region->snapshot`・`page->object_page`）の仕組みを使えるか調べる。`VM_OBJECT_CACHE_OBJECTS`（32）の上限の意味も調べる。

## 受け入れ

- 設計（page の所有、COW、reclaim、共有の mapping と private の mapping の違い、truncate・write との一貫性）と試験の方法、実装の Phase の分け方。HAL の変更が要るかどうか。

## 結果（q419-i01、2026-09-25）: 設計

### 今の仕組み（`src/kern/vmspace.c`）

- `MAP_SHARED` の file の region は `region->object`（file の VM object、`vm_object_get_shared()`）を持ち、fault は `vm_object_fault()` で object の page（page cache）を**複写せずに** map する。
- exec の snapshot の region（`region->snapshot`、`vmspace_map_exec_snapshot()`）は、固定した object の page を read-only で map し、`VM_MAPPING_COW` を付け、書き込みの fault で
  `vmspace_exec_cache_fault()` が private の page に複写する（COW）。
- **`MAP_PRIVATE` の file の region**（動的 loader が共有 library を map する形）は object を持たず、fault ごとに `fill_file_page()` が file を読んで private の page に複写する。
  `libLLVM.so` の page ごとに buffer cache → object の page → private の page と複写し、1 page 約 25 µs。
- `mprotect` は `VM_MAPPING_COW` の page から書き込みの権限を除く（全ての region で共通）。fork は object の region の page を複写せず、子で遅れて fault させる。

### 設計: private の file の region も object の page を read-only で map し、書き込みで COW

1. `mmap` の `MAP_PRIVATE` の通常の file（`vm_object_get_shared()` が成功するもの）で、region に `object` を持たせ、新しい flag `VM_REGION_PRIVATE_OBJECT` を立てる。object を得られない file は今までどおり。
2. fault（page が無い、読み・実行）: region が `VM_REGION_PRIVATE_OBJECT` で、page が region の data の範囲に丸ごと入り（ELF の zero tail の page でない）、要求が書き込みでないなら、
   `vm_object_fault(region->object, offset)` の page を **書き込みの権限を除いて** map し、`VM_MAPPING_COW` を付ける。それ以外（書き込みの fault、data の端の page）は今までどおり `fill_file_page()`。
3. fault（object の page が map 済みで書き込み）: snapshot と同じく `vmspace_exec_cache_fault()` で private の page に複写する。この関数の再確認（`region->snapshot == NULL` なら EAGAIN）を private object の region にも広げる。
4. fork: private object の region は、object を参照してから page の loop に入る（object の page は子で遅れて fault、既に COW で private になった page は今の private の page と同じく COW で共有）。
   今の `continue`（page を全て飛ばす）は `MAP_SHARED` の region だけにする。
5. `msync`（`vm_object_sync` の経路、2125 行付近）は `VM_REGION_SHARED` の region だけを同期する（private の region は file に書かない）。
6. 変えないもの: object の page の reclaim（map されている間は保たれる、今の仕組み）、file の truncate の revoke（object の mapping の revoke の今の経路）、`mprotect`、munmap・後始末（object の mapping の解除の今の経路）。

意味の違い: `MAP_PRIVATE` の page は、書き込むまで file の後の変更（`write()`）が見える（Linux と同じ。POSIX では未規定）。今は fault の時点の内容を複写していた。

### 期待する効果

clang・ld.lld の起動の text・rodata の fault（数万）が、object の page の探索（木）と map だけになる（複写と file の読みの経路が無くなる）。
2 回目以後の process は object の page を共有するので、buffer cache から object への複写も起きない。relocation で書く data の page（数百）は今までどおり複写。

### 試験

- guest: `ffault`（file の fault の ns/page）、link・compile の時間、expat の configure、kbench。
- 正しさ: 共有 library を使う全ての program（guest の sh・make の差分試験・対話試験）、fork の後の子と親が private の page を別々に持つこと（COW の試験の program: map して親子で書き分ける）、
  `mprotect` で書き込み可能にした後の書き込みが file を変えないこと、file を `write()` した後に書いていない page が新しい内容を見ること（Linux と同じ）、truncate の後の access、`SMP-STRESS.ELF`。
- 4 platform の build と boot。HAL の変更は要らない。

### 実装の Phase

| Phase | 内容 |
| --- | --- |
| ws046-p011 | 上の 1〜5 の実装と試験 |
