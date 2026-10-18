// SPDX-License-Identifier: GPL-2.0-only
/**
 * drives/blkdev.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 通用块设备层
 */
#include <drives/blkdev.h>
#include <print.h>

static gendisk_t g_disk_pool[BLK_MAX_DISKS];//磁盘池
static bdev_t    g_bdev_pool[BLK_MAX_BDEVS];//分区池
static int       g_nr_disks;
static int       g_nr_bdevs;
static int       g_sd_index;    //sd a/b/c...
static int       g_nvme_index;  //nvme0/nvme1...
static uint32_t  g_next_minor;  //次设备号
static _Bool     g_blk_ready;
static struct list_node g_disk_list;

static inline void blk_list_add_tail(struct list_node *node, struct list_node *head){
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}
#define blk_list_for_each(pos, head) for((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)
#define blk_list_entry(pos, type, member) ((type*)((uintptr_t)(pos) - __builtin_offsetof(type, member)))

void BlkInit(void){
    if(g_blk_ready)return;
    g_nr_disks = 0;
    g_nr_bdevs = 0;
    g_sd_index = 0;
    g_nvme_index = 0;
    g_next_minor = 1;
    g_disk_list.prev = &g_disk_list;
    g_disk_list.next = &g_disk_list;
    g_blk_ready = true;
}

//分配一个bdev并挂到盘的链表上
bdev_t *BlkNewBdev(gendisk_t *gd, uint64_t start, uint64_t nr_sectors,int part_type, int part_no, const char *type_name){
    if(!gd)return NULL;
    //检查是否超过上限
    if(g_nr_bdevs >= BLK_MAX_BDEVS){
        printk(PRINTK_ERR"blk:%s The number of block devices has reached the upper limit(%d)", gd->gd_name, BLK_MAX_BDEVS);
        return NULL;
    }
    bdev_t *bd = &g_bdev_pool[g_nr_bdevs++];//从池中取一个
    //初始化结构体
    memset(bd, 0, sizeof(*bd));
    bd->bd_disk        = gd;
    bd->bd_start       = start;
    bd->bd_nr_sectors  = nr_sectors;
    bd->bd_block_size  = 512;
    bd->bd_part_type   = part_type;
    bd->bd_part_no     = part_no;
    bd->bd_major       = (gd->gd_style == GD_STYLE_NVME) ? 259 : 8;
    bd->bd_minor       = g_next_minor ++;
    if(type_name)strncpy(bd->bd_part_name, type_name, BLK_PARTNAME_MAX - 1);
    //设置命名
    if(part_type == PART_WHOLE){
        strncpy(bd->bd_name, gd->gd_name, BLK_NAME_MAX - 1);
    }else if(gd->gd_style == GD_STYLE_NVME){
        sprintf(bd->bd_name, "%sp%d", gd->gd_name, part_no);
    }else{
        sprintf(bd->bd_name, "%s%d", gd->gd_name, part_no);
    }
    blk_list_add_tail(&bd->bd_list, &gd->gd_partitions);
    if(part_type != PART_WHOLE)gd->gd_nr_parts ++;//如果不是整盘，更新分区计数
    return bd;
}

gendisk_t *BlkAddDisk(disk_info_t *disk){
    if(!g_blk_ready)BlkInit();//初始化
    //参数检查
    if(!disk || !disk->present || !disk->ops){
        printk(PRINTK_WARNING"blk:Invalid disk parameter.");
        return NULL;
    }
    //检查是否超过上限
    if(g_nr_disks >= BLK_MAX_DISKS){
        printk(PRINTK_ERR"blk:The number of physical disks has reached the maximum.(%d)", BLK_MAX_DISKS);
        return NULL;
    }
    gendisk_t *gd = &g_disk_pool[g_nr_disks ++];//从磁盘池中取一个
    //初始化结构体
    memset(gd, 0, sizeof(*gd));
    gd->gd_disk_copy = *disk;
    gd->gd_disk = &gd->gd_disk_copy;
    gd->gd_sector_size = 512;
    gd->gd_nr_sectors  = disk->total_sectors;
    gd->gd_last_usable_lba = gd->gd_nr_sectors ? (gd->gd_nr_sectors - 1) : 0;
    gd->gd_partitions.prev = &gd->gd_partitions;
    gd->gd_partitions.next = &gd->gd_partitions;
    //NVMe使用nvme0n1，其他使用sda
    if(disk->ctrl_type == DISK_CTRL_NVME){
        gd->gd_style = GD_STYLE_NVME;
        sprintf(gd->gd_name, "nvme%dn1", g_nvme_index ++);
        gd->gd_max_transfer = 256;
    }else{
        gd->gd_style = GD_STYLE_SD;
        sprintf(gd->gd_name, "sd%c", (char)('a' + g_sd_index ++));
        gd->gd_max_transfer = (disk->ctrl_type == DISK_CTRL_ATA) ? 64 : 256;
    }
    blk_list_add_tail(&gd->gd_list, &g_disk_list);
    gd->gd_whole = BlkNewBdev(gd, 0, gd->gd_nr_sectors, PART_WHOLE, 0, "whole");//整盘本身也是一个块设备
    printk(PRINTK_DEBUG"blk:Register disk %s Sector number:%llu Single upper limit:%u sectors",gd->gd_name, gd->gd_nr_sectors, gd->gd_max_transfer);
    return gd;
}

