import struct
import sys
import os

def verify_nro(path):
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        data = f.read(0x100)
    
    magic = struct.unpack('<I', data[0:4])[0]
    mod0_off = struct.unpack('<I', data[4:8])[0]
    ident = data[8:16]
    
    if magic != 0x14000020:
        print(f"FAIL: Not a new-homebrew NRO (magic={magic:#x})")
        return False
    
    if ident != b'HOMEBREW':
        print(f"WARN: HOMEBREW identifier missing ({ident})")
    
    nro_magic = data[0x10:0x14]
    if nro_magic != b'NRO0':
        print(f"FAIL: NRO0 missing at 0x10 ({nro_magic})")
        return False
    
    version = struct.unpack('<I', data[0x14:0x18])[0]
    nro_size = struct.unpack('<I', data[0x18:0x1c])[0]
    flags = struct.unpack('<I', data[0x1c:0x20])[0]
    
    segments = []
    for i, off in enumerate([0x20, 0x28, 0x30]):
        foff = struct.unpack('<I', data[off:off+4])[0]
        mem = struct.unpack('<I', data[off+4:off+8])[0]
        segments.append((foff, mem))
    
    asset_off = struct.unpack('<I', data[0x38:0x3c])[0]
    asset_sz = struct.unpack('<I', data[0x3c:0x40])[0]
    
    print(f"File: {path}")
    print(f"File size: {size:#x} ({size} bytes)")
    print(f"NRO header size: {nro_size:#x} ({nro_size} bytes)")
    print(f"Flags: {flags:#x}")
    print("Segments:")
    names = ['text', 'rodata', 'data']
    ok = True
    for i, (foff, mem) in enumerate(segments):
        status = "OK" if mem > 0 else "FAIL: size=0"
        if mem == 0:
            ok = False
        print(f"  [{i}] {names[i]:6s}: file_off={foff:#010x}, mem_size={mem:#010x} ({mem} bytes) [{status}]")
    
    if nro_size > size:
        print(f"FAIL: nro_size ({nro_size:#x}) > file size ({size:#x})")
        ok = False
    
    # Check segment consistency
    text_off, text_sz = segments[0]
    ro_off, ro_sz = segments[1]
    data_off, data_sz = segments[2]
    
    if ro_off != text_off + text_sz:
        print(f"WARN: rodata file_off ({ro_off:#x}) != text end ({text_off+text_sz:#x})")
    if data_off != ro_off + ro_sz:
        print(f"WARN: data file_off ({data_off:#x}) != rodata end ({ro_off+ro_sz:#x})")
    
    total_mem = text_sz + ro_sz + data_sz
    print(f"Total mapped memory (text+ro+data): {total_mem:#x} ({total_mem} bytes = {total_mem/(1024*1024):.1f} MiB)")
    
    if mod0_off > 0 and mod0_off < len(data):
        if data[mod0_off:mod0_off+4] == b'MOD0':
            bss_start = struct.unpack('<I', data[mod0_off+8:mod0_off+12])[0]
            bss_end = struct.unpack('<I', data[mod0_off+12:mod0_off+16])[0]
            bss_sz = bss_end - bss_start
            print(f"MOD0 bss: {bss_sz:#x} ({bss_sz} bytes = {bss_sz/(1024*1024):.1f} MiB)")
    
    print(f"\nVerdict: {'PASS' if ok else 'FAIL'}")
    return ok

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python verify_nro.py <file.nro>")
        sys.exit(1)
    ok = verify_nro(sys.argv[1])
    sys.exit(0 if ok else 1)
