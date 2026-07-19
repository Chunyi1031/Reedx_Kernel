#ifndef _TYPES_H_
#define _TYPES_H_

#if !defined(ARCH_X86_64) && !defined(ARCH_AMD64) && \
    !defined(ARCH_X86_32) && !defined(ARCH_AMD32)
#define ARCH_X86_64
#endif

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;
typedef uint64_t           size_t;
#if defined(ARCH_X86_64) || defined(ARCH_AMD64)
    typedef uint64_t           uintptr_t;
#elif defined(ARCH_X86_32) || defined(ARCH_AMD32)
    typedef uint32_t           uintptr_t;
#else
    #error "Unknown architecture"
#endif

//兼容Linux源码
typedef uint8_t            u8;
typedef uint16_t           u16;
typedef uint32_t           u32;
typedef uint64_t           u64;

#define true  1
#define false 0
#define NULL ((void*)0)

typedef struct {          
    uint32_t  Data1;
    uint16_t  Data2;
    uint16_t  Data3;
    uint8_t   Data4[8]; 
} EFI_GUID;

typedef struct _EFI_DEVICE_PATH_PROTOCOL {
    uint8_t                           Type;
    uint8_t                           SubType;
    uint8_t                           Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#endif