/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * kernel/desc/irq.c — 硬件中断管理 (APIC 架构)
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/apic/apic.c (setup_APIC_timer /
 *       setup_local_APIC)
 *              arch/x86/kernel/irq.c (通用 IRQ 分发)
 */

#include <irq.h>
#include <drives/timer.h>

volatile uint64_t SYSTEM_TimerTicks = 0;

//屏蔽PIC中断
static void pic_disable(void){
	outb(0x21, 0xFF);
	outb(0xA1, 0xFF);
}

//发送中断结束信号
void send_eoi(uint8_t irq){
	(void)irq;
	lapic_send_eoi();
}

void irq_dispatch(uint32_t vector){
	vector -= IRQ_VECTOR_BASE;
	switch (vector) {
	case IRQ_TIMER:
		SYSTEM_TimerTicks++;
		break;
	default:
		break;
	}
	send_eoi((uint8_t)vector);
}

//APIC中断系统初始化
void InitAPIC(void){
	uint32_t apic_timer_freq;
	int i;
	pic_disable();
	lapic_enable();
	apic_timer_freq = lapic_timer_calibrate();
	lapic_timer_init(apic_timer_freq, IRQ_VECTOR_BASE + IRQ_TIMER);
	for (i = 0; i < NUM_IRQ_VECTORS; i++)set_intr_gate(IRQ_VECTOR_BASE + i,(void *)(irq_entries_start + i * 16));
}
