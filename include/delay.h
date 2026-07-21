/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * 参考：Linux 7.1.3 arch/x86/lib/delay.c (delay_loop / delay_tsc)
 *              arch/x86/include/asm/delay.h
 *              Intel SDM Vol.2B RDTSC
 */

#ifndef _DELAY_H_
#define _DELAY_H_

#include <types.h>
#include <drives/timer.h>

uint64_t rdtsc(void);//读取TSC当前值
void udelay(uint64_t us);//微秒级忙等待延迟
void mdelay(uint64_t ms);//毫秒级忙等待延迟

#endif
