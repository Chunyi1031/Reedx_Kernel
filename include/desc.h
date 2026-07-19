/*
 * 由DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * include/desc.h — x86 段描述符与 GDT 操作
 *
 * 原理：
 *   GDT（Global Descriptor Table，全局描述符表）是 x86 保护模式的基石。
 *   在 64 位长模式下，分段机制被大幅简化——CS/DS/ES/SS 的基址和限长被
 *   硬件忽略（强制平坦模式），但 GDT 仍然必须存在，因为：
 *     1. 段选择子的 DPL 字段决定当前特权级（CPL）
 *     2. FS/GS 的基址仍然生效（用于 TLS、per-CPU 数据）
 *     3. 长模式代码段描述符中 L=1 标志位告知 CPU 当前运行在 64 位模式
 *
 *   每个段描述符 8 字节，布局如下（x86 小端序）：
 *     [63:56] base[31:24]
 *     [55:52] Flags(G|D|L|AVL)
 *     [51:48] limit[19:16]
 *     [47:40] Access(P|DPL[1:0]|S|Type[3:0])
 *     [39:32] base[23:16]
 *     [31:16] base[15:0]
 *     [15:0]  limit[15:0]
 *
 *   lgdt 指令加载 10 字节 GDTR（2 字节 size + 4/8 字节 address）。
 *   加载后必须用 far jump/ret 刷新 CS，再重载其余段寄存器。
 *
 *   32/64 位切换：
 *     本文件通过 types.h 中的 ARCH_X86_64 / ARCH_X86_32 宏，
 *     使用预处理条件编译选择对应的 GDT 布局和汇编指令序列。
 *     desc_ptr.address 使用 uintptr_t 自动适配指针宽度。
 *
 * 参考：Linux 7.1.3 arch/x86/include/asm/desc_defs.h / desc.h / segment.h
 */

#ifndef _ASM_DESC_H_
#define _ASM_DESC_H_

#include <types.h>

/*
 * 8 字节段描述符结构体
 * 位域顺序与硬件定义严格一致（x86 小端）
 * 32 位和 64 位共用同一结构——硬件位布局完全相同，
 * 区别仅在于 L（长模式）和 D（默认操作大小）标志位的组合。
 */
struct desc_struct {
	u16 limit0;         /* limit 低 16 位 */
	u16 base0;          /* base 低 16 位 */
	u16 base1 : 8,      /* base 位 16-23 */
	     type  : 4,     /* 段类型（0x8=代码段可执行, 0x2=数据段可写） */
	     s     : 1,     /* 1=代码/数据段, 0=系统段 */
	     dpl   : 2,     /* 描述符特权级 (0=内核, 3=用户) */
	     p     : 1;     /* 存在位 */
	u16 limit1 : 4,     /* limit 位 16-19 */
	     avl   : 1,     /* 软件可用位 */
	     l     : 1,     /* 长模式 (64 位代码段需置 1) */
	     d     : 1,     /* 默认操作大小 (1=32 位, 0=16 位) */
	     g     : 1,     /* 粒度 (1=4KB 页, 0=字节) */
	     base2 : 8;     /* base 位 24-31 */
} __attribute__((packed));

/*
 * GDTR / IDTR 寄存器格式
 * lgdt/lidt 指令的操作数。
 * 原理：address 字段使用 uintptr_t——在 ARCH_X86_64 下为 8 字节，
 * 在 ARCH_X86_32 下为 4 字节，lgdt 指令根据当前模式自动识别指针宽度。
 */
struct desc_ptr {
	u16 size;        /* 表字节数 - 1 */
	uintptr_t address; /* 表线性基址 */
} __attribute__((packed));

/* ========== GDT 入口索引 ========== */
#if defined(ARCH_X86_64) || defined(ARCH_AMD64)
/*
 * x86_64 布局（与 Linux x86_64 一致）：
 * 保留 32 位兼容代码段，支持切换回 32 位兼容模式运行遗留代码。
 */
