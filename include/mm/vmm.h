#ifndef _MM_VIRTUAL_MM_H_
#define _MM_VIRTUAL_MM_H_

#include <mm/pgtables.h>

#define KERNEL_VIRTUAL_ADDR_START 0xFFFF800000000000ULL
#define KERNEL_VIRTUAL_ADDR_END   0xFFFFFFFFFFFFFFFFULL

#define PHYS_TO_VIRT(paddr) (paddr + KERNEL_VIRTUAL_ADDR_START)
#define VIRT_TO_PHYS(vaddr) (vaddr - KERNEL_VIRTUAL_ADDR_START)

void InitKernelMapping();//初始化内核高半映射

#endif