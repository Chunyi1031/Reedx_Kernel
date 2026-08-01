/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * drives/timer.c — TSC 校准 (PIT + CPU频率交叉验证)
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/tsc.c
 *              Intel SDM Vol.3 §17.17
 */

#include <drives/timer.h>
#include <io.h>
#include <print.h>

uint64_t tsc_freq_hz = 0;

static inline uint64_t rdtsc_serialized(void){
	uint32_t low, high;
	__asm__ volatile ("lfence\n	rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

void tsc_calibrate(void){
	uint32_t pit_count;
	uint64_t tsc_start, tsc_end, tsc_diff;
	uint64_t pit_tsc_freq;
	pit_count = PIT_OSC_FREQ / OS_TICK_HZ_CAL;
	outb(PIT_CMD_MODE_PORT, 0x30);
	io_wait();
	outb(PIT_CHL0_DATA_PORT, pit_count & 0xFF);
	io_wait();
	outb(PIT_CHL0_DATA_PORT, (pit_count >> 8) & 0xFF);
	io_wait();
	tsc_start = rdtsc_serialized();
	for (;;) {
		uint16_t cur;
		outb(PIT_CMD_MODE_PORT, 0x00);
		cur = inb(PIT_CHL0_DATA_PORT);
		cur |= (uint16_t)inb(PIT_CHL0_DATA_PORT) << 8;
		if(cur == 0 || cur > pit_count)break;
	}
	tsc_end = rdtsc_serialized();
	tsc_diff = tsc_end - tsc_start;
	pit_tsc_freq = tsc_diff * (1000 / CALIBRATION_MS);
	early_printk("TSC calib: PIT=%lu Hz, CPU_freq=%lu Hz\n",
	             (unsigned long)pit_tsc_freq, (unsigned long)SYSTEM_CPU_Fquency);
	//原理：SYSTEM_CPU_Fquency==0表示未从UEFI获取CPU频率，此时交叉验证毫无意义，
	//      直接信任PIT测量结果。只有当CPU频率已知时才做±2x合理性检查。
	if (SYSTEM_CPU_Fquency == 0) {
		tsc_freq_hz = pit_tsc_freq;
	} else if (pit_tsc_freq > SYSTEM_CPU_Fquency / 2
	           && pit_tsc_freq < SYSTEM_CPU_Fquency * 2) {
		tsc_freq_hz = pit_tsc_freq;
	} else {
		tsc_freq_hz = SYSTEM_CPU_Fquency;
	}
	if (tsc_freq_hz < 1000000)tsc_freq_hz = 1000000000;
	early_printk("TSC final: %lu Hz\n", (unsigned long)tsc_freq_hz);
}
