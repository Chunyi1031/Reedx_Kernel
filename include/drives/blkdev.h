// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/drives/blkdev.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 通用块设备层
 */
#ifndef _BLKDEV_H_
#define _BLKDEV_H_

#include <klib.h>
#include <drives/disk.h>

#define BLK_MAX_DISKS   4                                       //最多识别几块物理盘
#define BLK_MAX_PARTS   32                                      //每盘最多跟踪几个分区(不含整盘)
#define BLK_MAX_BDEVS   (BLK_MAX_DISKS * (BLK_MAX_PARTS + 1))   //bdev总数上限
#define BLK_NAME_MAX    24                                      //块设备名最大长度
#define BLK_FSTYPE_MAX  16                                      //文件系统类型名最大长度
#define BLK_PARTNAME_MAX 28                                     //分区名最大长度

//bdev类型
#define PART_WHOLE   0  //整盘
#define PART_PRIMARY 1  //主分区
#define PART_LOGICAL 2  //逻辑分区

//命名风格
#define GD_STYLE_SD   0  //sda/sda1
#define GD_STYLE_NVME 1  //nvme0n1/nvme0n1p1

//bio操作
#define BIO_READ  0 //读
#define BIO_WRITE 1 //写

#define BIO_VEC_MAX 8   //单个bio最多段数

struct gendisk;
typedef struct bdev bdev_t;//分区块设备

//一个物理盘
typedef struct gendisk {
    disk_info_t  gd_disk_copy;          //磁盘描述副本
    disk_info_t *gd_disk;               //指向副本
    char         gd_name[16];           //"nvme0n1"/"sda"
    int          gd_style;              //GD_STYLE_*
    uint64_t     gd_nr_sectors;         //总扇区数
    uint32_t     gd_sector_size;        //扇区大小
    uint32_t     gd_max_transfer;       //单条命令最多扇区数
    uint64_t     gd_last_usable_lba;    //GPT的最后一个可用LBA
    bdev_t      *gd_whole;              //整盘块设备
    int          gd_nr_parts;           //已发现分区数
    int          gd_scanned;            //分区表是否已扫描
    struct list_node gd_partitions;     //bdev链表(含整盘)
    struct list_node gd_list;
} gendisk_t;

//分区
struct bdev {
    char      bd_name[BLK_NAME_MAX];       //"nvme0n1p1"/"sda1"
    gendisk_t *bd_disk;                    //所属物理盘
    uint64_t  bd_start;                    //相对物理盘的起始扇区
    uint64_t  bd_nr_sectors;               //扇区数
    uint32_t  bd_block_size;               //FS逻辑块大小(512/1024/2048/4096)
    uint32_t  bd_major, bd_minor;          //设备号
    int       bd_part_type;                //PART_*
    int       bd_part_no;                  //分区号
    char      bd_part_name[BLK_PARTNAME_MAX];//分区类型名(GPT GUID对应名/whole)
    char      bd_label[BLK_PARTNAME_MAX];  //GPT 分区名(UTF-16 转 ASCII)
    char      bd_fs_type[BLK_FSTYPE_MAX];  //探测到的文件系统
    uint32_t  bd_fs_block_size;            //探测到的FS块大小
    void     *bd_holder;                   //挂载者
    struct list_node bd_list;
};

typedef struct bio_vec {
    void    *bv_data;
    uint32_t bv_len;
} bio_vec_t;

typedef struct bio {
    bdev_t   *bi_bdev;
    uint64_t  bi_sector;//相对bdev的起始扇区
    uint32_t  bi_size;//总字节数
    int       bi_op;//BIO_READ/BIO_WRITE
    int       bi_status;
    uint16_t  bi_vcnt;
    bio_vec_t bi_vec[BIO_VEC_MAX];
} bio_t;

//初始化与注册
void BlkInit(void);//初始化块设备层
gendisk_t *BlkAddDisk(disk_info_t *disk);//注册一块物理盘
bdev_t *BlkNewBdev(gendisk_t *gd, uint64_t start, uint64_t nr_sectors,int part_type, int part_no, const char *type_name);//分配一个bdev并挂到盘的链表上
int BlkPartScanAll(void);//扫描全部盘的分区表并识别文件系统
//查询
int BlkDiskCount(void);//磁盘总数
gendisk_t *BlkDiskAt(int idx);//获取磁盘池中的某个盘
int BlkBdevCount(void);//分区块设备总数
bdev_t *BlkBdevAt(int idx);//获取bdev池中的某个bdev
bdev_t *BlkWholeDevice(gendisk_t *gd);//检查设备是否为整盘块设备
bdev_t *BlkNameToBdev(const char *name);//由blk名查bdev
bdev_t *BlkFindBdev(uint64_t start_lba, uint64_t nr_sectors);//按分区起始LBA查
//I/O
int submit_bio(bio_t *bio);//同步提交
int bdev_read(bdev_t *bd, uint64_t off, void *buf, uint32_t len);//按字节读
int bdev_write(bdev_t *bd, uint64_t off, const void *buf, uint32_t len);//按字节写

void BlkDumpAll(void);//打印全部盘与分区(相当于内核版 lsblk)

#endif
