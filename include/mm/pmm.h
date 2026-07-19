//PMM-物理内存管理器
#ifndef _MEMORY_PHYICAL_MM_H_
#define _MEMORY_PHYICAL_MM_H_

#include <klib.h>
#include <mm/EfiMemDesc.h>
#include <mm/bitmap.h>

//物理内存管理(PMemory)函数定义
typedef struct OS_MEMORY_DESCRIPTOR{
    uint64_t Address;
    uint64_t PageSize;
    bitmap_t bitmap;
}OS_MEMORY_DESCRIPTOR;

enum MemType {
    PMM_FREE = false,
    PMM_USED = true,
};

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

extern OS_MEMORY_DESCRIPTOR* MemDescAddr;//内存描述符起始地址
extern int MemDescNum;//内存描述符数量
extern uint64_t MemDescSize;//内存描述符大小

UEFI_MEMORY_DESCRIPTOR* AnalysisMemoryMap(const uint32_t num);

int Init_Physical_Memory_Manager();//初始化物理内存管理器
uint64_t GetMemoryTotalSize();
/**
 * @brief 物理内存内存分配
 * @param pages 申请的页数
 * @return 返回分配的物理内存地址
 */
void* Pmm_Malloc(int pages);
/**
 * @brief 物理内存内存回收
 * @param addr 要回收的物理内存地址
 * @param pages 要回收的页数
 */
void Pmm_Free(void* addr,int pages);

#endif