#!/usr/bin/env python3
"""ws122-p003: makes small MP4 and Matroska files whose contents are known, and
what host-mediafile must print for each.

    make-media.py OUTDIR

writes, for each case NAME, OUTDIR/NAME.<ext>, OUTDIR/NAME.args (the seek time
host-mediafile is given, or nothing) and OUTDIR/NAME.expected (its output).

The cases:
  mp4-narrow   two tracks (avc1 with a B-frame shift in its edit list, ctts and
               stss; mp4a with an esds), interleaved chunks, stsz and stco
  mp4-wide     the same with co64 and a 16-bit stz2 for the sound
  mkv-cues     WebM: VP9 and Opus, two clusters, a BlockGroup with a reference,
               Xiph, EBML and fixed lacing, a block of an unknown track, cues
               after the clusters found through the seek head
  mkv-scan     the same without cues and seek head (seeking reads the clusters)
  bad-cut      an MP4 cut inside its movie box (refused)
  bad-junk     bytes that are no container (refused)

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import errno
import os
import struct
import sys

ENODATA = errno.ENODATA
EINVAL = errno.EINVAL


def pattern(track, index, size):
    return bytes((track * 31 + index * 7 + k) & 0xff for k in range(size))


def scale_us(value, units):
    whole = int(value / units) if value >= 0 else -int(-value / units)
    rest = value - whole * units
    part = rest * 1000000
    part = int(part / units) if part >= 0 else -int(-part / units)
    return whole * 1000000 + part


def packet_line(tag, track, pts, dts, key, data):
    return f"{tag} track={track} pts={pts} dts={dts} key={key} size={len(data)} sum={sum(data)}"


# ---- MP4 ----

def box(kind, payload):
    return struct.pack(">I4s", 8 + len(payload), kind) + payload


def full(kind, version, payload):
    return box(kind, bytes([version, 0, 0, 0]) + payload)


AVCC = b"\x01\x64\x00\x1f\xff\xe1\x00\x04abcd\x01\x00\x02ef"
DSI = b"\x11\x90"


def esds():
    specific = bytes([5, len(DSI)]) + DSI
    config = bytes([4, 13 + len(specific), 0x40, 0x15, 0, 0, 0]) + struct.pack(">II", 0, 0) + specific
    es = bytes([3, 3 + len(config) + 3]) + struct.pack(">HB", 1, 0) + config + bytes([6, 1, 2])
    return full(b"esds", 0, es)


def mp4_case(wide):
    video = {"timescale": 1000, "count": 10, "delta": 40,
             "sizes": [300 + 17 * i for i in range(10)],
             "cts": [80, 120, 40, 80, 80, 120, 40, 80, 80, 80],
             "keys": [0, 5], "shift": 80, "per_chunk": 2}
    audio = {"timescale": 48000, "count": 20, "delta": 1024,
             "sizes": [100 + 5 * i for i in range(20)], "per_chunk": 4}

    def moov(video_offsets, audio_offsets):
        mvhd = full(b"mvhd", 0, struct.pack(">IIII", 0, 0, 1000, 400) + bytes(80))
        # video track
        entry = (bytes(6) + struct.pack(">H", 1) + bytes(16) + struct.pack(">HH", 320, 240) +
                 struct.pack(">III", 0x480000, 0x480000, 0) + struct.pack(">H", 1) + bytes(32) +
                 struct.pack(">Hh", 0x18, -1) + box(b"avcC", AVCC))
        stsd = full(b"stsd", 0, struct.pack(">I", 1) + box(b"avc1", entry))
        stts = full(b"stts", 0, struct.pack(">III", 1, video["count"], video["delta"]))
        ctts = full(b"ctts", 0, struct.pack(">I", len(video["cts"])) +
                    b"".join(struct.pack(">Ii", 1, c) for c in video["cts"]))
        stss = full(b"stss", 0, struct.pack(">I", len(video["keys"])) +
                    b"".join(struct.pack(">I", k + 1) for k in video["keys"]))
        stsc = full(b"stsc", 0, struct.pack(">IIII", 1, 1, video["per_chunk"], 1))
        stsz = full(b"stsz", 0, struct.pack(">II", 0, video["count"]) +
                    b"".join(struct.pack(">I", s) for s in video["sizes"]))
        if wide:
            stco = full(b"co64", 0, struct.pack(">I", len(video_offsets)) +
                        b"".join(struct.pack(">Q", o) for o in video_offsets))
        else:
            stco = full(b"stco", 0, struct.pack(">I", len(video_offsets)) +
                        b"".join(struct.pack(">I", o) for o in video_offsets))
        stbl = box(b"stbl", stsd + stts + ctts + stss + stsc + stsz + stco)
        hdlr = full(b"hdlr", 0, bytes(4) + b"vide" + bytes(12) + b"\0")
        mdhd = full(b"mdhd", 0, struct.pack(">IIII", 0, 0, 1000, 400) + bytes(4))
        minf = box(b"minf", full(b"vmhd", 0, bytes(8)) + stbl)
        elst = full(b"elst", 0, struct.pack(">IIiI", 1, 400, video["shift"], 0x10000))
        vtrak = box(b"trak", full(b"tkhd", 0, bytes(80)) + box(b"edts", elst) +
                    box(b"mdia", mdhd + hdlr + minf))
        # audio track
        entry = (bytes(6) + struct.pack(">H", 1) + struct.pack(">HHI", 0, 0, 0) +
                 struct.pack(">HHHH", 2, 16, 0, 0) + struct.pack(">I", 48000 << 16) + esds())
        stsd = full(b"stsd", 0, struct.pack(">I", 1) + box(b"mp4a", entry))
        stts = full(b"stts", 0, struct.pack(">III", 1, audio["count"], audio["delta"]))
        stsc = full(b"stsc", 0, struct.pack(">I", 2) + struct.pack(">III", 1, 4, 1) +
                    struct.pack(">III", 4, 4, 1))
        if wide:
            stsz = full(b"stz2", 0, struct.pack(">I", 16) + struct.pack(">I", audio["count"]) +
                        b"".join(struct.pack(">H", s) for s in audio["sizes"]))
            stco = full(b"co64", 0, struct.pack(">I", len(audio_offsets)) +
                        b"".join(struct.pack(">Q", o) for o in audio_offsets))
        else:
            stsz = full(b"stsz", 0, struct.pack(">II", 0, audio["count"]) +
                        b"".join(struct.pack(">I", s) for s in audio["sizes"]))
            stco = full(b"stco", 0, struct.pack(">I", len(audio_offsets)) +
                        b"".join(struct.pack(">I", o) for o in audio_offsets))
        stbl = box(b"stbl", stsd + stts + stsc + stsz + stco)
        hdlr = full(b"hdlr", 0, bytes(4) + b"soun" + bytes(12) + b"\0")
        mdhd = full(b"mdhd", 0, struct.pack(">IIII", 0, 0, 48000, 20480) + bytes(4))
        minf = box(b"minf", full(b"smhd", 0, bytes(4)) + stbl)
        atrak = box(b"trak", full(b"tkhd", 0, bytes(80)) + box(b"mdia", mdhd + hdlr + minf))
        return box(b"moov", mvhd + vtrak + atrak)

    ftyp = box(b"ftyp", b"isom" + struct.pack(">I", 0x200) + b"isomavc1")
    chunks = []
    for k in range(5):
        chunks.append((0, list(range(2 * k, 2 * k + 2))))
        chunks.append((1, list(range(4 * k, 4 * k + 4))))
    placeholder = moov([0] * 5, [0] * 5)
    position = len(ftyp) + len(placeholder) + 8
    offsets = {0: [], 1: []}
    samples = {0: {}, 1: {}}
    data = b""
    for track, indices in chunks:
        offsets[track].append(position + len(data))
        for i in indices:
            size = video["sizes"][i] if track == 0 else audio["sizes"][i]
            samples[track][i] = position + len(data)
            data += pattern(track, i, size)
    content = ftyp + moov(offsets[0], offsets[1]) + box(b"mdat", data)

    # expected
    def times(track, i):
        if track == 0:
            dts = i * video["delta"]
            return (scale_us(dts + video["cts"][i] - video["shift"], 1000), scale_us(dts - video["shift"], 1000))
        dts = i * audio["delta"]
        return (scale_us(dts, 48000), scale_us(dts, 48000))

    def key(track, i):
        return 1 if track == 1 or i in video["keys"] else 0

    def payload(track, i):
        size = video["sizes"][i] if track == 0 else audio["sizes"][i]
        return pattern(track, i, size)

    def order(nexts):
        lines = []
        nexts = dict(nexts)
        while True:
            best = None
            for track in (0, 1):
                count = video["count"] if track == 0 else audio["count"]
                if nexts[track] < count:
                    offset = samples[track][nexts[track]]
                    if best is None or offset < best[0]:
                        best = (offset, track)
            if best is None:
                return lines
            track = best[1]
            i = nexts[track]
            pts, dts = times(track, i)
            lines.append((track, pts, dts, key(track, i), payload(track, i)))
            nexts[track] += 1

    lines = ["FORMAT mp4 duration_us=400000 tracks=2",
             f"TRACK 0 kind=1 codec=1 name=avc1 width=320 height=240 rate=0 channels=0 private={len(AVCC)} packets=10",
             f"TRACK 1 kind=2 codec=7 name=mp4a width=0 height=0 rate=48000 channels=2 private={len(DSI)} packets=20"]
    packets = order({0: 0, 1: 0})
    lines += [packet_line("PACKET", *p) for p in packets]
    lines.append(f"END error={ENODATA} packets={len(packets)}")
    # seek to 250 ms: the video's last sync sample presented by then, the sound from its decoding time
    seek = 250000
    chosen = 0
    for i in video["keys"]:
        if times(0, i)[0] <= seek:
            chosen = i
    start = times(0, chosen)[1]
    audio_next = 0
    for i in range(audio["count"]):
        if times(1, i)[1] > start:
            break
        audio_next = i
    lines.append(f"SEEK {seek} error=0")
    lines += [packet_line("AFTER", *p) for p in order({0: chosen, 1: audio_next})[:3]]
    return content, str(seek), lines


# ---- Matroska ----

def vsize(n):
    for length in range(1, 9):
        if n < (1 << (7 * length)) - 1:
            return ((1 << (7 * length)) | n).to_bytes(length, "big")
    raise ValueError(n)


def el(ident, payload):
    return ident.to_bytes((ident.bit_length() + 7) // 8, "big") + vsize(len(payload)) + payload


def uint(ident, value, width=None):
    if width is None:
        width = max(1, (value.bit_length() + 7) // 8)
    return el(ident, value.to_bytes(width, "big"))


def block_bytes(track, relative, flags, frames, lacing):
    head = vsize(track) + struct.pack(">hB", relative, flags | {None: 0, "xiph": 2, "fixed": 4, "ebml": 6}[lacing])
    if lacing is None:
        return head + frames[0]
    body = bytes([len(frames) - 1])
    if lacing == "xiph":
        for f in frames[:-1]:
            n = len(f)
            body += b"\xff" * (n // 255) + bytes([n % 255])
    elif lacing == "ebml":
        body += vsize(len(frames[0]))
        for prev, f in zip(frames[:-2], frames[1:-1]):
            body += (0x4000 | (len(f) - len(prev) + 8191)).to_bytes(2, "big")
    return head + body + b"".join(frames)


OPUSHEAD = b"OpusHead\x01\x02\x38\x01\x80\xbb\x00\x00\x00\x00\x00"


def mkv_case(with_cues):
    def frames(track, start, sizes):
        return [pattern(track, start + i, s) for i, s in enumerate(sizes)]

    v0 = frames(1, 0, [500])
    a0 = frames(2, 0, [300, 255, 10])
    v1 = frames(1, 1, [120])
    v2 = frames(1, 2, [130])
    v3 = frames(1, 3, [480])
    a1 = frames(2, 3, [50, 70, 30])
    a2 = frames(2, 6, [40, 40])
    junk = frames(3, 0, [20])
    cluster1 = el(0x1F43B675, uint(0xE7, 0) +
                  el(0xA3, block_bytes(1, 0, 0x80, v0, None)) +
                  el(0xA3, block_bytes(2, 0, 0x80, a0, "xiph")) +
                  el(0xA3, block_bytes(1, 40, 0, v1, None)) +
                  el(0xA0, el(0xA1, block_bytes(1, 80, 0, v2, None)) + el(0xFB, (-40).to_bytes(1, "big", signed=True))))
    cluster2 = el(0x1F43B675, uint(0xE7, 500, 2) +
                  el(0xA3, block_bytes(1, 0, 0x80, v3, None)) +
                  el(0xA3, block_bytes(2, 0, 0x80, a1, "ebml")) +
                  el(0xA3, block_bytes(2, 20, 0x80, a2, "fixed")) +
                  el(0xA3, block_bytes(3, 0, 0x80, junk, None)))
    ebml = el(0x1A45DFA3, uint(0x4286, 1) + uint(0x42F7, 1) + uint(0x42F2, 4) + uint(0x42F3, 8) +
              el(0x4282, b"webm") + uint(0x4287, 4) + uint(0x4285, 2))
    info = el(0x1549A966, uint(0x2AD7B1, 1000000) + el(0x4489, struct.pack(">d", 1000.0)))
    tracks = el(0x1654AE6B,
                el(0xAE, uint(0xD7, 1) + uint(0x83, 1) + el(0x86, b"V_VP9") +
                   el(0xE0, uint(0xB0, 320, 2) + uint(0xBA, 240, 1))) +
                el(0xAE, uint(0xD7, 2) + uint(0x83, 2) + el(0x86, b"A_OPUS") + el(0x63A2, OPUSHEAD) +
                   el(0xE1, el(0xB5, struct.pack(">d", 48000.0)) + uint(0x9F, 2))))

    def build(seek_position, c1, c2):
        head = b""
        if with_cues:
            head = el(0x114D9B74, el(0x4DBB, el(0x53AB, (0x1C53BB6B).to_bytes(4, "big")) +
                                     uint(0x53AC, seek_position, 8)))
        body = head + info + tracks + cluster1 + cluster2
        cues = b""
        if with_cues:
            cues = el(0x1C53BB6B,
                      el(0xBB, uint(0xB3, 0) + el(0xB7, uint(0xF7, 1) + uint(0xF1, c1, 8))) +
                      el(0xBB, uint(0xB3, 500, 2) + el(0xB7, uint(0xF7, 1) + uint(0xF1, c2, 8))))
        return body, cues

    body, cues = build(0, 0, 0)
    head_size = len(body) - len(info + tracks + cluster1 + cluster2)
    c1 = head_size + len(info + tracks)
    c2 = c1 + len(cluster1)
    body, cues = build(len(body), c1, c2)
    segment_payload = body + cues
    content = ebml + el(0x18538067, segment_payload)

    lines = ["FORMAT matroska duration_us=1000000 tracks=2",
             "TRACK 0 kind=1 codec=4 name=V_VP9 width=320 height=240 rate=0 channels=0 private=0 packets=0",
             f"TRACK 1 kind=2 codec=8 name=A_OPUS width=0 height=0 rate=48000 channels=2 private={len(OPUSHEAD)} packets=0"]
    packets = ([(0, 0, 1, v0[0])] + [(1, 0, 1, f) for f in a0] + [(0, 40000, 0, v1[0]), (0, 80000, 0, v2[0])] +
               [(0, 500000, 1, v3[0])] + [(1, 500000, 1, f) for f in a1] + [(1, 520000, 1, f) for f in a2])
    lines += [packet_line("PACKET", t, pts, pts, k, d) for t, pts, k, d in packets]
    lines.append(f"END error={ENODATA} packets={len(packets)}")
    seek = 600000
    lines.append(f"SEEK {seek} error=0")
    lines += [packet_line("AFTER", t, pts, pts, k, d) for t, pts, k, d in packets[6:9]]
    return content, str(seek), lines


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    cases = {}
    content, seek, lines = mp4_case(False)
    cases["mp4-narrow"] = ("mp4", content, seek, lines)
    narrow = content
    content, seek, lines = mp4_case(True)
    cases["mp4-wide"] = ("mp4", content, seek, lines)
    content, seek, lines = mkv_case(True)
    cases["mkv-cues"] = ("webm", content, seek, lines)
    content, seek, lines = mkv_case(False)
    cases["mkv-scan"] = ("webm", content, seek, lines)
    moov_at = narrow.index(b"moov") - 4
    cases["bad-cut"] = ("mp4", narrow[:moov_at + 200], "", [f"OPEN error={EINVAL}"])
    cases["bad-junk"] = ("mp4", bytes(range(256)) * 4, "", [f"OPEN error={EINVAL}"])
    for name, (ext, content, seek, lines) in cases.items():
        with open(os.path.join(out, f"{name}.{ext}"), "wb") as stream:
            stream.write(content)
        with open(os.path.join(out, f"{name}.args"), "w") as stream:
            stream.write(seek + "\n")
        with open(os.path.join(out, f"{name}.expected"), "w") as stream:
            stream.write("\n".join(lines) + "\n")
    print("make-media: " + " ".join(sorted(cases)))


if __name__ == "__main__":
    main()
