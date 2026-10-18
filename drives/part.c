// SPDX-License-Identifier: GPL-2.0-only
/**
 * drives/part.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 分区表扫描: GPT(含备份头/两级 CRC) / MBR(含扩展分区 EBR 链)
 * 以及文件系统识别(相当于内核版 blkid)
 */
#include <drives/blkdev.h>
#include <print.h>
#include <crc.h>

#define GPT_ENTRIES_MAX_BYTES 16384U //128项x128字节
#define MBR_EBR_CHAIN_MAX     64     //扩展分区链长上限

//GPT头
typedef struct __attribute__((packed)) gpt_header {
    uint8_t  signature[8];      //0x00 "EFI PART"
    uint32_t revision;          //0x08
    uint32_t header_size;       //0x0C
    uint32_t header_crc32;      //0x10
    uint32_t reserved;          //0x14
    uint64_t my_lba;            //0x18
    uint64_t alternate_lba;     //0x20
    uint64_t first_usable_lba;  //0x28
    uint64_t last_usable_lba;   //0x30
    uint8_t  disk_guid[16];     //0x38
    uint64_t part_entry_lba;    //0x48
    uint32_t num_part_entries;  //0x50
    uint32_t part_entry_size;   //0x54
    uint32_t part_entries_crc32;//0x58
} gpt_header_t;
//GPT表项
typedef struct __attribute__((packed)) gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attr;
    uint16_t name[36];          //UTF-16LE
} gpt_entry_t;
//MBR表项
typedef struct __attribute__((packed)) mbr_entry {
    uint8_t  status;   //0x00
    uint8_t  chs_start[3];
    uint8_t  type;     //0x04
    uint8_t  chs_end[3];
    uint32_t lba_start;//0x08
    uint32_t sectors;  //0x0C
} mbr_entry_t;
//MBR分区
typedef struct __attribute__((packed)) mbr_sector {
    uint8_t    boot_code[440];//0x000
    uint32_t   disk_sig;      //0x1B8
    uint16_t   reserved;      //0x1BC
    mbr_entry_t part[4];      //0x1BE
    uint16_t   signature;     //0x1FE
} mbr_sector_t;

_Static_assert(sizeof(gpt_header_t) == 92, "gpt_header must be 92 bytes");
_Static_assert(sizeof(gpt_entry_t)  == 128, "gpt_entry must be 128 bytes");
_Static_assert(sizeof(mbr_entry_t)  == 16, "mbr entry must be 16 bytes");
_Static_assert(sizeof(mbr_sector_t) == 512, "mbr sector must be 512 bytes");

//GPT分区类型GUID->可读名
typedef struct { uint8_t guid[16]; const char *name; } gpt_type_name_t;

//GUID及其对应可读名
static const gpt_type_name_t g_gpt_types[] = {
    {{0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B}, "EFI System"},
    {{0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4}, "Linux filesystem"},
    {{0xE3,0xBC,0x68,0x4F,0xCD,0xE8,0xB1,0x4D,0x96,0xE7,0xFB,0xCA,0xF9,0x84,0xB7,0x09}, "Linux root x86-64"},
    {{0x6D,0xFD,0x57,0x06,0xAB,0xA4,0xC4,0x43,0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F}, "Linux swap"},
    {{0xE1,0xC7,0x93,0x93,0xB4,0x2E,0x13,0x4F,0xB8,0x44,0x0E,0x14,0xE2,0xAE,0xF9,0x15}, "Linux /home"},
    {{0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7}, "Microsoft basic data"},
    {{0xA4,0xBB,0x94,0xDE,0xD1,0x06,0x40,0x4D,0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC}, "Windows recovery"},
    {{0x28,0x0F,0xD9,0x78,0x6E,0xA9,0xB8,0x4C,0xA3,0xE0,0x6E,0x6B,0x1F,0x0F,0xE3,0x66}, "Linux LVM"},
    {{0xAC,0x64,0x00,0x48,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC,0xA1,0xE3}, "Apple HFS+"},
};

