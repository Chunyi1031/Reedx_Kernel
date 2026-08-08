#ifndef _MM_PAGE_TABLES_H_
#define _MM_PAGE_TABLES_H_

#include <klib.h>

#define PAGE_SIZE 4096
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define PML4_INDEX(vaddr)   (((uint64_t)(vaddr) >> 39) & 0x1FF)
#define PDPT_INDEX(vaddr)   (((uint64_t)(vaddr) >> 30) & 0x1FF)
#define PD_INDEX(vaddr)     (((uint64_t)(vaddr) >> 21) & 0x1FF)
#define PT_INDEX(vaddr)     (((uint64_t)(vaddr) >> 12) & 0x1FF)

#define PTE_PRESENT         (1ULL << 0)     //页存在
#define PTE_WRITABLE        (1ULL << 1)     //页可写
#define PTE_USER            (1ULL << 2)     //用户可访问
#define PTE_WRITE_THROUGH   (1ULL << 3)     //写穿
#define PTE_CACHE_DISABLE   (1ULL << 4)     //禁用缓存
#define PTE_ACCESSED        (1ULL << 5)     //页已被访问
#define PTE_DIRTY           (1ULL << 6)     //页已被写入
#define PTE_HUGE            (1ULL << 7)     //大页
#define PTE_GLOBAL          (1ULL << 8)     //全局页
#define PTE_NO_EXECUTE      (1ULL << 63)    //禁止执行

typedef uintptr_t pml4_t[512];
typedef uintptr_t pdpt_t[512];
typedef uintptr_t pd_t[512];
typedef uintptr_t pt_t[512];

#define pte_get_paddr(pte)   (pte & ~0xFFFULL)
#define pte_get_flags(pte)  (pte & 0xFFFULL)
#define pte_is_present(pte) ((pte & PTE_PRESENT) != 0)
#define pte_is_huge(pte)    ((pte & PTE_HUGE) != 0)

/**
 * 获取虚拟地址对应的页表项（PTE）
 * 
 * @param pml4_phys  PML4 的物理地址
 * @param vaddr      虚拟地址
 * @param alloc      是否自动分配缺失的中间页表（1=分配，0=不分配）
 * @return           PTE 的虚拟地址指针，失败返回 NULL
 */
uintptr_t* get_pte(uintptr_t pml4_phys, uintptr_t vaddr, int alloc);

#endif