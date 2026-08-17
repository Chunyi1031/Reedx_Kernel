/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * kernel/desc/apic.c — Local APIC 初始化、定时器校准、MADT解析、I/O APIC配置
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/apic/apic.c
 *              arch/x86/include/asm/apic.h
 *              ACPI Specification 6.5 §5.2.12 (MADT)
 *              Intel SDM Vol.3 §10.12 (x2APIC)
 */

#include <apic.h>
#include <irq.h>
#include <drives/timer.h>
#include <print.h>
#include <delay.h>
#include <klib.h>
#include <mm/vmm.h>

//LAPIC基址（xAPIC MMIO 模式），x2APIC模式下不使用MMIO但保留此变量
static uintptr_t lapic_base = 0;
static _Bool lapic_x2apic_mode = false;//true=x2APIC(MSR), false=xAPIC(MMIO)

//I/O APIC MMIO基址，由 apic_parse_madt() 根据MADT填入，默认值兜底
static uintptr_t ioapic_base = IO_APIC_DEFAULT_PHYS_BASE;

//MADT解析结果
struct apic_madt_info APIC_MADT;

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

//CPUID：leaf → EAX/EBX/ECX/EDX
static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx){
	__asm__ volatile ("cpuid"
		: "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		: "a"(leaf));
}

//检查CPU是否支持x2APIC (CPUID.01H:ECX[21])
static _Bool cpu_has_x2apic(void){
	uint32_t eax, ebx, ecx, edx;
	cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
	return (ecx & CPUID_FEATURE_X2APIC) != 0;
}

//从LAPIC寄存器读32位值（自动分发 xAPIC MMIO / x2APIC MSR）
uint32_t lapic_read(uint32_t reg){
	if (lapic_x2apic_mode)return (uint32_t)rdmsr(X2APIC_MSR(reg));
	return *(volatile uint32_t *)(lapic_base + reg);
}

//向LAPIC寄存器写32位值（自动分发 xAPIC MMIO / x2APIC MSR）
void lapic_write(uint32_t reg, uint32_t val){
	if (lapic_x2apic_mode){wrmsr(X2APIC_MSR(reg), val);return;}
	*(volatile uint32_t *)(lapic_base + reg) = val;
}

_Bool lapic_is_x2apic(void){return lapic_x2apic_mode;}

//从IA32_APIC_BASE MSR读取LAPIC物理基址
//原理：xAPIC模式下返回MMIO基址（bits 12-35），x2APIC启用后MMIO基址仍保留在MSR中但不可用
uintptr_t lapic_get_base(void){
	uint64_t msr;
	msr = rdmsr(MSR_IA32_APICBASE);
	if (!(msr & MSR_IA32_APICBASE_BASE_MASK))return APIC_DEFAULT_PHYS_BASE;
	return msr & MSR_IA32_APICBASE_BASE_MASK;
}

//使能本地APIC
void lapic_enable(void){
	uint64_t msr;
	lapic_base = lapic_get_base();
	//MMIO基址转高半
	if (!lapic_x2apic_mode) lapic_base = PHYS_TO_VIRT(lapic_base);
	msr = rdmsr(MSR_IA32_APICBASE);
	//尝试x2APIC：硬件支持即启用
	if (cpu_has_x2apic()) {
		msr |= MSR_IA32_APICBASE_X2APIC;
		wrmsr(MSR_IA32_APICBASE, msr);
		lapic_x2apic_mode = true;
	}else{
		msr |= MSR_IA32_APICBASE_ENABLE;
		wrmsr(MSR_IA32_APICBASE, msr);
	}
	lapic_write(LAPIC_SPURIOUS,SPURIOUS_APIC_VECTOR | LAPIC_SPURIOUS_ENABLE | LAPIC_SPURIOUS_FOCUS_DISABLE);
}

//向LAPIC发送中断结束信号
void lapic_send_eoi(void){lapic_write(LAPIC_EOI, 0);}

//设置APIC定时器分频值
void lapic_timer_set_divisor(uint32_t divisor){lapic_write(LAPIC_TIMER_DIV, divisor);}

//获取当前LAPIC ID（xAPIC: bits 24-31, x2APIC: 全32位）
uint32_t lapic_get_id(void){
	if (lapic_x2apic_mode)return lapic_read(LAPIC_ID);//x2APIC ID寄存器返回完整32位
	return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

//使用TSC精确定时校准APIC定时器频率
uint32_t lapic_timer_calibrate(void){
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

//以指定频率启动周期定时器
void lapic_timer_init(uint32_t freq_hz, uint8_t vector){
	uint32_t ticks_per_interrupt;
	uint32_t divisor;
	lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_TIMER_ONESHOT | LAPIC_LVT_MASKED);
	lapic_write(LAPIC_TIMER_INITCNT, 0);
	lapic_timer_set_divisor(LAPIC_TIMER_DIV_16);
	divisor = 16;
	ticks_per_interrupt = freq_hz / (divisor * OS_TICK_HZ);
	lapic_write(LAPIC_LVT_TIMER,vector | LAPIC_LVT_TIMER_PERIODIC | LAPIC_LVT_DM_FIXED);
	lapic_write(LAPIC_TIMER_INITCNT, ticks_per_interrupt);
}

//从I/O APIC寄存器读32位值
static uint32_t ioapic_read(uint32_t reg){
	*(volatile uint32_t *)(ioapic_base + IOAPIC_IOREGSEL) = reg;
	return *(volatile uint32_t *)(ioapic_base + IOAPIC_IOWIN);
}

//向I/O APIC寄存器写32位值
static void ioapic_write(uint32_t reg, uint32_t val){
	*(volatile uint32_t *)(ioapic_base + IOAPIC_IOREGSEL) = reg;
	*(volatile uint32_t *)(ioapic_base + IOAPIC_IOWIN) = val;
}

//获取I/O APIC版本寄存器的最大重定向条目数
static uint8_t ioapic_get_max_entries(void){
	uint32_t ver;
	ver = ioapic_read(1);
	return (uint8_t)((ver >> 16) & 0xFF);
}

//从MADT解析APIC硬件信息
int apic_parse_madt(struct acpi_table_madt *madt){
	uint8_t *entry, *end = NULL;
	int i;
	if (!madt)return -1;
	memset(&APIC_MADT, 0, sizeof(APIC_MADT));
	APIC_MADT.lapic_addr = madt->address;
	entry = (uint8_t *)(madt + 1);
	end = (uint8_t *)madt + madt->header.length;
	while (entry < end) {
		struct acpi_subtable_header *sub = (struct acpi_subtable_header *)entry;
		if (sub->length == 0)break;
		switch (sub->type) {
		case ACPI_MADT_TYPE_LOCAL_APIC: {
			struct acpi_madt_local_apic *lapic = (struct acpi_madt_local_apic *)entry;
			if ((lapic->lapic_flags & 1) && !APIC_MADT.bsp_lapic_id)APIC_MADT.bsp_lapic_id = lapic->id;
			break;
		}
		case ACPI_MADT_TYPE_IO_APIC: {
			struct acpi_madt_io_apic *ioapic =(struct acpi_madt_io_apic *)entry;
			if (APIC_MADT.num_ioapics < MAX_IO_APICS) {
				int idx = APIC_MADT.num_ioapics++;
				APIC_MADT.ioapics[idx].id = ioapic->id;
				APIC_MADT.ioapics[idx].mmio_base = ioapic->address;
				APIC_MADT.ioapics[idx].gsi_base = ioapic->global_irq_base;
			}
			break;
		}
		case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE: {
			struct acpi_madt_interrupt_override *ovr = (struct acpi_madt_interrupt_override *)entry;
			if (APIC_MADT.num_overrides < MAX_ISA_OVERRIDES && ovr->bus == 0 && ovr->source_irq < 16) {
				int idx = APIC_MADT.num_overrides++;
				APIC_MADT.overrides[idx].bus = ovr->bus;
				APIC_MADT.overrides[idx].source_irq = ovr->source_irq;
				APIC_MADT.overrides[idx].gsi = ovr->global_irq;
				APIC_MADT.overrides[idx].flags = ovr->inti_flags;
				APIC_MADT.gsi_map[ovr->source_irq] = ovr->global_irq;
			}
			break;
		}
		case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE: {
			struct acpi_madt_local_apic_override *lapo = (struct acpi_madt_local_apic_override *)entry;
			APIC_MADT.lapic_addr_override = (uint32_t)lapo->address;
			break;
		}
		case ACPI_MADT_TYPE_LOCAL_X2APIC: {
			struct acpi_madt_local_x2apic *x2 = (struct acpi_madt_local_x2apic *)entry;
			APIC_MADT.has_x2apic_entries = 1;
			//x2APIC条目提供32位APIC ID，优先于type 0的8位ID
			if ((x2->lapic_flags & 1) && APIC_MADT.bsp_lapic_id == 0)APIC_MADT.bsp_lapic_id = x2->local_apic_id;
			break;
		}
		default:
			break;
		}
		entry += sub->length;
	}
	for (i = 0; i < 16; i++) {
		if (APIC_MADT.gsi_map[i] == 0 && i > 0)APIC_MADT.gsi_map[i] = (uint32_t)i;
	}
	APIC_MADT.parsed = true;
	return 0;
}

//配置I/O APIC重定向表项
//原理：x2APIC模式下APIC ID可达32位，但IOAPIC目的字段仅8位。
//      对单CPU(BSP)场景，取低8位即可；多CPU需逻辑目的模式，留待SMP实现。
static void ioapic_set_irq(uint32_t gsi, uint8_t vector, uint16_t mps_flags, _Bool unmask){
	uint32_t idx_low, idx_high;
	uint32_t low, high;
	uint32_t apic_id;
	uint8_t max_entries;
	uint8_t polarity_low, trigger_level;
	if (APIC_MADT.num_ioapics == 0)return;
	max_entries = ioapic_get_max_entries() + 1;
	if (gsi < APIC_MADT.ioapics[0].gsi_base)return;
	gsi -= APIC_MADT.ioapics[0].gsi_base;
	if (gsi >= max_entries)return;
	idx_low = IOAPIC_REDTBL_BASE + gsi * 2;
	idx_high = idx_low + 1;
	apic_id = lapic_get_id();
	low = vector & IOAPIC_REDTBL_VECTOR_MASK;
	low |= IOAPIC_REDTBL_DELIVERY_MODE_FIXED;
	low |= IOAPIC_REDTBL_DEST_MODE_PHYSICAL;
	polarity_low = ((mps_flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_LOW) ? 1 : 0;
	if (polarity_low)low |= IOAPIC_REDTBL_POLARITY_LOW;
	trigger_level = ((mps_flags & ACPI_MADT_TRIGGER_MASK) == ACPI_MADT_TRIGGER_LEVEL) ? 1 : 0;
	if (trigger_level)low |= IOAPIC_REDTBL_TRIGGER_LEVEL;
	if (!unmask)low |= IOAPIC_REDTBL_MASK;
	//目的字段仅8位，x2APIC的32位ID取低8位（单CPU安全）
	high = (apic_id & 0xFF) << 24;
	ioapic_write(idx_low, low);
	ioapic_write(idx_high, high);
}

//初始化I/OAPIC
void ioapic_init(void){
	int i;
	uint16_t flags;
	if (APIC_MADT.num_ioapics == 0)ioapic_base = IO_APIC_DEFAULT_PHYS_BASE;
	else ioapic_base = APIC_MADT.ioapics[0].mmio_base;
	ioapic_base = PHYS_TO_VIRT(ioapic_base);//MMIO基址转高半
	for (i = 0; i < 16; i++) {
		uint32_t gsi = APIC_MADT.gsi_map[i];
		uint8_t vector = IRQ_VECTOR_BASE + (uint8_t)i;
		int j;
		flags = 0;
		for (j = 0; j < APIC_MADT.num_overrides; j++) {
			if (APIC_MADT.overrides[j].source_irq == (uint8_t)i) {
				flags = APIC_MADT.overrides[j].flags;
				break;
			}
		}
		ioapic_set_irq(gsi, vector, flags, i == IRQ_KEYBOARD);
	}
}
