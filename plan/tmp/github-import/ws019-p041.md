<!-- awesome-plan project=zedbsd record=ws019-p041 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase041/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p041: UFS file extent provider

Status: completed q174
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 90 active minutes

Implement the common file_extents capability for native UFS. Export ordered
512-byte-sector mappings with bounded scratch and contiguous-run coalescing.
Caller retains the prepared backing claim; mount lock serializes metadata
inspection. Validate regular-file geometry, complete-sector EOF, every direct
and indirect allocation, CG metadata exclusions, allocation bits, volume and
summary bounds. Refuse holes, malformed pointers, snapshots and unsupported
legacy rotational CG layout. Propagate callback and I/O errors; do not allocate
file blocks. Fixed scratch, no per-file-size mapping allocation in provider.

Focused production UFS host test covers direct/indirect mappings, fragmentation,
coalescing, tail EOF, malformed pointers/bitmap and callback errors. Three native
builds verify integrated callback registration. Formatter/swap admission remains
FAT-only until canonical identity and snapshot/claim exclusion are implemented
in the following phase. Do not claim UFS swap completion from this provider.
