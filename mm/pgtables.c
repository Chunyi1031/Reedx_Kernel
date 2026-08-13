#include <mm/pgtables.h>
#include <mm/pmm.h>

#define PHYS_TO_VIRT_TEMP(paddr)  ((void*)(uintptr_t)(paddr))
#define VIRT_TO_PHYS_TEMP(vaddr)  ((uintptr_t)(vaddr))

uintptr_t UEFI_PML4 = 0;
uintptr_t KERNEL_PML4 = 0;

uintptr_t get_cr3(){
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
void set_cr3(uintptr_t cr3){
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3));
}

uintptr_t* get_pte(uintptr_t pml4_phys, uintptr_t vaddr, int alloc) {
    if(!pml4_phys) return NULL;
    uintptr_t *pml4 = (uint64_t*)PHYS_TO_VIRT_TEMP(pml4_phys);//PML4物理地址转换为虚拟地址
    //PML4
    uint64_t pml4_idx = PML4_INDEX(vaddr);
    uintptr_t pml4_entry = pml4[pml4_idx];
    //如果不存在
    if(!pte_is_present(pml4_entry)){
        if (!alloc) return NULL;
        //分配PDPT页
        uintptr_t pdpt_phys = (uintptr_t)Pmm_Malloc(1);
        if (!pdpt_phys) return NULL;
        uintptr_t *pdpt = (uintptr_t*)PHYS_TO_VIRT_TEMP(pdpt_phys);
        memset(pdpt, 0, PAGE_SIZE);
        pml4[pml4_idx] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;
    }
    uintptr_t *pdpt = (uintptr_t*)PHYS_TO_VIRT_TEMP(pte_get_paddr(pml4[pml4_idx]));
    //PDPT
    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    uintptr_t pdpt_entry = pdpt[pdpt_idx];
    if(pte_is_present(pdpt_entry) && pte_is_huge(pdpt_entry))return &pdpt[pdpt_idx];
    if(!pte_is_present(pdpt_entry)){
        if (!alloc) return NULL;
        uintptr_t pd_phys = (uintptr_t)Pmm_Malloc(1);
        if (!pd_phys) return NULL;
        uintptr_t *pd = (uintptr_t*)PHYS_TO_VIRT_TEMP(pd_phys);
        memset(pd, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = pd_phys | PTE_PRESENT | PTE_WRITABLE;
    }
    uintptr_t *pd = (uintptr_t*)PHYS_TO_VIRT_TEMP(pte_get_paddr(pdpt[pdpt_idx]));
    //PD（2MB 大页检测）
    uint64_t pd_idx = PD_INDEX(vaddr);
    uintptr_t pd_entry = pd[pd_idx];
    if(pte_is_present(pd_entry) && pte_is_huge(pd_entry))return &pd[pd_idx];//2MB大页
    if (!pte_is_present(pd_entry)) {
        if (!alloc) return NULL;
        uintptr_t pt_phys = (uintptr_t)Pmm_Malloc(1);
        if (!pt_phys) return NULL;
        uintptr_t *pt = (uintptr_t*)PHYS_TO_VIRT_TEMP(pt_phys);
        memset(pt, 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t *pt = (uint64_t*)PHYS_TO_VIRT_TEMP(pte_get_paddr(pd[pd_idx]));
    //PT
    uint64_t pt_idx = PT_INDEX(vaddr);
    return &pt[pt_idx];
}

int vmm_map_page(uintptr_t pml4_phys, uintptr_t vaddr, uintptr_t paddr, uint64_t flags) {
    //参数检查
    if (!pml4_phys) return 1;
    if ((vaddr & (PAGE_SIZE - 1)) || (paddr & (PAGE_SIZE - 1))) return 2;
    //获取PTE
    uintptr_t *pte = get_pte(pml4_phys, vaddr, 1);
    if (!pte) return 3;
    if((*pte & PTE_PRESENT) && (!(*pte & PTE_CAN_COVERED)))return 4;//检查是否覆盖
    *pte = (paddr & PAGE_MASK) | (flags & (0xFFFULL | PTE_NO_EXECUTE)) | PTE_PRESENT;//设置页表项
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");//刷新TLB
    return 0;
}

int vmm_unmap_page(uintptr_t pml4_phys, uintptr_t vaddr, uintptr_t *out_paddr) {
    if (!pml4_phys) return 1;
    if (vaddr & (PAGE_SIZE - 1)) return 2;
    //获取PTE（不分配新页表）
    uint64_t *pte = get_pte(pml4_phys, vaddr, 0);
    if (!pte) return 3;
    if (!(*pte & PTE_PRESENT)) return 4;//检查是否分配
    if(out_paddr)*out_paddr = pte_get_paddr(*pte);//保存物理地址
    *pte = 0;//清除PTE
    asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");//刷新TLB
    return 0;
}

/*DeepSeek V4 Pro*/
int InitKernelPageTable() {
    if (!UEFI_PML4) return -1;

    uint64_t *uefi_pml4 = (uint64_t*)PHYS_TO_VIRT_TEMP(UEFI_PML4);

    // 分配内核 PML4 页
    uintptr_t new_pml4_phys = (uintptr_t)Pmm_Malloc(1);
    if (!new_pml4_phys) return 1;
    uint64_t *new_pml4 = (uint64_t*)PHYS_TO_VIRT_TEMP(new_pml4_phys);
    memset(new_pml4, 0, PAGE_SIZE);

    for (int pml4_idx = 0; pml4_idx < 512; pml4_idx++) {
        uint64_t pml4_entry = uefi_pml4[pml4_idx];
        if (!pte_is_present(pml4_entry)) continue;

        // 分配内核 PDPT
        uintptr_t new_pdpt_phys = (uintptr_t)Pmm_Malloc(1);
        if (!new_pdpt_phys) return -1;
        uint64_t *new_pdpt = (uint64_t*)PHYS_TO_VIRT_TEMP(new_pdpt_phys);
        uint64_t *uefi_pdpt = (uint64_t*)PHYS_TO_VIRT_TEMP(pte_get_paddr(pml4_entry));
        memset(new_pdpt, 0, PAGE_SIZE);
        new_pml4[pml4_idx] = new_pdpt_phys | PTE_PRESENT | PTE_WRITABLE;

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            uint64_t pdpt_entry = uefi_pdpt[pdpt_idx];
            if (!pte_is_present(pdpt_entry)) continue;

            if (pte_is_huge(pdpt_entry)) {
                // 1GB 大页：直接复制并添加可写
                new_pdpt[pdpt_idx] = pdpt_entry | PTE_WRITABLE;
                continue;
            }

            // 分配内核 PD
            uintptr_t new_pd_phys = (uintptr_t)Pmm_Malloc(1);
            if (!new_pd_phys) return -1;
            uint64_t *new_pd = (uint64_t*)PHYS_TO_VIRT_TEMP(new_pd_phys);
            uint64_t *uefi_pd = (uint64_t*)PHYS_TO_VIRT_TEMP(pte_get_paddr(pdpt_entry));
            memset(new_pd, 0, PAGE_SIZE);
            new_pdpt[pdpt_idx] = new_pd_phys | PTE_PRESENT | PTE_WRITABLE;

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                uint64_t pd_entry = uefi_pd[pd_idx];
                if (!pte_is_present(pd_entry)) continue;

                if (pte_is_huge(pd_entry)) {
                    // 2MB 大页：直接复制并添加可写
                    new_pd[pd_idx] = pd_entry | PTE_WRITABLE;
                    continue;
                }

                // 分配内核 PT
                uintptr_t new_pt_phys = (uintptr_t)Pmm_Malloc(1);
                if (!new_pt_phys) return -1;
                uint64_t *new_pt = (uint64_t*)PHYS_TO_VIRT_TEMP(new_pt_phys);
                uint64_t *uefi_pt = (uint64_t*)PHYS_TO_VIRT_TEMP(pte_get_paddr(pd_entry));
                memset(new_pt, 0, PAGE_SIZE);
                new_pd[pd_idx] = new_pt_phys | PTE_PRESENT | PTE_WRITABLE;

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t pt_entry = uefi_pt[pt_idx];
                    if (!pte_is_present(pt_entry)) continue;
                    new_pt[pt_idx] = pt_entry | PTE_WRITABLE;
                }
            }
        }
    }

    // 切换到内核页表
    set_cr3(new_pml4_phys);
    KERNEL_PML4 = new_pml4_phys;

    return 0;
}
/*DeepSeek V4 Pro-END*/