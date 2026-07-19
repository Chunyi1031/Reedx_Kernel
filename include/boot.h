#ifndef _BOOT_H_
#define _BOOT_H_

#include <types.h>

typedef struct ScreenInfo {
    uint32_t Width;//屏幕宽
    uint32_t Height;//屏幕高
    void* FrameBufferBase;//帧缓冲区基地址
    uint64_t FrameBufferSize;//帧缓冲区大小
} __attribute__((packed)) ScreenInfo;

//内存映射
typedef struct MEMORY_MAP{
    uint64_t MapSize;
    uint64_t DescriptorSize;
    uint32_t DescriptorVersion;
    uint32_t padding; 
    void* Buffer;
} __attribute__((packed)) UEFI_MEMORY_MAP;

typedef struct DiskInfo{
    void* BootDiskHandle;//UEFI句柄
    uint64_t DiskSize;//磁盘总大小（字节）
    uint64_t PartitionStart;//分区起始LBA）
    EFI_GUID PartitionGuid;//GPT分区GUID
    //设备路径
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
    int DevicePathSize;
} __attribute__((packed)) DiskInfo;

typedef struct BootParam{
    ScreenInfo screen_info;//屏幕信息
    UEFI_MEMORY_MAP memory_info;//内存信息
    uint64_t CPU_Fquency;//参考CPU频率
    void* RSDP;//ACPI RSDP地址
    void* Font_Buffer;//字体缓冲区地址
    uint64_t Font_Size;//字体大小
    void* KernelStartAddress;//内核起始地址
    uint64_t KernelSize;//内核大小
    void* KernelStackAddress;//内核栈地址
    uint64_t KernelStackSize;//内核栈大小
    DiskInfo disk_info;//磁盘信息
} __attribute__((packed)) BootParam;

extern BootParam *SYSTEM_BootParam;

#endif