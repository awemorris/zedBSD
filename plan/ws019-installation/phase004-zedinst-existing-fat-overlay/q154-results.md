# q154 native source inspection

Date: 2026-09-09
Status: component accepted; full p004 uncleared

[p021](../phase021-nested-mount/results.md) removes q153's nested mount obstacle.
`temp/q154-source2/result.json` reports PASS source inspection. Actual read-only
USB ESP/payload mounts supply BOOTX64.EFI, vmunix and rootfs.img to the production
Noct source module. Format/malformed-header checks, 64 failed exact reads,
identity/digest capture and admission-manifest construction pass. Both mounts
and their owned directories are removed successfully. The guest also repeats
576 admission combinations and the 102 transaction policy scenarios.

This acceptance uses actual source artifacts but does not publish an installation.
GPT/FAT metadata and unrelated sentinel hashes are unchanged; so is the original
production image SHA-256. See p021 results for builds and mount lifecycle tests.

Continue source/destination discovery and retained raw GPT/BPB verification,
owned workspace/mount handling, confirmation, public packaging and complete
p004 acceptance under [the integration design](admission-design.md). The new
populated-tmpfs teardown defect is tracked independently as p022. No user decision
or unavailable Noct ioctl blocks these remaining implementation tasks.
