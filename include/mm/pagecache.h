/**
 * include/mm/pagecache.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * Reedx文件页缓存(page cache)
 */
#ifndef _MM_PAGECACHE_H_
#define _MM_PAGECACHE_H_

#include <klib.h>

typedef uint64_t (*pcache_fill_t)(void *ctx, uint64_t off, void *buf, uint64_t len);//从文件的off处读取len字节到buf，返回实际读到的字节数

void PageCacheInit(void);//初始化缓存
_Bool PageCacheReady(void);//缓存是否可用
void PageCacheSetLimit(uint64_t pages);//设置容量上限(页)
uint64_t PageCacheLimit(void);//当前容量上限(页)
void PageCacheFlush(void);//清空缓存并释放全部缓存页
void PageCacheInvalidate(uint64_t ino);//使某文件的全部缓存页失效
void PageCacheInvalidateRange(uint64_t ino, uint64_t off, uint64_t len);//使某文件的一段区间失效
/**
 * @brief 按需读取(带预读)
 * @param ino   文件标识
 * @param off   文件内起始偏移
 * @param buf   目标缓冲
 * @param len   读取长度
 * @param fsize 文件当前大小(参与缓存键, 调用方须保证与实际一致)
 * @param fill  未命中时的填充回调
 * @param ctx   传给填充回调的上下文
 * @return 实际拷贝到buf的字节数; 小于len时由调用方决定是否回退直接读
 */
uint64_t PageCacheRead(uint64_t ino, uint64_t off, void *buf, uint64_t len,uint64_t fsize, pcache_fill_t fill, void *ctx);
void PageCacheStats(void);//打印统计信息

#endif
