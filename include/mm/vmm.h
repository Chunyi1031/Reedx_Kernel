#ifndef _MM_VIRTUAL_MM_H_
#define _MM_VIRTUAL_MM_H_

#include <mm/pgtables.h>

#define KERNEL_VIRTUAL_ADDR_START 0xFFFF800000000000ULL
#define KERNEL_VIRTUAL_ADDR_END   0xFFFFFFFFFFFFFFFFULL
/* 用户地址空间上界（x86_64 规范，低于此地址为合法用户地址） */
#define USER_VADDR_MAX            0x0000800000000000ULL

#define PHYS_TO_VIRT(paddr) (((uintptr_t)(paddr) >= KERNEL_VIRTUAL_ADDR_START) ? (uintptr_t)(paddr) : ((uintptr_t)(paddr) + KERNEL_VIRTUAL_ADDR_START))
#define VIRT_TO_PHYS(vaddr) (((uintptr_t)(vaddr) >= KERNEL_VIRTUAL_ADDR_START) ? ((uintptr_t)(vaddr) - KERNEL_VIRTUAL_ADDR_START) : (uintptr_t)(vaddr))

#define VM_READ      0x01
#define VM_WRITE     0x02
#define VM_EXEC      0x04
#define VM_SHARED    0x08

void InitKernelMapping();//初始化内核高半映射
void switch_kernel_stack_to_high(void);//切换内核栈到高半
void switch_kernel_info_to_high(void);//设置栈信息到高地址

//虚拟内存区域（VMA）
typedef struct vm_area_struct {
    struct list_node vm_list;       //链表节点
    uintptr_t vm_start;             //起始地址（包含）
    uintptr_t vm_end;               //结束地址（不包含）
    uint64_t vm_flags;              //标志（读/写/执行）
    uint64_t vm_page_count;         //覆盖的页数
    struct mm_struct *vm_mm;        //所属的 mm_struct
} vm_area_t;

//任务内存描述符
typedef struct mm_struct {
    struct list_node mmap;          //VMA链表头
    vm_area_t *mmap_cache;          //上次查找的VMA缓存
    pml4_t *pgd;                    //PML4物理地址
    int mm_users;                   //使用此描述符的任务数
    int mm_count;                   //总引用计数
    spinlock_t mm_lock;
    //用户空间布局
    uintptr_t start_code, end_code;
    uintptr_t start_data, end_data;
    uintptr_t start_brk, brk;       //堆
    uintptr_t start_stack;          //栈起始地址
    uint64_t total_vm;              //已映射的虚拟页总数
    uint64_t rss;                   //已占用的物理页数（Resident Set Size）
} mm_struct;

mm_struct* vmm_create_address_space(void);//创建地址空间
void mmget(mm_struct *mm);//增加引用计数
void mmput(mm_struct *mm);//减少引用计数
void vmm_destroy_address_space(mm_struct *mm);//销毁地址空间
/**
 * @brief 复制地址空间
 * @param src_mm 源地址空间
 * @return 新的地址空间
 */
mm_struct* vmm_clone_address_space(mm_struct *src_mm);
/**
 * 查找包含addr的VMA
 * @param mm: 地址空间
 * @param addr: 虚拟地址
 * @return 找到返回 VMA 指针，否则返回 NULL
 */
vm_area_t* find_vma(mm_struct *mm, uintptr_t addr);
/**
 * 在用户地址空间映射一段匿名内存（立即分配物理页）
 * @param mm: 地址空间
 * @param vaddr: 虚拟地址（自动页对齐）
 * @param length: 长度（字节）
 * @param flags: VM_READ/VM_WRITE/VM_EXEC
 * @return 成功返回映射的虚拟地址，失败返回 NULL
 * @author DeepSeek V4 Pro
 */
int vmm_mmap(mm_struct *mm, uintptr_t vaddr, uint64_t length, uint64_t flags);
/**
 * @brief 为用户地址空间映射一页
 * @param mm: 地址空间
 * @param vaddr: 虚拟地址
 * @param paddr: 物理地址
 * @param flags: 权限标志
 * @return 成功返回0，失败返回负值
 * @author DeepSeek V4 Pro
 */
int vmm_map_user_page(mm_struct *mm, uintptr_t vaddr, uintptr_t paddr, uint64_t flags);

/**
 * @brief 取消一段用户内存映射并释放物理页
 * @param mm 地址空间
 * @param vaddr 起始虚拟地址
 * @param length 长度(字节)
 * @return 0成功,负数失败
 */
int vmm_munmap(mm_struct *mm, uintptr_t vaddr, uint64_t length);

/**
 * @brief 修改一段用户内存的访问权限
 * @param mm 地址空间
 * @param vaddr 起始虚拟地址
 * @param length 长度
 * @param prot 新权限(VM_READ/VM_WRITE/VM_EXEC)
 * @return 0成功,负数失败
 */
int vmm_mprotect(mm_struct *mm, uintptr_t vaddr, uint64_t length, uint64_t prot);

#endif