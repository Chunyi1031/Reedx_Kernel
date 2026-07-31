#ifndef _BOOT_H_
#define _BOOT_H_

#include <types.h>

/* 内核内部屏幕信息 —— 简洁便于驱动使用
 * 由 LoadBootParam() 从引导程序的 BootScreenInfo 逐字段填充 */
typedef struct ScreenInfo {
    uint32_t Width;              /* 屏幕宽 */
    uint32_t Height;             /* 屏幕高 */
    void*    FrameBufferBase;    /* 帧缓冲区基地址 */
    uint64_t FrameBufferSize;   /* 帧缓冲区大小 */
} __attribute__((packed)) ScreenInfo;

/* 引导程序屏幕信息 —— 与引导程序 screen_info_t 布局严格一致
 * 原理：此结构体嵌在 BootParam 中，引导程序直接按此布局写入，
 * 内核通过 LoadBootParam 逐字段拷贝到 ScreenInfo */
typedef struct BootScreenInfo {
    _Bool     IsAvailable;       /* 屏幕可用标志 */
    uint32_t* FrameBuffer;       /* 帧缓冲区地址 */
    uint64_t  FrameBuffer_Size;  /* 帧缓冲区大小 */
    uint32_t  Hieght;            /* 屏幕高 */
    uint32_t  Width;             /* 屏幕宽 */
    uint32_t  PixelFormat;       /* 像素格式 */
} __attribute__((packed)) BootScreenInfo;

/* 内存映射 —— 与引导程序 memory_info_t 布局严格一致 */
typedef struct MEMORY_MAP {
    uint64_t MapSize;
    uint64_t DescriptorSize;
    uint32_t DescriptorVersion;
    void*    Buffer;
} __attribute__((packed)) UEFI_MEMORY_MAP;

/* 磁盘信息 —— 与引导程序 disk_info_t 布局严格一致 */
typedef struct DiskInfo {
    _Bool    IsGpt;
    EFI_GUID PartitionGuid;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
} __attribute__((packed)) DiskInfo;

/* 内核启动参数 —— 与引导程序 boot_param_t 字节精确匹配
 * 原理：引导程序在栈上构建此结构体后直接跳转到 KernelStart(BootParam*)，
 * 内核通过指针访问，任何字段偏移错误都会导致崩溃。*/
typedef struct BootParam {
    UEFI_MEMORY_MAP   MemoryInfo;
    BootScreenInfo    ScreenInfo;
    DiskInfo          DiskInfo;
    void*             RSDP;
    void*             SMBIOS;
    void*             RuntimeServices;
    uint64_t          RandomSeed;
    uint64_t          KernelAddress;
    uint64_t          KernelSize;
    uint64_t          KernelStackAddress;
    uint64_t          KernelStackSize;
    _Bool             PrintLog;
} __attribute__((packed)) BootParam;

extern BootParam *SYSTEM_BootParam;

#endif