#define GDT_ENTRY_NULL          0   /* 必须为空的第 0 项 */
#define GDT_ENTRY_KERNEL32_CS   1   /* 内核 32 位兼容代码段 */
#define GDT_ENTRY_KERNEL_CS     2   /* 内核 64 位代码段 */
#define GDT_ENTRY_KERNEL_DS     3   /* 内核数据段 */
#define GDT_ENTRY_USER32_CS     4   /* 用户 32 位代码段 */
#define GDT_ENTRY_USER_DS       5   /* 用户数据段 */
#define GDT_ENTRY_USER_CS       6   /* 用户 64 位代码段 */
#elif defined(ARCH_X86_32) || defined(ARCH_AMD32)
/*
 * x86_32 布局——无长模式，仅 32 位保护模式。
 * 代码段使用 D=1 标志（32 位默认操作大小）。
 */
#define GDT_ENTRY_NULL          0   /* 必须为空的第 0 项 */
#define GDT_ENTRY_KERNEL_CS     1   /* 内核 32 位代码段 */
#define GDT_ENTRY_KERNEL_DS     2   /* 内核数据段 */
#define GDT_ENTRY_USER_CS       3   /* 用户 32 位代码段 */
#define GDT_ENTRY_USER_DS       4   /* 用户数据段 */
#else
#error "Unknown architecture: must define ARCH_X86_64 or ARCH_X86_32"
#endif

#define GDT_ENTRIES             16  /* GDT 总条目数 */

/* ========== 段选择子（selector = index * 8 + RPL）========== */
#if defined(ARCH_X86_64) || defined(ARCH_AMD64)
#define __KERNEL_CS     (GDT_ENTRY_KERNEL_CS * 8)
#define __KERNEL_DS     (GDT_ENTRY_KERNEL_DS * 8)
#define __USER_CS       (GDT_ENTRY_USER_CS * 8 + 3)
#define __USER_DS       (GDT_ENTRY_USER_DS * 8 + 3)
#define __USER32_CS     (GDT_ENTRY_USER32_CS * 8 + 3)
#define __KERNEL32_CS   (GDT_ENTRY_KERNEL32_CS * 8)
#elif defined(ARCH_X86_32) || defined(ARCH_AMD32)
#define __KERNEL_CS     (GDT_ENTRY_KERNEL_CS * 8)
#define __KERNEL_DS     (GDT_ENTRY_KERNEL_DS * 8)
#define __USER_CS       (GDT_ENTRY_USER_CS * 8 + 3)
#define __USER_DS       (GDT_ENTRY_USER_DS * 8 + 3)
#endif

/* ========== 描述符访问字节标志位 ========== */
#define DESC_A      0x0001   /* 已访问 (Accessed) */
#define DESC_RW     0x0002   /* 可读 (代码段) / 可写 (数据段) */
#define DESC_EC     0x0004   /* 向下扩展 (数据段) / 一致 (代码段) */
#define DESC_E      0x0008   /* 可执行 (代码段) */
#define DESC_S      0x0010   /* 描述符类型: 1=代码/数据段 (非系统段) */
#define DESC_DPL0   0x0000   /* DPL = 0 (内核) */
#define DESC_DPL3   0x0060   /* DPL = 3 (用户) */
#define DESC_P      0x0080   /* 存在 (Present) */

/* 高半字标志位（相对于 16 位 flags 字段中的位偏移） */
#define DESC_AVL    (1 << 12) /* 软件可用 */
#define DESC_L      (1 << 13) /* 长模式 64 位代码 */
#define DESC_DB     (1 << 14) /* 默认操作大小 (32-bit) */
#define DESC_G      (1 << 15) /* 粒度 4KB */

