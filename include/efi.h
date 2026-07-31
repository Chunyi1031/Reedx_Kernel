/**
 * UEFI 运行时服务定义
 * 基于 EDK2 RuntimeServices 标准定义（MdePkg/Include/Uefi/UefiSpec.h）
 */

#ifndef _REEDX_UEFI_H_
#define _REEDX_UEFI_H_

#include <types.h>
#include <mm/EfiMemDesc.h>

#define EFIAPI
#define IN
#define OUT
#define OPTIONAL

typedef uint64_t EFI_STATUS;         // UEFI 状态码（UINTN = uint64_t on x86_64）
typedef char BOOLEAN;                // UEFI 布尔类型
typedef unsigned short CHAR16;       // UEFI 宽字符

typedef struct {
  uint16_t  Year;                    // 年
  uint8_t   Month;                   // 月
  uint8_t   Day;                     // 日
  uint8_t   Hour;                    // 时
  uint8_t   Minute;                  // 分
  uint8_t   Second;                  // 秒
  uint8_t   Pad1;                    // 对齐填充
  uint32_t  Nanosecond;              // 纳秒
  int16_t   TimeZone;                // 时区（分钟偏移，UTC 以西为正）
  uint8_t   Daylight;                // 夏令时标志
  uint8_t   Pad2;                    // 对齐填充
} EFI_TIME;

/* 实时时钟设备通过 EFI 接口暴露的能力信息 */
typedef struct {
  u32     Resolution;                // 报告分辨率（每秒计数），PC-AT CMOS RTC 为 1Hz
  u32     Accuracy;                  // 计时精度（百万分之一误差率）
  BOOLEAN SetsToZero;                // TRUE：设置时间时清除分辨率以下低位；FALSE：不清除
} EFI_TIME_CAPABILITIES;

/* EFI 胶囊头部（用于固件更新） */
typedef struct {
  EFI_GUID  CapsuleGuid;             // 定义胶囊内容的 GUID
  uint32_t  HeaderSize;              // 头部大小，可能包含扩展头部
  uint32_t  Flags;                   // 胶囊属性位掩码
  uint32_t  CapsuleImageSize;        // 胶囊总大小（含头部），单位字节
} EFI_CAPSULE_HEADER;

/* 系统复位类型 */
typedef enum {
  EfiResetCold,            // 冷复位：系统完全重新上电，异步执行
  EfiResetWarm,            // 热复位：处理器重置但不破坏待处理周期
  EfiResetShutdown,        // 关机：进入 ACPI G2/S5 或 G3 状态
  EfiResetPlatformSpecific // 平台特定复位：由 ResetData 中的 GUID 定义具体行为
} EFI_RESET_TYPE;

/* EFI 表头（所有标准 EFI 表的前缀） */
typedef struct {
  uint64_t Signature;      // 64 位签名，标识后续表的类型
  uint32_t Revision;       // EFI 规范版本（高 16 位主版本，低 16 位副版本）
  uint32_t HeaderSize;     // 整表大小（含此头部），单位字节
  uint32_t CRC32;          // 整表 32 位 CRC（计算时此字段置 0）
  uint32_t Reserved;       // 保留，必须为 0
} EFI_TABLE_HEADER;

/*
 * 返回当前时间和日期信息，以及硬件平台的时间保持能力。
 * @param[out] Time             接收当前时间快照的存储指针。
 * @param[out] Capabilities     可选：接收实时时钟设备能力信息。
 * @retval EFI_SUCCESS          操作成功。
 * @retval EFI_INVALID_PARAMETER Time 为 NULL。
 * @retval EFI_DEVICE_ERROR     因硬件错误无法获取时间。
 */
typedef EFI_STATUS (EFIAPI *EFI_GET_TIME)(OUT EFI_TIME *Time, OUT EFI_TIME_CAPABILITIES *Capabilities OPTIONAL);

/*
 * 设置当前本地时间和日期。
 * @param[in] Time              当前时间指针。
 * @retval EFI_SUCCESS          操作成功。
 * @retval EFI_INVALID_PARAMETER 时间字段超出范围。
 * @retval EFI_DEVICE_ERROR     因硬件错误无法设置时间。
 */
typedef EFI_STATUS (EFIAPI *EFI_SET_TIME)(IN EFI_TIME *Time);