//缓冲
static uint8_t g_sec[512] __attribute__((aligned(4096)));
static uint8_t g_gpt_arr[GPT_ENTRIES_MAX_BYTES] __attribute__((aligned(4096)));
static uint8_t g_probe[2048] __attribute__((aligned(4096)));

//GPT分区类型GUID转可读名
static const char *gpt_type_name(const uint8_t *guid){
    for(uint32_t i = 0;i < (sizeof(g_gpt_types) / sizeof(g_gpt_types[0]));i ++){
        if(memcmp(g_gpt_types[i].guid, guid, 16) == 0)return g_gpt_types[i].name;
    }
    return "unknown";
}

//UTF-16LE(GPT分区名)转ASCII
static void gpt_label_to_ascii(const uint16_t *src, int srcmax, char *dst, int dstsz){
    int i = 0;
    for(;i < srcmax && i < dstsz - 1;i ++){
        uint16_t c = src[i];
        if(!c)break;
        dst[i] = (c < 0x80) ? (char)c : '?';
    }
    dst[i] = 0;
}

//MBR分区类型码转可读名
static const char *mbr_type_name(uint8_t type){
    switch(type){
    case 0x01: return "FAT12";
    case 0x04:
    case 0x06: return "FAT16";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: return "FAT32 CHS";
    case 0x0C: return "FAT32 LBA";
    case 0x0E: return "FAT16 LBA";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0x8E: return "Linux LVM";
    case 0xEF: return "EFI System";
    default:   return "MBR";
    }
}

//读取并校验一个GPT头
static int gpt_read_header(gendisk_t *gd, uint64_t lba, gpt_header_t *out){
    if(!gd || !gd->gd_whole)return -1;
    if(lba >= gd->gd_nr_sectors)return -2;
    if(bdev_read(gd->gd_whole, lba * 512, g_sec, 512))return -3;
    gpt_header_t *h = (gpt_header_t*)g_sec;
    if(memcmp(h->signature, "EFI PART", 8) != 0)return -4;
    if(h->header_size < sizeof(gpt_header_t) || h->header_size > 512)return -5;
    uint32_t saved = h->header_crc32;
    h->header_crc32 = 0;
    uint32_t crc = Crc32(0, h, h->header_size);
    h->header_crc32 = saved;
    if(crc != saved)return -6;
    *out = *h;
    return 0;
}

//扫描GPT
static int gpt_scan(gendisk_t *gd){
    gpt_header_t h;
    int r = gpt_read_header(gd, 1, &h);
    if(r){
        //主头坏了就试备份头(最后一个 LBA)
        if(gd->gd_nr_sectors < 2)return r;
        int r2 = gpt_read_header(gd, gd->gd_nr_sectors - 1, &h);
        if(r2){
            printk(PRINTK_DEBUG"blk: %s GPT header is invalid (main=%d backup=%d)", gd->gd_name, r, r2);
            return r2;
        }
        printk(PRINTK_WARNING"blk: %s The main GPT header is damaged, and the backup header has been used.", gd->gd_name);
    }
    if(h.part_entry_size < sizeof(gpt_entry_t) || h.part_entry_size > 512)return -7;
    if(!h.num_part_entries)return -8;

    uint64_t arr_bytes = (uint64_t)h.num_part_entries * h.part_entry_size;
    uint32_t need = (uint32_t)(arr_bytes > GPT_ENTRIES_MAX_BYTES ? GPT_ENTRIES_MAX_BYTES : arr_bytes);
    uint32_t rd   = (need + 511u) & ~511u;
    if(rd > GPT_ENTRIES_MAX_BYTES)rd = GPT_ENTRIES_MAX_BYTES;
    if(bdev_read(gd->gd_whole, h.part_entry_lba * 512, g_gpt_arr, rd)){
        printk(PRINTK_ERR"blk: %s Failed to read partition item array.", gd->gd_name);
        return -9;
    }
    //分区项数组CRC
    if(arr_bytes <= GPT_ENTRIES_MAX_BYTES){
        uint32_t crc = Crc32(0, g_gpt_arr, need);
        if(crc != h.part_entries_crc32){
            printk(PRINTK_ERR"blk: %s CRC error in partition item array (calculated =%x expected =%x)",
                   gd->gd_name, crc, h.part_entries_crc32);
            return -10;
        }
    }else{
        printk(PRINTK_WARNING"blk: %s Too many partition entries (%llu bytes), only the first %u entries will be parsed and CRC will be skipped.",
               gd->gd_name, arr_bytes, need / h.part_entry_size);
    }

    gd->gd_last_usable_lba = h.last_usable_lba;
    uint32_t maxent = need / h.part_entry_size;
    for(uint32_t i = 0;i < maxent;i ++){
        gpt_entry_t *e = (gpt_entry_t*)(g_gpt_arr + (uint64_t)i * h.part_entry_size);
        int empty = 1;
        for(int k = 0;k < 16;k ++)if(e->type_guid[k]){ empty = 0; break; }
        if(empty)continue;
        if(e->last_lba < e->first_lba){
            printk(PRINTK_WARNING"blk: %s The range of GPT item %u is illegal, skipping.", gd->gd_name, i);
            continue;
        }
        uint64_t nr = e->last_lba - e->first_lba + 1;
        if(e->first_lba >= gd->gd_nr_sectors)continue;
        if(e->first_lba + nr > gd->gd_nr_sectors)nr = gd->gd_nr_sectors - e->first_lba;
        bdev_t *bd = BlkNewBdev(gd, e->first_lba, nr, PART_PRIMARY, (int)i + 1,gpt_type_name(e->type_guid));
        if(!bd)break;
        uint16_t lbl[36];
        memcpy(lbl, e->name, sizeof(lbl));
        gpt_label_to_ascii(lbl, 36, bd->bd_label, BLK_PARTNAME_MAX);
    }
    return 0;
}

