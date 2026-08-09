#include <mm/vmm.h>
#include <mm/pgtables.h>
#include <mm/pmm.h>
#include <boot.h>
#include <print.h>

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