/* 获取唤醒闹钟时间 */
typedef EFI_STATUS (EFIAPI *EFI_GET_WAKEUP_TIME)(OUT BOOLEAN *Enabled, OUT BOOLEAN *Pending, OUT EFI_TIME *Time);

/*
 * 设置系统唤醒闹钟时间。
 * @param[in] Enable            启用或禁用唤醒闹钟。
 * @param[in] Time              Enable 为 TRUE 时指定唤醒时间；Enable 为 FALSE 时可选（可为 NULL）。
 * @retval EFI_SUCCESS          操作成功。
 * @retval EFI_INVALID_PARAMETER 时间字段超出范围。
 * @retval EFI_DEVICE_ERROR     因硬件错误无法设置唤醒时间。
 */
typedef EFI_STATUS (EFIAPI *EFI_SET_WAKEUP_TIME)(IN BOOLEAN Enable, IN EFI_TIME *Time OPTIONAL);

/*
 * 将 EFI 固件的运行时寻址模式从物理地址切换为虚拟地址。
 * @param[in] MemoryMapSize      VirtualMap 的字节大小。
 * @param[in] DescriptorSize     VirtualMap 中每个条目的字节大小。
 * @param[in] DescriptorVersion  VirtualMap 条目的结构体版本号。
 * @param[in] VirtualMap         内存描述符数组，包含所有运行时范围的新虚拟地址映射。
 * @retval EFI_SUCCESS          虚拟地址映射已应用。
 * @retval EFI_UNSUPPORTED      EFI 固件不在运行时或已在虚拟地址模式下。
 * @retval EFI_INVALID_PARAMETER DescriptorSize 或 DescriptorVersion 无效。
 * @retval EFI_NO_MAPPING       内存映射中某个需要映射的范围未提供虚拟地址。
 * @retval EFI_NOT_FOUND        为未在内存映射中找到的地址提供了虚拟地址。
 */
typedef EFI_STATUS (EFIAPI *EFI_SET_VIRTUAL_ADDRESS_MAP)(IN uint64_t MemoryMapSize, IN uint64_t DescriptorSize, IN uint32_t DescriptorVersion, IN UEFI_MEMORY_DESCRIPTOR *VirtualMap);

/*
 * 获取后续内存访问应使用的新虚拟地址（指针转换）。
 * @param[in]      DebugDisposition 被转换指针的类型信息。
 * @param[in, out] Address          指向指针的指针，将被修正为新的虚拟地址映射值。
 * @retval EFI_SUCCESS          Address 指向的指针已修正。
 * @retval EFI_INVALID_PARAMETER Address 为 NULL，或 *Address 为 NULL 且未设置 EFI_OPTIONAL_PTR 位。
 * @retval EFI_NOT_FOUND        Address 不在当前内存映射中（通常为致命错误）。
 */
typedef EFI_STATUS (EFIAPI *EFI_CONVERT_POINTER)(IN uint64_t DebugDisposition, IN OUT void **Address);

/*
 * 获取变量的值。
 * @param[in]      VariableName  以 Null 结尾的变量名字符串。
 * @param[in]      VendorGuid    厂商唯一标识符。
 * @param[out]     Attributes    可选：返回变量属性位掩码。
 * @param[in, out] DataSize      输入时为 Data 缓冲区大小；输出时为实际返回数据大小。
 * @param[out]     Data          可选：接收变量内容的缓冲区（可为 NULL 配合 DataSize=0 查询所需大小）。
 * @retval EFI_SUCCESS           操作成功。
 * @retval EFI_NOT_FOUND         变量未找到。
 * @retval EFI_BUFFER_TOO_SMALL  DataSize 过小。
 * @retval EFI_INVALID_PARAMETER  VariableName、VendorGuid 或 DataSize 为 NULL。
 * @retval EFI_DEVICE_ERROR      因硬件错误无法读取变量。
 * @retval EFI_SECURITY_VIOLATION 因认证失败无法读取变量。
 */
typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(IN CHAR16 *VariableName, IN EFI_GUID *VendorGuid, OUT uint32_t *Attributes OPTIONAL, IN OUT uint64_t *DataSize, OUT void *Data OPTIONAL);

