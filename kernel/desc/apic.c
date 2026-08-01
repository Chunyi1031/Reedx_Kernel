/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * kernel/desc/apic.c — Local APIC 初始化、定时器校准、MADT解析、I/O APIC配置
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/apic/apic.c (calibrate_APIC_clock / acpi_parse_madt)
 *              arch/x86/include/asm/apic.h (native_apic_mem_read/write)
 *              ACPI Specification 6.5 §5.2.12 (MADT)
 *              Intel 64 and IA-32 Architectures SDM Vol.3 §10.5 (APIC Timer)
 */

#include <apic.h>
#include <irq.h>
#include <drives/timer.h>
#include <print.h>
#include <delay.h>
#include <klib.h>

//LAPIC基址（xAPIC MMIO 模式）
static uintptr_t lapic_base = 0;

//I/O APIC MMIO基址，由 apic_parse_madt() 根据MADT填入，默认值兜底
static uintptr_t ioapic_base = IO_APIC_DEFAULT_PHYS_BASE;

//MADT解析结果：全局存储，ioapic_init()依赖此结构获取正确的IOAPIC基址和GSI路由
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
	//如果MADT解析给出了type 5覆盖地址且与MSR一致，以MSR为准
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
	//停止旧定时器
	lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_TIMER_ONESHOT | LAPIC_LVT_MASKED);
	lapic_write(LAPIC_TIMER_INITCNT, 0);
	lapic_timer_set_divisor(LAPIC_TIMER_DIV_16);//设置分频器为16
	divisor = 16;
	ticks_per_interrupt = freq_hz / (divisor * OS_TICK_HZ);//计算每次中断的初始计数值
	//启动周期定时器
	lapic_write(LAPIC_LVT_TIMER,vector | LAPIC_LVT_TIMER_PERIODIC | LAPIC_LVT_DM_FIXED);
	lapic_write(LAPIC_TIMER_INITCNT, ticks_per_interrupt);
}

//从I/O APIC寄存器读32位值：先写索引到IOREGSEL，再从IOWIN读数据
static uint32_t ioapic_read(uint32_t reg){
	*(volatile uint32_t *)(ioapic_base + IOAPIC_IOREGSEL) = reg;
	return *(volatile uint32_t *)(ioapic_base + IOAPIC_IOWIN);
}

//向I/O APIC寄存器写32位值：先写索引到IOREGSEL，再写数据到IOWIN
static void ioapic_write(uint32_t reg, uint32_t val){
	*(volatile uint32_t *)(ioapic_base + IOAPIC_IOREGSEL) = reg;
	*(volatile uint32_t *)(ioapic_base + IOAPIC_IOWIN) = val;
}

//读取BSP的LAPIC ID（APIC ID寄存器bits 24-31）
static uint8_t lapic_get_id(void)
{
	return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
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
	APIC_MADT.lapic_addr = madt->address;//MADT表头中的LAPIC物理基址
	//遍历所有子表
	entry = (uint8_t *)(madt + 1);
	end = (uint8_t *)madt + madt->header.length;
	while (entry < end) {
		struct acpi_subtable_header *sub = (struct acpi_subtable_header *)entry;
		if (sub->length == 0)break;
		switch (sub->type) {
		//LAPIC
		case ACPI_MADT_TYPE_LOCAL_APIC: {
			struct acpi_madt_local_apic *lapic = (struct acpi_madt_local_apic *)entry;
			if ((lapic->lapic_flags & 1) && !APIC_MADT.bsp_lapic_id)APIC_MADT.bsp_lapic_id = lapic->id;//第一个可用的CPU作为BSP
			break;
		}
		//IOAPIC
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
		//中断重映射
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
		//LAPIC地址覆盖
		case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE: {
			struct acpi_madt_local_apic_override *lapo = (struct acpi_madt_local_apic_override *)entry;
			APIC_MADT.lapic_addr_override = (uint32_t)lapo->address;
			break;
		}
		default:
			break;
		}
		entry += sub->length;
	}
	//对未通过override重映射的ISA IRQ，默认采用1:1映射
	for (i = 0; i < 16; i++) {
		if (APIC_MADT.gsi_map[i] == 0 && i > 0)APIC_MADT.gsi_map[i] = (uint32_t)i;
	}
	APIC_MADT.parsed = true;
	return 0;
}

//配置I/O APIC重定向表项，通过GSI号直接定位IOAPIC引脚并设置路由
static void ioapic_set_irq(uint32_t gsi, uint8_t vector, uint16_t mps_flags, _Bool unmask){
	uint32_t idx_low, idx_high;
	uint32_t low, high;
	uint8_t bsp_id;
	uint8_t max_entries;
	uint8_t polarity_low, trigger_level;
	if (APIC_MADT.num_ioapics == 0)return;
	max_entries = ioapic_get_max_entries() + 1;
	if (gsi < APIC_MADT.ioapics[0].gsi_base)return;
	gsi -= APIC_MADT.ioapics[0].gsi_base;
	if (gsi >= max_entries)return;
	idx_low = IOAPIC_REDTBL_BASE + gsi * 2;
	idx_high = idx_low + 1;
	bsp_id = lapic_get_id();
	low = vector & IOAPIC_REDTBL_VECTOR_MASK;
	low |= IOAPIC_REDTBL_DELIVERY_MODE_FIXED;
	low |= IOAPIC_REDTBL_DEST_MODE_PHYSICAL;
	//根据MPS flags设置极性：bit0-1为极性掩码，3表示低电平有效
	polarity_low = ((mps_flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_LOW) ? 1 : 0;
	if (polarity_low)low |= IOAPIC_REDTBL_POLARITY_LOW;
	//根据MPS flags设置触发模式：bit2-3为触发掩码，3表示电平触发
	trigger_level = ((mps_flags & ACPI_MADT_TRIGGER_MASK) == ACPI_MADT_TRIGGER_LEVEL) ? 1 : 0;
	if (trigger_level)low |= IOAPIC_REDTBL_TRIGGER_LEVEL;
	if (!unmask)low |= IOAPIC_REDTBL_MASK;
	high = ((uint32_t)bsp_id << 24);
	ioapic_write(idx_low, low);
	ioapic_write(idx_high, high);
}

//初始化I/OAPIC
void ioapic_init(void){
	int i;
	uint16_t flags;
	if (APIC_MADT.num_ioapics == 0)ioapic_base = IO_APIC_DEFAULT_PHYS_BASE;
	else ioapic_base = APIC_MADT.ioapics[0].mmio_base;
	//遍历16条ISA IRQ，找到对应的GSI
	for (i = 0; i < 16; i++) {
		uint32_t gsi = APIC_MADT.gsi_map[i];
		uint8_t vector = IRQ_VECTOR_BASE + (uint8_t)i;
		int j;
		flags = 0;
		//查找是否有针对此IRQ的override条目，应用其极性/触发标志
		for (j = 0; j < APIC_MADT.num_overrides; j++) {
			if (APIC_MADT.overrides[j].source_irq == (uint8_t)i) {
				flags = APIC_MADT.overrides[j].flags;
				break;
			}
		}
		ioapic_set_irq(gsi, vector, flags, i == IRQ_KEYBOARD);
	}
}
