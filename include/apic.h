/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 *
 * 参考：Linux 7.1.3 arch/x86/include/asm/apicdef.h (寄存器定义)
 *              arch/x86/include/asm/msr-index.h (MSR_IA32_APICBASE)
 *              arch/x86/include/asm/apic.h (apic_mem_read/write/eoi)
 */

#ifndef _INT_APIC_H_
#define _INT_APIC_H_

#include <types.h>
#include <acpi/acpi.h>

//MSR定义
#define MSR_IA32_APICBASE           0x0000001B
#define MSR_IA32_APICBASE_ENABLE    (1 << 11)//APIC全局使能
#define MSR_IA32_APICBASE_BSP       (1 << 8)//BSP标志
#define MSR_IA32_APICBASE_X2APIC    (1 << 10)//x2APIC模式使能
#define MSR_IA32_APICBASE_BASE_MASK 0xFFFFFFFFFF000ULL//物理基址（bits MAXPHYSADDR:12，覆盖到 35:12）

#define APIC_DEFAULT_PHYS_BASE      0xFEE00000 //LAPIC默认物理基址（MSR 未初始化时的回退值）
#define IO_APIC_DEFAULT_PHYS_BASE   0xFEC00000 //IOAPIC默认物理基址（无MADT时的回退值）

//x2APIC MSR基址：MSR = 0x800 + (寄存器偏移 >> 4)
#define MSR_X2APIC_BASE             0x800
#define X2APIC_MSR(reg)             (MSR_X2APIC_BASE + ((reg) >> 4))

//CPUID x2APIC特性位
#define CPUID_LEAF_FEATURES         1
#define CPUID_FEATURE_X2APIC        (1 << 21)//ECX bit 21

//LAPIC寄存器偏移（相对基址，xAPIC/x2APIC 共用相同偏移，访问方式不同）
#define LAPIC_ID                    0x020   //APIC ID寄存器
#define LAPIC_VERSION               0x030   //APIC版本寄存器
#define     GET_LAPIC_VERSION(x)    ((x) & 0xFF)
#define     GET_LAPIC_MAXLVT(x)     (((x) >> 16) & 0xFF)

#define LAPIC_TPR                   0x080   //任务优先级寄存器
#define LAPIC_APR                   0x090   //仲裁优先级寄存器
#define LAPIC_PPR                   0x0A0   //处理器优先级寄存器
#define LAPIC_EOI                   0x0B0   //EOI寄存器（写0表示中断结束）
#define LAPIC_RRR                   0x0C0   //远程读取寄存器
#define LAPIC_LDR                   0x0D0   //逻辑目的寄存器
#define LAPIC_DFR                   0x0E0   //目的格式寄存器
#define     LAPIC_DFR_FLAT          0xFFFFFFFF

#define LAPIC_SPURIOUS              0x0F0   //虚假中断向量寄存器
#define     LAPIC_SPURIOUS_ENABLE   (1 << 8)   //APIC软件使能位
#define     LAPIC_SPURIOUS_FOCUS_DISABLE (1 << 9)  //禁止焦点处理器

#define LAPIC_ISR                   0x100   //中断服务寄存器 (8×32bit)
#define LAPIC_TMR                   0x180   //触发模式寄存器 (8×32bit)
#define LAPIC_IRR                   0x200   //中断请求寄存器 (8×32bit)
#define LAPIC_ESR                   0x280   //错误状态寄存器
#define LAPIC_ICR_LO                0x300   //中断命令寄存器低32位
#define LAPIC_ICR_HI                0x310   //中断命令寄存器高32位(xAPIC 目的)

#define LAPIC_LVT_TIMER             0x320   //LVT定时器
#define LAPIC_LVT_THERMAL           0x330   //LVT温度传感器
#define LAPIC_LVT_PERFMON           0x340   //LVT性能监视计数器
#define LAPIC_LVT_LINT0             0x350   //LVT本地中断0
#define LAPIC_LVT_LINT1             0x360   //LVT本地中断 1
#define LAPIC_LVT_ERROR             0x370   //LVT错误

//LVT通用标志位
#define     LAPIC_LVT_MASKED        (1 << 16)  //中断屏蔽
#define     LAPIC_LVT_LEVEL         (1 << 15)  //电平触发
#define     LAPIC_LVT_REMOTE_IRR    (1 << 14)  //远程IRR
#define     LAPIC_LVT_POLARITY_LOW  (1 << 13)  //低电平有效
#define     LAPIC_LVT_DELIVERY_STS  (1 << 12)  //发送状态
#define     LAPIC_LVT_TIMER_ONESHOT (0 << 17)  //定时器单次模式
#define     LAPIC_LVT_TIMER_PERIODIC (1 << 17) //定时器周期模式
#define     LAPIC_LVT_TIMER_TSCDEADLINE (2 << 17) //TSC 死线模式
#define     LAPIC_LVT_DM_FIXED      0x000      //固定交付模式
#define     LAPIC_LVT_DM_NMI        0x400      //NMI 交付模式
#define     LAPIC_LVT_DM_EXTINT     0x700      //外部中断交付模式

#define LAPIC_TIMER_INITCNT         0x380   //定时器初始计数
#define LAPIC_TIMER_CURCNT          0x390   //定时器当前计数
#define LAPIC_TIMER_DIV             0x3E0   //定时器分频配置
#define     LAPIC_TIMER_DIV_1       0x0B    //不分频
#define     LAPIC_TIMER_DIV_2       0x00    //÷2
#define     LAPIC_TIMER_DIV_4       0x01    //÷4
#define     LAPIC_TIMER_DIV_8       0x02    //÷8
#define     LAPIC_TIMER_DIV_16      0x03    //÷16
#define     LAPIC_TIMER_DIV_32      0x08    //÷32
#define     LAPIC_TIMER_DIV_64      0x09    //÷64
#define     LAPIC_TIMER_DIV_128     0x0A    //÷128

