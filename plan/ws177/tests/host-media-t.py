#!/usr/bin/env python3
# ws177-p027〜p030 (案 T): makes media files with the host's ffmpeg, reads each with the media file reader
# (host-media-t, built by host-media-t.sh) and with ffprobe, and compares the packets of each track: their times (in
# microseconds, the reader's rounding), sizes, key flags and the Adler-32 of their bytes.  Some files are changed
# after they are made (a box renamed, a track's ID, an offset past the end, the file cut short) and compared with
# what the unchanged file's packets say the reader must find.  Seeks are checked against the key frames ffprobe
# lists.  Prints one line a case and, last, "host-media-t: PASS" or "host-media-t: FAIL".
#   python3 -I plan/ws177/tests/host-media-t.py DRIVER OUTDIR [GROUP...]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import json
import os
import struct
import subprocess
import sys

DRIVER = sys.argv[1]
OUT = sys.argv[2]
GROUPS = sys.argv[3:] or ["mp4"]
FAILED = []

# The inputs every file is made from: 3 s of a test picture (10 fps) and a tone.
VIDEO_IN = ["-f", "lavfi", "-i", "testsrc=size=64x48:rate=10"]
AUDIO_IN = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000"]
H264 = ["-c:v", "libx264", "-g", "10", "-bf", "2", "-pix_fmt", "yuv420p"]
AAC = ["-c:a", "aac", "-b:a", "16k"]


def ffmpeg(name, args):
    """Makes OUT/name with ffmpeg from the test inputs."""
    path = os.path.join(OUT, name)
    subprocess.run(["ffmpeg", "-v", "error", "-y"] + args + [path], check=True)
    return path