//扫描MBR
static int mbr_scan(gendisk_t *gd){
    if(!gd || !gd->gd_whole)return -1;
    if(bdev_read(gd->gd_whole, 0, g_sec, 512))return -2;
    mbr_sector_t *m = (mbr_sector_t*)g_sec;
    if(m->signature != 0xAA55)return -3;
    int part_no = 0;
    uint64_t ext_base = 0;
    int has_ext = 0;
    for(int i = 0;i < 4;i ++){
        mbr_entry_t *e = &m->part[i];
        if(!e->type || !e->sectors)continue;
        if(e->type == 0x05 || e->type == 0x0F || e->type == 0x85){
            ext_base = e->lba_start;
            has_ext = 1;
            continue;
        }
        part_no ++;
        BlkNewBdev(gd, e->lba_start, e->sectors, PART_PRIMARY, part_no, mbr_type_name(e->type));
    }
    if(!has_ext)return 0;
    int logical_no = 5;
    uint64_t ebr = ext_base;
    for(int guard = 0;guard < MBR_EBR_CHAIN_MAX;guard ++){
        if(ebr >= gd->gd_nr_sectors)break;
        if(bdev_read(gd->gd_whole, ebr * 512, g_sec, 512))break;
        mbr_sector_t *b = (mbr_sector_t*)g_sec;
        if(b->signature != 0xAA55)break;
        mbr_entry_t *e0 = &b->part[0];
        mbr_entry_t *e1 = &b->part[1];
        if(e0->type && e0->sectors)BlkNewBdev(gd, ebr + e0->lba_start, e0->sectors, PART_LOGICAL,logical_no ++, mbr_type_name(e0->type));
        if(!(e1->type == 0x05 || e1->type == 0x0F || e1->type == 0x85) || !e1->lba_start)break;
        ebr = ext_base + e1->lba_start;
    }
    return 0;
}

