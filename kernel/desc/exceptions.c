/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * kernel/desc/exceptions.c — 异常分发与注册
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/traps.c (do_error_trap / def_err)
 *              arch/x86/kernel/idt.c (idt_setup_early_handler)
 */

#include <idt.h>
#include <serial.h>
#include <klib.h>
#include <print.h>

/* 异常名称表，与 INT_GATE_* 宏对应 */
static const char *const exception_names[] = {
	[0]  = "#DE Divide Error",
	[1]  = "#DB Debug",
	[2]  = "NMI Non-Maskable Interrupt",
	[3]  = "#BP Breakpoint",
	[4]  = "#OF Overflow",
	[5]  = "#BR Bound Range Exceeded",
	[6]  = "#UD Invalid Opcode",
	[7]  = "#NM Device Not Available",
	[8]  = "#DF Double Fault",
	[9]  = "Coprocessor Segment Overrun",
	[10] = "#TS Invalid TSS",
	[11] = "#NP Segment Not Present",
	[12] = "#SS Stack-Segment Fault",
	[13] = "#GP General Protection",
	[14] = "#PF Page Fault",
	[15] = "Reserved(15)",
	[16] = "#MF x87 FPU Error",
	[17] = "#AC Alignment Check",
	[18] = "#MC Machine Check",
	[19] = "#XM SIMD Exception",
	[20] = "#VE Virtualization Exception",
	[21] = "#CP Control Protection",
	[22] = "Reserved(22)",
	[23] = "Reserved(23)",
	[24] = "Reserved(24)",
	[25] = "Reserved(25)",
	[26] = "Reserved(26)",
	[27] = "Reserved(27)",
	[28] = "#HV Hypervisor Injection",
	[29] = "#VC VMM Communication",
	[30] = "#SX Security Exception",
	[31] = "Reserved(31)",
};

//异常公共分发函数
void exc_dispatch(uint32_t vector,uintptr_t rip){
	if (vector < NUM_EXCEPTION_VECTORS){
		if(vector == INT_GATE_NMI){
			static volatile uint64_t nmi_counter = 0;
    		nmi_counter++;
			return;
		}
		print_error();
		early_printk("%s at RIP=%p\n",exception_names[vector],rip);
	}else{
		SerialWriteString(SERIAL_COM1,"Unknown exception");
		return;
	}
	DrawString("The machine needs to restart\n",SYSTEM_ScreenInfo.Width/2-145,SYSTEM_ScreenInfo.Height/2-8,COLOR_YELLOW);
	SYSTEM_STOP();
}

void interrupt_PF(uint64_t fault_addr, uint64_t error_code){
	print_error();
	early_printk("%s\nCR2=%p  Error Code=%X\n",exception_names[INT_GATE_PF],fault_addr,error_code);
	TTY_Print(error_code & (1<<2) ? "[USER]" : "[SUPER]",error_code & (1<<2) ? COLOR_YELLOW : COLOR_CYAN);
    TTY_Print(error_code & (1<<1) ? "[WRITE]" : "[READ]", COLOR_YELLOW);
    TTY_Print(error_code & (1<<4) ? "[EXEC]" : "", COLOR_YELLOW);
	SYSTEM_STOP();
}

void interrupt_GP(uint64_t error_code, uint64_t rip){
    print_error();
	early_printk("%s\nError Code=%p RIP=%p\n",exception_names[INT_GATE_GP],error_code,rip);
	DrawString("The machine needs to restart\n",SYSTEM_ScreenInfo.Width/2-145,SYSTEM_ScreenInfo.Height/2-8,COLOR_YELLOW);
    SYSTEM_STOP();
}

void setup_exceptions(void){
	for (int i = 0; i < NUM_EXCEPTION_VECTORS; i++)set_trap_gate(i, exc_entries_start + i * 16);
	set_trap_gate(INT_GATE_GP,int_13_handler);
	set_trap_gate(INT_GATE_PF,int_14_handler);
}
