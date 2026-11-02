#!/usr/bin/env python3
"""T3DOTM build helper: fold the NRO rodata segment into the data segment.

Why: the recompiled image carries ~596 RELR (pack-relative-relocs) targets
inside the .rodata region. libnx's __nx_dynamic applies RELR writes without
mprotecting the target pages, and NRO loaders (hbmenu) map the NRO "rodata"
segment read-only by fixed policy (the NRO header has no per-segment
permission bits). Any write to a read-only mapping aborts with a Data Abort
before SDK constructors run — i.e. no logs at all.

The ELF link script already marks the .rodata PT_LOAD Read|Write so the
linker keeps the layout unchanged; this step mirrors that at the NRO level:
the header's rodata fields are folded into data, so loaders map the whole
region Read|Write. On-disk bytes and total mapped size stay unchanged.
"""
import struct
import sys


def main(path):
    with open(path, "r+b") as f:
        f.seek(0x10)
        if f.read(4) != b"NRO0":
            sys.exit("error: %s: not an NRO (missing NRO0 magic)" % path)

        def rd(off):
            f.seek(off)
            return struct.unpack("<I", f.read(4))[0]

        ro_off = rd(0x28)
        ro_size = rd(0x2C)
        d_off = rd(0x30)
        d_size = rd(0x34)
        print("NRO before: rodata off=%#x size=%#x; data off=%#x size=%#x" %
              (ro_off, ro_size, d_off, d_size))

        if ro_size == 0:
            print("NRO: rodata segment already folded into data; nothing to do")
            return 0
        if d_off != ro_off + ro_size:
            sys.exit("error: unexpected layout (ro_off+ro_size %#x != data_off %#x)" %
                     (ro_off + ro_size, d_off))

        new_d_size = d_size + ro_size
        f.seek(0x2C)
        f.write(struct.pack("<I", 0))          # rodata_size = 0
        f.seek(0x30)
        f.write(struct.pack("<I", ro_off))     # data_off = old rodata_off
        f.seek(0x34)
        f.write(struct.pack("<I", new_d_size))  # data_size += old rodata_size
        f.flush()
        print("NRO after : rodata size=0; data off=%#x size=%#x (end %#x)" %
              (ro_off, new_d_size, ro_off + new_d_size))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: nropatch_rodata.py <file.nro>")
    sys.exit(main(sys.argv[1]))