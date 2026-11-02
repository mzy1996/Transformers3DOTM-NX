#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import struct
import sys

TARGET_SYMBOLS = {
    b'pthread_setspecific',
    b'pthread_getspecific',
    b'threadTlsSet',
    b'threadTlsGet',
    b'__cxa_get_globals',
}

def parse_elf64_symbols(data, base_offset=0x10):
    """Parse ELF64 symbol table from raw bytes."""
    # ELF header
    e_ident = data[base_offset:base_offset+16]
    if e_ident[:4] != b'\x7fELF':
        print(f"Not an ELF file at offset {base_offset}, magic: {e_ident[:4]!r}")
        return {}

    ei_class = e_ident[4]
    if ei_class != 2:  # ELFCLASS64
        print(f"Not ELF64: class={ei_class}")
        return {}

    ei_data = e_ident[5]
    if ei_data == 1:
        endian = '<'
    elif ei_data == 2:
        endian = '>'
    else:
        print(f"Unknown endian: {ei_data}")
        return {}

    # Read ELF header fields
    eh = base_offset + 16
    e_type, e_machine = struct.unpack_from(endian + 'HH', data, eh)
    e_version = struct.unpack_from(endian + 'I', data, eh + 4)[0]
    e_entry = struct.unpack_from(endian + 'Q', data, eh + 8)[0]
    e_phoff = struct.unpack_from(endian + 'Q', data, eh + 16)[0]
    e_shoff = struct.unpack_from(endian + 'Q', data, eh + 24)[0]
    e_flags = struct.unpack_from(endian + 'I', data, eh + 32)[0]
    e_ehsize = struct.unpack_from(endian + 'H', data, eh + 36)[0]
    e_phentsize = struct.unpack_from(endian + 'H', data, eh + 38)[0]
    e_phnum = struct.unpack_from(endian + 'H', data, eh + 40)[0]
    e_shentsize = struct.unpack_from(endian + 'H', data, eh + 42)[0]
    e_shnum = struct.unpack_from(endian + 'H', data, eh + 44)[0]
    e_shstrndx = struct.unpack_from(endian + 'H', data, eh + 46)[0]

    print(f"ELF64: machine={e_machine} (0xB5=AArch64), sections={e_shnum}, shoff={e_shoff:#x}")
    print(f"  phoff={e_phoff:#x}, phnum={e_phnum}, shstrndx={e_shstrndx}")

    if e_shnum == 0 or e_shoff == 0:
        print("No section headers")
        return {}

    # Read section headers
    sections = []
    for i in range(e_shnum):
        off = base_offset + e_shoff + i * e_shentsize
        sh_name = struct.unpack_from(endian + 'I', data, off)[0]
        sh_type = struct.unpack_from(endian + 'I', data, off + 4)[0]
        sh_flags = struct.unpack_from(endian + 'Q', data, off + 8)[0]
        sh_addr = struct.unpack_from(endian + 'Q', data, off + 16)[0]
        sh_offset = struct.unpack_from(endian + 'Q', data, off + 24)[0]
        sh_size = struct.unpack_from(endian + 'Q', data, off + 32)[0]
        sh_link = struct.unpack_from(endian + 'I', data, off + 40)[0]
        sh_info = struct.unpack_from(endian + 'I', data, off + 44)[0]
        sections.append({
            'name_idx': sh_name,
            'type': sh_type,
            'flags': sh_flags,
            'addr': sh_addr,
            'offset': sh_offset,
            'size': sh_size,
            'link': sh_link,
            'info': sh_info,
        })

    # Find .shstrtab
    if e_shstrndx >= e_shnum:
        print(f"Invalid shstrndx: {e_shstrndx}")
        return {}

    shstrtab = sections[e_shstrndx]
    shstrtab_data = data[base_offset + shstrtab['offset']:base_offset + shstrtab['offset'] + shstrtab['size']]

    def get_section_name(idx):
        end = shstrtab_data.find(b'\x00', idx)
        if end == -1:
            end = len(shstrtab_data)
        return shstrtab_data[idx:end]

    symtab = None
    strtab = None
    for sec in sections:
        name = get_section_name(sec['name_idx'])
        if name == b'.symtab' and sec['type'] == 2:  # SHT_SYMTAB
            symtab = sec
        elif name == b'.strtab' and sec['type'] == 3:  # SHT_STRTAB
            strtab = sec

    print(f"Found symtab: {symtab is not None}, strtab: {strtab is not None}")

    if not symtab or not strtab:
        return {}

    # Read .strtab
    strtab_data = data[base_offset + strtab['offset']:base_offset + strtab['offset'] + strtab['size']]

    # Read symbols
    symbols = {}
    sym_count = symtab['size'] // 24  # ELF64_Sym size
    for i in range(sym_count):
        off = base_offset + symtab['offset'] + i * 24
        st_name = struct.unpack_from(endian + 'I', data, off)[0]
        st_info = data[off + 4]
        st_other = data[off + 5]
        st_shndx = struct.unpack_from(endian + 'H', data, off + 6)[0]
        st_value = struct.unpack_from(endian + 'Q', data, off + 8)[0]
        st_size = struct.unpack_from(endian + 'Q', data, off + 16)[0]

        if st_name == 0:
            continue
        name_end = strtab_data.find(b'\x00', st_name)
        if name_end == -1:
            name_end = len(strtab_data)
        name = strtab_data[st_name:name_end]

        # Filter target symbols
        if name in TARGET_SYMBOLS:
            bind = st_info >> 4
            type_ = st_info & 0xF
            symbols[name.decode('utf-8', errors='replace')] = {
                'value': st_value,
                'size': st_size,
                'bind': bind,
                'type': type_,
                'shndx': st_shndx,
            }

    return symbols


if __name__ == '__main__':
    nro_path = sys.argv[1] if len(sys.argv) > 1 else 'J:/XBOX360 port/Transformers Dark of the Moon/recomp/out/sw8/T3DOTM.nro'
    with open(nro_path, 'rb') as f:
        data = f.read()

    # Check NRO magic
    if data[:4] == b'NRO0':
        print(f"NRO file detected, size={len(data)} bytes")
        print(f"NRO header: {data[:16].hex()}")
        base = 0x10
    else:
        print("No NRO header, assuming raw ELF")
        base = 0

    symbols = parse_elf64_symbols(data, base)
    if symbols:
        print("\n=== Found Target Symbols ===")
        for name, info in sorted(symbols.items()):
            print(f"{name}: addr={info['value']:#018x}, size={info['size']}, bind={info['bind']}, type={info['type']}, shndx={info['shndx']}")
    else:
        print("\nNo target symbols found in ELF symbol table.")
