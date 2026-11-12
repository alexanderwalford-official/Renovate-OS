"""
fatcheck.py - check a FAT16 / FAT32 disk image is consistent, after AerOS has written to it

A small stand-in for fsck.fat, so the tests need nothing more than Python. It walks every
folder from the root and checks that:

  - every copy of the FAT is the same
  - each file's cluster chain is as long as its size needs, and ends properly
  - no cluster belongs to two files ("cross-linked"), and none is used by nothing ("lost")
  - every folder's "." and ".." point where they should
  - long file name entries match the short name they belong to (their checksum)

    errors, files = check('disk.img')     # errors is empty when the disk is consistent

Run on its own (python3 fatcheck.py disk.img), it prints every file and any errors.
"""

import struct
import sys


def check(path):
    """Check the FAT disk image at path. Gives (errors, files): a list of problems (empty if
    there are none) and a list of every file and folder found, as paths."""
    data = open(path, 'rb').read()
    base = 0
    # an image that starts with a partition table: use the first FAT partition in it
    if data[0] not in (0xEB, 0xE9):
        for i in range(4):
            entry = data[446 + i * 16:446 + i * 16 + 16]
            if entry[4] in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E):
                base = struct.unpack_from('<I', entry, 8)[0] * 512
                break

    # the boot sector's BIOS parameter block: the layout of the whole volume
    boot = data[base:base + 512]
    bytes_per_sector, per_cluster, reserved, fat_count, root_entries, total16, _, fat_size16 = \
        struct.unpack_from('<HBHBHHBH', boot, 11)
    total32, fat_size32 = struct.unpack_from('<II', boot, 32)
    total = total16 or total32
    fat_size = fat_size16 or fat_size32
    root_sectors = (root_entries * 32 + 511) // 512
    data_start = reserved + fat_count * fat_size + root_sectors
    clusters = (total - data_start) // per_cluster
    fat32 = clusters >= 65525
    errors = []
    files = []

    def sector(n):
        return data[base + n * 512:base + n * 512 + 512]

    fats = [data[base + (reserved + i * fat_size) * 512:base + (reserved + (i + 1) * fat_size) * 512]
            for i in range(fat_count)]
    for i in range(1, fat_count):
        if fats[i] != fats[0]:
            errors.append(f'FAT copy {i} differs from the first')
    fat = fats[0]

    def next_cluster(c):
        if fat32:
            return struct.unpack_from('<I', fat, c * 4)[0] & 0x0FFFFFFF
        return struct.unpack_from('<H', fat, c * 2)[0]

    end_of_chain = 0x0FFFFFF8 if fat32 else 0xFFF8
    owner = {}   # cluster -> the file that uses it

    def chain(first, who):
        out = []
        c = first
        while 2 <= c < clusters + 2:
            if c in owner:
                errors.append(f'cross-linked: cluster {c} is used by {owner[c]} and {who}')
                break
            owner[c] = who
            out.append(c)
            following = next_cluster(c)
            if following == 0:
                errors.append(f'{who}: its chain runs into a free cluster after {c}')
                break
            if following >= end_of_chain:
                break
            c = following
        return out

    def read_clusters(chain_list):
        size = per_cluster * 512
        return b''.join(data[base + (data_start + (c - 2) * per_cluster) * 512:
                             base + (data_start + (c - 2) * per_cluster) * 512 + size] for c in chain_list)

    def long_name_checksum(name):
        s = 0
        for b in name:
            s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
        return s

    def walk(raw, path, self_cluster, parent_cluster, is_root):
        long_parts = []
        for at in range(0, len(raw), 32):
            entry = raw[at:at + 32]
            if entry[0] == 0:        # the end of the folder
                break
            if entry[0] == 0xE5:     # a deleted entry
                if long_parts:
                    errors.append(f'{path}: long name entries left before a deleted entry')
                long_parts = []
                continue
            attributes = entry[11]
            if attributes == 0x0F:   # part of a long file name, for the entry that follows
                long_parts.append(entry)
                continue
            if attributes & 0x08:    # the volume label
                long_parts = []
                continue
            short_name = entry[0:11]
            long_name = None
            if long_parts:
                if any(part[13] != long_name_checksum(short_name) for part in long_parts):
                    errors.append(f'{path}: a long name does not match {short_name!r}')
                else:
                    chars = []
                    for part in reversed(long_parts):
                        for o in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                            ch = struct.unpack_from('<H', part, o)[0]
                            if ch in (0, 0xFFFF):
                                break
                            chars.append(chr(ch))
                    long_name = ''.join(chars)
                long_parts = []
            first = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
            size = struct.unpack_from('<I', entry, 28)[0]
            short = short_name[:8].decode('latin1').rstrip()
            if short_name[8] != 32:
                short += '.' + short_name[8:].decode('latin1').rstrip()
            if short == '.':
                if first != self_cluster:
                    errors.append(f'{path}: "." points at {first}, not {self_cluster}')
                continue
            if short == '..':
                if first != parent_cluster:
                    errors.append(f'{path}: ".." points at {first}, not {parent_cluster}')
                continue
            full = path + (long_name or short)
            if attributes & 0x10:
                folder_chain = chain(first, full + '/')
                files.append(full + '/')
                if not folder_chain:
                    errors.append(f'{full}: a folder with no clusters')
                    continue
                walk(read_clusters(folder_chain), full + '/', first, 0 if is_root else self_cluster, False)
            else:
                file_chain = chain(first, full) if first else []
                needed = (size + per_cluster * 512 - 1) // (per_cluster * 512)
                if len(file_chain) != needed:
                    errors.append(f'{full}: {size} bytes need {needed} clusters, its chain has {len(file_chain)}')
                files.append(full)
        if long_parts:
            errors.append(f'{path}: long name entries left at the end of the folder')

    if fat32:
        root_cluster = struct.unpack_from('<I', boot, 44)[0]
        walk(read_clusters(chain(root_cluster, '/')), '/', root_cluster, 0, True)
    else:
        raw = b''.join(sector(reserved + fat_count * fat_size + i) for i in range(root_sectors))
        walk(raw, '/', 0, 0, True)

    lost = [c for c in range(2, clusters + 2) if next_cluster(c) != 0 and c not in owner]
    if lost:
        errors.append(f'{len(lost)} lost cluster(s), e.g. {lost[:10]}')
    return errors, files


if __name__ == '__main__':
    found_errors, found_files = check(sys.argv[1])
    for name in found_files:
        print('  ' + name)
    for error in found_errors:
        print('ERROR: ' + error)
    print('CLEAN' if not found_errors else f'{len(found_errors)} error(s)')
    sys.exit(1 if found_errors else 0)
