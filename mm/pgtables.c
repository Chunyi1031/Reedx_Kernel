#include <mm/pgtables.h>
#include <mm/pmm.h>

#define PHYS_TO_VIRT_TEMP(paddr)  ((void*)(uintptr_t)(paddr))
#define VIRT_TO_PHYS_TEMP(vaddr)  ((uintptr_t)(vaddr))

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