def scale_us(value, den, num=1):
    """The reader's microseconds of value units of num/den seconds (whole seconds and the rest, each truncated)."""
    units = den // num if num != 0 and den % num == 0 else None
    if units is None:
        return int(value * num * 1000000 // den)
    whole = int(value / units) if value >= 0 else -int(-value // units)
    rest = value - whole * units
    return whole * 1000000 + int(rest * 1000000 / units)


def probe(path):
    """ffprobe's streams and, for each, its packets (pts_us, dts_us, size, key, adler32, pos)."""
    out = subprocess.run(["ffprobe", "-v", "error", "-show_data_hash", "adler32", "-show_streams",
                          "-show_packets", "-of", "json", path], check=True, capture_output=True, text=True).stdout
    data = json.loads(out)
    streams = []
    for stream in data["streams"]:
        num, den = (int(x) for x in stream["time_base"].split("/"))
        streams.append({"type": stream["codec_type"], "num": num, "den": den, "packets": []})
    for packet in data.get("packets", []):
        stream = streams[packet["stream_index"]]
        pts = packet.get("pts", packet.get("dts"))
        dts = packet.get("dts", pts)
        stream["packets"].append({
            "pts": scale_us(int(pts), stream["den"], stream["num"]),
            "dts": scale_us(int(dts), stream["den"], stream["num"]),
            "size": int(packet["size"]),
            "key": 1 if "K" in packet["flags"] else 0,
            "adler32": packet["data_hash"].split(":")[1],
            "pos": int(packet.get("pos", -1)),
        })
    return [s for s in streams if s["type"] in ("video", "audio")]


def run(path, seeks=()):
    """The reader's tracks and packets of a file (and its seeks)."""
    result = subprocess.run([DRIVER, path] + [str(s) for s in seeks], capture_output=True, text=True, timeout=60)
    tracks, packets, end, afters = [], {}, None, []
    for line in result.stdout.splitlines():
        words = line.split()
        fields = dict(w.split("=", 1) for w in words[1:] if "=" in w)
        if words[0] == "OPEN":
            return {"open": int(fields["error"]), "stderr": result.stderr}
        if words[0] == "FORMAT":
            fmt = {"name": words[1], "duration": int(fields["duration_us"])}
        elif words[0] == "TRACK":
            tracks.append(fields)
            packets[int(words[1])] = []
        elif words[0] == "PACKET":
            packets[int(fields["track"])].append({"pts": int(fields["pts"]), "dts": int(fields["dts"]),
                                                   "size": int(fields["size"]), "key": int(fields["key"]),
                                                   "adler32": fields["adler32"]})
        elif words[0] == "END":
            end = int(fields["error"])
        elif words[0] == "DROPPED":
            tracks[int(fields["track"])]["dropped"] = fields["count"]
        elif words[0] == "SEEK":
            afters.append({"time": int(words[1]), "error": int(fields["error"]), "packets": {}})
        elif words[0] == "AFTER":
            afters[-1]["packets"][int(fields["track"])] = {"pts": int(fields["pts"]), "dts": int(fields["dts"]),
                                                            "key": int(fields["key"])}
    return {"open": 0, "format": fmt, "tracks": tracks, "packets": packets, "end": end, "afters": afters,
            "status": result.returncode, "stderr": result.stderr}


def compare(name, got, streams, dropped=None, fmt=None, seeks=(), ignore=()):
    """Checks the reader's tracks against ffprobe's streams; returns a list of problems."""
    problems = []
    if got["open"] != 0:
        return ["open error %d %s" % (got["open"], got.get("stderr", "")[:200])]
    if got["status"] != 0 or got["stderr"]:
        problems.append("exit %d stderr %s" % (got["status"], got["stderr"][:300]))
    if fmt is not None and got["format"]["name"] != fmt:
        problems.append("format %s, not %s" % (got["format"]["name"], fmt))
    if got["end"] != 61:  # ENODATA
        problems.append("reading ended with %s" % got["end"])
    if len(got["tracks"]) != len(streams):
        problems.append("%d tracks, ffprobe %d" % (len(got["tracks"]), len(streams)))
        return problems
    for index, stream in enumerate(streams):
        mine = got["packets"][index]
        theirs = stream["packets"]
        keys = [k for k in ("pts", "dts", "size", "key", "adler32") if k not in ignore]
        if len(mine) != len(theirs):
            problems.append("track %d: %d packets, expected %d" % (index, len(mine), len(theirs)))
        for i, (a, b) in enumerate(zip(mine, theirs)):
            diff = [k for k in keys if a[k] != b[k]]
            if diff:
                problems.append("track %d packet %d: %s, expected %s" % (
                    index, i, {k: a[k] for k in diff}, {k: b[k] for k in diff}))
                break
        if int(got["tracks"][index]["packets"]) not in (0, len(mine)):
            problems.append("track %d: packet_count %s, read %d" % (index, got["tracks"][index]["packets"], len(mine)))
        want_dropped = 0 if dropped is None else dropped[index]
        if int(got["tracks"][index]["dropped"]) != want_dropped:
            problems.append("track %d: dropped %s, expected %d" % (index, got["tracks"][index]["dropped"], want_dropped))
    for after in got["afters"]:
        problems += check_seek(after, streams)
    return problems


def check_seek(after, streams):
    """The first video packet after a seek is the last key frame at or before the time (the first one when none is);
    without video, the first track's first packet is at or before the time and not more than 2 s before it."""
    problems = []
    if after["error"] != 0:
        return ["seek %d: error %d" % (after["time"], after["error"])]
    if not any(stream["type"] == "video" for stream in streams):
        got = after["packets"].get(0)
        last = streams[0]["packets"][-1]["pts"]
        if got is None or got["pts"] > max(after["time"], streams[0]["packets"][0]["pts"]) or \
                got["pts"] < min(after["time"], last) - 2000000:
            problems.append("seek %d: first sound packet %s" % (after["time"], got))
        return problems
    for index, stream in enumerate(streams):
        if stream["type"] != "video":
            continue
        keys = [p for p in stream["packets"] if p["key"]]
        chosen = keys[0]
        for p in keys:
            if p["pts"] <= after["time"]:
                chosen = p
        got = after["packets"].get(index)
        if got is None or got["pts"] != chosen["pts"] or not got["key"]:
            problems.append("seek %d: video %s, expected the key frame at %d" % (after["time"], got, chosen["pts"]))
        break
    return problems


def report(name, problems):
    """Prints a case's result."""
    if problems:
        FAILED.append(name)
        print("%s: FAILED" % name)
        for problem in problems[:8]:
            print("    " + problem)
    else:
        print("%s: ok" % name)


# MP4 boxes: the ones that hold other boxes, and a walker that lists every box with its place.
CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"mvex", b"moof", b"traf", b"edts", b"dinf"}


def boxes(data, start=0, end=None, depth=0):
    """Every box in data[start:end] and inside the containers: (type, offset, header size, size, depth)."""
    end = len(data) if end is None else end
    found = []
    position = start
    while position + 8 <= end:
        size, kind = struct.unpack(">I4s", data[position:position + 8])
        header = 8
        if size == 1:
            size = struct.unpack(">Q", data[position + 8:position + 16])[0]
            header = 16
        elif size == 0:
            size = end - position
        if size < header or position + size > end:
            break
        found.append((kind, position, header, size, depth))
        if kind in CONTAINERS:
            found += boxes(data, position + header, position + size, depth + 1)
        position += size
    return found


def mutate(source, name, change):
    """Writes a copy of source as OUT/name after change(bytearray) edits it."""
    data = bytearray(open(source, "rb").read())
    change(data)
    path = os.path.join(OUT, name)
    open(path, "wb").write(data)
    return path


def rename_boxes(kind, new):
    """A change that renames every box of a type (the reader then skips it)."""
    def change(data):
        for found, offset, header, size, depth in boxes(bytes(data)):
            if found == kind:
                data[offset + 4:offset + 8] = new
    return change


def renumber_tracks(mapping):
    """A change that gives the tracks new IDs in tkhd, trex and tfhd."""
    def change(data):
        for kind, offset, header, size, depth in boxes(bytes(data)):
            payload = offset + header
            if kind == b"tkhd":
                place = payload + (20 if data[payload] == 1 else 12)
            elif kind in (b"trex", b"tfhd"):
                place = payload + 4
            else:
                continue
            old = struct.unpack(">I", data[place:place + 4])[0]
            data[place:place + 4] = struct.pack(">I", mapping[old])
    return change


def cut(streams, length, fragment_end=None):
    """The packets left when a file is cut at length, and how many of each track's are counted as left out."""
    kept, dropped = [], []
    for stream in streams:
        inside = [p for p in stream["packets"] if p["pos"] + p["size"] <= length]
        known = [p for p in stream["packets"] if p["pos"] + p["size"] > length and
                 (fragment_end is None or p["pos"] < fragment_end)]
        kept.append(dict(stream, packets=inside))
        dropped.append(len(known))
    return kept, dropped


def group_mp4():
    """ws177-p027: fragmented MP4 and damaged indexes."""
    base = VIDEO_IN + AUDIO_IN + ["-t", "3"] + H264 + AAC + ["-use_editlist", "0"]
    seeks = (0, 1000000, 1550000, 2900000, 10000000)
    made = {}
    for name, flags in (("fmp4-moof", "frag_keyframe+empty_moov+default_base_moof"),
                        ("fmp4-base", "frag_keyframe+empty_moov"),
                        ("fmp4-moov-first", "frag_keyframe"),
                        ("fmp4-separate", "frag_keyframe+empty_moov+separate_moof+default_base_moof"),
                        ("fmp4-every-frame", "frag_every_frame+empty_moov+default_base_moof"),
                        ("mp4-faststart", "faststart")):
        made[name] = ffmpeg(name + ".mp4", base + ["-movflags", flags])
        streams = probe(made[name])
        report(name, compare(name, run(made[name], seeks), streams, fmt="mp4", seeks=seeks))

    # Without tfdt the times go on from the track's last sample: the same packets.
    source = made["fmp4-moof"]
    streams = probe(source)
    path = mutate(source, "fmp4-no-tfdt.mp4", rename_boxes(b"tfdt", b"free"))
    report("fmp4-no-tfdt", compare("fmp4-no-tfdt", run(path, seeks), streams, seeks=seeks))

    # Tracks named by IDs that are not their places (7 and 3): the fragments still find them.
    path = mutate(source, "fmp4-ids.mp4", renumber_tracks({1: 7, 2: 3}))
    report("fmp4-ids", compare("fmp4-ids", run(path, seeks), streams, seeks=seeks))

    # A fragmented file cut in the middle of a fragment's data: what is in the file plays; the cut fragment's
    # samples past the end are counted, the fragments after it are not known.
    data = open(source, "rb").read()
    moofs = [offset for kind, offset, header, size, depth in boxes(data) if kind == b"moof"]
    length = moofs[len(moofs) // 2] + 300
    following = [m for m in moofs if m > length]
    path = mutate(source, "fmp4-cut.mp4", lambda d: d.__delitem__(slice(length, None)))
    kept, dropped = cut(streams, length, following[0] if following else None)
    report("fmp4-cut", compare("fmp4-cut", run(path, (0, 1000000)), kept, dropped=dropped))

    # A plain MP4 (index first) cut short: every sample past the end is known and counted.
    source = made["mp4-faststart"]
    streams = probe(source)
    length = os.path.getsize(source) * 6 // 10
    path = mutate(source, "mp4-cut.mp4", lambda d: d.__delitem__(slice(length, None)))
    kept, dropped = cut(streams, length)
    report("mp4-cut", compare("mp4-cut", run(path, (0, 1000000)), kept, dropped=dropped))

    # A plain MP4 whose video track's last chunk is placed past the end of the file: that chunk's samples go.
    data = open(source, "rb").read()
    found = boxes(data)
    stco = [(offset, header) for kind, offset, header, size, depth in found if kind == b"stco"][0]
    count = struct.unpack(">I", data[stco[0] + stco[1] + 4:stco[0] + stco[1] + 8])[0]
    place = stco[0] + stco[1] + 8 + (count - 1) * 4
    last = struct.unpack(">I", data[place:place + 4])[0]

    def move_last_chunk(d):
        d[place:place + 4] = struct.pack(">I", 0xfffffff0)
    path = mutate(source, "mp4-bad-offset.mp4", move_last_chunk)
    kept = [dict(streams[0], packets=[p for p in streams[0]["packets"] if p["pos"] < last])] + streams[1:]
    dropped = [len(streams[0]["packets"]) - len(kept[0]["packets"])] + [0] * (len(streams) - 1)
    report("mp4-bad-offset", compare("mp4-bad-offset", run(path, (0,)), kept, dropped=dropped))


def ts_streams(streams):
    """ffprobe's packets of a transport stream timed as the reader times them: from the earliest first PTS, on the
    33-bit clock (a time just before the start is negative)."""
    raw = [s for s in streams]
    firsts = [s["raw"][0][0] for s in raw if s["raw"]]
    start = firsts[0]
    for first in firsts:
        if relative90(first, start) < 0:
            start = first
    timed = []
    for stream in raw:
        packets = []
        for (pts, dts), packet in zip(stream["raw"], stream["packets"]):
            packets.append(dict(packet, pts=scale_us(relative90(pts, start), 90000),
                                dts=scale_us(relative90(dts, start), 90000)))
        timed.append(dict(stream, packets=packets))
    return timed


def relative90(raw, start):
    """A 90 kHz time from the start on the wrapping 33-bit clock."""
    difference = (raw - start) % (1 << 33)
    if difference >= (1 << 33) - (1 << 30):
        difference -= 1 << 33
    return difference


def probe_ts(path):
    """ffprobe's streams of a transport stream, timed from its start."""
    streams = probe(path)
    out = subprocess.run(["ffprobe", "-v", "error", "-show_packets", "-show_entries", "packet=stream_index,pts,dts",
                          "-of", "json", path], check=True, capture_output=True, text=True).stdout
    data = json.loads(out)
    kinds = json.loads(subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", path], check=True,
                                      capture_output=True, text=True).stdout)["streams"]
    order = [i for i, k in enumerate(kinds) if k["codec_type"] in ("video", "audio")]
    raws = {i: [] for i in order}
    for packet in data.get("packets", []):
        if packet["stream_index"] in raws:
            pts = int(packet.get("pts", packet.get("dts")))
            raws[packet["stream_index"]].append((pts, int(packet.get("dts", pts))))
    for stream, index in zip(streams, order):
        stream["raw"] = raws[index]
    return ts_streams(streams)


def prefix(name, got, streams, most_dropped=1):
    """Checks that each track's packets are the first ones ffprobe lists (a file cut short)."""
    problems = []
    if got["open"] != 0:
        return ["open error %d" % got["open"]]
    if got["end"] != 61:
        problems.append("reading ended with %s" % got["end"])
    for index, stream in enumerate(streams):
        mine = got["packets"][index]
        theirs = stream["packets"][:len(mine)]
        if not mine or len(mine) > len(stream["packets"]):
            problems.append("track %d: %d packets of %d" % (index, len(mine), len(stream["packets"])))
        for i, (a, b) in enumerate(zip(mine, theirs)):
            if any(a[k] != b[k] for k in ("pts", "dts", "size", "key", "adler32")):
                problems.append("track %d packet %d: %s, expected %s" % (index, i, a, b))
                break
        if int(got["tracks"][index]["dropped"]) > most_dropped:
            problems.append("track %d: dropped %s" % (index, got["tracks"][index]["dropped"]))
    return problems


def group_ts():
    """ws177-p028: MPEG-TS and M2TS."""
    seeks = (0, 1000000, 1550000, 2900000, 10000000)
    base = VIDEO_IN + ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "3"] + H264
    cases = (("ts-h264-aac", base + AAC + ["-f", "mpegts"]),
             ("ts-h264-mp3", base + ["-c:a", "libmp3lame", "-b:a", "64k", "-f", "mpegts"]),
             ("m2ts", base + AAC + ["-f", "mpegts", "-mpegts_m2ts_mode", "1"]),
             ("ts-late", base + AAC + ["-f", "mpegts", "-output_ts_offset", "1000"]),
             ("ts-wrap", base + AAC + ["-f", "mpegts", "-output_ts_offset", "95442"]))
    made = {}
    for name, args in cases:
        made[name] = ffmpeg(name + ".ts", args)
        streams = probe_ts(made[name])
        report(name, compare(name, run(made[name], seeks), streams, fmt="mpegts", seeks=seeks))

    # Zero bytes between two packets: the packets are found again and nothing is lost.
    source = made["ts-h264-aac"]
    streams = probe_ts(source)
    place = (os.path.getsize(source) // 188 // 2) * 188

    def insert_junk(d):
        d[place:place] = bytes(100)
    path = mutate(source, "ts-junk.ts", insert_junk)
    report("ts-junk", compare("ts-junk", run(path, seeks), streams, seeks=seeks))

    # Cut in the middle of a packet: what is whole plays, the last PES of a track may be left out.
    length = os.path.getsize(source) * 6 // 10 + 77
    path = mutate(source, "ts-cut.ts", lambda d: d.__delitem__(slice(length, None)))
    report("ts-cut", prefix("ts-cut", run(path, (0, 1000000)), streams))

    # An audio packet in the middle of a PES lost: that PES is left out and counted, the video is whole.
    data = open(source, "rb").read()
    audio_pid = None
    for offset in range(0, len(data), 188):
        pid = ((data[offset + 1] & 0x1f) << 8) | data[offset + 2]
        if pid == 0x101:
            audio_pid = pid
            if offset > len(data) // 2 and not data[offset + 1] & 0x40:
                lost = offset
                break
    path = mutate(source, "ts-lost.ts", lambda d: d.__delitem__(slice(lost, lost + 188)))
    got = run(path)
    problems = compare("ts-lost", got, [streams[0]] + [dict(streams[1], packets=[])])
    problems = [p for p in problems if not p.startswith("track 1")]
    audio = got["packets"].get(1, [])
    if int(got["tracks"][1]["dropped"]) != 1 or not 0 < len(streams[1]["packets"]) - len(audio) <= 40:
        problems.append("audio: dropped %s, %d of %d packets" % (got["tracks"][1]["dropped"], len(audio),
                                                                  len(streams[1]["packets"])))
    report("ts-lost", problems)


def group_ogg():
    """ws177-p029: Ogg with Opus, Vorbis and Theora."""
    seeks = (0, 1000000, 1550000, 2900000, 10000000)
    cases = (("ogg-opus", "opus", ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "3",
                                   "-c:a", "libopus", "-b:a", "32k"]),
             ("ogg-vorbis", "ogg", ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t", "3",
                                    "-c:a", "libvorbis"]),
             ("ogg-theora", "ogv", VIDEO_IN + ["-f", "lavfi", "-i", "sine=sample_rate=44100", "-t", "3",
                                               "-c:v", "libtheora", "-g", "10", "-c:a", "libvorbis"]),
             ("ogg-theora-opus", "ogv", VIDEO_IN + ["-f", "lavfi", "-i", "sine=sample_rate=48000", "-t", "3",
                                                    "-c:v", "libtheora", "-g", "7", "-c:a", "libopus"]))
    made = {}
    for name, extension, args in cases:
        made[name] = ffmpeg(name + "." + extension, args)
        streams = probe(made[name])
        report(name, compare(name, run(made[name], seeks), streams, fmt="ogg", seeks=seeks))

    # A page in the middle damaged (one byte of its body): its packets are left out and counted, the rest read.
    source = made["ogg-theora"]
    streams = probe(source)
    data = open(source, "rb").read()
    pages = []
    position = 0
    while True:
        position = data.find(b"OggS", position)
        if position < 0:
            break
        pages.append(position)
        position += 4
    damaged = pages[len(pages) // 2]
    path = mutate(source, "ogg-crc.ogv", lambda d: d.__setitem__(damaged + 40, d[damaged + 40] ^ 0xff))
    got = run(path)
    problems = []
    if got["open"] != 0 or got["end"] != 61:
        problems.append("open %s end %s" % (got["open"], got.get("end")))
    else:
        lost = sum(len(stream["packets"]) - len(got["packets"][index]) for index, stream in enumerate(streams))
        dropped = sum(int(track["dropped"]) for track in got["tracks"])
        if not 0 < lost <= 64 or dropped < 1:
            problems.append("lost %d packets, dropped %d" % (lost, dropped))
    report("ogg-crc", problems)

    # Cut in the middle of a page: each track's packets are ffprobe's first ones.
    length = len(data) * 6 // 10 + 11
    path = mutate(source, "ogg-cut.ogv", lambda d: d.__delitem__(slice(length, None)))
    report("ogg-cut", prefix("ogg-cut", run(path, (0, 1000000)), streams))


def riff_chunks(data):
    """The top-level chunks of the first RIFF and inside its lists: (id, offset of the header, size)."""
    found = []

    def walk(start, end):
        position = start
        while position + 8 <= end:
            kind = data[position:position + 4]
            size = struct.unpack("<I", data[position + 4:position + 8])[0]
            found.append((kind, position, size))
            if kind == b"LIST":
                walk(position + 12, min(position + 8 + size, end))
            position += 8 + size + (size & 1)
    walk(12, len(data))
    return found


def group_avi():
    """ws177-p030: AVI."""
    seeks = (0, 1000000, 1550000, 2900000, 10000000)
    cases = (("avi-mpeg4-mp3", VIDEO_IN + ["-f", "lavfi", "-i", "sine=sample_rate=48000", "-t", "3", "-c:v", "mpeg4",
                                           "-g", "10", "-c:a", "libmp3lame", "-b:a", "64k"]),
             ("avi-h264-pcm", VIDEO_IN + ["-f", "lavfi", "-i", "sine=sample_rate=8000", "-t", "3"] + H264[:4] +
              ["-bf", "0", "-c:a", "pcm_s16le"]),
             ("avi-mjpeg", VIDEO_IN + ["-t", "3", "-c:v", "mjpeg"]))
    made = {}
    for name, args in cases:
        made[name] = ffmpeg(name + ".avi", args)
        streams = probe(made[name])
        report(name, compare(name, run(made[name], seeks), streams, fmt="avi", seeks=seeks))

    # Without idx1 (renamed JUNK): the movi list's chunks are read, the same packets (key frames from the bytes).
    source = made["avi-mpeg4-mp3"]
    streams = probe(source)
    path = mutate(source, "avi-no-idx1.avi", lambda d: d.__setitem__(slice(d.find(b"idx1"), d.find(b"idx1") + 4), b"JUNK"))
    report("avi-no-idx1", compare("avi-no-idx1", run(path, seeks), streams, seeks=seeks))

    # An idx1 entry pointing past the end of the file: that packet is left out and counted.
    data = open(source, "rb").read()
    index = data.find(b"idx1") + 8
    entry = index + 16 * 10
    track = int(data[entry:entry + 2])

    def move_entry(d):
        d[entry + 8:entry + 12] = struct.pack("<I", 0x7ffffff0)
    path = mutate(source, "avi-bad-entry.avi", move_entry)
    got = run(path)
    problems = []
    if got["open"] != 0:
        problems.append("open error %d" % got["open"])
    else:
        lost = [len(stream["packets"]) - len(got["packets"][i]) for i, stream in enumerate(streams)]
        dropped = [int(t["dropped"]) for t in got["tracks"]]
        want = [1 if i == track else 0 for i in range(len(streams))]
        if lost != want or dropped != want:
            problems.append("lost %s, dropped %s, expected %s" % (lost, dropped, want))
    report("avi-bad-entry", problems)

    # Cut short (the index lost with the end): the chunks are read, each track's packets ffprobe's first ones.
    length = len(data) * 6 // 10 + 5
    path = mutate(source, "avi-cut.avi", lambda d: d.__delitem__(slice(length, None)))
    report("avi-cut", prefix("avi-cut", run(path, (0, 1000000)), streams))


def main():
    """Runs the groups asked for."""
    os.makedirs(OUT, exist_ok=True)
    for group in GROUPS:
        globals()["group_" + group]()
    if FAILED:
        print("host-media-t: FAIL (%s)" % " ".join(FAILED))
        return 1
    print("host-media-t: PASS")
    return 0


sys.exit(main())
