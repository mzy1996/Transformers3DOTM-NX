#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""xex_dump.py - Decrypt an Xbox 360 XEX2 (BASIC compression) to a flat image
and dump PPC instruction bytes at guest virtual addresses.

Mirrors the decryption path in tools/xex_delta_patch/src/main.cpp
(xex_patch.exe). Output is the same flat image (starts with a decrypted PE
MZ header), so guest address = load_address + offset_in_flat.
"""
import argparse
import struct
import sys

from Crypto.Cipher import AES

XEX2_RETAIL_KEY = bytes([
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91,
])

# security info field offsets
SEC_OFF_IMAGE_SIZE = 0x04
SEC_OFF_AES_KEY = 0x150
SEC_OFF_IMAGE_FLAGS = 0x10C
SEC_OFF_LOAD_ADDRESS = 0x110
SEC_OFF_EXPORT_TABLE = 0x160
SEC_OFF_PAGE_DESC_COUNT = 0x180
SEC_OFF_PAGE_DESCRIPTORS = 0x184


def u16(b, off):
    return struct.unpack_from('>H', b, off)[0]


def u32(b, off):
    return struct.unpack_from('>I', b, off)[0]


def aes_decrypt_cbc(key, data):
    return AES.new(key, AES.MODE_CBC, b'\x00' * 16).decrypt(data)


def parse_xex(path):
    data = open(path, 'rb').read()
    if data[:4] != b'XEX2':
        raise ValueError('not an XEX2 file')
    header_size = u32(data, 0x08)
    security_offset = u32(data, 0x10)
    header_count = u32(data, 0x14)
    if security_offset + 0x184 > header_size:
        raise ValueError('security info out of range')
    opts = {}
    for i in range(header_count):
        o = 0x18 + i * 8
        k = u32(data, o)
        v = u32(data, o + 4)
        opts[k] = v
    return data, header_size, security_offset, opts


def decrypt_flat(data, header_size, security_offset, opts):
    ffi_off = opts.get(0x000003FF)
    if ffi_off is None:
        raise ValueError('FILE_FORMAT_INFO missing')
    info_size = u32(data, ffi_off)
    enc = u16(data, ffi_off + 4)
    comp = u16(data, ffi_off + 6)
    print(f'[xex] header=0x{header_size:X} enc={enc} comp={comp} info_size=0x{info_size:X}',
          file=sys.stderr)
    if comp != 1:
        raise ValueError(f'compression {comp} != BASIC(1)')

    sec = security_offset
    image_size = u32(data, sec + SEC_OFF_IMAGE_SIZE)
    aes_key = bytes(data[sec + SEC_OFF_AES_KEY: sec + SEC_OFF_AES_KEY + 16])
    load_address = u32(data, sec + SEC_OFF_LOAD_ADDRESS)
    page_count = u32(data, sec + SEC_OFF_PAGE_DESC_COUNT)
    print(f'[xex] image_size=0x{image_size:X} load=0x{load_address:X} '
          f'pages={page_count} aes={aes_key.hex().upper()}', file=sys.stderr)

    session_key = aes_decrypt_cbc(XEX2_RETAIL_KEY, aes_key)

    payload = data[header_size:]
    flat = bytearray()
    pd_base = sec + SEC_OFF_PAGE_DESCRIPTORS
    chunks = []
    for i in range(page_count):
        v = u32(data, pd_base + i * 0x18)
        info = v & 0xF
        page_count_n = v >> 4
        chunks.append((page_count_n, info))
    total = sum(n * 0x10000 for n, _ in chunks)
    print(f'[xex] page-desc total=0x{total:X} ({"OK" if total == image_size else "WARN != image_size"})',
          file=sys.stderr)

    off = 0
    p = 0
    for n, info in chunks:
        for _ in range(n):
            if info & 0x1:
                data_size = u32(data, pd_base + 0x8 + 0)
                zero_size = u32(data, pd_base + 0x8 + 4)
            else:
                data_size = u32(data, pd_base + 0x8 + 0)
                zero_size = u32(data, pd_base + 0x8 + 4)
            # page descriptor layout used by xex_patch: after count comes
            # (data_size, zero_size) pairs; re-scan from a fixed table.
            break
        break
    # The above loop is a placeholder; real logic below reads the block table
    # from the FFI (info_size - 8) / 8 entries.
    return None


def decrypt_flat_v2(data, header_size, security_offset, opts):
    ffi_off = opts.get(0x000003FF)
    info_size = u32(data, ffi_off)
    enc = u16(data, ffi_off + 4)
    comp = u16(data, ffi_off + 6)
    if comp != 1 and comp != 0:
        raise ValueError(f'compression {comp} != BASIC(1)/NONE(0)')

    sec = security_offset
    image_size = u32(data, sec + SEC_OFF_IMAGE_SIZE)
    aes_key = bytes(data[sec + SEC_OFF_AES_KEY: sec + SEC_OFF_AES_KEY + 16])
    load_address = u32(data, sec + SEC_OFF_LOAD_ADDRESS)

    session_key = aes_decrypt_cbc(XEX2_RETAIL_KEY, aes_key)
    payload = data[header_size:]

    # basic block table: (info_size - 8) / 8 entries after the 8-byte FFI hdr
    block_count = (info_size - 8) // 8
    blocks = ffi_off + 8
    flat = bytearray(image_size)
    p = 0
    d = 0
    for n in range(block_count):
        data_size = u32(data, blocks + n * 8)
        zero_size = u32(data, blocks + n * 8 + 4)
        if p + data_size > len(payload) or d + data_size > len(flat):
            raise ValueError(f'block {n} out of range')
        chunk = payload[p:p + data_size]
        if enc == 1:
            out = aes_decrypt_cbc(session_key, chunk)
        else:
            out = chunk
        flat[d:d + len(out)] = out
        p += data_size
        d += data_size + zero_size
    if flat[:2] != b'MZ':
        raise ValueError('decrypted image has no MZ header (bad key?)')
    return bytes(flat), load_address


def ppc_disasm(insn):
    """Minimal PPC decode used for eyeballing dump bytes."""
    op = (insn >> 26) & 0x3F
    names = {
        14: 'addi', 15: 'addis', 16: 'bc', 18: 'b', 20: 'rlwimi',
        21: 'rlwinm', 23: 'rlwnm', 24: 'ori', 25: 'oris', 26: 'xori',
        28: 'and', 31: 'X-form', 32: 'lwz', 33: 'lwzu', 34: 'lbz',
        35: 'lbzu', 36: 'stw', 37: 'stwu', 38: 'stb', 40: 'lhz',
        42: 'lha', 44: 'sth', 46: 'lmw', 47: 'stmw', 48: 'lfs',
        50: 'lfd', 52: 'stfs', 54: 'stfd', 58: 'D-form', 60: 'D-form',
        62: 'D-form',
    }
    name = names.get(op, f'op{op}')
    if op == 18:  # b / bl / bctr...
        li = (insn >> 2) & 0x3FFFF
        if li & 0x20000:
            li -= 0x40000
        aa = (insn >> 1) & 1
        lk = insn & 1
        return f'b{"l" if lk else ""} 0x{li * 4:07X}{"+AA" if aa else ""} (imm)'
    if op == 16:
        bo = (insn >> 21) & 0x1F
        bi = (insn >> 16) & 0x1F
        bd = (insn >> 2) & 0x3FFF
        if bd & 0x2000:
            bd -= 0x4000
        lk = insn & 1
        return f'bc{"l" if lk else ""} bo={bo} bi={bi} +0x{bd * 4:05X}'
    if op == 14:
        rt = (insn >> 21) & 0x1F
        ra = (insn >> 16) & 0x1F
        simm = insn & 0xFFFF
        if simm & 0x8000:
            simm -= 0x10000
        return f'addi r{rt}, r{ra}, {simm}'
    if op == 31:
        xop = (insn >> 1) & 0x3FF
        r = {266: 'add', 444: 'or', 23: 'lwzx', 87: 'lbzx', 24: 'lwzux',
             39: 'lbzux', 144: 'mtcrf', 339: 'mfspr', 467: 'mtspr',
             982: 'icbi', 986: 'dcbi', 0: 'cmp', 32: 'cmpl', 54: 'dcbst',
             150: 'stwcx', 151: 'stwx', 215: 'stbx', 72: 'mulhd', 73: 'mulhdu',
             75: 'mulhw', 76: 'mulhwu', 233: 'mullw', 235: 'mullw.', 20: 'lwarx',
             340: 'mcrxr', 21: 'lwzx', 52: 'lhzx', 116: 'lhzux', 84: 'lhax',
             87: 'lbzx', 119: 'lbzux', 151: 'stwx', 183: 'stwx.', 199: 'stbx',
             247: 'sthx', 279: 'sthx.', 534: 'extsh', 922: 'extsh.', 266: 'add',
             778: 'add.', 233: 'mullw', 235: 'mullw.', 40: 'subf', 552: 'subf.',
             105: 'subfc', 617: 'subfc.'}
        return f'{r.get(xop, f"xop{xop}")} (X-form)'
    if op == 32:
        rt = (insn >> 21) & 0x1F
        ra = (insn >> 16) & 0x1F
        d = insn & 0xFFFF
        if d & 0x8000:
            d -= 0x10000
        return f'lwz r{rt}, {d}(r{ra})'
    if op == 36:
        rt = (insn >> 21) & 0x1F
        ra = (insn >> 16) & 0x1F
        d = insn & 0xFFFF
        if d & 0x8000:
            d -= 0x10000
        return f'stw r{rt}, {d}(r{ra})'
    return name


def main():
    ap = argparse.ArgumentParser(description='Decrypt XEX2 flat image and dump PPC bytes')
    ap.add_argument('xex', help='default.xex')
    ap.add_argument('--out', help='write flat image to file')
    ap.add_argument('--dump', help='guest virtual address(es), comma separated (e.g. 0x83287440)')
    ap.add_argument('--len', type=lambda s: int(s, 0), default=0x40, help='bytes to dump per address')
    args = ap.parse_args()

    data, header_size, security_offset, opts = parse_xex(args.xex)
    flat, load = decrypt_flat_v2(data, header_size, security_offset, opts)
    print(f'[xex] flat={len(flat)} bytes, load=0x{load:X}', file=sys.stderr)

    if args.out:
        with open(args.out, 'wb') as f:
            f.write(flat)
        print(f'[xex] wrote {args.out} ({len(flat)} bytes)')

    if args.dump:
        for a in args.dump.split(','):
            addr = int(a.strip(), 0)
            off = addr - load
            print(f'--- 0x{addr:X} (flat +0x{off:X}) ---')
            if off < 0 or off + args.len > len(flat):
                print('  (out of range)')
                continue
            chunk = flat[off:off + args.len]
            for i in range(0, len(chunk), 4):
                insn = struct.unpack_from('>I', chunk, i)[0]
                print(f'  {off + i:08X}  {insn:08X}  {ppc_disasm(insn)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())