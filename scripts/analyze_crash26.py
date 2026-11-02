#!/usr/bin/env python3
"""Map crash(26) ARM64 PC/LR back to PPC addresses in T3DOTM.nro."""
import struct
import sys

NRO = r'J:/XBOX360 port/Transformers Dark of the Moon/recomp/out/sw8/T3DOTM.nro'

with open(NRO, 'rb') as f:
    data = bytearray(f.read())

# NRO header
assert data[0x10:0x14] == b'NRO0'
text_off = struct.unpack_from('<I', data, 0x20)[0]
text_size = struct.unpack_from('<I', data, 0x30)[0]
ro_off = struct.unpack_from('<I', data, 0x24)[0]
ro_size = struct.unpack_from('<I', data, 0x34)[0]

print(f"text_off={text_off:#x} text_size={text_size:#x}")
print(f"ro_off={ro_off:#x} ro_size={ro_size:#x}")

# Search .rodata for known PPC addresses from overrides.toml
ppc_addrs = [
    0x832F5648,  # batch 24
    0x833D6FC8,  # batch 23
    0x8334D5C8,  # batch 22
    0x832F7068,  # batch 21
    0x8334D6A0,  # batch 20
    0x833CF038,  # batch 19
    0x831735B0,  # batch 17
    0x8255B3D8,  # batch 15
]

for ppc in ppc_addrs:
    # Search for 32-bit PPC address in rodata (little-endian)
    b = struct.pack('<I', ppc)
    idx = 0
    found = []
    while True:
        idx = data.find(b, idx)
        if idx == -1:
            break
        if ro_off <= idx < ro_off + ro_size:
            found.append(f"rodata+{idx-ro_off:#x}")
        idx += 1
    if found:
        print(f"PPC {ppc:#x} found at: {', '.join(found)}")

# Also search for 64-bit sign-extended forms or pointer patterns
for ppc in ppc_addrs:
    b64 = struct.pack('<Q', ppc | 0xFFFFFFFF00000000)  # sign-extended 32-bit in 64-bit
    idx = 0
    found = []
    while True:
        idx = data.find(b64, idx)
        if idx == -1:
            break
        if ro_off <= idx < ro_off + ro_size:
            found.append(f"rodata+{idx-ro_off:#x}")
        idx += 1
    if found:
        print(f"PPC64-ext {ppc:#x} found at: {', '.join(found)}")

# Search for the crash PC offset 0x5569b14 and LR 0x5547c20 in rodata
# (might be stored in exception tables or jump tables)
for val, name in [(0x5569b14, 'PC'), (0x5547c20, 'LR')]:
    b = struct.pack('<Q', val)
    idx = 0
    found = []
    while True:
        idx = data.find(b, idx)
        if idx == -1:
            break
        sec = "text" if text_off <= idx < text_off+text_size else ("rodata" if ro_off <= idx < ro_off+ro_size else "other")
        found.append(f"{sec}+{idx:#x}")
        idx += 1
    if found:
        print(f"ARM64 {name}={val:#x} found at: {', '.join(found)}")

# Try to find indirect dispatch table patterns near LR=0x5547c20 in .text
# The pattern at 0x5547c00 loads [x21, #32], then ldr x0, [x1, x2, lsl #3]
# Let's search for ldr x0, [x1, x2, lsl #3] instruction bytes in .text
# Encoding: size=11 (64-bit), V=0, opc=01 (ldr), 1 0 0 0 0 1 1 Rm 0 1 0 1 1 0 1 0 1 0 1 Rn Rt
# Actually ldr X0, [X1, X2, LSL #3]:
#   1100 0011 0001 0010 1 0 1 0 1 1 0 0 0 0 1
# In little-endian: need to compute exactly. Let's just search the bytes from the disassembly.
# From objdump at 0x5547c08: f8627820
pattern = bytes.fromhex('f8627820')
idx = text_off
found = []
while True:
    idx = data.find(pattern, idx)
    if idx == -1 or idx >= text_off + text_size:
        break
    found.append(f"text+{idx-text_off:#x}")
    idx += 1
print(f"ldr x0, [x1, x2, lsl #3] found at: {', '.join(found[:10])}")

# Also look for ldr x19, [x0] (f9400013) near 0x5569b14
pattern2 = bytes.fromhex('f9400013')
idx = text_off
found = []
while True:
    idx = data.find(pattern2, idx)
    if idx == -1 or idx >= text_off + text_size:
        break
    found.append(f"text+{idx-text_off:#x}")
    idx += 1
print(f"ldr x19, [x0] found at: {', '.join(found[:20])}")

# Find all function prologues matching the crash function (cbz x0, ret; stp x29,x30...)
# At 0x5569b00: b4000200 (cbz x0, +0x40) then a9be7bfd (stp x29,x30,[sp,-32]!)
prologue = bytes.fromhex('b4000200a9be7bfd')
idx = text_off
found = []
while True:
    idx = data.find(prologue, idx)
    if idx == -1 or idx >= text_off + text_size:
        break
    found.append(f"text+{idx-text_off:#x}")
    idx += 1
print(f"prologue cbz+stp at: {', '.join(found[:20])}")
