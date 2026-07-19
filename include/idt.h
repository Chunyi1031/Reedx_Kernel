/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * include/idt.h — x86_64 中断描述符表 (IDT) 定义与操作
 *
 *   初始化流程：
 *     1. 将 256 个门描述符全部填充为指向 default_int_handler_entry 的
 *        中断门（P=1, DPL=0, IST=0, selector=__KERNEL_CS）
 *     2. 构造 IDTR 描述符，执行 lidt 加载
 *     3. 此后 CPU 收到任何中断/异常都会进入 default_int_handler_entry
 *        汇编入口，该入口保存寄存器 → 调用 C 函数打印信息 →
 *        恢复寄存器 → iretq 返回
 *
 * 参考：Linux 7.1.3 arch/x86/include/asm/desc_defs.h (gate_struct)
 *              arch/x86/kernel/idt.c (idt_setup_from_table)
 */

#ifndef _ASM_IDT_H_
#define _ASM_IDT_H_

#include <types.h>
#include <desc.h>

//总条目数
#define IDT_ENTRIES             256
#define NUM_EXCEPTION_VECTORS   32

//门类型
enum {
	GATE_INTERRUPT  = 0xE,  //中断门：进入时 IF=0
	GATE_TRAP       = 0xF,  //陷阱门：进入时 IF 不变
	GATE_CALL       = 0xC,  //调用门（32 位遗留，64 位不使用)
	GATE_TASK       = 0x5,  //任务门（32 位遗留，64 位不使用）
};

//来自:https://wiki.osdev.org/Interrupt_Descriptor_Table
#define INT_GATE_DE		0 //除错误，故障，无错误码
#define INT_GATE_DB		1 //调试异常，陷阱,无错误码
#define INT_GATE_NMI	2 //不可屏蔽中断，中断，无错误码
#define INT_GATE_BP		3 //断点，陷阱，无错误码
#define INT_GATE_OF		4 //溢出，陷阱，无错误码
#define INT_GATE_BR		5 //越界，故障，无错误码
#define INT_GATE_UD		6 //未定义的协处理器指令，故障，无错误码
#define INT_GATE_NM		7 //设备不存在，故障，无错误码
#define INT_GATE_DF		8 //双重故障，中止，有错误码(0)
#define INT_GATE_9		9 //协处理器段溢出（保留），故障，无错误码
#define INT_GATE_TS		10//无效TSS，故障，有错误码
#define INT_GATE_NP		11//段不存在，故障，有错误码
#define INT_GATE_SS		12//堆栈段异常，故障，有错误码
#define INT_GATE_GP		13//一般保护异常，故障，有错误码
#define INT_GATE_PF		14//缺页异常，故障，有错误码
#define INT_GATE_15		15//Intel reserved. Do not use.
#define INT_GATE_MF		16//浮点错误，故障，无错误码
#define INT_GATE_AC		17//对齐检查，故障，有错误码(0)
#define INT_GATE_MC		18//机器检查，中止，无错误码
#define INT_GATE_XF		19//SIMD浮点异常，故障，无错误码
#define INT_GATE_VE		20//虚拟化异常，故障,无错误码
#define INT_GATE_CP		21//控制流保护，故障，有错误码

//特权级常量
#define DPL0            0x0
#define DPL3            0x3

//默认 IST（不使用中断栈表）
#define DEFAULT_IST     0

#define cli() __asm__ __volatile__("cli" ::: "memory") //禁用中断
#define sti() __asm__ __volatile__("sti" ::: "memory") //启用中断

/*
 * idt_bits — 门描述符的属性字节（字节 4-5 的位域分组）
 *
 * 硬件定义（字节 4 bits[2:0]=IST, bits[7:3]=0；
 *          字节 5 bits[3:0]=type, bit4=0, bits[6:5]=DPL, bit7=P）
 * 此结构将两个字节合并为一个 u16 位域，与 Linux 7.1.3 desc_defs.h 一致。
 */
struct idt_bits {
	u16 ist   : 3,   //中断栈表索引 (0=不使用 IST)
	     zero  : 5,   //必须为 0
	     type  : 5,   //门类型 (GATE_INTERRUPT=0xE, GATE_TRAP=0xF)
	     dpl   : 2,   //描述符特权级 (0=内核, 3=用户)
	     p     : 1;   //存在位 (1=有效)
} __attribute__((packed));

/*
 * gate_desc — x86_6416字节中断门描述符
 *
 * 与 32 位模式下 8 字节门描述符的区别：
 *   64 位新增 offset_high（处理程序地址高 32 位）和 reserved 字段。
 */
struct gate_desc {
	u16 offset_low;        /* offset[15:0] — 处理程序地址低 16 位 */
	u16 segment;           /* 代码段选择子 */
	struct idt_bits bits;  /* IST + type + DPL + P */
	u16 offset_middle;     /* offset[31:16] — 处理程序地址中 16 位 */
	u32 offset_high;       /* offset[63:32] — 处理程序地址高 32 位 */
	u32 reserved;          /* 保留，必须为 0 */
} __attribute__((packed));

/**
 * @brief 填充一个门描述符
 * @param type:  门类型 (GATE_INTERRUPT 或 GATE_TRAP)
 * @param func:  处理程序线性地址 (void*)
 * @param dpl:   描述符特权级 (DPL0 或 DPL3)
 * @param ist:   中断栈表索引 (0=不使用)
 * @param seg:   代码段选择子
 */
static inline void pack_gate(struct gate_desc *gate, unsigned int type, unsigned long func, unsigned int dpl, unsigned int ist, unsigned int seg)
{
	gate->offset_low  = (u16)(func >> 0);
	gate->offset_middle = (u16)(func >> 16);
	gate->offset_high = (u32)(func >> 32);
	gate->segment     = (u16)seg;
	gate->bits.ist    = ist;
	gate->bits.zero   = 0;
	gate->bits.type   = type;
	gate->bits.dpl    = dpl;
	gate->bits.p      = 1;
	gate->reserved    = 0;
}

//执行lidt指令将desc_ptr加载到IDTR
static inline void load_idt(const struct desc_ptr *dtr)
{
	__asm__ volatile ("lidt %0" : : "m" (*dtr) : "memory");
}

extern void default_int_handler_entry(void);//默认中断处理程序入口 (kernel/asm/idt_handler.S)
extern void default_int_handler(void);//默认中断处理程序(kernel/desc/idt.c)
extern void int_13_handler(void);
extern void int_14_handler(void);
extern void interrupt_GP(uint64_t error_code, uint64_t rip);
extern void interrupt_PF(uint64_t fault_addr, uint64_t error_code);
void setup_idt(void);//初始化并加载IDT，所有256个向量指向默认处理程序

/**
 * @brief 设置中断处理程序（中断门）
 * @param vector 中断号
 * @param handler 处理函数
 */
void set_intr_gate(uint32_t vector, void *handler);
/**
 * @brief 设置中断处理程序（陷阱门）
 * @param vector 中断号
 * @param handler 处理函数
 */
void set_trap_gate(uint32_t vector, void *handler);

extern char exc_entries_start[];//由 .rept 宏生成的 32 个异常入口桩数组，每个 16 字节对齐

void setup_exceptions(void);//初始化 32 个异常处理程序并注册到 IDT

#endif
