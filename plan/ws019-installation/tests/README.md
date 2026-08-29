# WS019 shared test index

Last updated: 2026-08-29

This directory will hold reusable installer and disk-administration fixtures
copied into the WS plan area when the relevant Phase is extracted. Do not use
`.internal/` as a test source.

The p001 design must allocate stable cases for at least:

- protective MBR and primary/backup GPT encode/decode, CRC, boundary, overlap,
  GUID, name, and malformed-table handling;
- 64-bit raw block offsets above 4 GiB and exactly one backend submission per
  written sector;
- mounted/root/swap descendant refusal and safe rescan;
- FAT32 and UFS1 provisioning failure boundaries;
- existing-partition and whole-disk confirmation/refusal behavior;
- native-root and overlay-root QEMU NVMe boots; and
- interrupted/partial installation reporting and recovery.
