# q157 results: source and destination discovery

Date: 2026-09-09
Queue: finished; ws019-p004 uncleared (public integration remains)

The live QEMU runner completed with exit 0 and `PASS source and destination
discovery`. Evidence: `../temp/q157-discovery2/result.json` and its guest log.
The native path resolved firmware/configuration provenance on the ordinary
boot USB, retained source/destination metadata, selected distinct NVMe ESP and
payload partitions, and repeated identity observations. Source artifact capture
and mount cleanup also completed.

The result records equal before/after GPT, FAT boot-sector and sentinel hashes,
equal second-payload hashes, and an unchanged production image hash. These are
read-only discovery acceptance results, not evidence of a completed install.

The first native run, retained in `../temp/q157-discovery/result.json`, failed
because the ordinary source USB has a hybrid GPT/MBR layout. The correction
admits only the source's narrowly checked read-only profile: standard GPT
headers, exactly one protective MBR entry, and one FAT BIOS alias matching a
GPT partition extent. Other unsupported layouts still fail; a restricted table
is never accepted as a writable destination. Native discovery now passes with
the ordinary source image rather than a simplified substitute.

Validation:

- `/tmp/zedbsd-q157-host-final.log`: eight JIT/interpreter runs covering
  selection, metadata, discovery, and the hybrid profile; discovery includes
  twelve refusal cases and the hybrid fixture twenty-one mutations.
- `/tmp/zedbsd-q157-fixture2.log`: private amd64 fixture build passed.
- `../temp/q157-discovery2/result.json`: native discovery passed, protected
  hashes unchanged. Runner session 53521 was polled to terminal exit 0.

Resume: integrate exclusive workspace creation, owned read-only source mounts,
destination mounts, source/configuration uniqueness checks, interactive
confirmation, the existing publication transaction, and cleanup into the
public command. Then execute the full p004 acceptance and p005 installed boot.
Repeated metadata observations do not constitute an atomic device reservation.
