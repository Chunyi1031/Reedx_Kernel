#include <irq.h>
#include <drives/timer.h>
#include <drives/ps2kbd.h>
#include <print.h>
#include <task.h>
#include <futex.h>

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
		SYSTEM_TimerTicks++;//增加计数
		timeout_wake_check();//检查并唤醒超时的msleep任务
		futex_timeout_check();//检查并唤醒超时的futex等待者
		send_eoi((uint8_t)vector);//发送EOI
		schedule();//调度
		return;
	case IRQ_KEYBOARD:
		Keyboard_IRQ();
		break;
	default:
		break;
	}
	send_eoi((uint8_t)vector);
}

//APIC中断系统初始化
void InitAPIC(void){
	uint32_t apic_timer_freq;
	int i, ret;
	ret = apic_parse_madt(SYSTEM_ACPI.madt);//从MADT解析APIC
	if (ret != 0)early_printk("APIC MADT parse failed: %d, using defaults\n", ret);
	pic_disable();//禁用PIC
	lapic_enable();//启用LAPIC
	apic_timer_freq = lapic_timer_calibrate();//校准APIC定时器频率
	lapic_timer_init(apic_timer_freq, IRQ_VECTOR_BASE + IRQ_TIMER);//设置定时器中断
	ioapic_init();
	for (i = 0; i < NUM_IRQ_VECTORS; i++)set_intr_gate(IRQ_VECTOR_BASE + i,(void *)(irq_entries_start + i * 16));//设置IDT
}
