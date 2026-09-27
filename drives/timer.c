/*
 * 由Hermes Agent + DeepSeek-V4-Pro生成
 *
 * drives/timer.c — TSC 校准 (CPUID 0x16 → 0x15+晶振候选 → PIT)
 *
 * 原理：CPUID 0x16 直接返回CPU基准频率(MHz)，无歧义，优先使用。
 *       CPUID 0x15 需要晶振频率，多数CPU ECX=0，此时用常见值试探。
 *       PIT 仅最后兜底，因I/O延迟使其在实体机上偏差可达6x。
 *
 * 参考：Intel SDM Vol.3 §17.17
 */

#include <drives/timer.h>
#include <io.h>

uint64_t tsc_freq_hz = 0;

static inline void cpuid_leaf(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx){
	__asm__ volatile ("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(leaf));
}

static inline uint64_t rdtsc_serialized(void){
	uint32_t low, high;
	__asm__ volatile ("lfence\n\trdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

//CPUID 0x16：基准MHz（Intel Skylake+），无歧义，优先
static int tsc_from_cpuid16(uint64_t *freq){
	uint32_t eax, ebx, ecx, edx, max_leaf;
	cpuid_leaf(0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf < 0x16)return -1;
	cpuid_leaf(0x16, &eax, &ebx, &ecx, &edx);
	if (eax == 0)return -1;
	*freq = (uint64_t)eax * 1000000ULL;
	return 0;
}

//CPUID 0x15：TSC/晶振比率，ECX已知时精确，未知时用候选值试探
static int tsc_from_cpuid15(uint64_t *freq){
	uint32_t eax, ebx, ecx, edx, max_leaf, i;
	static const uint32_t crystal_hz[] = {0, 24000000, 19200000, 25000000};
	cpuid_leaf(0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf < 0x15)return -1;
	cpuid_leaf(0x15, &eax, &ebx, &ecx, &edx);
	if (eax == 0 || ebx == 0)return -1;
	if (ecx != 0) {
		*freq = (uint64_t)ecx * ebx / eax;
		return 0;
	}
	for (i = 0; i < 4; i++) {
		if (crystal_hz[i] == 0)continue;
		uint64_t f = (uint64_t)crystal_hz[i] * ebx / eax;
		if (f >= 500000000 && f <= 10000000000ULL) {
			*freq = f;
			return 0;
		}
	}
	return -1;
}

//PIT兜底：10ms计数窗口
static uint64_t tsc_from_pit(void){
	uint32_t pit_count;
	uint64_t tsc_start, tsc_end, tsc_diff;
	pit_count = PIT_OSC_FREQ / OS_TICK_HZ_CAL;
	outb(PIT_CMD_MODE_PORT, 0x30);
	io_wait();
	outb(PIT_CHL0_DATA_PORT, pit_count & 0xFF);
	io_wait();
	outb(PIT_CHL0_DATA_PORT, (pit_count >> 8) & 0xFF);
	io_wait();
	tsc_start = rdtsc_serialized();
	{
		uint32_t guard = 0;
		while (guard++ < 10000000) {
			uint16_t cur;
			outb(PIT_CMD_MODE_PORT, 0x00);
			cur = inb(PIT_CHL0_DATA_PORT);
			cur |= (uint16_t)inb(PIT_CHL0_DATA_PORT) << 8;
			if(cur == 0 || cur > pit_count)break;
		}
	}
	tsc_end = rdtsc_serialized();
	tsc_diff = tsc_end - tsc_start;
	return tsc_diff * (1000 / CALIBRATION_MS);
}

void tsc_calibrate(void){
	uint64_t freq = 0;
	if (tsc_from_cpuid16(&freq) == 0)goto done;
	if (tsc_from_cpuid15(&freq) == 0)goto done;
	freq = tsc_from_pit();
done:
	if (SYSTEM_CPU_Fquency > 0) {
		if (freq < SYSTEM_CPU_Fquency / 2 || freq > SYSTEM_CPU_Fquency * 2)freq = SYSTEM_CPU_Fquency;
	}
	if (freq < 1000000)freq = 1000000000;
	tsc_freq_hz = freq;
}
