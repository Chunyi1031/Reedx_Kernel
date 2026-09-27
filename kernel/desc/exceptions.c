/*
 * 由Hermes Agent + DeepSeek-V4-Pro生成
 *
 * kernel/desc/exceptions.c — 异常分发与注册
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

void interrupt_PF(uint64_t fault_addr, uint64_t error_code, uintptr_t rip, uintptr_t user_rsp){
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
	print_error();
	early_printk("%s pid=%d\nRIP=%p CR2=%p Error Code=%X\n",exception_names[INT_GATE_PF],current_task?(int)current_task->pid:-1,rip,fault_addr,error_code);
	(void)user_rsp;
	TTY_Print(error_code & PF_ERR_USER ? "[USER]" : "[SUPER]", error_code & PF_ERR_USER ? COLOR_YELLOW : COLOR_CYAN);
	TTY_Print(error_code & PF_ERR_WRITE ? "[WRITE]" : "[READ]", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_EXEC ? "[EXEC]" : "", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_PRESENT ? "[PRESENT]" : "[NOT PRESENT]", COLOR_YELLOW);
	TTY_Print(error_code & PF_ERR_RSVD ? "[RESERVED]" : "", COLOR_YELLOW);
	DrawString("The machine needs to restart\n",SYSTEM_ScreenInfo.Width/2-145,SYSTEM_ScreenInfo.Height/2-8,COLOR_YELLOW);
	SYSTEM_STOP();
}

//#GP容忍探测:x2APIC等MSR能力探针使用。
volatile uintptr_t gp_recover_ip = 0;
volatile uint64_t gp_probe_active = 0;

void interrupt_GP(uint64_t error_code, uint64_t rip){
    if(gp_probe_active && gp_recover_ip){
        gp_probe_active = 0;//汇编handler检测gp_recover_ip非零后跳转恢复
        return;
    }
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
