#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Write a small HFS (Mac OS Standard) volume that Open Firmware can read.

The volume holds a few folders and files laid out contiguously: the boot
blocks, the master directory block (and its copy near the end), the volume
bitmap, an empty extents overflow tree, the catalog tree, then the files'
data.  One folder can be blessed (the master directory block's Finder
information word 0), which is where Open Firmware looks for the file of
type 'tbxi' when it boots "\\\\:tbxi".

Values referenced: Apple, Inside Macintosh: Files (1992), chapter 2
"File Manager": the master directory block's fields and offsets, the
volume bitmap, the B*-tree node descriptor, header record and map record,
the catalog key (key length, reserved byte, parent ID, Str31 name; index
keys padded to 37 bytes), the folder, file and thread records, the
extent records; Apple Technical Note TN1150 "HFS Plus Volume Format",
which also describes HFS's node size of 512 bytes, the reserved catalog
node IDs (1 the root's parent, 2 the root folder, 3 the extents file,
4 the catalog file, first free 16) and Finder information word 0 as the
blessed folder.
"""

import struct

SECTOR = 512
NODE = 512
MAC_EPOCH_OFFSET = 2082844800  # seconds from 1904-01-01 to 1970-01-01

ROOT_PARENT_ID = 1
ROOT_FOLDER_ID = 2
EXTENTS_FILE_ID = 3
CATALOG_FILE_ID = 4
FIRST_USER_ID = 16

CATALOG_MAX_KEY = 37
EXTENTS_MAX_KEY = 7

LEAF_KIND = 0xFF
INDEX_KIND = 0
HEADER_KIND = 1

FOLDER_RECORD = 1
FILE_RECORD = 2
FOLDER_THREAD = 3


class Folder:
    """A folder: its name, its children by name, and the ID it gets."""

    def __init__(self, name):
        self.name = name
        self.children = {}
        self.ident = None


class File:
    """A file: its name, data, Finder type and creator, and the ID it gets."""

    def __init__(self, name, data, file_type=b"\0\0\0\0", creator=b"\0\0\0\0"):
        self.name = name
        self.data = data
        self.file_type = file_type
        self.creator = creator
        self.ident = None
        self.first_block = 0
        self.block_count = 0


def mac_name(name):
    """Encode a name as the catalog's Mac Roman bytes, at most 31 of them."""
    raw = name.encode("mac_roman")
    if not raw or len(raw) > 31 or b":" in raw:
        raise ValueError(f"bad HFS name: {name!r}")
    return raw


def name_order(raw):
    """The catalog's order for names: case folded, as the File Manager compares."""
    return raw.upper()


def catalog_key(parent, raw_name, pad_to=None):
    """A catalog key; a leaf key is padded to an even size, an index key to the maximum."""
    length = 6 + len(raw_name)
    if pad_to is not None:
        body = struct.pack(">BI", 0, parent) + bytes([len(raw_name)]) + raw_name
        body = body.ljust(pad_to, b"\0")
        return bytes([pad_to]) + body
    key = bytes([length]) + struct.pack(">BI", 0, parent) + bytes([len(raw_name)]) + raw_name
    if len(key) % 2:
        key += b"\0"
    return key


def folder_record(ident, valence, when):
    """A folder record: its valence (children) and ID; 70 bytes."""
    return struct.pack(">BBHHIIII16s16s16s", FOLDER_RECORD, 0, 0, valence, ident,
                       when, when, 0, b"", b"", b"")


def file_record(entry, block_size, when):
    """A file record whose data fork is one extent; 102 bytes."""
    finder = struct.pack(">4s4sHIH", entry.file_type, entry.creator, 0, 0, 0)
    physical = entry.block_count * block_size
    extents = struct.pack(">HHHHHH", entry.first_block, entry.block_count, 0, 0, 0, 0)
    return struct.pack(">BBBB16sIHIIHIIIII16sH12s12sI",
                       FILE_RECORD, 0, 0, 0, finder, entry.ident,
                       0, len(entry.data), physical,
                       0, 0, 0,
                       when, when, 0, b"", 0, extents, b"\0" * 12, 0)


def thread_record(parent, raw_name):
    """A folder thread: the folder's parent and name; 46 bytes."""
    name = bytes([len(raw_name)]) + raw_name
    return struct.pack(">BB8sI32s", FOLDER_THREAD, 0, b"", parent, name.ljust(32, b"\0"))


def node_bytes(kind, height, records, forward=0, backward=0):
    """One 512-byte node: the descriptor, the records and the offsets at its end."""
    body = bytearray(NODE)
    struct.pack_into(">IIBBHH", body, 0, forward, backward, kind & 0xFF, height, len(records), 0)
    at = 14
    offsets = []
    for record in records:
        offsets.append(at)
        body[at:at + len(record)] = record
        at += len(record)
    offsets.append(at)
    if at + 2 * len(offsets) > NODE:
        raise ValueError("records overflow a node")
    for index, offset in enumerate(offsets):
        struct.pack_into(">H", body, NODE - 2 * (index + 1), offset)
    return bytes(body)


