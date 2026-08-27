#ifndef _BOOT_H_
#define _BOOT_H_

#include <types.h>

//屏幕信息
typedef struct ScreenInfo {
    uint32_t Width;              //屏幕宽
    uint32_t Height;             //屏幕高
    void*    FrameBufferBase;    //帧缓冲区基地址
    uint64_t FrameBufferSize;   //帧缓冲区大小
} __attribute__((packed)) ScreenInfo;

//引导程序屏幕信息
typedef struct BootScreenInfo {
    _Bool     IsAvailable;       //屏幕可用标志
    uint32_t* FrameBuffer;       //帧缓冲区地址
    uint64_t  FrameBuffer_Size;  //帧缓冲区大小
    uint32_t  Hieght;            //屏幕高
    uint32_t  Width;             //屏幕宽
    uint32_t  PixelFormat;       //像素格式
} __attribute__((packed)) BootScreenInfo;

//内存映射
typedef struct MEMORY_MAP {
    uint64_t MapSize;
    uint64_t DescriptorSize;
    uint32_t DescriptorVersion;
    void*    Buffer;
} __attribute__((packed)) UEFI_MEMORY_MAP;

//磁盘信息
typedef struct DiskInfo {
    _Bool    IsGpt;
    EFI_GUID PartitionGuid;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
} __attribute__((packed)) DiskInfo;

//内核启动参数
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