/*
 * 枚举当前变量名。
 * @param[in, out] VariableNameSize VariableName 缓冲区大小，需能容纳输入字符串。
 * @param[in, out] VariableName     输入时为上次 GetNextVariableName 返回的变量名；输出时为当前变量名。
 * @param[in, out] VendorGuid       输入时为上次 GetNextVariableName 返回的 VendorGuid；输出时为当前 VendorGuid。
 * @retval EFI_SUCCESS           操作成功。
 * @retval EFI_NOT_FOUND         下一个变量未找到。
 * @retval EFI_BUFFER_TOO_SMALL  VariableNameSize 过小，已更新为所需大小。
 * @retval EFI_INVALID_PARAMETER VariableNameSize、VariableName 或 VendorGuid 为 NULL；
 *                               或 VariableName/VendorGuid 不是已存在变量的名称和 GUID；
 *                               或输入缓冲区前 VariableNameSize 字节中未找到 Null 终止符。
 * @retval EFI_DEVICE_ERROR      因硬件错误无法读取变量。
 */
typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_VARIABLE_NAME)(IN OUT uint64_t *VariableNameSize, IN OUT CHAR16 *VariableName, IN OUT EFI_GUID *VendorGuid);

/*
 * 设置变量的值。
 * @param[in] VariableName  以 Null 结尾的变量名字符串（非空）。
 * @param[in] VendorGuid    厂商唯一标识符。
 * @param[in] Attributes    设置的变量属性位掩码。
 * @param[in] DataSize      Data 缓冲区的字节大小。除非设置了 EFI_VARIABLE_APPEND_WRITE 属性，否则大小为 0 将删除变量。
 * @param[in] Data          变量内容。
 * @retval EFI_SUCCESS           操作成功。
 * @retval EFI_INVALID_PARAMETER 属性位、名称或 GUID 组合无效；或变量名为空字符串。
 * @retval EFI_OUT_OF_RESOURCES  存储空间不足。
 * @retval EFI_DEVICE_ERROR      因硬件错误无法写入。
 * @retval EFI_WRITE_PROTECTED   变量只读或不可删除。
 * @retval EFI_SECURITY_VIOLATION 认证验证失败。
 * @retval EFI_NOT_FOUND         要更新或删除的变量不存在。
 */
typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(IN CHAR16 *VariableName, IN EFI_GUID *VendorGuid, IN uint32_t Attributes, IN uint64_t DataSize, IN void *Data);

/*
 * 返回平台单调计数器的高 32 位。
 * @param[out] HighCount  返回值的指针。
 * @retval EFI_SUCCESS           操作成功。
 * @retval EFI_INVALID_PARAMETER HighCount 为 NULL。
 * @retval EFI_DEVICE_ERROR      设备故障。
 */
typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_HIGH_MONO_COUNT)(OUT uint32_t *HighCount);

/*
 * 重置整个平台。
 * @param[in] ResetType   复位类型（冷复位/热复位/关机/平台特定）。
 * @param[in] ResetStatus 复位状态码。
 * @param[in] DataSize    ResetData 的字节大小。
 * @param[in] ResetData   可选：对于 EfiResetCold/Warm/Shutdown，以 Null 结尾描述字符串开头；
 *                        对于 EfiResetPlatformSpecific，字符串后跟 EFI_GUID 定义具体复位类型。
 */
typedef void (EFIAPI *EFI_RESET_SYSTEM)(IN EFI_RESET_TYPE ResetType, IN EFI_STATUS ResetStatus, IN uint64_t DataSize, IN void *ResetData OPTIONAL);

/*
 * 向固件传递胶囊（固件更新包）。
 * @param[in] CapsuleHeaderArray 胶囊头部指针数组的虚拟地址。
 * @param[in] CapsuleCount       CapsuleHeaderArray 中的胶囊数量。
 * @param[in] ScatterGatherList  可选：描述胶囊物理内存位置的 EFI_CAPSULE_BLOCK_DESCRIPTOR 物理指针。
 * @retval EFI_SUCCESS           有效胶囊已传递（若无 CAPSULE_FLAGS_PERSIST_ACROSS_RESET 则已处理完毕）。
 * @retval EFI_INVALID_PARAMETER CapsuleCount 为 0 或胶囊头部标志无效。
 * @retval EFI_DEVICE_ERROR      胶囊更新启动后因设备错误失败。
 * @retval EFI_OUT_OF_RESOURCES  ExitBootServices 后运行时资源不足，可在 ExitBootServices 前重试。
 * @retval EFI_UNSUPPORTED       该胶囊类型不受此平台支持。
 */
