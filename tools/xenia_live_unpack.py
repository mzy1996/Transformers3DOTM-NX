#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xenia_live_unpack.py - Extract files from Xbox 360 LIVE (XContent) containers.

Implementation mirrors xenia-canary STFS container code:
  - src/xenia/vfs/devices/xcontent_container_device.h  (kBlockSize, LIVE magic)
  - src/xenia/vfs/devices/stfs_xbox.h                  (StfsVolumeDescriptor, hash entries)
  - src/xenia/vfs/devices/xcontent_devices/stfs_container_device.{h,cc}
  - src/xenia/kernel/xam/xcontent/xcontent.h           (XContentHeader/XContentMetadata)

LIVE container layout:
  0x000  XContentHeader (0x344): magic "LIVE" + signature + licenses + content_id + header_size
  0x344  XContentMetadata (0x93D6): volume_type, volume_descriptor (STFS/SVOD), etc.
  round_up(header_size, 0x1000) -> STFS volume start

STFS volume:
  data blocks have logical index; BlockToOffset() adds reserved hash-table blocks.
  Directory entries live in a chain of 0x1000-byte blocks (64 entries each),
  linked through level-0 hash table next-block pointers.
"""
import argparse
import os
import struct
import sys

KBLOCK = 0x1000
END_OF_CHAIN = 0xFFFFFF
ENTRIES_PER_DIR_BLOCK = KBLOCK // 0x40  # 64
BLOCKS_PER_HASH_LEVEL = (170, 28900, 4913000)


def u16_be(data, off):
    return struct.unpack_from(">H", data, off)[0]


def u32_be(data, off):
    return struct.unpack_from(">I", data, off)[0]


def u64_be(data, off):
    return struct.unpack_from(">Q", data, off)[0]


def u24_le(data, off):
    return data[off] | (data[off + 1] << 8) | (data[off + 2] << 16)


def round_up(value, align):
    return (value + align - 1) & ~(align - 1)


class StfsReader:
    """Reads blocks from an STFS volume after the LIVE container header."""

    def __init__(self, data, vol_off, descriptor):
        self.data = data
        self.vol_off = vol_off
        self.desc = descriptor
        self.read_only = (descriptor["flags"] & 1) != 0  # read_only_format bit0
        self.blocks_per_hash_table = 1 if self.read_only else 2
        self.block_step = (
            170 + self.blocks_per_hash_table,
            28900 + 171 * self.blocks_per_hash_table,
        )
        self.hash_cache = {}

    def block_to_offset(self, block_index):
        block = block_index
        for i, level_base in enumerate(BLOCKS_PER_HASH_LEVEL):
            block += ((block_index + level_base) // level_base) * self.blocks_per_hash_table
            if block_index < level_base:
                break
        return self.vol_off + (block << 12)

    def block_to_hash_block_number(self, block_index, hash_level):
        if hash_level == 2:
            return self.block_step[1]
        if block_index < BLOCKS_PER_HASH_LEVEL[hash_level]:
            return 0 if hash_level == 0 else self.block_step[hash_level - 1]
        block = (block_index // BLOCKS_PER_HASH_LEVEL[hash_level]) * self.block_step[hash_level]
        if hash_level == 0:
            block += ((block_index // BLOCKS_PER_HASH_LEVEL[1]) + 1) * self.blocks_per_hash_table
            if block_index < BLOCKS_PER_HASH_LEVEL[1]:
                return block
        return block + self.blocks_per_hash_table

    def _hash_table(self, block_number):
        key = block_number
        if key not in self.hash_cache:
            off = self.vol_off + (block_number << 12)
            self.hash_cache[key] = self.data[off:off + KBLOCK]
        return self.hash_cache[key]

    def get_block_hash(self, block_index):
        """Return (next_block, info_raw) for the given data block."""
        secondary_offset = 0  # read-only: no secondary hash table
        levels = 0  # read-only: only level 0 needed
        if not self.read_only:
            secondary_offset = KBLOCK if self.desc["root_active_index"] else 0
            total = self.desc["total_block_count"]
            levels = 2
            for lvl, base in enumerate(BLOCKS_PER_HASH_LEVEL):
                if total < base:
                    levels = lvl
                    break
        hash_off = self.block_to_hash_block_number(block_index, 0)
        table = self._hash_table(hash_off)
        record = block_index % BLOCKS_PER_HASH_LEVEL[0]
        entry = table[record * 0x18:(record + 1) * 0x18]
        info_raw = u32_be(entry, 0x14)
        next_block = info_raw & 0xFFFFFF
        return next_block, info_raw


def parse_live_container(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[0:4] != b"LIVE":
        raise ValueError(f"not a LIVE container (magic={data[0:4]!r})")
    header_size = u32_be(data, 0x340)
    # XContentMetadata fields at file offset 0x344:
    #   +0x00 content_type, +0x04 metadata_version, +0x08 content_size,
    #   +0x10 execution_info(0x18), +0x28 console_id[5], +0x2D profile_id(8),
    #   +0x35 volume_descriptor(union stfs/svod, 0x24), +0x59 data_file_count,
    #   +0x5D data_file_size, +0x65 volume_type, ...
    meta_off = 0x344
    content_type = u32_be(data, meta_off + 0x00)
    volume_type = u32_be(data, meta_off + 0x65)
    data_file_count = u32_be(data, meta_off + 0x59)
    content_size = u64_be(data, meta_off + 0x08)
    vol_off = round_up(header_size, KBLOCK)

    info = {
        "magic": data[0:4].decode("ascii"),
        "header_size": header_size,
        "content_type": content_type,
        "volume_type": volume_type,  # 0=STFS, 1=SVOD
        "data_file_count": data_file_count,
        "content_size": content_size,
        "volume_offset": vol_off,
        "container_size": len(data),
    }

    if volume_type != 0:
        raise ValueError(f"volume_type={volume_type} != STFS(0), SVOD unsupported")

    # volume_descriptor (stfs) at file offset 0x344 + 0x35
    d_off = meta_off + 0x35
    descriptor = {
        "descriptor_length": data[d_off],
        "version": data[d_off + 1],
        "flags": data[d_off + 2],
        "file_table_block_count": u16_be(data, d_off + 3),
        "file_table_block_number": u24_le(data, d_off + 5),
        "total_block_count": u32_be(data, d_off + 0x1C),
        "free_block_count": u32_be(data, d_off + 0x20),
    }
    descriptor["read_only_format"] = (descriptor["flags"] & 1) != 0
    descriptor["root_active_index"] = (descriptor["flags"] & 2) != 0
    info["stfs_descriptor"] = descriptor
    return info, data


def read_directory_entries(reader, info):
    """Walk the file-table block chain, return flat list of raw entries."""
    desc = info["stfs_descriptor"]
    entries = []
    block_index = desc["file_table_block_number"]
    count = desc["file_table_block_count"]
    n = 0
    while block_index != END_OF_CHAIN and n < count:
        off = reader.block_to_offset(block_index)
        block = reader.data[off:off + KBLOCK]
        for m in range(ENTRIES_PER_DIR_BLOCK):
            rec = block[m * 0x40:(m + 1) * 0x40]
            if rec[0] == 0:  # empty name marks end of directory block entries
                break
            name_raw = rec[0:40]
            flags = rec[40]
            name_len = flags & 0x3F
            contiguous = (flags >> 6) & 1
            is_dir = (flags >> 7) & 1
            name = name_raw[:name_len].decode("cp1252", errors="replace")
            valid_blocks = u24_le(rec, 41)
            allocated_blocks = u24_le(rec, 44)
            start_block = u24_le(rec, 47)
            directory_index = u16_be(rec, 50)
            length = u32_be(rec, 52)
            entries.append({
                "name": name,
                "is_dir": is_dir,
                "contiguous": contiguous,
                "valid_blocks": valid_blocks,
                "allocated_blocks": allocated_blocks,
                "start_block": start_block,
                "directory_index": directory_index,
                "length": length,
            })
        n += 1
        nxt, _ = reader.get_block_hash(block_index)
        block_index = nxt
    return entries


def build_tree(entries):
    """directory_index refers to the global entry list index; 0xFFFF = root.
    Parents always appear before children (depth-first ordering)."""
    nodes = [None] * len(entries)
    roots = []
    for idx, e in enumerate(entries):
        node = (e["is_dir"], e["name"], [] if e["is_dir"] else e)
        nodes[idx] = node
        parent_idx = e["directory_index"]
        if parent_idx == 0xFFFF:
            roots.append(node)
        else:
            if parent_idx >= len(nodes) or nodes[parent_idx] is None:
                raise ValueError(
                    f"entry #{idx} ({e['name']!r}) parent #{parent_idx} not found")
            nodes[parent_idx][2].append(node)
    return roots


def extract_file(reader, entry, out_path):
    """Read file data by following the data-block chain."""
    remaining = entry["length"]
    block_index = entry["start_block"]
    chunks = []
    while remaining > 0 and block_index != END_OF_CHAIN:
        off = reader.block_to_offset(block_index)
        size = min(KBLOCK, remaining)
        chunks.append(reader.data[off:off + size])
        remaining -= size
        nxt, _ = reader.get_block_hash(block_index)
        block_index = nxt
    if remaining != 0:
        raise IOError(f"file {entry['name']}: missing {remaining} bytes")
    data = b"".join(chunks)
    if len(data) != entry["length"]:
        raise IOError(
            f"file {entry['name']}: got {len(data)} bytes, expected {entry['length']}")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(data)


def extract_all(root_nodes, reader, out_dir, prefix=""):
    stats = {"files": 0, "dirs": 0, "bytes": 0}
    for is_dir, name, payload in root_nodes:
        rel = os.path.join(prefix, name)
        if is_dir:
            cur = os.path.join(out_dir, rel)
            os.makedirs(cur, exist_ok=True)
            stats["dirs"] += 1
            sub = extract_all(payload, reader, out_dir, rel)
            for k, v in sub.items():
                stats[k] += v
        else:
            out_path = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            extract_file(reader, payload, out_path)
            stats["files"] += 1
            stats["bytes"] += payload["length"]
    return stats


def main():
    ap = argparse.ArgumentParser(description="Extract Xbox 360 LIVE container")
    ap.add_argument("source", help="path to LIVE container (TU or DLC)")
    ap.add_argument("out_dir", help="output directory")
    ap.add_argument("--list-only", action="store_true",
                    help="only print the file table, do not extract")
    args = ap.parse_args()

    info, data = parse_live_container(args.source)
    desc = info["stfs_descriptor"]
    print(f"[container] magic={info['magic']} size={info['container_size']}")
    print(f"[container] header_size=0x{info['header_size']:X} volume_offset=0x{info['volume_offset']:X}")
    print(f"[container] content_type=0x{info['content_type']:08X} "
          f"volume_type={info['volume_type']} data_file_count={info['data_file_count']} "
          f"content_size={info['content_size']}")
    print(f"[stfs] file_table_block={desc['file_table_block_number']} "
          f"file_table_blocks={desc['file_table_block_count']} "
          f"total_blocks={desc['total_block_count']} free_blocks={desc['free_block_count']} "
          f"read_only={desc['read_only_format']}")

    reader = StfsReader(data, info["volume_offset"], desc)
    entries = read_directory_entries(reader, info)
    print(f"[stfs] directory entries: {len(entries)}")

    if args.list_only:
        for idx, e in enumerate(entries):
            kind = "D" if e["is_dir"] else "F"
            parent = "root" if e["directory_index"] == 0xFFFF else f"#{e['directory_index']}"
            print(f"  [{idx:4d}] {kind} parent={parent:5s} start={e['start_block']:6d} "
                  f"len={e['length']:10d} name={e['name']!r}")
        return 0

    tree = build_tree(entries)
    stats = extract_all(tree, reader, args.out_dir)
    print(f"[extract] files={stats['files']} dirs={stats['dirs']} bytes={stats['bytes']}")
    print(f"[extract] written to {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())