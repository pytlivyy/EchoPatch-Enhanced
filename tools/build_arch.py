"""Packs a folder into a LithTech Jupiter EX .Arch00 archive.

Usage: build_arch.py <source folder> <output .Arch00>
"""
import hashlib, os, struct, sys


def pad4(b):
    return b + b'\0' * (-len(b) % 4)


def layout(files):
    """files: {archive path ('dir\\name'): size}. Returns (header without GUID, tables, paths in data order)."""
    parent = lambda p: p.rpartition('\\')[0]
    tree = {''}
    for p in files:
        while p:
            p = parent(p)
            tree.add(p)
    tree = {d: ([], []) for d in tree}  # dir -> (subdirs, files)
    for d in tree:
        if d:
            tree[parent(d)][0].append(d)
    for p in files:
        tree[parent(p)][1].append(p.rpartition('\\')[2])
    for subs, names in tree.values():
        subs.sort(key=str.lower)
        names.sort(key=str.lower)

    # Name table: depth-first (dir, its files, its subdirs).
    names, offsets = bytearray(), {}

    def name_off(s):
        if s not in offsets:
            offsets[s] = len(names)
            names.extend(pad4(s.encode() + b'\0'))

    def walk(d):
        name_off(d)
        for f in tree[d][1]:
            name_off(f)
        for s in tree[d][0]:
            walk(s)
    walk('')

    # Directory table: sorted by full path, case-insensitively; file table and data follow it.
    dirs = sorted(tree, key=str.lower)
    index = {d: i for i, d in enumerate(dirs)}
    order = [f'{d}\\{f}' if d else f for d in dirs for f in tree[d][1]]
    order.sort(key=lambda p: p.lower().startswith('crc\\'))  # retail stores the CRC file's data last
    data_off = 48 + len(names) + 32 * len(files) + 16 * len(dirs)
    pos = {}
    for p in order:
        pos[p] = data_off
        data_off += files[p]

    ftab, dtab = bytearray(), bytearray()
    for d in dirs:
        subs, fnames = tree[d]
        sibs = tree[parent(d)][0] if d else []
        nxt = sibs.index(d) + 1 if d else 0
        dtab += struct.pack('<IiiI', offsets[d], index[subs[0]] if subs else -1,
                            index[sibs[nxt]] if d and nxt < len(sibs) else -1, len(fnames))
        for f in fnames:
            p = f'{d}\\{f}' if d else f
            ftab += struct.pack('<IQQQI', offsets[f], pos[p], files[p], files[p], 0)

    head = struct.pack('<4s7I', b'LTAR', 3, len(names), len(dirs), len(files), 1, 0, 1)
    return head, bytes(names) + bytes(ftab) + bytes(dtab), order


def build(src, out):
    files = {}
    for root, _, names in os.walk(src):
        for n in names:
            rel = os.path.relpath(os.path.join(root, n), src).replace(os.sep, '\\')
            files[rel] = os.path.getsize(os.path.join(root, n))
    head, tables, order = layout(files)
    data = b''.join(open(os.path.join(src, *p.split('\\')), 'rb').read() for p in order)
    guid = hashlib.md5(tables + data).digest()  # deterministic, so identical inputs give identical archives
    with open(out, 'wb') as f:
        f.write(head + guid + tables + data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2])
