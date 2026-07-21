/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * kernel/desc/apic.c — Local APIC 初始化、定时器校准与操作
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/apic/apic.c (calibrate_APIC_clock)
 *              arch/x86/include/asm/apic.h (native_apic_mem_read/write)
 *              Intel 64 and IA-32 Architectures SDM Vol.3 §10.5 (APIC Timer)
 */

#include <apic.h>
#include <drives/timer.h>
#include <print.h>
#include <delay.h>

//LAPIC基址（xAPIC MMIO 模式）
static uintptr_t lapic_base = 0;

//读MSR,ECX=寄存器编号，结果EDX:EAX
static inline uint64_t rdmsr(uint32_t msr){
	uint32_t low, high;
	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

//写MSR：ECX=寄存器编号，EDX:EAX=值
static inline void wrmsr(uint32_t msr, uint64_t val){
	uint32_t low = (uint32_t)val;
	uint32_t high = (uint32_t)(val >> 32);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

//从LAPIC寄存器读32位值（MMIO访问）
uint32_t lapic_read(uint32_t reg){
	return *(volatile uint32_t *)(lapic_base + reg);
}

//向LAPIC寄存器写32位值（MMIO访问）
void lapic_write(uint32_t reg, uint32_t val){
	*(volatile uint32_t *)(lapic_base + reg) = val;
}

//从IA32_APIC_BASE MSR读取LAPIC物理基址
uintptr_t lapic_get_base(void){
	uint64_t msr;
	msr = rdmsr(MSR_IA32_APICBASE);
	if (!(msr & MSR_IA32_APICBASE_BASE_MASK))return APIC_DEFAULT_PHYS_BASE;
	return msr & MSR_IA32_APICBASE_BASE_MASK;
}

//使能本地APIC（xAPIC MMIO模式）
void lapic_enable(void){
	uint64_t msr;
	lapic_base = lapic_get_base();
	msr = rdmsr(MSR_IA32_APICBASE);
	msr |= MSR_IA32_APICBASE_ENABLE;
	wrmsr(MSR_IA32_APICBASE, msr);
	lapic_write(LAPIC_SPURIOUS,SPURIOUS_APIC_VECTOR | LAPIC_SPURIOUS_ENABLE | LAPIC_SPURIOUS_FOCUS_DISABLE);
}

//向LAPIC发送中断结束信号
void lapic_send_eoi(void){
	lapic_write(LAPIC_EOI, 0);
}

//设置APIC定时器分频值
void lapic_timer_set_divisor(uint32_t divisor){
	lapic_write(LAPIC_TIMER_DIV, divisor);
}

//使用TSC精确定时校准APIC定时器频率
uint32_t lapic_timer_calibrate(void)
{
	uint32_t initial_ticks, remaining_ticks, elapsed_ticks;
	uint32_t freq_hz;
	uint64_t tsc_start, tsc_target;
	lapic_timer_set_divisor(LAPIC_TIMER_DIV_1);
	lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_TIMER_ONESHOT | LAPIC_LVT_MASKED);
	tsc_start = rdtsc();
	initial_ticks = 0xFFFFFFFF;
	lapic_write(LAPIC_TIMER_INITCNT, initial_ticks);
	tsc_target = tsc_start + tsc_freq_hz / (1000 / CALIBRATION_MS);
	while (rdtsc() < tsc_target)__asm__ volatile ("pause");
	remaining_ticks = lapic_read(LAPIC_TIMER_CURCNT);
	elapsed_ticks = initial_ticks - remaining_ticks;
	freq_hz = elapsed_ticks * (1000 / CALIBRATION_MS);
	if (freq_hz < 1000000)freq_hz = 1000000000;
	return freq_hz;
}

/**
 * @brief 启动APIC周期定时器
 *
 * 先停止旧定时器（清除校准阶段残留的 Delivery Status），
 * 再配置分频器 → 周期模式 → 设定初始计数值。
 *
 * @param freq_hz 定时器频率 (Hz)，例如 100 表示每 10ms 触发一次
 * @param vector  中断向量号，应为在 IDT 中注册的有效向量
 *
 * 参考：Intel SDM Vol.3 §10.5.4 APIC Timer
 */
void lapic_timer_init(uint32_t freq_hz, uint8_t vector)
{
	uint32_t ticks_per_interrupt;
	uint32_t divisor;
	//停止旧定时器：先屏蔽，写0清INITCNT (同时清除Delivery Status)
	lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_TIMER_ONESHOT | LAPIC_LVT_MASKED);
	lapic_write(LAPIC_TIMER_INITCNT, 0);
	lapic_timer_set_divisor(LAPIC_TIMER_DIV_16);
	divisor = 16;
	ticks_per_interrupt = freq_hz / (divisor * OS_TICK_HZ);
	//配置LVT Timer
	lapic_write(LAPIC_LVT_TIMER,vector | LAPIC_LVT_TIMER_PERIODIC | LAPIC_LVT_DM_FIXED);
	lapic_write(LAPIC_TIMER_INITCNT, ticks_per_interrupt);
}
