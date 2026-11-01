<!-- awesome-plan project=zedbsd record=ws334-p038 -->

# ws334-p038 Windows の file: の頁の副資源（画像・CSS の url()）

- 親: [WS334](../ws.md)。2026-11-02 06:10 Q1 が p034 の結果から作成。
- Status: in-progress（2026-11-02 06:10 P5 q1909-i01）
- 依存: p034。

## 目的

Windows（Wine）で file: の頁の `<img>` と CSS の `url()` の画像が出ない不具合を直す（p034 の結果の Bug 候補 1）。

- `page/link.c` の `link_base_local` は `base[0]=='/'` だけを local とみなすので、`Z:\...` の base で `page_fetch(base, 相対 href)` が EACCES になる。
- `page/images.c` の `images_fetch` は `page_resolve_location` が返す local の path を href として渡すので、`Z:\...` が scheme `z` の URL と解され EPROTONOSUPPORT になる。
- 案（p034）: base が local かは `wb_os_path_is_absolute` と file: URL で判定する。location が local の path の時は `net_url_from_file_path` で file: URL にして渡す。http の頁の `/style.css` を壊さないよう、base が local の時だけにする。
- p034 の Bug 候補 2（`vm_heap_set_stack_base` の契約、header の comment）も同じ Phase で header の comment に足す。

## 受け入れ

- `plan/ws334/tests/host-tests-windows.sh` で host-canvas-background・host-file-subresource・host-svg-length・host-svg-text が host と同じになる（GC の揺れの 3 本は除く）。
- host の WS074 の試験（host-build、段 1）の前後が同じ。cross-build・storage-test・scheme-test が PASS。