//虚假中断向量
#define SPURIOUS_APIC_VECTOR        0xFF

//IOAPIC寄存器
#define IOAPIC_IOREGSEL             0x00    //索引寄存器偏移
#define IOAPIC_IOWIN                0x10    //数据窗口偏移
#define IOAPIC_REDTBL_BASE          0x10    //重定向表起始索引

//IOAPIC重定向条目字段（低32位）
#define IOAPIC_REDTBL_VECTOR_MASK   0x000000FF
#define IOAPIC_REDTBL_DELIVERY_MODE_FIXED    (0 << 8)
#define IOAPIC_REDTBL_DELIVERY_MODE_LOWEST   (1 << 8)
#define IOAPIC_REDTBL_DELIVERY_MODE_SMI      (2 << 8)
#define IOAPIC_REDTBL_DELIVERY_MODE_NMI      (4 << 8)
#define IOAPIC_REDTBL_DELIVERY_MODE_INIT     (5 << 8)
#define IOAPIC_REDTBL_DELIVERY_MODE_EXTINT   (7 << 8)
#define IOAPIC_REDTBL_DEST_MODE_LOGICAL      (1 << 11)
#define IOAPIC_REDTBL_DEST_MODE_PHYSICAL     (0 << 11)
#define IOAPIC_REDTBL_DELIVERY_PENDING       (1 << 12)
#define IOAPIC_REDTBL_POLARITY_LOW           (1 << 13)
#define IOAPIC_REDTBL_REMOTE_IRR             (1 << 14)
#define IOAPIC_REDTBL_TRIGGER_LEVEL          (1 << 15)
#define IOAPIC_REDTBL_TRIGGER_EDGE           (0 << 15)
#define IOAPIC_REDTBL_MASK                   (1 << 16)

//MADT解析结果（从MADT子表收集的APIC硬件信息）
#define MAX_IO_APICS        4    //最多支持的I/O APIC数量
#define MAX_ISA_OVERRIDES   16   //最多支持的中断重映射条目数

//单个I/O APIC信息，从MADT type 1子表提取
//原理：MADT type 1给出I/O APIC的MMIO基址和GSI起始编号，内核据此访问IOAPIC寄存器
struct apic_ioapic {
	uint8_t  id;              //I/O APIC ID（与LAPIC ID共享同一命名空间）
	uint32_t mmio_base;       //MMIO基址（物理地址，内核用页表映射前直接使用）
	uint32_t gsi_base;        //全局系统中断起始编号（此IOAPIC的pin 0对应的GSI）
};

//中断重映射信息，从MADT type 2子表提取
//原理：ISA IRQ与GSI的默认映射是 1:1，但某些平台（如多IOAPIC或ACPI兼容模式）
//      会通过Interrupt Source Override 重映射，例如 ISA IRQ0→GSI2
struct apic_override {
	uint8_t  bus;             //源总线 (0=ISA)
	uint8_t  source_irq;      //原始ISA IRQ号
	uint32_t gsi;             //重映射后的GSI号
	uint16_t flags;           //MPS INTI flags（极性+触发模式，见ACPI_MADT_POLARITY/TRIGGER_*）
};

//从MADT解析出的完整APIC配置信息
//原理：汇总所有type 0/1/2/5/9子表，为APIC初始化提供正确的硬件地址和中断路由
struct apic_madt_info {
	uint32_t  lapic_addr;                 //MADT表头中的LAPIC物理基址（通常0xFEE00000）
	uint32_t  lapic_addr_override;        //type 5子表给出的覆盖值（0表示无覆盖）
	uint32_t  bsp_lapic_id;               //BSP的LAPIC ID（优先用x2APIC的32位ID，否则用xAPIC的8位）
	uint8_t   has_x2apic_entries;         //MADT中存在x2APIC条目（暗示硬件支持x2APIC）
	int       num_ioapics;                //已发现的I/O APIC数量
	struct    apic_ioapic ioapics[MAX_IO_APICS];
	int       num_overrides;              //中断重映射条目数量
	struct    apic_override overrides[MAX_ISA_OVERRIDES];
	uint32_t  gsi_map[16];                //ISA IRQ→GSI 快速查找表（16条传统ISA IRQ）
	_Bool     parsed;                     //是否已完成解析
};

extern struct apic_madt_info APIC_MADT;

//LAPIC寄存器读写（自动分发xAPIC MMIO / x2APIC MSR）
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t val);

//LAPIC基本操作
uintptr_t lapic_get_base(void);//读取 MSR 获取 LAPIC 物理基址
void lapic_enable(void);//使能LAPIC，尝试切换到x2APIC模式
void lapic_send_eoi(void);//发送EOI
uint32_t lapic_get_id(void);//获取当前LAPIC ID（xAPIC返回8位，x2APIC返回32位）
_Bool lapic_is_x2apic(void);//是否处于x2APIC模式

//APIC定时器
uint32_t lapic_timer_calibrate(void);//使用TSC校准APIC定时器，返回Hz
void lapic_timer_init(uint32_t freq_hz, uint8_t vector);
void lapic_timer_set_divisor(uint32_t divisor);//设置分频器

int apic_parse_madt(struct acpi_table_madt *madt);
void ioapic_init(void);//I/O APIC初始化

#endif
