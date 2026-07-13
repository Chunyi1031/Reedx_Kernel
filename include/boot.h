#ifndef _BOOT_H_
#define _BOOT_H_

#include <types.h>

typedef struct e820_entry {
    uint64_t addr;//起始地址
    uint64_t size;//大小
    uint32_t type;//类型
} __attribute__((packed)) e820_entry;

typedef struct ScreenInfo {
    uint32_t Width;//屏幕宽
    uint32_t Height;//屏幕高
    void* FrameBufferBase;//帧缓冲区基地址
    uint64_t FrameBufferSize;//帧缓冲区大小
} ScreenInfo;

typedef struct MemoryInfo {
    e820_entry* e820_entries;//E820条目指针
    uint32_t e820_entries_count;//E820条目数量
    uint64_t e820_entries_size;//E820条目大小
} MemoryInfo;

typedef struct DiskInfo{
    void* BootDiskHandle;//UEFI句柄
    uint64_t DiskSize;//磁盘总大小（字节）
    uint64_t PartitionStart;//分区起始LBA）
    EFI_GUID PartitionGuid;//GPT分区GUID
    //设备路径
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
    int DevicePathSize;
} DiskInfo;

typedef struct BootParam{
    ScreenInfo screen_info;//屏幕信息
    MemoryInfo memory_info;//内存信息
    uint64_t CPU_Fquency;//参考CPU频率
    void* RSDP;//ACPI RSDP地址
    void* Font_Buffer;//字体缓冲区地址
    void* KernelStartAddress;//内核起始地址
    void* KernelStackAddress;//内核栈地址
    DiskInfo disk_info;//磁盘信息
} BootParam;

#endif