def fits(records):
    """Whether records fit one node with their offsets."""
    return 14 + sum(len(r) for r in records) + 2 * (len(records) + 1) <= NODE


def header_node(depth, root, leaf_records, first_leaf, last_leaf, max_key, total, free, forward=0):
    """The header node: the header record, the user record and the node map."""
    header = struct.pack(">HIIIIHHII76s", depth, root, leaf_records, first_leaf, last_leaf,
                         NODE, max_key, total, free, b"")
    user = b"\0" * 128
    map_size = NODE - 14 - len(header) - len(user) - 2 * 4
    bitmap = bytearray(map_size)
    for node in range(total - free):
        bitmap[node // 8] |= 0x80 >> (node % 8)
    return node_bytes(HEADER_KIND, 0, [header, user, bytes(bitmap)], forward=forward)


def build_tree(leaf_items, max_key, total_nodes):
    """Pack sorted (key, data) leaf items into a B*-tree of total_nodes nodes."""
    leaves = []
    current = []
    for key, data in leaf_items:
        record = key + data
        if current and not fits(current + [record]):
            leaves.append(current)
            current = []
        current.append(record)
    if current:
        leaves.append(current)

    # Node 0 is the header; the leaves follow, then the index levels.
    nodes = {}
    leaf_numbers = list(range(1, 1 + len(leaves)))
    level = []
    for index, records in enumerate(leaves):
        forward = leaf_numbers[index + 1] if index + 1 < len(leaves) else 0
        backward = leaf_numbers[index - 1] if index > 0 else 0
        nodes[leaf_numbers[index]] = (LEAF_KIND, 1, records, forward, backward)
        first_key = records[0][:records[0][0] + 1]
        level.append((first_key, leaf_numbers[index]))

    height = 1
    next_number = 1 + len(leaves)
    while len(level) > 1:
        height += 1
        parents = []
        current = []
        for key, child in level:
            parent_id = struct.unpack_from(">I", key, 2)[0]
            name = key[7:7 + key[6]]
            record = catalog_index_key(parent_id, name, max_key) + struct.pack(">I", child)
            if current and not fits(current + [record]):
                parents.append(current)
                current = []
            current.append(record)
        if current:
            parents.append(current)
        level = []
        numbers = list(range(next_number, next_number + len(parents)))
        next_number += len(parents)
        for index, records in enumerate(parents):
            forward = numbers[index + 1] if index + 1 < len(parents) else 0
            backward = numbers[index - 1] if index > 0 else 0
            nodes[numbers[index]] = (INDEX_KIND, height, records, forward, backward)
            level.append((records[0][:records[0][0] + 1], numbers[index]))

    used = next_number
    if used > total_nodes:
        raise ValueError("tree does not fit its file")
    root = level[0][1] if level else 0
    out = bytearray()
    out += header_node(height if leaves else 0, root, len(leaf_items),
                       leaf_numbers[0] if leaves else 0, leaf_numbers[-1] if leaves else 0,
                       max_key, total_nodes, total_nodes - used)
    for number in range(1, total_nodes):
        if number in nodes:
            kind, node_height, records, forward, backward = nodes[number]
            out += node_bytes(kind, node_height, records, forward, backward)
        else:
            out += bytes(NODE)
    return bytes(out)


def catalog_index_key(parent, raw_name, max_key):
    """An index node's catalog key, padded to the maximum key length."""
    return catalog_key(parent, raw_name, pad_to=max_key)


def make_volume(size, root, name="zedBSD", blessed=None, when=None):
    """Lay out a volume of size bytes holding the tree under root (a Folder).

    blessed is the Folder whose ID goes in the Finder information's word 0.
    Returns the volume's bytes.
    """
    if when is None:
        when = 0
    mac_when = (when + MAC_EPOCH_OFFSET) & 0xFFFFFFFF
    sectors = size // SECTOR

    # The allocation block: the smallest multiple of a sector that keeps the count in 16 bits.
    block_size = SECTOR
    while (sectors - 6) * SECTOR // block_size > 0xFFFF:
        block_size += SECTOR
    bitmap_sectors = ((sectors * SECTOR // block_size) + SECTOR * 8 - 1) // (SECTOR * 8)
    first_sector = 3 + bitmap_sectors
    block_count = (sectors - first_sector - 2) * SECTOR // block_size

    # IDs for every folder and file, depth first in name order.
    root.ident = ROOT_FOLDER_ID
    next_id = FIRST_USER_ID
    entries = []

    def assign(folder):
        nonlocal next_id
        for key in sorted(folder.children, key=lambda n: name_order(mac_name(n))):
            child = folder.children[key]
            child.ident = next_id
            next_id += 1
            entries.append((folder, child))
            if isinstance(child, Folder):
                assign(child)

    assign(root)

    # The trees' sizes: the extents tree is a header alone, the catalog as many nodes as it needs.
    raw_volume = mac_name(name)
    record_bytes = 0
    for parent, child in entries:
        record_bytes += 7 + len(mac_name(child.name)) + 1 + 102 + 46
    catalog_nodes = max(8, 4 + 2 * (record_bytes // (NODE // 2) + 1))
    extents_nodes = 4

    def blocks_for(byte_count):
        return (byte_count + block_size - 1) // block_size

    # Each tree's file is whole blocks, and every node of it is the tree's.
    extents_start = 0
    extents_blocks = blocks_for(extents_nodes * NODE)
    extents_nodes = extents_blocks * block_size // NODE
    catalog_start = extents_start + extents_blocks
    catalog_blocks = blocks_for(catalog_nodes * NODE)
    catalog_nodes = catalog_blocks * block_size // NODE
    next_block = catalog_start + catalog_blocks

    # The files' data, contiguous after the trees.
    for _, child in entries:
        if isinstance(child, File):
            child.block_count = blocks_for(len(child.data))
            child.first_block = next_block if child.block_count else 0
            next_block += child.block_count
    if next_block > block_count:
        raise ValueError("files do not fit the volume")

    # The catalog's leaf records: each folder's thread and record, each file's record.
    items = []
    items.append((catalog_key(ROOT_PARENT_ID, raw_volume), folder_record(ROOT_FOLDER_ID, len(root.children), mac_when)))
    items.append((catalog_key(ROOT_FOLDER_ID, b""), thread_record(ROOT_PARENT_ID, raw_volume)))
    for parent, child in entries:
        raw = mac_name(child.name)
        if isinstance(child, Folder):
            items.append((catalog_key(parent.ident, raw), folder_record(child.ident, len(child.children), mac_when)))
            items.append((catalog_key(child.ident, b""), thread_record(parent.ident, raw)))
        else:
            items.append((catalog_key(parent.ident, raw), file_record(child, block_size, mac_when)))
    items.sort(key=lambda item: (struct.unpack_from(">I", item[0], 2)[0], name_order(item[0][7:7 + item[0][6]])))
    catalog = build_tree(items, CATALOG_MAX_KEY, catalog_nodes)
    extents = build_tree([], EXTENTS_MAX_KEY, extents_nodes)

    # The counts the master directory block keeps.
    files_in_root = sum(1 for c in root.children.values() if isinstance(c, File))
    folders_in_root = sum(1 for c in root.children.values() if isinstance(c, Folder))
    file_count = sum(1 for _, c in entries if isinstance(c, File))
    folder_count = sum(1 for _, c in entries if isinstance(c, Folder))
    finder = [0] * 8
    if blessed is not None:
        finder[0] = blessed.ident

    volume = bytearray(sectors * SECTOR)

    def block_offset(block):
        return first_sector * SECTOR + block * block_size

    volume[block_offset(extents_start):block_offset(extents_start) + len(extents)] = extents
    volume[block_offset(catalog_start):block_offset(catalog_start) + len(catalog)] = catalog
    for _, child in entries:
        if isinstance(child, File) and child.data:
            at = block_offset(child.first_block)
            volume[at:at + len(child.data)] = child.data

    # The volume bitmap: every block used so far.
    bitmap_at = 3 * SECTOR
    for block in range(next_block):
        volume[bitmap_at + block // 8] |= 0x80 >> (block % 8)

    # The master directory block, and its copy in the second sector from the end.
    mdb = struct.pack(">2sIIHHHHHIIHIH28sIHIIIHII8IHHHI12sI12s",
                      b"BD", mac_when, mac_when, 0x0100, files_in_root, 3, next_block,
                      block_count, block_size, block_size * 4, first_sector, next_id,
                      block_count - next_block, bytes([len(raw_volume)]) + raw_volume.ljust(27, b"\0"),
                      0, 0, 0, block_size * extents_blocks, block_size * catalog_blocks,
                      folders_in_root, file_count, folder_count, *finder,
                      0, 0, 0,
                      extents_blocks * block_size,
                      struct.pack(">HHHHHH", extents_start, extents_blocks, 0, 0, 0, 0),
                      catalog_blocks * block_size,
                      struct.pack(">HHHHHH", catalog_start, catalog_blocks, 0, 0, 0, 0))
    volume[2 * SECTOR:2 * SECTOR + len(mdb)] = mdb
    volume[(sectors - 2) * SECTOR:(sectors - 2) * SECTOR + len(mdb)] = mdb
    return bytes(volume)