/* ========== 常用段类型组合宏 ========== */
/*
 * 原理：
 *   L=1（长模式）仅在 x86_64 下有效，告知 CPU 代码段运行在 64 位模式。
 *   D/B=1（默认 32 位操作）在 32 位保护模式下是必需的；
 *   在 64 位模式下，数据段仍使用 D/B=1（兼容传统 32 位数据访问），
 *   代码段则使用 L=1 替代 D/B。
 *   G=1（4KB 粒度）配合 limit=0xFFFFF 覆盖完整的 4GB 地址空间。
 *
 *   DESC_CODE64:  P|DPL0|S|E|RW|A + L       （内核 64 位代码）
 *   DESC_CODE32:  P|DPL0|S|E|RW|A + D/B + G  （内核 32 位代码）
 *   DESC_DATA:    P|DPL0|S|RW|A + D/B + G    （内核数据，平坦 4GB）
 */

/* 内核 64 位代码段——仅 x86_64 长模式 */
#define DESC_CODE64         (DESC_A | DESC_RW | DESC_E | DESC_S | DESC_P | DESC_DPL0 | DESC_L)
/* 内核 32 位代码段——x86_32 保护模式 / x86_64 兼容模式 */
#define DESC_CODE32         (DESC_A | DESC_RW | DESC_E | DESC_S | DESC_P | DESC_DPL0 | DESC_DB | DESC_G)
/* 内核数据段——通用，两架构共用（D/B=1 在 32 位和 64 位下含义相同） */
#define DESC_DATA           (DESC_A | DESC_RW | DESC_S | DESC_P | DESC_DPL0 | DESC_DB | DESC_G)

/* 用户态对应 */
#define DESC_USER_CODE64    (DESC_A | DESC_RW | DESC_E | DESC_S | DESC_P | DESC_DPL3 | DESC_L)
#define DESC_USER_CODE32    (DESC_A | DESC_RW | DESC_E | DESC_S | DESC_P | DESC_DPL3 | DESC_DB | DESC_G)
#define DESC_USER_DATA      (DESC_A | DESC_RW | DESC_S | DESC_P | DESC_DPL3 | DESC_DB | DESC_G)

/* 向后兼容别名 */
#define DESC_KERNEL_CODE32  DESC_CODE32
#define DESC_USER_CODE32_   DESC_USER_CODE32

/*
 * GDT_ENTRY_INIT — 从 flags/base/limit 构造一个 desc_struct 初始化器
 *
 * flags:  16 位组合标志 (低 8 位 = Access Byte, 高 8 位 = Flags Nibble)
 * base:   32 位段基址（通常为 0，平坦模式）
 * limit:  20 位段限长（粒度为字节时最大 1MB，4KB 时最大 4GB）
 *
 * 此宏与架构无关——硬件位域布局在 32 位和 64 位下完全一致。
 */
#define GDT_ENTRY_INIT(flags, base, limit)                          \
	{                                                               \
		.limit0  = ((limit) >>  0) & 0xFFFF,                       \
		.limit1  = ((limit) >> 16) & 0x000F,                       \
		.base0   = ((base)  >>  0) & 0xFFFF,                       \
		.base1   = ((base)  >> 16) & 0x00FF,                       \
		.base2   = ((base)  >> 24) & 0x00FF,                       \
		.type    = ((flags) >>  0) & 0x000F,                       \
		.s       = ((flags) >>  4) & 0x0001,                       \
		.dpl     = ((flags) >>  5) & 0x0003,                       \
		.p       = ((flags) >>  7) & 0x0001,                       \
		.avl     = ((flags) >> 12) & 0x0001,                       \
		.l       = ((flags) >> 13) & 0x0001,                       \
		.d       = ((flags) >> 14) & 0x0001,                       \
		.g       = ((flags) >> 15) & 0x0001,                       \
	}

/*
 * load_gdt — 执行 lgdt 指令将 desc_ptr 加载到 GDTR
 *
 * 调用后 CS 中缓存的描述符不会自动刷新，必须紧跟一次远跳转/远返回
 * 或通过 setup_gdt() 提供的完整流程。
 */
static inline void load_gdt(const struct desc_ptr *dtr)
{
	__asm__ volatile ("lgdt %0" : : "m" (*dtr) : "memory");
}

/* 初始化并加载 GDT，同时刷新全部段寄存器 */
void setup_gdt(void);

#endif /* _ASM_DESC_H_ */
