#!/usr/bin/env python3
"""Analyze an Xbox 360 XEX2 executable using rexglue-sdk's exact struct layouts."""
import struct
import sys

def u32(b, off):
    if off < 0 or off + 4 > len(b):
        return 0
    return struct.unpack_from('>I', b, off)[0]

def u16(b, off):
    if off < 0 or off + 2 > len(b):
        return 0
    return struct.unpack_from('>H', b, off)[0]

def cstr(b, off):
    end = b.find(b'\x00', off)
    if end < 0: end = len(b)
    return b[off:end].decode('latin1', 'replace')

HEADER_KEYS = {
    0x000002FF: 'RESOURCE_INFO', 0x000003FF: 'FILE_FORMAT_INFO',
    0x000005FF: 'DELTA_PATCH_DESCRIPTOR', 0x00000405: 'BASE_REFERENCE',
    0x000080FF: 'BOUNDING_PATH', 0x00008105: 'DEVICE_ID',
    0x00010001: 'ORIGINAL_BASE_ADDRESS', 0x00010100: 'ENTRY_POINT',
    0x00010201: 'IMAGE_BASE_ADDRESS', 0x000103FF: 'IMPORT_LIBRARIES',
    0x00018002: 'CHECKSUM_TIMESTAMP', 0x00018102: 'ENABLED_FOR_CALLCAP',
    0x00018200: 'ENABLED_FOR_FASTCAP', 0x000183FF: 'ORIGINAL_PE_NAME',
    0x000200FF: 'STATIC_LIBRARIES', 0x00020104: 'TLS_INFO',
    0x00020200: 'DEFAULT_STACK_SIZE', 0x00020301: 'DEFAULT_FILESYSTEM_CACHE_SIZE',
    0x00020401: 'DEFAULT_HEAP_SIZE', 0x00028002: 'PAGE_HEAP_SIZE_AND_FLAGS',
    0x00030000: 'SYSTEM_FLAGS', 0x00040006: 'EXECUTION_INFO',
    0x00040201: 'TITLE_WORKSPACE_SIZE', 0x00040310: 'GAME_RATINGS',
    0x00040404: 'LAN_KEY', 0x000405FF: 'XBOX360_LOGO',
    0x000406FF: 'MULTIDISC_MEDIA_IDS', 0x000407FF: 'ALTERNATE_TITLE_IDS',
    0x00040801: 'ADDITIONAL_TITLE_MEMORY', 0x00E10402: 'EXPORTS_BY_NAME',
}

PLATFORMS = {0x80: 'Xbox 360', 0x68: 'Xbox 1', 0x00: 'PC'}
COMPRESSION = {0: 'NONE', 1: 'BASIC', 2: 'NORMAL', 3: 'DELTA'}
ENCRYPTION = {0: 'NONE', 1: 'NORMAL'}

def fmt_ver(v):
    if v is None: return '?'
    return f'{v>>28}.{(v>>24)&0xF}.{(v>>8)&0xFFFF}.{v&0xFF}'