int BlkDiskCount(void){
    return g_nr_disks;
}

gendisk_t *BlkDiskAt(int idx){
    if(idx < 0 || idx >= g_nr_disks)return NULL;
    return &g_disk_pool[idx];
}

int BlkBdevCount(void){
    return g_nr_bdevs;
}

bdev_t *BlkBdevAt(int idx){
    if(idx < 0 || idx >= g_nr_bdevs)return NULL;
    return &g_bdev_pool[idx];
}

bdev_t *BlkWholeDevice(gendisk_t *gd){
    return gd ? gd->gd_whole : NULL;
}

bdev_t *BlkNameToBdev(const char *name){
    if(!name)return NULL;
    for(int i = 0;i < g_nr_bdevs;i ++){
        if(strcmp(g_bdev_pool[i].bd_name, name) == 0)return &g_bdev_pool[i];
    }
    return NULL;
}

//按分区起始LBA查找块设备
bdev_t *BlkFindBdev(uint64_t start_lba, uint64_t nr_sectors){
    for(int i = 0;i < g_nr_bdevs;i ++){
        bdev_t *bd = &g_bdev_pool[i];
        if(bd->bd_part_type == PART_WHOLE)continue;
        if(bd->bd_start != start_lba)continue;
        if(nr_sectors && bd->bd_nr_sectors != nr_sectors)continue;
        return bd;
    }
    for(int i = 0;i < g_nr_bdevs;i ++){
        bdev_t *bd = &g_bdev_pool[i];
        if(bd->bd_part_type != PART_WHOLE && bd->bd_start == start_lba)return bd;
    }
    return NULL;
}

int submit_bio(bio_t *bio){
    if(!bio || !bio->bi_bdev)return -1;
    bdev_t *bd = bio->bi_bdev;
    gendisk_t*gd = bd->bd_disk;
    if(!gd || !gd->gd_disk || !gd->gd_disk->ops)return -1;
    if(!bio->bi_size)return 0;
    uint32_t ss = gd->gd_sector_size ? gd->gd_sector_size : 512;
    uint64_t need = (uint64_t)bio->bi_size / ss;
    if(!need)return -1;
    if(bio->bi_sector + need > bd->bd_nr_sectors){
        printk(PRINTK_ERR"blk:%s overrun lba:%llu sectors:%llu part size:%llu",bd->bd_name, bio->bi_sector, need, bd->bd_nr_sectors);
        bio->bi_status = -1;
        return -1;
    }
    uint64_t lba = bd->bd_start + bio->bi_sector;
    uint32_t maxseg = gd->gd_max_transfer ? gd->gd_max_transfer : 1;
    for(uint16_t i = 0;i < bio->bi_vcnt;i ++){
        uint8_t *p = (uint8_t*)bio->bi_vec[i].bv_data;
        uint32_t left = bio->bi_vec[i].bv_len;
        while(left >= ss){
            uint32_t chunk = left / ss;
            if(chunk > maxseg)chunk = maxseg;
            int r = (bio->bi_op == BIO_WRITE) ? DiskWrite(gd->gd_disk, lba, chunk, p) : DiskRead (gd->gd_disk, lba, chunk, p);
            if(r){
                bio->bi_status = r;
                return r;
            }
            lba += chunk;
            p += (uint64_t)chunk * ss;
            left -= chunk * ss;
        }
    }
    bio->bi_status = 0;
    return 0;
}

