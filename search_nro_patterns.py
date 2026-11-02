#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import struct

NRO_PATH = r'J:/XBOX360 port/Transformers Dark of the Moon/recomp/out/sw8/T3DOTM.nro'

# Read NRO header to find text segment bounds
with open(NRO_PATH, 'rb') as f:
    data = bytearray(f.read())

# Parse NRO header at offset 0x10
assert data[0x10:0x14] == b'NRO0', "Not an NRO file"

nro_size = struct.unpack_from('<I', data, 0x18)[0]
nro_flags = struct.unpack_from('<I', data, 0x1C)[0]
text_off = struct.unpack_from('<I', data, 0x20)[0]
ro_off = struct.unpack_from('<I', data, 0x24)[0]
data_off = struct.unpack_from('<I', data, 0x28)[0]
bss_size = struct.unpack_from('<I', data, 0x2C)[0]
text_size = struct.unpack_from('<I', data, 0x30)[0]
ro_size = struct.unpack_from('<I', data, 0x34)[0]
data_size = struct.unpack_from('<I', data, 0x38)[0]

print(f"NRO segments: text_off={text_off:#x}, text_size={text_size:#x}")
print(f"  ro_off={ro_off:#x}, ro_size={ro_size:#x}")
print(f"  data_off={data_off:#x}, data_size={data_size:#x}")
print(f"  bss_size={bss_size:#x}")

text_start = text_off
text_end = text_off + text_size

# Patterns to search for (little-endian instruction bytes)
patterns = {
    'threadTlsSet': bytes([
        0x62, 0xd0, 0x3b, 0xd5,   # mrs x2, tpidrro_el0
        0x40, 0xcc, 0x20, 0x8b,   # add x0, x2, w0, sxtw #3
        0x01, 0x84, 0x00, 0xf9,   # str x1, [x0, #264]
        0xc0, 0x03, 0x5f, 0xd6,   # ret
    ]),
    'threadTlsGet': bytes([
        0x61, 0xd0, 0x3b, 0xd5,   # mrs x1, tpidrro_el0
        0x20, 0xcc, 0x20, 0x8b,   # add x0, x1, w0, sxtw #3
        0x00, 0x84, 0x40, 0xf9,   # ldr x0, [x0, #264]
        0xc0, 0x03, 0x5f, 0xd6,   # ret
    ]),
    '__syscall_tls_set_prologue': bytes([
        0xfd, 0x7b, 0xbf, 0xa9,   # stp x29, x30, [sp, #-16]!
        0xfd, 0x03, 0x00, 0x91,   # mov x29, sp
        # bl threadTlsSet (relocation varies)
        # mov w0, #0
        # ldp x29, x30, [sp], #16
        # ret
    ]),
}

for name, pattern in patterns.items():
    idx = 0
    while True:
        idx = data.find(pattern, idx)
        if idx == -1:
            break
        if text_start <= idx < text_end:
            print(f"FOUND {name} at file offset {idx:#x} (in .text)")
        else:
            print(f"FOUND {name} at file offset {idx:#x} (outside .text)")
        idx += 1

# Search for adrp + ldr patterns that could be __cxa_get_globals key references
# In eh_globals.o, __cxa_get_globals starts with:
#   adrp x0, ...
#   adrp x1, ...
#   add x1, x1, #0x0
#   ldrb w0, [x0]
#   tbnz w0, #0, 1c
#   mov x0, x1
#   ret
# This is a distinctive pattern: adrp x0; adrp x1; add x1, x1, #0; ldrb w0, [x0]; tbnz w0, #0, ...

# Let's just search for the mrs tpidrro_el0 instruction, which is extremely rare
tpidrro_mrs = bytes([0x61, 0xd0, 0x3b, 0xd5])  # mrs x1, tpidrro_el0
tpidrro_mrs_x2 = bytes([0x62, 0xd0, 0x3b, 0xd5])  # mrs x2, tpidrro_el0

for pattern, desc in [(tpidrro_mrs, 'mrs x1, tpidrro_el0'), (tpidrro_mrs_x2, 'mrs x2, tpidrro_el0')]:
    idx = 0
    while True:
        idx = data.find(pattern, idx)
        if idx == -1:
            break
        if text_start <= idx < text_end:
            print(f"FOUND {desc} at file offset {idx:#x} (in .text)")
        idx += 1

# Now try to find __cxa_get_globals by its call to pthread_getspecific and malloc
# In the .o file, the sequence after the key load is:
#   bl pthread_getspecific
#   mov x1, x0
#   cbz x0, 44
#   mov x0, x1
#   ldp x29, x30, [sp], #32
#   ret
#   mov x0, #0x10
#   bl malloc
# This is hard to match directly.

# Alternative: find all pthread_key_create calls to identify eh_globals_init
# _GLOBAL__sub_I_ starts with stp x29,x30; adrp x1; adrp x0; mov x29,sp; add x1; add x0; bl pthread_key_create
# The sequence "stp x29,x30,[sp,#-16]!; adrp x1; adrp x0; mov x29,sp; add x1,x1,#0; add x0,x0,#0; bl pthread_key_create"
# has a specific pattern.

# Let's search for stp x29,x30,[sp,#-16]! followed by two adrp instructions
# stp x29,x30,[sp,#-16]! = 0xfd7bbf_a9 (last byte varies due to immediate)
# Actually stp x29,x30,[sp,#-16]! = a9bf7bfd in little-endian

init_pattern = bytes([0xfd, 0x7b, 0xbf, 0xa9])  # stp x29, x30, [sp, #-16]!
idx = 0
while True:
    idx = data.find(init_pattern, idx)
    if idx == -1:
        break
    if text_start <= idx < text_end:
        # Check if followed by adrp x1 and adrp x0
        # adrp x1, ... : 0x90000001 to 0x91ffffe1 variations (little-endian)
        # Actually adrp x1 is encoded as: 1 << 29 | immlo << 29 ... it's complex
        # Just note the location and dump surrounding bytes for manual inspection
        print(f"FOUND possible eh_globals_init at file offset {idx:#x} (in .text)")
        print(f"  bytes: {data[idx:idx+32].hex()}")
    idx += 1
