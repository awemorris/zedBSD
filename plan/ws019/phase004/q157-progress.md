# q157 progress

Date: 2026-09-09
Status: historical intermediate checkpoint; superseded by [q157 results](q157-results.md)

Implemented discovery.noct: strict sysctl record parsing, unique firmware/config
origin resolution on one source disk, retained source/target metadata, explicit
distinct ESP/payload selection and repeated global identity checks. Both first
and final global PARTUUID records are checked against the retained GPT; target
identities are not silently replaced with stale cached records.

Host JIT/interpreter pass healthy selection and 12 refusal cases in
installer-discovery.noct. Private fixture build passes `/tmp/zedbsd-q157-fixture.log`.
QEMU fixture adds a second FAT32 payload partition and separately hashes its BPB
and sentinel; the first remains ESP. Native metadata, large-seek, fault and
source-independent gates pass, but actual discovery reports unsupported GPT.
`temp/q157-discovery/result.json` remains FAIL guest.

Analysis: normal boot.img contains MBR type 0x0c extent start 133120/count360448
plus protective 0xee start1/count495615. Disk has495616 sectors; primary GPT CRC
matches and last-sector GPT signature exists. diskpart DP_UNSUPPORTED covers
hybrid editing refusal as well as other unsupported layouts; do not clear that
flag blindly. Apply the source-only profile refinement in q157-design.md.

No QEMU or build process remains live at this checkpoint. Queue q157 stays
in-progress. Public installer integration is still incomplete.
