#include <mm/vmm.h>
#include <mm/pgtables.h>
#include <mm/pmm.h>
#include <boot.h>
#include <print.h>
#include <task.h>

void InitKernelMapping() {
    BootParam *bp = SYSTEM_BootParam;
    uint64_t total_mapped = 0;
    //遍历所有物理内存描述符，逐页映射
    for (int i = 0; i < MemDescNum; i++) {
        uintptr_t paddr = MemDescAddr[i].Address;//获取地址
        uint64_t pages = MemDescAddr[i].PageSize;//获取页数量
        //逐页映射
        for(uint64_t j = 0; j < pages; j++) {
            uintptr_t pa = paddr + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
            total_mapped += PAGE_SIZE;
        }
    }
    //映射内核
    if (bp && bp->KernelAddress && bp->KernelSize) {
        uint64_t k_pages = (bp->KernelSize + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t j = 0; j < k_pages; j++) {
            uintptr_t pa = bp->KernelAddress + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
    //映射内核栈
    if (bp && bp->KernelStackAddress && bp->KernelStackSize) {
        uint64_t s_pages = (bp->KernelStackSize + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t j = 0; j < s_pages; j++) {
            uintptr_t pa = bp->KernelStackAddress + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
}

__attribute__((noinline, naked))
void switch_kernel_stack_to_high(void) {
    __asm__ volatile(
        "mov %%rsp, %%rax\n\t"
        "mov $0xFFFF800000000000, %%rbx\n\t"
        "add %%rbx, %%rax\n\t"
        "mov (%%rsp), %%rcx\n\t"
        "sub $8, %%rax\n\t"
        "mov %%rcx, (%%rax)\n\t"
        "mov %%rax, %%rsp\n\t"
        "and $0xFFFFFFFFFFFFFFF0, %%rsp\n\t"
        "ret\n\t"
        :
        :
        : "rax", "rbx", "rcx", "memory"
    );
}

void switch_kernel_info_to_high(void) {
    uintptr_t old_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(old_rsp));//获取栈
    uintptr_t new_rsp = PHYS_TO_VIRT(old_rsp);
    //确保当前 RSP 所在页及前一页在高地址已映射（栈向下增长）
    uintptr_t cur_page = old_rsp & PAGE_MASK;
    for (int i = 0; i < 2; i++) {
        uintptr_t pa = cur_page - i * PAGE_SIZE;
        uintptr_t *pte = (uintptr_t*)get_pte(KERNEL_PML4, PHYS_TO_VIRT(pa), 0);
        if (!pte || !pte_is_present(*pte)) {
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
    current_task->context.rsp = new_rsp;
}