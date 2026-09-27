/*
 * include/acpi/acpi.h — x86_64 ACPI 表结构精简定义
 *
 * Copyright (C) 2026 Liu Chunyi
 * 
 * 仅保留内核必需的：RSDP/RSDT/XSDT/FADT/MADT/HPET/MCFG
 * 删除：ARM GIC, RAS, DMAR, SRAT, SLIT, HEST, BERT, IVRS, IORT 等
 */

#ifndef _ACPI_H_
#define _ACPI_H_

#include <types.h>

typedef u64 acpi_physical_address;

#define ACPI_NAMESEG_SIZE         4   //签名/ID字段固定4字节
#define ACPI_OEM_ID_SIZE          6   //OEM ID 固定6字节
#define ACPI_OEM_TABLE_ID_SIZE    8   //OEM Table ID 固定8字节
#define ACPI_NONSTRING             //标记非C字符串（无需NUL终止符）

//表签名（仅保留x86_64内核会用到的）
#define ACPI_SIG_RSDP    "RSD PTR "  //Root System Description Pointer
#define ACPI_SIG_RSDT    "RSDT"      //Root System Description Table
#define ACPI_SIG_XSDT    "XSDT"      //Extended System Description Table
#define ACPI_SIG_FADT    "FACP"      //Fixed ACPI Description Table
#define ACPI_SIG_FACS    "FACS"      //Firmware ACPI Control Structure
#define ACPI_SIG_MADT    "APIC"      //Multiple APIC Description Table
#define ACPI_SIG_HPET    "HPET"      //High Precision Event Timer
#define ACPI_SIG_MCFG    "MCFG"      //PCI Memory Mapped Configuration

#define ACPI_RSDT_ENTRY_SIZE  (sizeof(u32))
#define ACPI_XSDT_ENTRY_SIZE  (sizeof(u64))

//表签名比较宏
#define ACPI_COMPARE_NAMESEG(a, b)  (*(u32 *)(a) == *(u32 *)(b))

//acpi_generic_address Space ID
enum AcpiGenericAddressSpacsID {
	ACPI_GAS_SYSTEM_MEMORY 		= 0,
	ACPI_GAS_SYSTEM_IO 			= 1,
	ACPI_GAS_PCI				= 2,
	ACPI_GAS_EC					= 3,
	ACPI_GAS_SYSTEM_MB			= 4,
	ACPI_GAS_CMOS_SYSTEM		= 5,
	ACPI_GAS_PCI_BAR			= 6,
	ACPI_GAS_IPMI				= 7,
	ACPI_GAS_UIOI				= 8,
	ACPI_GAS_SERIAL				= 9
};

#pragma pack(1)

struct acpi_table_header {
	char  signature[ACPI_NAMESEG_SIZE] ACPI_NONSTRING;
	u32   length;
	u8    revision;
	u8    checksum;
	char  oem_id[ACPI_OEM_ID_SIZE] ACPI_NONSTRING;
	char  oem_table_id[ACPI_OEM_TABLE_ID_SIZE] ACPI_NONSTRING;
	u32   oem_revision;
	char  asl_compiler_id[ACPI_NAMESEG_SIZE] ACPI_NONSTRING;
	u32   asl_compiler_revision;
}__attribute__ ((packed));

struct acpi_generic_address {
	u8  space_id;      //地址空间类型：0=系统内存, 1=系统IO
	u8  bit_width;
	u8  bit_offset;
	u8  access_width;
	u64 address;
}__attribute__ ((packed));

struct acpi_table_rsdp {
	char  signature[8];                      //"RSD PTR "
	u8    checksum;
	char  oem_id[ACPI_OEM_ID_SIZE];
	u8    revision;                          //0=ACPI 1.0, 2=ACPI 2.0+
	u32   rsdt_physical_address;             //RSDT 32位物理地址
	u32   length;                            //ACPI 2.0+
	u64   xsdt_physical_address;             //XSDT 64位物理地址
	u8    extended_checksum;
	u8    reserved[3];
}__attribute__ ((packed));

