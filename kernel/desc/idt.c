/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * kernel/desc/idt.c — IDT 初始化与加载
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/idt.c (idt_setup_from_table /
 *       idt_setup_early_handler)
 *              arch/x86/include/asm/desc.h (pack_gate / native_write_idt_entry)
 */

#include <idt.h>
#include <serial.h>
#include <klib.h>

//参考Linux 7.1.3 arch/x86/kernel/idt.c中idt_table：static gate_desc idt_table[IDT_ENTRIES] __page_aligned_bss;
static struct gate_desc idt_table[IDT_ENTRIES] __attribute__((aligned(4096)));

//参考Linux 7.1.3 arch/x86/include/asm/desc.h中native_write_idt_entry
static inline void write_idt_entry(struct gate_desc *idt, int entry, const struct gate_desc *gate)
{
	idt[entry] = *gate;
}

//默认中断处理程序，由汇编入口default_int_handler_entry调用
void default_int_handler(void){
	SerialWriteString(SERIAL_COM1, "INT:Unknown interrupt\n");
}

void setup_idt(void)
{
	struct gate_desc gate;
	struct desc_ptr idtr;
	int i;
	pack_gate(&gate, GATE_INTERRUPT, (uintptr_t)default_int_handler_entry, DPL0, DEFAULT_IST,  __KERNEL_CS);
	for (i = 0; i < IDT_ENTRIES; i++)write_idt_entry(idt_table, i, &gate);
	idtr.size = (u16)(sizeof(idt_table) - 1);
	idtr.address = (uintptr_t)&idt_table;
	load_idt(&idtr);//lidt加载新IDT
}

//为指定向量注册中断处理程序（中断门）
void set_intr_gate(uint32_t vector, void *handler){
	struct gate_desc gate;
	pack_gate(&gate, GATE_INTERRUPT, (uintptr_t)handler, DPL0, DEFAULT_IST, __KERNEL_CS);
	write_idt_entry(idt_table, (int)vector, &gate);
}
//为指定向量注册中断处理程序（陷阱门）
void set_trap_gate(uint32_t vector, void *handler){
	struct gate_desc gate;
	pack_gate(&gate, GATE_TRAP, (uintptr_t)handler, DPL0, DEFAULT_IST, __KERNEL_CS);
	write_idt_entry(idt_table, (int)vector, &gate);
}
