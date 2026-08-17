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
#define PTE_COW             (1ULL << 9)     //写时复制
#define PTE_NO_EXECUTE      (1ULL << 63)    //禁止执行
#define PTE_CAN_COVERED     (1ULL << 62)    //可覆盖

typedef uintptr_t pml4_t[512];
typedef uintptr_t pdpt_t[512];
typedef uintptr_t pd_t[512];
typedef uintptr_t pt_t[512];

#define pte_get_paddr(pte)   ((pte) & 0x000FFFFFFFFFF000ULL)
#define pte_get_flags(pte)  (pte & 0xFFFULL)
#define pte_is_present(pte) ((pte & PTE_PRESENT) != 0)
#define pte_is_huge(pte)    ((pte & PTE_HUGE) != 0)

extern uintptr_t UEFI_PML4;
extern uintptr_t KERNEL_PML4;

uintptr_t get_cr3();
void set_cr3(uintptr_t cr3);

/**
 * 获取虚拟地址对应的页表项（PTE）
 * 
 * @param pml4_phys  PML4 的物理地址
 * @param vaddr      虚拟地址
 * @param alloc      是否自动分配缺失的中间页表（1=分配，0=不分配）
 * @param user       新分配的中间页表是否设置 U/S=1（1=用户可访问）
 * @return           PTE 的虚拟地址指针，失败返回 NULL
 */
uintptr_t* get_pte(uintptr_t pml4_phys, uintptr_t vaddr, int alloc, int user);

/**
 * 建立虚拟地址到物理地址的映射
 * 
 * @param pml4_phys  PML4 物理地址
 * @param vaddr      虚拟地址（4KB 对齐）
 * @param paddr      物理地址（4KB 对齐）
 * @param flags      页表项权限标志
 * @return           0=成功，1=PML4无效 2=地址未对齐 3=分配失败 4=覆盖保护
 */
int vmm_map_page(uintptr_t pml4_phys, uintptr_t vaddr, uintptr_t paddr, uint64_t flags);

/**
 * 取消虚拟地址的映射
 * 
 * @param pml4_phys  PML4 物理地址
 * @param vaddr      虚拟地址（4KB 对齐）
 * @param out_paddr  输出被取消映射的物理地址（可为 NULL）
 * @return           0=成功，1=参数错误，2=地址未对齐，3=页表不存在，4=未映射
 */
int vmm_unmap_page(uintptr_t pml4_phys, uintptr_t vaddr, uintptr_t *out_paddr);

int InitKernelPageTable();//初始化内核页表

#endif