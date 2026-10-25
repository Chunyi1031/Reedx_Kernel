#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# scripts/mk_ext2_part.py
#
# 在已有的 GPT 镜像(默认 System.img)上追加一个 ext2 分区:
#   1. 在空闲区域申请一个新分区, 类型填 "Linux filesystem"
#   2. 同步主/备分区表, 重算两个 CRC32
#   3. 在新分区里用 mke2fs 造一个 ext2 文件系统
#
# 用法: sudo ./scripts/mk_ext2_part.py [镜像] [分区大小MiB]
# 注意: 需要 root(要改设备节点)或者用 loop 设备; 这里只操作镜像文件本身。
#
import os
import struct
import subprocess
import sys
import uuid
import zlib

SECTOR = 512
LINUX_FS_GUID = bytes([0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                       0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4])
ENTRY_SIZE = 128


def read_gpt(f):
    f.seek(SECTOR)
    hdr = f.read(92)
    if hdr[:8] != b"EFI PART":
        sys.exit("不是 GPT 镜像: LBA1 没有 EFI PART 签名")
    part_lba, num, esize = struct.unpack_from("<QII", hdr, 0x48)
    f.seek(part_lba * SECTOR)
    arr = bytearray(f.read(num * esize))
    return hdr, part_lba, num, esize, arr


def guid_bytes_to_str(g):
    a, b, c = struct.unpack_from("<IHH", g, 0)
    return "%08X-%04X-%04X-%s-%s" % (a, b, c, g[8:10].hex().upper(), g[10:16].hex().upper())


def entry_used(e):
    return any(e[:16])


def main():
    img = sys.argv[1] if len(sys.argv) > 1 else "System.img"
    size_mib = int(sys.argv[2]) if len(sys.argv) > 2 else 48

    total = os.path.getsize(img) // SECTOR
    with open(img, "r+b") as f:
        hdr, part_lba, num, esize, arr = read_gpt(f)
        alt_lba, first_usable, last_usable = struct.unpack_from("<QQQ", hdr, 0x20)
        backup_arr_lba = alt_lba - num * esize // SECTOR

        # 已占用的 LBA 区间 + GPT 自身保留区
        used = [(0, part_lba + num * esize // SECTOR - 1),
                (backup_arr_lba, total - 1)]
        for i in range(num):
            e = arr[i * esize:(i + 1) * esize]
            if not entry_used(e):
                continue
            s, l = struct.unpack_from("<QQ", e, 32)
            print("  已存在分区 %d: LBA %d..%d (%d MiB) 类型 %s" %
                  (i + 1, s, l, (l - s + 1) * SECTOR // (1024 * 1024),
                   guid_bytes_to_str(e[:16])))
            used.append((s, l))

        # 从 first_usable 开始找第一段足够大的空洞
        used.sort()
        cursor = first_usable
        start = None
        for s, l in used:
            if s > cursor and (s - cursor) * SECTOR >= size_mib * 1024 * 1024:
                start = cursor
                break
            cursor = max(cursor, l + 1)
        if start is None and (last_usable - cursor + 1) * SECTOR >= size_mib * 1024 * 1024:
            start = cursor
        if start is None:
            sys.exit("镜像里没有 %d MiB 的空闲连续空间" % size_mib)

        # 找第一个空表项
        slot = next((i for i in range(num) if not entry_used(arr[i * esize:(i + 1) * esize])), None)
        if slot is None:
            sys.exit("GPT 表项已满")

        nsec = size_mib * 1024 * 1024 // SECTOR
        end = start + nsec - 1
        print("新建分区 %d: LBA %d..%d (%d MiB)" % (slot + 1, start, end, size_mib))

        # 构造表项: 类型 + 唯一 GUID + 起止 LBA + 名字
        name = "REEDX EXT2"
        e = bytearray(esize)
        e[0:16] = LINUX_FS_GUID
        e[16:32] = uuid.uuid4().bytes
        struct.pack_into("<QQQ", e, 32, start, end, 0)
        enc = name.encode("utf-16-le")[:70]
        e[56:56 + len(enc)] = enc
        arr[slot * esize:(slot + 1) * esize] = e

        # 写回主表和备表, 同步 CRC
        arr_crc = zlib.crc32(arr) & 0xFFFFFFFF
        f.seek(part_lba * SECTOR)
        f.write(arr)
        f.seek(backup_arr_lba * SECTOR)
        f.write(arr)

        for hdr_lba, hdr_entry_lba in ((1, part_lba), (alt_lba, backup_arr_lba)):
            h = bytearray(hdr)
            struct.pack_into("<Q", h, 0x48, hdr_entry_lba)  # part_entry_lba
            struct.pack_into("<I", h, 0x58, arr_crc)        # part_entries_crc32
            struct.pack_into("<I", h, 0x10, 0)              # 先清零再算
            size = struct.unpack_from("<I", h, 0x0C)[0]
            crc = zlib.crc32(bytes(h[:size])) & 0xFFFFFFFF
            struct.pack_into("<I", h, 0x10, crc)
            f.seek(hdr_lba * SECTOR)
            f.write(bytes(h[:size]))

        # 在新分区里造 ext2, 顺便放几个示意文件
        tree = "/tmp/reedx-ext2-tree"
        os.makedirs(os.path.join(tree, "docs"), exist_ok=True)
        with open(os.path.join(tree, "readme.txt"), "w") as t:
            t.write("reedx ext2 partition\n")
        with open(os.path.join(tree, "docs", "note.txt"), "w") as t:
            t.write("hello from ext2\n")
        part = "/tmp/reedx-ext2-part.img"
        with open(part, "wb") as t:
            f.seek(start * SECTOR)
            left = nsec * SECTOR
            while left:
                n = min(left, 1 << 20)
                t.write(f.read(n))
                left -= n
        subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", "1024", "-I", "128",
                        "-L", "REEDXEXT2", "-F", "-d", tree, part], check=True)
        with open(part, "rb") as t:
            f.seek(start * SECTOR)
            left = nsec * SECTOR
            while left:
                n = min(left, 1 << 20)
                f.write(t.read(n))
                left -= n
        os.unlink(part)

    print("完成")


if __name__ == "__main__":
    main()
