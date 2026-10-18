// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/drives/bcache.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 块缓存(buffer cache)
 */
#ifndef _DRIVES_BCACHE_H_
#define _DRIVES_BCACHE_H_

#include <klib.h>
#include <drives/blkdev.h>

#define BC_PAGE_SIZE   4096     //页大小
#define BC_MAX_PAGES   1024     //描述符池大小(=4MB缓存上限)
#define BC_HASH_BUCKETS 256     //哈希桶数

//页状态位
#define BP_VALID   0x01   //内容已从磁盘读入
#define BP_DIRTY   0x02   //有未落盘的修改
#define BP_LOCKED  0x04   //正在做I/O
#define BP_DROP    0x08   //用完立刻丢弃
#define BP_ONLRU   0x10   //当前挂在LRU链表上

struct bcache_page;

typedef struct buffer_head {
    struct bcache_page *b_page;
    uint64_t b_block;   //相对bdev的块号
    uint8_t *b_data;    //指向页内对应偏移
    uint32_t b_size;    //bd_block_size
} buffer_head_t;

//初始化/统计
void BCacheInit(uint64_t max_pages);//块缓存初始化
_Bool BCacheReady(void);//缓存是否已初始化
void BCacheSetLimit(uint64_t max_pages);//调整容量上限(页)
uint64_t BCacheLimit(void);//当前容量上限(页)
void BCacheStats(void);//打印命中/未命中/脏页/淘汰统计
int BCacheSelfTest(bdev_t *bd);//读同一页两次, 验证命中(0=通过)

//读
int  sb_bread(bdev_t *bd, uint64_t block, buffer_head_t *bh);//必要时从磁盘填充
int  sb_bget (bdev_t *bd, uint64_t block, buffer_head_t *bh);//只取不读

//释放/标脏/回写
void sb_brelse(buffer_head_t *bh);//释放引用
void sb_bforget(buffer_head_t *bh);//释放并丢弃该页
void sb_mark_dirty(buffer_head_t *bh);//只标脏, 不落盘
void sb_bwrite_dirty(buffer_head_t *bh);//标脏并立刻落盘

//同步
int BCacheFlush(void);//所有脏页落盘
int BCacheSyncBdev(bdev_t *bd);//单个设备的脏页落盘
int BCacheInvalidate(bdev_t *bd);//丢弃某设备的全部缓存

#endif