//按字节读
int bdev_read(bdev_t *bd, uint64_t off, void *buf, uint32_t len){
    if(!bd || !buf || !len)return bd ? 0 : -1;
    if(off + len > bd->bd_nr_sectors * 512ULL)return -1;
    if(((off | len) & 511ULL) == 0){
        bio_t bio;
        memset(&bio, 0, sizeof(bio));
        bio.bi_bdev    = bd;
        bio.bi_sector  = off / 512;
        bio.bi_size    = len;
        bio.bi_op      = BIO_READ;
        bio.bi_vcnt    = 1;
        bio.bi_vec[0].bv_data = buf;
        bio.bi_vec[0].bv_len  = len;
        return submit_bio(&bio);
    }
    uint8_t  sec[512] __attribute__((aligned(16)));
    uint8_t *dst  = (uint8_t*)buf;
    uint64_t pos  = off;
    uint32_t left = len;
    while(left){
        uint64_t lba = pos / 512;
        uint32_t in  = (uint32_t)(pos % 512);
        uint32_t n   = 512 - in;
        if(n > left)n = left;
        bio_t bio;
        memset(&bio, 0, sizeof(bio));
        bio.bi_bdev    = bd;
        bio.bi_sector  = lba;
        bio.bi_size    = 512;
        bio.bi_op      = BIO_READ;
        bio.bi_vcnt    = 1;
        bio.bi_vec[0].bv_data = sec;
        bio.bi_vec[0].bv_len  = 512;
        if(submit_bio(&bio))return -1;
        memcpy(dst, sec + in, n);
        dst  += n;
        pos  += n;
        left -= n;
    }
    return 0;
}

//按字节写
int bdev_write(bdev_t *bd, uint64_t off, const void *buf, uint32_t len){
    if(!bd || !buf || !len)return bd ? 0 : -1;
    if(off + len > bd->bd_nr_sectors * 512ULL)return -1;
    if(((off | len) & 511ULL) == 0){
        bio_t bio;
        memset(&bio, 0, sizeof(bio));
        bio.bi_bdev    = bd;
        bio.bi_sector  = off / 512;
        bio.bi_size    = len;
        bio.bi_op      = BIO_WRITE;
        bio.bi_vcnt    = 1;
        bio.bi_vec[0].bv_data = (void*)buf;
        bio.bi_vec[0].bv_len  = len;
        return submit_bio(&bio);
    }
    uint8_t  sec[512] __attribute__((aligned(16)));
    const uint8_t *src = (const uint8_t*)buf;
    uint64_t pos  = off;
    uint32_t left = len;
    while(left){
        uint64_t lba = pos / 512;
        uint32_t in  = (uint32_t)(pos % 512);
        uint32_t n   = 512 - in;
        if(n > left)n = left;
        bio_t bio;
        memset(&bio, 0, sizeof(bio));
        bio.bi_bdev    = bd;
        bio.bi_sector  = lba;
        bio.bi_size    = 512;
        bio.bi_op      = BIO_READ;
        bio.bi_vcnt    = 1;
        bio.bi_vec[0].bv_data = sec;
        bio.bi_vec[0].bv_len  = 512;
        if(submit_bio(&bio))return -1;
        memcpy(sec + in, src, n);
        memset(&bio, 0, sizeof(bio));
        bio.bi_bdev    = bd;
        bio.bi_sector  = lba;
        bio.bi_size    = 512;
        bio.bi_op      = BIO_WRITE;
        bio.bi_vcnt    = 1;
        bio.bi_vec[0].bv_data = sec;
        bio.bi_vec[0].bv_len  = 512;
        if(submit_bio(&bio))return -1;
        src  += n;
        pos  += n;
        left -= n;
    }
    return 0;
}

//打印全部盘与分区
void BlkDumpAll(void){
    struct list_node *n;
    blk_list_for_each(n, &g_disk_list){
        gendisk_t *gd = blk_list_entry(n, gendisk_t, gd_list);
        char line[256];
        int  len = 0;
        len += sprintf(line + len, "blk: %s %lluMB%s", gd->gd_name,((gd->gd_nr_sectors * gd->gd_sector_size) / (1024ULL * 1024ULL)),gd->gd_table ? gd->gd_table : "");
        struct list_node *m;
        blk_list_for_each(m, &gd->gd_partitions){
            bdev_t *bd = blk_list_entry(m, bdev_t, bd_list);
            if(bd->bd_part_type == PART_WHOLE)continue;//整盘信息已在前面
            if(len > 200)break;
            len += sprintf(line + len, " | p%d %llu+%llu %s(%s)",bd->bd_part_no, bd->bd_start, bd->bd_nr_sectors,bd->bd_part_name,bd->bd_fs_type[0] ? bd->bd_fs_type : "?");
        }
        printk(PRINTK_INFO"%s", line);
    }
}
