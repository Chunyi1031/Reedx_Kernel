// SPDX-License-Identifier: GPL-2.0-only
/*
 * 由Hermes Agent + DeepSeek-V4-Pro生成
 *
 * kernel/desc/exceptions.c — 异常分发与注册
 * 
 * Copyright (C) 2026 Liu Chunyi
 */

#include <idt.h>
#include <serial.h>
#include <klib.h>
#include <print.h>
#include <signals.h>
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

//按SIG_DFL语义终止出错进程
static void __attribute__((noreturn)) user_exception_exit(int sig){
	if(current_task && current_task->mm){
		current_task->sig_exit = sig;
		UserTaskExit(128 + sig);
	}
	SYSTEM_STOP();
}

//异常向量转默认信号
static int exc_to_signal(uint32_t vector){
	switch(vector){
	case INT_GATE_DE: return SIGFPE;
	case INT_GATE_MF: return SIGFPE;
	case INT_GATE_UD: return SIGILL;
	case INT_GATE_BP: return SIGTRAP;
	default:          return SIGSEGV;
	}
}

//读控制寄存器, 崩溃现场里一起打出来
static inline uint64_t read_cr0(void){ uint64_t v; __asm__ volatile("mov %%cr0,%0" : "=r"(v)); return v; }
static inline uint64_t read_cr2(void){ uint64_t v; __asm__ volatile("mov %%cr2,%0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void){ uint64_t v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void){ uint64_t v; __asm__ volatile("mov %%cr4,%0" : "=r"(v)); return v; }

//崩溃信息同时打到屏幕和串口
static void panic_print(const char *fmt, ...){
        char buf[256];
        __builtin_va_list args;
        __builtin_va_start(args, fmt);
        vsprintf(buf, fmt, args);
        __builtin_va_end(args);
        TTY_Print(buf, CurrentConsoleStyle.TextColor);
        SerialWriteString(SERIAL_COM1, buf);
}

//把整帧寄存器与控制寄存器打出来
static void dump_frame(const char *reason, const exc_frame_t *f){
        panic_print("\n==:( KERNEL CRASH: %s ):===\n", reason);
        panic_print("vector=%u errcode=%p\n", (unsigned)f->vector, (void*)f->errcode);
        panic_print("RAX=%p RBX=%p RCX=%p RDX=%p\n", (void*)f->rax, (void*)f->rbx, (void*)f->rcx, (void*)f->rdx);
        panic_print("RSI=%p RDI=%p RBP=%p RSP=%p\n", (void*)f->rsi, (void*)f->rdi, (void*)f->rbp, (void*)f->rsp);
        panic_print("R8 =%p R9 =%p R10=%p R11=%p\n", (void*)f->r8, (void*)f->r9, (void*)f->r10, (void*)f->r11);
        panic_print("R12=%p R13=%p R14=%p R15=%p\n", (void*)f->r12, (void*)f->r13, (void*)f->r14, (void*)f->r15);
        panic_print("RIP=%p CS=%p RFLAGS=%p SS=%p\n", (void*)f->rip, (void*)f->cs, (void*)f->rflags, (void*)f->ss);
        panic_print("CR0=%p CR2=%p CR3=%p CR4=%p\n", (void*)read_cr0(), (void*)read_cr2(), (void*)read_cr3(), (void*)read_cr4());
        if(current_task)panic_print("task: pid=%d name=%s kstack=%p\n",(int)current_task->pid, current_task->name, current_task->kernel_stack);
        else panic_print("task: none\n");
}

//异常公共分发函数
void exc_dispatch(exc_frame_t *f){
	uint32_t vector = (uint32_t)f->vector;
	uintptr_t rip   = (uintptr_t)f->rip;
	uint64_t cs     = f->cs;
	if (vector < NUM_EXCEPTION_VECTORS){
		if(vector == INT_GATE_NMI){
			static volatile uint64_t nmi_counter = 0;
			nmi_counter++;
			return;
		}
		if((cs & 3) == 3){
			//用户态异常
			early_printk("%s [USER] pid=%d RIP=%p\n",exception_names[vector],current_task?(int)current_task->pid:-1,rip);
			user_exception_exit(exc_to_signal(vector));
		}
		print_error();
		dump_frame(exception_names[vector], f);
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

//写时复制处理
static int do_wp_page(mm_struct *mm, uintptr_t fault_addr){
	if (!mm || !mm->pgd) return -1;
	uintptr_t vaddr = fault_addr & PAGE_MASK;
	//逐级定位并修复路径上的 COW/只读中间条目
	uint64_t *table = (uint64_t*)PHYS_TO_VIRT((uintptr_t)mm->pgd);
	int index = (int)PML4_INDEX(vaddr);
	uint64_t entry = table[index];
	if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return -1;
	if (entry & PTE_COW) table[index] = (entry & ~PTE_COW) | PTE_WRITABLE;
	table = (uint64_t*)PHYS_TO_VIRT(pte_get_paddr(table[index]));
	index = (int)PDPT_INDEX(vaddr);
	entry = table[index];
	if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return -1;
	if (entry & PTE_COW) table[index] = (entry & ~PTE_COW) | PTE_WRITABLE;
	table = (uint64_t*)PHYS_TO_VIRT(pte_get_paddr(table[index]));
	index = (int)PD_INDEX(vaddr);
	entry = table[index];
	if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return -1;
	if (entry & PTE_COW) table[index] = (entry & ~PTE_COW) | PTE_WRITABLE;
	table = (uint64_t*)PHYS_TO_VIRT(pte_get_paddr(table[index]));
	//叶子PTE
	index = (int)PT_INDEX(vaddr);
	uint64_t *pte = &table[index];
	entry = *pte;
	if (!(entry & PTE_PRESENT)) return -1;
	if (!(entry & PTE_COW)) return -1;//非COW页
	//分配新物理页并拷贝旧内容
	void *new_paddr = Pmm_Malloc(1);
	if (!new_paddr) return -1;
	memcpy((void*)PHYS_TO_VIRT((uintptr_t)new_paddr),(void*)PHYS_TO_VIRT(pte_get_paddr(entry)), PAGE_SIZE);
	//更新叶子PTE
	uint64_t new_pte = (uintptr_t)new_paddr | ((entry & 0xFFFULL) & ~PTE_COW) | PTE_WRITABLE;
	new_pte |= entry & PTE_NO_EXECUTE;
	*pte = new_pte;
	__asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
	mm->rss++;//新页计入常驻集
	//解除当前mm对旧COW页的引用(仍有共享者时保留,归零才释放)
	Pmm_Free((void*)pte_get_paddr(entry),1);
	return 0;
}

void interrupt_PF(exc_frame_t *f){
	uint64_t fault_addr = read_cr2();
	uint64_t error_code = f->errcode;
	uintptr_t rip       = (uintptr_t)f->rip;
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
				if (!(vma->vm_flags & VM_EXEC) && cpu_nx_enabled) flags |= PTE_NO_EXECUTE;
				//映射（用户页表 CR3 下安全：页表遍历经高半，不用 get_pte/vmm_map_page）
				if (vmm_map_user_page(current_task->mm, page_vaddr, (uintptr_t)paddr, flags) != 0) {
					Pmm_Free(paddr, 1);
					goto pf_error;
				}
				current_task->mm->rss++;
				return;
			}
		}
	}
	//写时复制
	if ((error_code & PF_ERR_PRESENT) && (error_code & PF_ERR_WRITE)) {
		if (current_task && current_task->mm && fault_addr < USER_VADDR_MAX) {
			vm_area_t *vma = find_vma(current_task->mm, fault_addr);
			if (vma && (vma->vm_flags & VM_WRITE) &&
			    do_wp_page(current_task->mm, fault_addr) == 0) return;
		}
	}
pf_error:
	//用户态缺页且无法处理
	if(error_code & PF_ERR_USER){
		early_printk("%s [USER] pid=%d RIP=%p CR2=%p ec=%X\n",exception_names[INT_GATE_PF],current_task?(int)current_task->pid:-1,rip,fault_addr,error_code);
		user_exception_exit(SIGSEGV);
	}
	print_error();
	dump_frame(exception_names[INT_GATE_PF], f);
	TTY_Print(error_code & PF_ERR_USER ? "[USER]" : "[SUPER]", error_code & PF_ERR_USER ? COLOR_YELLOW : COLOR_CYAN);
	TTY_Print(error_code & PF_ERR_WRITE ? "[WRITE]" : "[READ]", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_EXEC ? "[EXEC]" : "", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_PRESENT ? "[PRESENT]" : "[NOT PRESENT]", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_RSVD ? "[RESERVED]" : "", COLOR_YELLOW);
	TTY_PrintChar('\n',0);
	DrawString("The machine needs to restart\n",SYSTEM_ScreenInfo.Width/2-145,SYSTEM_ScreenInfo.Height/2-8,COLOR_YELLOW);
	SYSTEM_STOP();
}

//#GP容忍探测:x2APIC等MSR能力探针使用。
volatile uintptr_t gp_recover_ip = 0;
volatile uint64_t gp_probe_active = 0;

void interrupt_GP(exc_frame_t *f){
	uint64_t error_code = f->errcode;
	uintptr_t rip       = (uintptr_t)f->rip;
	uint64_t cs         = f->cs;
	if(gp_probe_active && gp_recover_ip){
		gp_probe_active = 0;
		return;
	}
	//用户态#GP
	if((cs & 3) == 3){
		early_printk("%s [USER] pid=%d ec=%p RIP=%p\n",exception_names[INT_GATE_GP],current_task?(int)current_task->pid:-1,error_code,rip);
		user_exception_exit(SIGSEGV);
	}
	print_error();
	dump_frame(exception_names[INT_GATE_GP], f);
	DrawString("The machine needs to restart\n",SYSTEM_ScreenInfo.Width/2-145,SYSTEM_ScreenInfo.Height/2-8,COLOR_YELLOW);
	SYSTEM_STOP();
}

void setup_exceptions(void){
	for (int i = 0; i < NUM_EXCEPTION_VECTORS; i++)set_trap_gate(i, exc_entries_start + i * 16);
	set_trap_gate(INT_GATE_GP,int_13_handler);
	set_trap_gate(INT_GATE_PF,int_14_handler);
}