typedef EFI_STATUS (EFIAPI *EFI_UPDATE_CAPSULE)(IN EFI_CAPSULE_HEADER **CapsuleHeaderArray, IN uint64_t CapsuleCount, IN uintptr_t ScatterGatherList OPTIONAL);

/*
 * 查询胶囊是否可被 UpdateCapsule() 支持。
 * @param[in]  CapsuleHeaderArray  胶囊头部指针数组。
 * @param[in]  CapsuleCount        胶囊数量。
 * @param[out] MaximumCapsuleSize  输出 UpdateCapsule 支持的最大胶囊大小。
 * @param[out] ResetType           返回胶囊更新所需的复位类型。
 * @retval EFI_SUCCESS           有效答案已返回。
 * @retval EFI_UNSUPPORTED       胶囊类型不受支持，MaximumCapsuleSize 和 ResetType 未定义。
 * @retval EFI_INVALID_PARAMETER MaximumCapsuleSize 为 NULL。
 * @retval EFI_OUT_OF_RESOURCES  运行时资源不足以处理。
 */
typedef EFI_STATUS (EFIAPI *EFI_QUERY_CAPSULE_CAPABILITIES)(IN EFI_CAPSULE_HEADER **CapsuleHeaderArray, IN uint64_t CapsuleCount, OUT uint64_t *MaximumCapsuleSize, OUT EFI_RESET_TYPE *ResetType);

/*
 * 查询 EFI 变量存储信息。
 * @param[in]  Attributes                   指定要查询的变量类型属性位掩码。
 * @param[out] MaximumVariableStorageSize   指定属性变量的最大存储空间。
 * @param[out] RemainingVariableStorageSize 指定属性变量的剩余存储空间。
 * @param[out] MaximumVariableSize          指定属性的单个变量最大大小。
 * @retval EFI_SUCCESS            有效答案已返回。
 * @retval EFI_INVALID_PARAMETER  属性位组合无效。
 * @retval EFI_UNSUPPORTED        该属性不受支持，输出值未定义。
 */
typedef EFI_STATUS (EFIAPI *EFI_QUERY_VARIABLE_INFO)(IN uint32_t Attributes, OUT uint64_t *MaximumVariableStorageSize, OUT uint64_t *RemainingVariableStorageSize, OUT uint64_t *MaximumVariableSize);

/* EFI 运行时服务表 */
typedef struct {
  EFI_TABLE_HEADER                Hdr;                         // 表头
  /* 时间服务 */
  EFI_GET_TIME                    GetTime;                     // 获取时间
  EFI_SET_TIME                    SetTime;                     // 设置时间
  EFI_GET_WAKEUP_TIME             GetWakeupTime;               // 获取唤醒时间
  EFI_SET_WAKEUP_TIME             SetWakeupTime;               // 设置唤醒时间
  /* 虚拟内存服务 */
  EFI_SET_VIRTUAL_ADDRESS_MAP     SetVirtualAddressMap;        // 设置虚拟地址映射
  EFI_CONVERT_POINTER             ConvertPointer;              // 转换指针
  /* 变量服务 */
  EFI_GET_VARIABLE                GetVariable;                 // 获取变量
  EFI_GET_NEXT_VARIABLE_NAME      GetNextVariableName;         // 获取下一个变量名
  EFI_SET_VARIABLE                SetVariable;                 // 设置变量
  /* 杂项服务 */
  EFI_GET_NEXT_HIGH_MONO_COUNT    GetNextHighMonotonicCount;   // 获取单调计数器高32位
  EFI_RESET_SYSTEM                ResetSystem;                 // 系统复位
  /* UEFI 2.0 胶囊服务 */
  EFI_UPDATE_CAPSULE              UpdateCapsule;               // 更新胶囊
  EFI_QUERY_CAPSULE_CAPABILITIES  QueryCapsuleCapabilities;    // 查询胶囊能力
  /* UEFI 2.0 杂项服务 */
  EFI_QUERY_VARIABLE_INFO         QueryVariableInfo;           // 查询变量信息
} EFI_RUNTIME_SERVICES;

extern EFI_RUNTIME_SERVICES *UEFI_RuntimeServices;
extern _Bool UEFI_UseRT;

#endif
