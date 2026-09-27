/*
 * 由Hermes Agent + DeepSeek-V4-Pro生成
 *
 * Copyright (C) 2026 Liu Chunyi
 * 
 * 参考：Intel SDM Vol.2B RDTSC
 */

#ifndef _DELAY_H_
#define _DELAY_H_

#include <types.h>
#include <drives/timer.h>

uint64_t rdtsc(void);//读取TSC当前值
void udelay(uint64_t us);//微秒级忙等待延迟
void mdelay(uint64_t ms);//毫秒级忙等待延迟

#endif
