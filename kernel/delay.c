#include <delay.h>

//读取TSC当前值
uint64_t rdtsc(void){
	uint32_t low, high;
	__asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

//微秒级忙等待延迟
void udelay(uint64_t us){
	uint64_t ticks_per_us, target;
	if (!tsc_freq_hz || !us)
		return;
	ticks_per_us = tsc_freq_hz / 1000000ULL;
	target = rdtsc() + us * ticks_per_us;
	while (rdtsc() < target)
		__asm__ volatile ("pause");
}

//毫秒级忙等待延迟
void mdelay(uint64_t ms){
	udelay(ms * 1000);
}