"""Minimal CHM (ITSF) directory reader: lists internal file entries without
decompressing. Used by the test harness to assert expected structures exist.
"""
import struct


def read_dir(path):
    """Returns {name: (section, offset, length)} for every entry in the CHM."""
    data = open(path, "rb").read()
    if data[:4] != b"ITSF":
        raise ValueError("not an ITSF file: %s" % path)
    hs1_off = struct.unpack_from("<Q", data, 0x48)[0]
    if data[hs1_off:hs1_off + 4] != b"ITSP":
        raise ValueError("bad ITSP directory")
    chunk_size = struct.unpack_from("<I", data, hs1_off + 0x10)[0]
    n_chunks = struct.unpack_from("<I", data, hs1_off + 0x2C)[0]
    dir_base = hs1_off + 0x54

    def encint(buf, p):
        v = 0
        while True:
            b = buf[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if not (b & 0x80):
                break
        return v, p

    entries = {}
    for c in range(n_chunks):
        base = dir_base + c * chunk_size
        if data[base:base + 4] != b"PMGL":
            continue
        free = struct.unpack_from("<I", data, base + 4)[0]
        p = base + 0x14
        end = base + chunk_size - free
        while p < end:
            nlen, p = encint(data, p)
            name = data[p:p + nlen].decode("utf-8", "replace")
            p += nlen
            sec, p = encint(data, p)
            off, p = encint(data, p)
            ln, p = encint(data, p)
            entries[name] = (sec, off, ln)
    return entries
