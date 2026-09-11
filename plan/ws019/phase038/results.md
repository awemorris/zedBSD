# p038 results

Completed q171.

q171-view1 terminated with exit 1 before any source mount: the test parser
expected spaces but diskpart machine output uses tabs. Corrected the fixture
to accept the actual tab-delimited contract. Production, source rootfs.img and
target hashes all unchanged. This was a fixture error, not a product defect.
Evidence: ../temp/q171-view1/result.json and /tmp/zedbsd-q171-view1.log.


q171-view2 terminated successfully (exit 0). Existing `mount -t ufs -r loop0`
exposed the boot lower as an independent source view. The fixture resolved
loop0 through live registration 7 (not its name), checked read-only flags and
33554432-byte geometry, and checked the mounted root device number. Two complete
mount/unmount cycles preserved the live root-image identity and ordinary root
operation. Writes were refused and the overlay-only marker was absent. The
source /bin/noct SHA256 matched the build input. Cleanup completed.

Actual text installer source screen captured and visually inspected; Escape
cancelled successfully. The capture is 1280x800 physical framebuffer with the
current centered text area; this is not the future 640x480 graphic frontend.
Screenshot: ../temp/q171-view2/installer-source.png.

The complete rootfs.img, disposable target and production image hashes remained
unchanged. Evidence: ../temp/q171-view2/result.json and
/tmp/zedbsd-q171-view2.log. No production source changed, so a rebuild was not
required. This proves the existing mount mechanism, not p007 integration or
complete native installation. Next integrate typed UFS workspace mounts and
live source revalidation, then implement native UFS swap prerequisites.