//文件系统识别
static void probe_fs(bdev_t *bd){
    if(!bd || !bd->bd_nr_sectors)return;
    memset(bd->bd_fs_type, 0, sizeof(bd->bd_fs_type));
    bd->bd_fs_block_size = 0;
    uint64_t bytes = bd->bd_nr_sectors * 512ULL;
    uint32_t len = (bytes >= sizeof(g_probe)) ? (uint32_t)sizeof(g_probe) : (uint32_t)bytes;
    len &= ~511u;
    if(!len)return;
    if(bdev_read(bd, 0, g_probe, len))return;
    //FAT32 / FAT12/16 / exFAT / NTFS
    if(len >= 0x5A && memcmp(g_probe + 0x52, "FAT32   ", 8) == 0){
        strcpy(bd->bd_fs_type, "vfat");
        bd->bd_fs_block_size = (uint32_t)g_probe[0x0B] | ((uint32_t)g_probe[0x0C] << 8);
        return;
    }
    if(len >= 11 && memcmp(g_probe + 3, "EXFAT   ", 8) == 0){
        strcpy(bd->bd_fs_type, "exfat");
        return;
    }
    if(len >= 0x5A && (memcmp(g_probe + 0x36, "FAT", 3) == 0 || memcmp(g_probe + 0x52, "FAT", 3) == 0)){
        strcpy(bd->bd_fs_type, "vfat");
        return;
    }
    if(len >= 11 && memcmp(g_probe + 3, "NTFS    ", 8) == 0){
        strcpy(bd->bd_fs_type, "ntfs");
        return;
    }
    //ext2/3/4: 超级块在分区内偏移 1024, magic 位于 +0x38
    if(len >= 1024 + 0x68){
        uint8_t *sb    = g_probe + 1024;
        uint16_t magic = (uint16_t)((uint16_t)sb[0x38] | ((uint16_t)sb[0x39] << 8));
        if(magic == 0xEF53){
            uint32_t compat   = *(uint32_t*)(sb + 0x5C);
            uint32_t incompat = *(uint32_t*)(sb + 0x60);
            uint32_t log_bs   = *(uint32_t*)(sb + 0x18);
            if(incompat & 0x00000040u)     strcpy(bd->bd_fs_type, "ext4");//extent
            else if(compat & 0x00000004u)  strcpy(bd->bd_fs_type, "ext3");//has_journal
            else                           strcpy(bd->bd_fs_type, "ext2");
            if(log_bs < 8)bd->bd_fs_block_size = 1024u << log_bs;
            return;
        }
    }
    strcpy(bd->bd_fs_type, "unknown");
}

//扫描一块盘的分区表
static int part_scan_disk(gendisk_t *gd){
    if(!gd || gd->gd_scanned)return 0;
    gd->gd_scanned = 1;
    if(!gd->gd_nr_sectors)return -1;
    //LBA0是不是有效MBR
    int has_mbr = 0;
    if(bdev_read(gd->gd_whole, 0, g_sec, 512) == 0){
        mbr_sector_t *m = (mbr_sector_t*)g_sec;
        if(m->signature == 0xAA55)has_mbr = 1;
    }
    //优先GPT
    if(gpt_scan(gd) == 0){
        gd->gd_table = " GPT";
        printk(PRINTK_DEBUG"blk: %s partition-table=GPT partitions=%d", gd->gd_name, gd->gd_nr_parts);
        return 0;
    }
    if(has_mbr && mbr_scan(gd) == 0){
        gd->gd_table = " MBR";
        printk(PRINTK_DEBUG"blk: %s partition-table=MBR partitions=%d", gd->gd_name, gd->gd_nr_parts);
        return 0;
    }
    //裸盘
    gd->gd_table = " bare";
    printk(PRINTK_WARNING"blk: %s Partition table not found, treated as bare disk.", gd->gd_name);
    return 1;
}

int BlkPartScanAll(void){
    CrcInit();
    int disks = 0;
    for(int i = 0;i < BlkDiskCount();i ++){
        gendisk_t *gd = BlkDiskAt(i);
        if(!gd || gd->gd_scanned)continue;
        part_scan_disk(gd);
        disks ++;
    }
    for(int i = 0;i < BlkBdevCount();i ++){
        bdev_t *bd = BlkBdevAt(i);
        if(bd->bd_part_type == PART_WHOLE && bd->bd_disk && bd->bd_disk->gd_nr_parts > 0)continue;
        probe_fs(bd);
    }
    return disks;
}