struct acpi_table_rsdt {
	struct acpi_table_header header;
	u32 table_offset_entry[];               //指向子表的32位指针数组
}__attribute__ ((packed));

struct acpi_table_xsdt {
	struct acpi_table_header header;
	u64 table_offset_entry[];               //指向子表的64位指针数组
}__attribute__ ((packed));

struct acpi_table_fadt {
	struct acpi_table_header header;
	u32   facs;                  //FACS 32位物理地址
	u32   dsdt;                  //DSDT 32位物理地址
	u8    model;
	u8    preferred_profile;
	u16   sci_interrupt;
	u32   smi_command;
	u8    acpi_enable;
	u8    acpi_disable;
	u8    s4_bios_request;
	u8    pstate_control;
	u32   pm1a_event_block;
	u32   pm1b_event_block;
	u32   pm1a_control_block;
	u32   pm1b_control_block;
	u32   pm2_control_block;
	u32   pm_timer_block;
	u32   gpe0_block;
	u32   gpe1_block;
	u8    pm1_event_length;
	u8    pm1_control_length;
	u8    pm2_control_length;
	u8    pm_timer_length;
	u8    gpe0_block_length;
	u8    gpe1_block_length;
	u8    gpe1_base;
	u8    cst_control;
	u16   c2_latency;
	u16   c3_latency;
	u16   flush_size;
	u16   flush_stride;
	u8    duty_offset;
	u8    duty_width;
	u8    day_alarm;
	u8    month_alarm;
	u8    century;
	u16   boot_flags;
	u8    reserved;
	u32   flags;
	struct acpi_generic_address reset_register;
	u8    reset_value;
	u16   arm_boot_flags;
	u8    minor_revision;
	u64   Xfacs;
	u64   Xdsdt;
	struct acpi_generic_address xpm1a_event_block;
	struct acpi_generic_address xpm1b_event_block;
	struct acpi_generic_address xpm1a_control_block;
	struct acpi_generic_address xpm1b_control_block;
	struct acpi_generic_address xpm2_control_block;
	struct acpi_generic_address xpm_timer_block;
	struct acpi_generic_address xgpe0_block;
	struct acpi_generic_address xgpe1_block;
	struct acpi_generic_address sleep_control;
	struct acpi_generic_address sleep_status;
	u64   hypervisor_id;
}__attribute__ ((packed));

//FADT flags
#define ACPI_FADT_8042           (1 << 1)  //系统有8042键盘控制器
#define ACPI_FADT_HW_REDUCED     (1 << 20) //无ACPI硬件（ACPI 5.0）

//子表通用头
struct acpi_subtable_header {
	u8 type;
	u8 length;
}__attribute__ ((packed));

struct acpi_table_madt {
	struct acpi_table_header header;
	u32 address;   //LAPIC物理基址
	u32 flags;
}__attribute__ ((packed));

#define ACPI_MADT_PCAT_COMPAT  (1)  //系统同时有双8259 PIC

//MADT子表类型（仅保留x86_64相关的）
enum acpi_madt_type {
	ACPI_MADT_TYPE_LOCAL_APIC           = 0,
	ACPI_MADT_TYPE_IO_APIC              = 1,
	ACPI_MADT_TYPE_INTERRUPT_OVERRIDE   = 2,
	ACPI_MADT_TYPE_NMI_SOURCE           = 3,
	ACPI_MADT_TYPE_LOCAL_APIC_NMI       = 4,
	ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE  = 5,
	ACPI_MADT_TYPE_LOCAL_X2APIC         = 9,
	ACPI_MADT_TYPE_LOCAL_X2APIC_NMI     = 10,
};

//0: 处理器Local APIC
struct acpi_madt_local_apic {
	struct acpi_subtable_header header;
	u8  processor_id;   //ACPI处理器ID
	u8  id;             //LAPIC ID
	u32 lapic_flags;    //bit0=处理器可用
}__attribute__ ((packed));

