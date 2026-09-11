"""Create canonical journal media containing recoverable, unreachable owners."""
import struct
from ufs_format import Image, IFDIR, IFREG


def seed_orphans():
    image = Image(32 * 1024**2, profile='journal-snapshot')
    image.next_ino = 255
    file_ino = image.add_file(2, 'orphan-file', b'orphan payload')
    image.next_ino = 511
    directory_ino = image.add_dir(2, 'orphan-directory')
    area = bytearray(16)
    struct.pack_into('<I', area, 0, 16)
    area[4], area[6], area[7] = 1, 1, ord('x')
    area[8:] = b'retained'
    attribute = image._alloc_block(area)
    file_offset = image.inode_offset(file_ino)
    struct.pack_into('<I', image.data, file_offset + 92, len(area))
    struct.pack_into('<Q', image.data, file_offset + 96, attribute)
    struct.pack_into('<Q', image.data, file_offset + 24, 2 * image.block_size // 512)
    data = bytearray(image.finish())

    # Keep allocated typed owners and their references, but remove namespace links.
    root = image.inode_offset(2)
    root_block = struct.unpack_from('<Q', data, root + 112)[0] * image.frag_size
    root_size = struct.unpack_from('<Q', data, root + 16)[0]
    position, removed = 0, 0
    while position < root_size:
        number, length = struct.unpack_from('<IH', data, root_block + position)
        assert length >= 8 and position + length <= root_size
        if number in (file_ino, directory_ino):
            struct.pack_into('<I', data, root_block + position, 0)
            removed += 1
        position += length
    assert removed == 2
    struct.pack_into('<H', data, root + 2, 2)
    metadata = []
    for number, kind in ((file_ino, IFREG), (directory_ino, IFDIR)):
        offset = image.inode_offset(number)
        assert struct.unpack_from('<H', data, offset)[0] & 0o170000 == kind
        struct.pack_into('<H', data, offset + 2, 0)
        if kind == IFDIR:
            # An empty-size directory can retain committed initialized backing.
            struct.pack_into('<Q', data, offset + 16, 0)
        cg, local = divmod(number, image.ipg)
        bitmap = (image.cg_base(cg) + image.cblk) * image.frag_size + 168
        assert data[bitmap + local // 8] & (1 << (local % 8))
        metadata.append({'inode': number, 'offset': offset,
                         'bitmap_byte': bitmap + local // 8,
                         'bitmap_mask': 1 << (local % 8)})
    return bytes(data), metadata


def verify_reclaimed(path, metadata):
    data = path.read_bytes()
    for item in metadata:
        assert not data[item['bitmap_byte']] & item['bitmap_mask'], item
        assert struct.unpack_from('<HH', data, item['offset']) == (0, 0), item
        assert struct.unpack_from('<QQ', data, item['offset'] + 16) == (0, 0), item
