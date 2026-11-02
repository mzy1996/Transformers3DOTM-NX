import struct, sys, hashlib

def parse_pe(path):
    d = open(path, "rb").read()
    if d[:2] != b"MZ":
        return None, None
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e_lfanew:e_lfanew+4] != b"PE\0\0":
        return None, None
    coff = e_lfanew + 4
    machine, nsec, timestamp, ptr_sym, nsym, opt_size, chars = struct.unpack_from(
        "<HHIIIHH", d, coff)
    opt = coff + 20
    magic = struct.unpack_from("<H", d, opt)[0]  # 0x10b PE32 / 0x20b PE32+
    if magic == 0x10b:
        (entry, base) = struct.unpack_from("<II", d, opt+16)
        is64 = False
    else:
        (entry, base) = struct.unpack_from("<IQ", d, opt+16)
        is64 = True
    sects = []
    sec_off = opt + opt_size
    for i in range(nsec):
        name = d[sec_off+i*40: sec_off+i*40+8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, sec_off+i*40+8)
        sects.append((name, vaddr, vsize, raddr, rsize))
    return dict(machine=machine, nsec=nsec, chars=chars, opt_size=opt_size,
                magic=magic, entry=entry, base=base, is64=is64, sects=sects,
                pe_off=e_lfanew), d

def main():
    base_flat, base_d = parse_pe("extracted/base_default.flat")
    tu_flat, tu_d = parse_pe(sys.argv[1] if len(sys.argv) > 1 else "extracted/TU/default_tu.flat")
    for tag, f in (("base", base_flat), ("TU", tu_flat)):
        if f is None:
            print(f"{tag}: NOT PE")
            continue
        print(f"{tag}: machine=0x{f['machine']:x} nsec={f['nsec']} opt=0x{f['opt_size']:x} "
              f"entry=0x{f['entry']:X} base=0x{f['base']:X} is64={f['is64']}")
        for s in f['sects']:
            print(f"  {tag} sect {s[0]:8s} vaddr=0x{s[1]:X} vsize=0x{s[2]:X} "
                  f"raddr=0x{s[3]:X} rsize=0x{s[4]:X}")
    assert base_d and tu_d
    print("=== size ===")
    print(f"base: {len(base_d)} TU: {len(tu_d)} grown={len(tu_d)-len(base_d):+d}")
    print("=== diff ===")
    n = min(len(base_d), len(tu_d))
    diffs = []
    for off in range(0, n, 0x1000):
        a = base_d[off:off+0x1000]
        b = tu_d[off:off+0x1000]
        if a != b:
            diffs.append((off, len(a)))
    # aggregate contiguous
    merged = []
    for off, ln in diffs:
        if merged and off == merged[-1][0] + merged[-1][1]:
            merged[-1] = (merged[-1][0], merged[-1][1] + ln)
        else:
            merged.append([off, ln])
    tot = sum(ln for _, ln in merged)
    print(f"changed regions: {len(merged)} total_changed=0x{tot:X} ({tot/0x10000:.2f} pages)")
    for off, ln in merged[:40]:
        print(f"  0x{off:08X} +0x{ln:X}")
    # SHA1 of whole images
    print(f"sha1 base: {hashlib.sha1(base_d).hexdigest()}")
    print(f"sha1 TU  : {hashlib.sha1(tu_d).hexdigest()}")

if __name__ == "__main__":
    main()