//物理内存管理-UEFI 内存描述符
#ifndef _MEMORY_EFI_MEM_DESC_H_
#define _MEMORY_EFI_MEM_DESC_H_

#include <klib.h>

// UEFI 规范定义的内存属性位
#define MEMORY_UC 0x0000000000000001  // Uncached - 不可缓存
#define MEMORY_WC 0x0000000000000002  // Write Combining - 写组合
#define MEMORY_WT 0x0000000000000004  // Write Through - 写通过
#define MEMORY_WB 0x0000000000000008  // Write Back - 写回
#define MEMORY_UCE 0x0000000000000010 // Uncached Export - 不可缓存导出
#define MEMORY_WP 0x0000000000001000  // Write Protect - 写保护
#define MEMORY_RP 0x0000000000002000  // Read Protect - 读保护
#define MEMORY_XP 0x0000000000004000  // Execute Protect - 执行保护
#define MEMORY_RO 0x0000000000020000  // Read Only -

// 内存类型定义
#define RESERVED_MEMORY_TYPE 0
#define LOADER_CODE 1
#define LOADER_DATA 2
#define BOOT_SERVICES_CODE 3
#define BOOT_SERVICES_DATA 4
#define RUNTIME_SERVICES_CODE 5
#define RUNTIME_SERVICES_DATA 6
#define CONVENTIONAL_MEMORY 7          // 用内存
#define UNUSABLE_MEMORY 8              // 不可用
#define ACPI_RECLAIM_MEMORY 9          // 可用但会被ACPI回收
#define ACPI_MEMORY_NVS 10             // 不可用
#define MEMORY_MAPPED_IO 11            // 不可用（MMIO）
#define MEMORY_MAPPED_IO_PORT_SPACE 12 // 不可用
#define PAL_CODE 13                    // 不可用（安腾处理器）
#define PERSISTENT_MEMORY 14           // 持久内存

// 定义内存描述符结构体，用于描述一段内存区域的信息
typedef struct UEFI_MEMORY_DESCRIPTOR
{
    uint32_t Type;           // 内存类型（如可用内存、保留内存等）
    uintptr_t PhysicalStart; // 物理起始地址
    uintptr_t VirtualStart;  // 虚拟起始地址
    uint64_t NumberOfPages;  // 页数量
    uint64_t Attribute;      // 属性（如可读、可写、可执行等）
} UEFI_MEMORY_DESCRIPTOR;

extern UEFI_MEMORY_MAP *SYSTEM_MemoryMap; // 全局内存映射

#endif