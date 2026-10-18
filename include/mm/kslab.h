// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/mm/kslab.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 内核小对象分配器(slab)
 */
#ifndef _MM_KSLAB_H_
#define _MM_KSLAB_H_

#include <klib.h>

struct kmem_cache;

void  KslabInit(void);//初始化分配器
/**
 * @brief 创建/复用一个对象缓存
 * @param name     名字(只用于统计/调试)
 * @param obj_size 对象大小（内部按 16 字节对齐）
 * @return 缓存句柄，失败NULL
 */
struct kmem_cache *KslabCreate(const char *name, uint32_t obj_size);
void *KslabAlloc(struct kmem_cache *c);//分配一个对象
void *KslabZalloc(struct kmem_cache *c);//分配并清零
void KslabFree(struct kmem_cache *c, void *obj);//归还对象

uint32_t KslabObjectSize(struct kmem_cache *c);//对象大小
uint32_t KslabPageCount(struct kmem_cache *c);//已占用的页数
uint32_t KslabInUse(struct kmem_cache *c);//已分配对象数
void KslabStats(void);//打印每个cache的使用情况

#endif