def parse(path):
    b = open(path, 'rb').read()
    print(f'== {path}  ({len(b)} bytes) ==')
    if b[:4] != b'XEX2':
        print('NOT XEX2'); return
    module_flags = u32(b, 4); header_size = u32(b, 8)
    security_offset = u32(b, 0x10); header_count = u32(b, 0x14)
    print(f'module_flags=0x{module_flags:08X} header_size=0x{header_size:X} '
          f'security_offset=0x{security_offset:X} header_count={header_count}')

    headers = []
    for i in range(header_count):
        off = 0x18 + i * 8
        key = u32(b, off); val = u32(b, off + 4)
        headers.append((key, val))
        kname = HEADER_KEYS.get(key, f'Key{key:08X}')
        low = key & 0xFF
        if low == 0:
            print(f'  opt[{i:02d}] {kname:26s} key=0x{key:08X} value=0x{val:08X}')
        elif low == 1:
            v = u32(b, val)
            if v is not None:
                print(f'  opt[{i:02d}] {kname:26s} key=0x{key:08X} -> value@0x{val:X} = 0x{v:08X}')
            else:
                print(f'  opt[{i:02d}] {kname:26s} key=0x{key:08X} value(direct)=0x{val:08X}')
        else:
            print(f'  opt[{i:02d}] {kname:26s} key=0x{key:08X} -> data@0x{val:X}')

    # ---- execution info ----
    exe_off = next((v for k, v in headers if k == 0x00040006), None)
    if exe_off:
        media_id = u32(b, exe_off); version = u32(b, exe_off + 4)
        base_ver = u32(b, exe_off + 8); title_id = u32(b, exe_off + 0xC)
        platform = b[exe_off + 0x10]; disc_num = b[exe_off + 0x12]; disc_cnt = b[exe_off + 0x13]
        savegame = u32(b, exe_off + 0x14)
        print(f'\nExecutionInfo: title_id=0x{title_id:08X} media_id=0x{media_id:08X} '
              f'version={fmt_ver(version)} base={fmt_ver(base_ver)} '
              f'platform={PLATFORMS.get(platform, hex(platform))} '
              f'disc={disc_num}/{disc_cnt} savegame=0x{savegame:08X}')

    # ---- file format info ----
    ff_off = next((v for k, v in headers if k == 0x000003FF), None)
    if ff_off:
        info_size = u32(b, ff_off); enc = u16(b, ff_off + 4); comp = u16(b, ff_off + 6)
        print(f'\nFileFormat: size=0x{info_size:X} encryption={ENCRYPTION.get(enc, enc)} '
              f'compression={COMPRESSION.get(comp, comp)}')
        if comp == 2:
            win = u32(b, ff_off + 8)
            print(f'  LZX window_size=0x{win:X} (2^{win.bit_length()-1}) blocks:')
            # normal compression: window_size + first block {block_size, hash[20]}
            boff = ff_off + 12
            total = 0
            while boff + 24 <= info_size + ff_off and total < 0x1000:
                bsize = u32(b, boff)
                if not bsize: break
                total += bsize
                boff += 24
            print(f'  decompressed size ~0x{total:X} bytes')
        if comp == 1:
            boff = ff_off + 8
            total = 0
            while boff + 8 <= info_size + ff_off and total < 0x1000000:
                dsize = u32(b, boff); zsize = u32(b, boff + 4)
                total += dsize
                if zsize == 0: break
                boff += 8
            print(f'  decompressed size ~0x{total:X} bytes')

    # ---- security info ----
    print(f'\nSecurityInfo @ 0x{security_offset:X}:')
    sec_size = u32(b, security_offset); image_size = u32(b, security_offset + 4)
    image_flags = u32(b, security_offset + 0x10C); load_addr = u32(b, security_offset + 0x110)
    import_table_count = u32(b, security_offset + 0x128)
    xgd2_media_id = b[security_offset + 0x140: security_offset + 0x150].hex()
    aes_key = b[security_offset + 0x150: security_offset + 0x160].hex()
    export_table = u32(b, security_offset + 0x160)
    region = u32(b, security_offset + 0x178); allowed_media = u32(b, security_offset + 0x17C)
    page_desc_count = u32(b, security_offset + 0x180)
    print(f'  sec_size=0x{sec_size:X} image_size=0x{image_size:X} image_flags=0x{image_flags:08X}')
    print(f'  load_address=0x{load_addr:08X} import_table_count={import_table_count}')
    print(f'  xgd2_media_id={xgd2_media_id}')
    print(f'  aes_key={aes_key}')
    print(f'  export_table=0x{export_table:08X} region=0x{region:08X} allowed_media=0x{allowed_media:08X}')
    print(f'  page_descriptor_count={page_desc_count}')
    page_size = 0x1000 if (image_flags & 0x10000000) else 0x10000  # XEX_IMAGE_PAGE_SIZE_4KB
    total_pages = 0; code_pages = 0; data_pages = 0; ro_pages = 0
    names = {1: 'CODE', 2: 'DATA', 3: 'RODATA'}
    print(f'  assumed page_size=0x{page_size:X} ({"4KB" if page_size == 0x1000 else "64KB"})')
    for i in range(page_desc_count):
        off = security_offset + 0x184 + i * 0x18
        # Disk bytes are big-endian. rexglue does: desc.value = byte_swap(be_value)
        # with be<> auto-swapping on access, netting: info = raw & 0xF, page_count = raw >> 4.
        raw = u32(b, off)
        info = raw & 0xF
        page_count = raw >> 4
        addr = load_addr + total_pages * page_size
        total_pages += page_count
        if info == 1: code_pages += page_count
        elif info == 2: data_pages += page_count
        elif info == 3: ro_pages += page_count
        if i < 8 or i > page_desc_count - 5 or page_count != 1:
            print(f'    page[{i:02d}] type={names.get(info, info)} pages={page_count} '
                  f'(@0x{addr:08X}-0x{load_addr + total_pages * page_size - 1:08X})')
    print(f'  total pages={total_pages} (0x{total_pages * page_size:X} bytes) '
          f'code={code_pages} data={data_pages} rodata={ro_pages}')
    print(f'  section end check: base+image_size=0x{load_addr + image_size:08X}')

    # ---- import libraries ----
    for k, v in headers:
        if k == 0x000103FF:
            off = v
            total = u32(b, off); st_size = u32(b, off + 4); st_count = u32(b, off + 8)
            print(f'\nImportLibraries @0x{off:X} total_size=0x{total:X} '
                  f'string_table(count={st_count} size=0x{st_size:X})')
            strings = []
            i = 0; o = 0
            while o < st_count and i < st_size:
                s = cstr(b, off + 12 + i)
                strings.append(s)
                i += len(s) + 1
                if i % 4: i += 4 - (i % 4)
                o += 1
            lib_off = 12 + st_size
            while lib_off < total:
                lib_size = u32(b, off + lib_off)
                if not lib_size: break
                lib_id = u32(b, off + lib_off + 0x18)
                lib_ver = u32(b, off + lib_off + 0x1C)
                lib_min = u32(b, off + lib_off + 0x20)
                name_idx = u16(b, off + lib_off + 0x24) & 0xFF
                count = u16(b, off + lib_off + 0x26)
                name = strings[name_idx] if name_idx < len(strings) else f'str{name_idx}'
                print(f'  lib "{name}" id=0x{lib_id:08X} ver={fmt_ver(lib_ver)} '
                      f'min={fmt_ver(lib_min)} imports={count}')
                lib_off += lib_size
            break

    # ---- resource info ----
    for k, v in headers:
        if k == 0x000002FF:
            off = v
            size = u32(b, off)
            n = (size - 4) // 16
            print(f'\nResources @0x{off:X} count={n}:')
            for i in range(n):
                ro = off + 4 + i * 16
                name = cstr(b, ro)
                addr = u32(b, ro + 8); size_r = u32(b, ro + 12)
                print(f'    {name or "?":8s} addr=0x{addr:08X} size=0x{size_r:X}')
            break

    # ---- PE original name ----
    for k, v in headers:
        if k == 0x000183FF:
            size = u32(b, v)
            print(f'\nOriginalPEName: {cstr(b, v + 4)}')
            break

if __name__ == '__main__':
    parse(sys.argv[1])