//1: I/O APIC
struct acpi_madt_io_apic {
	struct acpi_subtable_header header;
	u8  id;               //I/O APIC ID
	u8  reserved;
	u32 address;          //I/O APIC MMIO基址
	u32 global_irq_base;  //全局系统中断起始编号
}__attribute__ ((packed));

//2: 中断源重映射
struct acpi_madt_interrupt_override {
	struct acpi_subtable_header header;
	u8  bus;           //0=ISA
	u8  source_irq;    //原始IRQ号
	u32 global_irq;    //重映射后的GSI
	u16 inti_flags;    //MPS INTI flags (极性+触发模式)
}__attribute__ ((packed));

//MPS INTI flags
#define ACPI_MADT_POLARITY_MASK    (3 << 0)
#define ACPI_MADT_POLARITY_HIGH    (0 << 0)
#define ACPI_MADT_POLARITY_LOW     (3 << 0)
#define ACPI_MADT_TRIGGER_MASK     (3 << 2)
#define ACPI_MADT_TRIGGER_EDGE     (0 << 2)
#define ACPI_MADT_TRIGGER_LEVEL    (3 << 2)

//3: NMI源
struct acpi_madt_nmi_source {
	struct acpi_subtable_header header;
	u16 inti_flags;
	u32 global_irq;
}__attribute__ ((packed));

//4: Local APIC NMI
struct acpi_madt_local_apic_nmi {
	struct acpi_subtable_header header;
	u8  processor_id;
	u16 inti_flags;
	u8  lint;
}__attribute__ ((packed));

//5: Local APIC地址覆盖
struct acpi_madt_local_apic_override {
	struct acpi_subtable_header header;
	u16 reserved;
	u64 address;
}__attribute__ ((packed));

//9: Local x2APIC
struct acpi_madt_local_x2apic {
	struct acpi_subtable_header header;
	u16 reserved;
	u32 local_apic_id;
	u32 lapic_flags;
	u32 uid;
}__attribute__ ((packed));

//10: Local x2APIC NMI
struct acpi_madt_local_x2apic_nmi {
	struct acpi_subtable_header header;
	u16 inti_flags;
	u32 uid;
	u8  lint;
	u8  reserved[3];
}__attribute__ ((packed));

struct acpi_table_hpet {
	struct acpi_table_header header;
	u32  id;                     //硬件ID
	struct acpi_generic_address address;  //定时器块基址
	u8   sequence;               //HPET序号
	u16  minimum_tick;           //周期模式最小滴答
	u8   flags;                  //bit0-1: 页保护级别
}__attribute__ ((packed));

#define ACPI_HPET_PAGE_PROTECT_MASK  (3)

struct acpi_table_mcfg {
	struct acpi_table_header header;
	u8 reserved[8];
}__attribute__ ((packed));

struct acpi_mcfg_allocation {
	u64 address;          //MMIO基址
	u16 pci_segment;      //PCI段组号
	u8  start_bus_number; //起始总线号
	u8  end_bus_number;   //结束总线号
	u32 reserved;
};

#pragma pack()

typedef struct sys_acpi_info {
	struct acpi_table_rsdp* rsdp;
	struct acpi_table_xsdt* xsdt;
	struct acpi_table_rsdt* rsdt;
	struct acpi_table_fadt* fadt;
	struct acpi_table_madt* madt;
	_Bool is_init;
} sys_acpi_info;

extern sys_acpi_info SYSTEM_ACPI;

/**
 * @brief 初始化ACPI
 * @param rsdp RSDP表物理地址
 * @return 0:成功 1:参数错误 2：表损坏 3：RSDT/XSDT表未找到 4：FADT表未找到 5：MADT表未找到
 */
int InitACPI(struct acpi_table_rsdp* rsdp);

#endif
