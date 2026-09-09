"""Actual CLI query records, with all media operations linked to abort stubs."""
import subprocess
import sys

binary = sys.argv[1]


def query(profile, value, status=0):
    argv = [binary, "-t", "ufs", "--profile=native"] if profile == "ufs-native" else [binary, "-t", profile]
    result = subprocess.run(argv + ["--check-size", str(value)], capture_output=True, text=True)
    assert result.returncode == status, (argv, value, result.returncode, result.stderr)
    if status:
        assert not result.stdout, result.stdout
        return
    fields = result.stdout.rstrip("\n").split("\t")
    assert result.stdout.endswith("\n") and len(fields) == 8, fields
    assert fields[:5] == ["format", "1", profile, "512", str(value)], fields
    free, inodes, unit = map(int, fields[5:])
    assert 0 < free < value and free % unit == 0
    if profile == "ufs-native":
        assert inodes > 0 and unit == 8192
    else:
        assert inodes == 0 and 512 <= unit <= 32768
        sectors = value // 512
        # Independently find minimal FAT sectors for the reported cluster unit.
        spc = unit // 512
        low, high = 1, sectors
        while low < high:
            middle = (low + high) // 2
            clusters = (sectors - 32 - 2 * middle) // spc
            if middle * 128 >= clusters + 2:
                high = middle
            else:
                low = middle + 1
        clusters = (sectors - 32 - 2 * low) // spc
        assert free == (clusters - 1) * unit


for size in [4194304, 67108864, 268435456, 4294967296, 5 << 40]:
    query("ufs-native", size)
for size in [67108864, 268435456, 4294967296]:
    query("fat32", size)
for profile in ["ufs-native", "fat32"]:
    for value in ["", "-1", "+512", "12x", "18446744073709551616"]:
        query(profile, value, 2)
    for value in [0, 513, 1024]:
        query(profile, value, 1)
query("ufs-native", 1 << 60, 1)
query("fat32", 1 << 41, 1)
for argv in [
    ["-t", "ufs", "--check-size", "67108864"],
    ["-t", "fat32", "--check-size"],
    ["-t", "fat32", "--check-size", "67108864", "extra"],
    ["-t", "fat32", "--check-size", "--check-size"],
    ["--check-size", "67108864"],
]:
    result = subprocess.run([binary] + argv, capture_output=True)
    assert result.returncode == 2 and not result.stdout, result
with open("/dev/full", "wb") as full:
    result = subprocess.run([binary, "-t", "fat32", "--check-size", "67108864"], stdout=full)
    assert result.returncode == 1
print("mkfs capacity CLI PASS: valid geometry, refusals, output failure, no media I/O")
