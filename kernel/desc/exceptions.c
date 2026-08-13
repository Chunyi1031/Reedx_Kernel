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
#include <task.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/pgtables.h>

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

//缺页错误码位定义
#define PF_ERR_PRESENT  (1ULL << 0)//页已存在（保护违例）
#define PF_ERR_WRITE    (1ULL << 1)//写访问
#define PF_ERR_USER     (1ULL << 2)//用户态访问
#define PF_ERR_RSVD     (1ULL << 3)//保留位被置位
#define PF_ERR_EXEC     (1ULL << 4)//取指访问

void interrupt_PF(uint64_t fault_addr, uint64_t error_code, uintptr_t rip){
	//按需分页：用户态、页不存在、非保留位错误
	if ((error_code & PF_ERR_USER) && !(error_code & PF_ERR_PRESENT) && !(error_code & PF_ERR_RSVD)) {
		if (current_task && current_task->mm) {
			vm_area_t *vma = find_vma(current_task->mm, fault_addr);
			if (vma) {
				//权限检查
				if ((error_code & PF_ERR_WRITE) && !(vma->vm_flags & VM_WRITE)) goto pf_error;
				if ((error_code & PF_ERR_EXEC) && !(vma->vm_flags & VM_EXEC)) goto pf_error;
				//分配物理页
				void *paddr = Pmm_Malloc(1);
				if (!paddr) goto pf_error;
				memset((void*)PHYS_TO_VIRT((uintptr_t)paddr), 0, PAGE_SIZE);
				//构造页表项
				uintptr_t page_vaddr = fault_addr & PAGE_MASK;
				uint64_t flags = PTE_PRESENT | PTE_USER;
				if (vma->vm_flags & VM_WRITE) flags |= PTE_WRITABLE;
				if (!(vma->vm_flags & VM_EXEC)) flags |= PTE_NO_EXECUTE;
				//映射
				if (vmm_map_page((uintptr_t)current_task->mm->pgd, page_vaddr, (uintptr_t)paddr, flags) != 0) {
					Pmm_Free(paddr, 1);
					goto pf_error;
				}
				current_task->mm->rss++;
				return;
			}
		}
	}
pf_error:
	print_error();
	early_printk("%s\nRIP=%p CR2=%p Error Code=%X\n",exception_names[INT_GATE_PF],rip,fault_addr,error_code);
	TTY_Print(error_code & PF_ERR_USER ? "[USER]" : "[SUPER]", error_code & PF_ERR_USER ? COLOR_YELLOW : COLOR_CYAN);
	TTY_Print(error_code & PF_ERR_WRITE ? "[WRITE]" : "[READ]", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_EXEC ? "[EXEC]" : "", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_PRESENT ? "[PRESENT]" : "[NOT PRESENT]", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_RSVD ? "[RESERVED]" : "", COLOR_YELLOW);
	DrawString("The machine needs to restart\n",SYSTEM_ScreenInfo.Width/2-145,SYSTEM_ScreenInfo.Height/2-8,COLOR_YELLOW);
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
