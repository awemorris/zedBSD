#!/usr/bin/env python3
"""Check actual producer images, including a deliberately large directory."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools/build"))
spec = importlib.util.spec_from_file_location("arch_check", REPO / "tools/build/check-arch-overlay-ufs.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
ufs = checker.load_checker()
with tempfile.TemporaryDirectory(prefix="q086-directory-") as work:
    work = Path(work)
    tree = work / "tree"
    tree.mkdir()
    for i in range(100):
        (tree / (f"{i:03d}-" + "x" * 110)).write_bytes(b"x")
    image = work / "large.ufs"
    subprocess.run([str(REPO / "build/zedimage-host"), "ufs", str(16 * 1024 * 1024),
                    str(tree), str(image)], check=True)
    fs = ufs.UFS(image.read_bytes())
    fs.validate()
    large = checker.large_directories(fs)
    assert len(large) == 1 and large[0][0] == "/" and large[0][2] > fs.bsize
    print("S34 PASS actual zedimage producer multi-block directory detected:", large)
for image in [REPO / "build/arch-images/amd64.ufs", REPO / "build/data.img"]:
    fs = ufs.UFS(image.read_bytes())
    fs.validate()
    large = checker.large_directories(fs)
    assert not large, (image, large)
    print(f"S35 IMAGE {image.relative_to(REPO)} multi-block-directories=0")
print("S35 PASS current root/data images within directory mutation limit")
