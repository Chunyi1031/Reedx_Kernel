#ifndef _MM_VIRTUAL_MM_H_
#define _MM_VIRTUAL_MM_H_

#include <mm/pgtables.h>

#define KERNEL_VIRTUAL_ADDR_START 0xFFFF800000000000ULL
#define KERNEL_VIRTUAL_ADDR_END   0xFFFFFFFFFFFFFFFFULL

#define PHYS_TO_VIRT(paddr) (((uintptr_t)(paddr) >= KERNEL_VIRTUAL_ADDR_START) ? (uintptr_t)(paddr) : ((uintptr_t)(paddr) + KERNEL_VIRTUAL_ADDR_START))
#define VIRT_TO_PHYS(vaddr) (((uintptr_t)(vaddr) >= KERNEL_VIRTUAL_ADDR_START) ? ((uintptr_t)(vaddr) - KERNEL_VIRTUAL_ADDR_START) : (uintptr_t)(vaddr))

void InitKernelMapping();//初始化内核高半映射
void switch_kernel_stack_to_high(void);//切换内核栈到高半
void switch_kernel_info_to_high(void);//设置栈信息到高地址

#endif