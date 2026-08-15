/*
 * 由DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * kernel/desc/gdt.c — GDT 初始化与加载
 *
 * 原理：
 *   1. 定义静态 GDT 表，使用 GDT_ENTRY_INIT 宏逐项填充描述符
 *   2. 构造 GDTR 指针，执行 lgdt 加载
 *   3. 通过远返回刷新 CS 进入目标代码段
 *   4. 重载 DS/ES/FS/GS/SS 全部数据段寄存器
 *
 *   32/64 位切换：
 *     ARCH_X86_64:  使用 pushq/lretq，GDT 含 64 位代码段和 32 位兼容段
 *     ARCH_X86_32:  使用 pushl/lret，GDT 仅含 32 位保护模式段
 *
 *   注意：
 *     在 64 位长模式下 CS/DS/ES/SS 的基址和限长被硬件忽略，
 *     但 FS/GS 的基址仍然有效（通过 MSR 或描述符），这里全部设为平坦模式。
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/cpu/common.c 中的 GDT 初始化
 */
#include <desc.h>
#include <kstring.h>

/*
 * GDT 表 — 必须在全局数据段中定义，确保 lgdt 时地址有效
 *
 * base=0, limit=0xFFFFF, G=1 → 覆盖 4GB，32 位和 64 位均足够。
 * 64 位模式下需要完整的 64 位虚拟地址空间时，后续可通过 TSS/LDT 扩展。
 */
#if defined(ARCH_X86_64) || defined(ARCH_AMD64)
/*
 * x86_64 布局（与 Linux x86_64 一致）：
 *   [0]  NULL         — 硬件要求第一项必须为零
 *   [1]  KERNEL32_CS  — 内核 32 位代码段（兼容模式切换到 32 位）
 *   [2]  KERNEL_CS    — 内核 64 位代码段（当前运行环境）
 *   [3]  KERNEL_DS    — 内核数据段
 *   [4]  USER32_CS    — 用户 32 位代码段（用户态兼容模式）
 *   [5]  USER_DS      — 用户数据段
 *   [6]  USER_CS      — 用户 64 位代码段
 *   [7-15]            — 保留（TSS/LDT/percpu 等后续扩展）
 */
static struct desc_struct gdt_table[GDT_ENTRIES] __attribute__((aligned(16))) = {
	[GDT_ENTRY_NULL]         = GDT_ENTRY_INIT(0, 0, 0),
	[GDT_ENTRY_KERNEL32_CS]  = GDT_ENTRY_INIT(DESC_CODE32,      0, 0xFFFFF),
	[GDT_ENTRY_KERNEL_CS]    = GDT_ENTRY_INIT(DESC_CODE64,      0, 0xFFFFF),
	[GDT_ENTRY_KERNEL_DS]    = GDT_ENTRY_INIT(DESC_DATA,        0, 0xFFFFF),
	[GDT_ENTRY_USER32_CS]    = GDT_ENTRY_INIT(DESC_USER_CODE32, 0, 0xFFFFF),
	[GDT_ENTRY_USER_DS]      = GDT_ENTRY_INIT(DESC_USER_DATA,   0, 0xFFFFF),
	[GDT_ENTRY_USER_CS]      = GDT_ENTRY_INIT(DESC_USER_CODE64, 0, 0xFFFFF),
};
#elif defined(ARCH_X86_32) || defined(ARCH_AMD32)
/*
 * x86_32 布局（32 位保护模式标准布局）：
 *   [0]  NULL       — 硬件要求第一项必须为零
 *   [1]  KERNEL_CS  — 内核 32 位代码段（D=1 保护模式）
 *   [2]  KERNEL_DS  — 内核数据段（平坦 4GB）
 *   [3]  USER_CS    — 用户 32 位代码段（DPL=3）
 *   [4]  USER_DS    — 用户数据段（DPL=3）
 *   [5-15]          — 保留（TSS/LDT 等后续扩展）
 */
static struct desc_struct gdt_table[GDT_ENTRIES] __attribute__((aligned(16))) = {
	[GDT_ENTRY_NULL]       = GDT_ENTRY_INIT(0, 0, 0),
	[GDT_ENTRY_KERNEL_CS]  = GDT_ENTRY_INIT(DESC_CODE32,      0, 0xFFFFF),
	[GDT_ENTRY_KERNEL_DS]  = GDT_ENTRY_INIT(DESC_DATA,        0, 0xFFFFF),
	[GDT_ENTRY_USER_CS]    = GDT_ENTRY_INIT(DESC_USER_CODE32, 0, 0xFFFFF),
	[GDT_ENTRY_USER_DS]    = GDT_ENTRY_INIT(DESC_USER_DATA,   0, 0xFFFFF),
};
#endif

/*
 * setup_gdt — 加载 GDT 并刷新全部段寄存器
 *
 * 调用时机：内核启动早期，在进入 C 代码后、使用任何段相关功能前。
 *
 * 步骤：
 *   1. 构造 GDTR 指针（表大小 = GDT_ENTRIES * 8 - 1）
 *   2. 执行 lgdt 指令加载新 GDT 到 GDTR
 *   3. 远返回到 1f 标签，同时将 CS 设为目标选择子
 *   4. 依次重载 DS/ES/FS/GS/SS 为数据段选择子
 */
__attribute__((optimize("-O0")))
void setup_gdt(void)
{
	struct desc_ptr gdtr;
	/*
	 * GDTR.size = 表字节数 - 1。
	 * GDTR.address 使用 uintptr_t 强制转换——在 ARCH_X86_64 下为
	 * 64 位指针，在 ARCH_X86_32 下为 32 位指针，lgdt 自动适配。
	 */
	gdtr.size = (u16)(sizeof(gdt_table) - 1);
	gdtr.address = (uintptr_t)&gdt_table;

	/* lgdt 加载新 GDT 基址 */
	__asm__ volatile (
		"lgdt %0\n\t"
		:
		: "m" (gdtr)
		: "memory"
	);

	/*
	 * 远返回刷新 CS：
	 *   push 新 CS 选择子 → push 返回地址(标签 1) → lret 弹出 IP 和 CS
	 *
	 * 原理：
	 *   ARCH_X86_64: 使用 pushq（8 字节操作数）+ lretq（REX.W 前缀）
	 *    因为 64 位模式下栈操作和返回地址都是 8 字节。
	 *   ARCH_X86_32: 使用 pushl（4 字节操作数）+ lret
	 *    因为 32 位模式下栈操作和返回地址都是 4 字节。
	 */
#if defined(ARCH_X86_64) || defined(ARCH_AMD64)
	__asm__ volatile (
		"pushq %0\n\t"
		"leaq 1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n"
		"1:\n\t"
		"movw %1, %%ds\n\t"
		"movw %1, %%es\n\t"
		"movw %1, %%fs\n\t"
		"movw %1, %%gs\n\t"
		"movw %1, %%ss\n\t"
		:
		: "i" ((u16)__KERNEL_CS), "r" ((u16)__KERNEL_DS)
		: "rax", "memory"
	);
#elif defined(ARCH_X86_32) || defined(ARCH_AMD32)
	__asm__ volatile (
		"pushl %0\n\t"
		"pushl $1f\n\t"
		"lret\n"
		"1:\n\t"
		"movw %1, %%ds\n\t"
		"movw %1, %%es\n\t"
		"movw %1, %%fs\n\t"
		"movw %1, %%gs\n\t"
		"movw %1, %%ss\n\t"
		:
		: "i" ((u16)__KERNEL_CS), "r" ((u16)__KERNEL_DS)
		: "memory"
	);
#endif
}

/*
 * setup_tss — 初始化 TSS 并加载 TR
 *
 * 原理：
 *   ring3 → ring0 的中断/异常需要从 TSS.RSP0 读取内核栈指针。
 *   内核此前没有自己的 TSS，TR 指向 UEFI 的 TSS（低物理地址，
 *   用户页表不可见），导致 ring3 异常在栈切换阶段就三重错误。
 *
 * 步骤：
 *   1. 清零 TSS，设置 rsp0 为专用内核栈顶（高半地址）
 *   2. 将 16 字节 TSS 描述符写入 GDT 第 7 项（占用 7、8 两个槽位）
 *   3. ltr 加载任务寄存器
 */
static struct tss_struct cpu_tss __attribute__((aligned(16)));
static u8 tss_rsp0_stack[4096] __attribute__((aligned(16)));

void setup_tss(void)
{
	memset(&cpu_tss, 0, sizeof(cpu_tss));
	cpu_tss.rsp0 = (uint64_t)(tss_rsp0_stack + sizeof(tss_rsp0_stack));

	struct tss_desc *td = (struct tss_desc *)&gdt_table[GDT_ENTRY_TSS];
	uint64_t base = (uint64_t)&cpu_tss;
	uint32_t limit = sizeof(cpu_tss) - 1;
	td->limit0  = limit & 0xFFFF;
	td->base0   = base & 0xFFFF;
	td->base1   = (base >> 16) & 0xFF;
	td->type    = 0x9;      /* 64-bit TSS (available) */
	td->zero    = 0;
	td->dpl     = 0;
	td->p       = 1;
	td->limit1  = (limit >> 16) & 0xF;
	td->avl     = 0;
	td->zero2   = 0;
	td->g       = 0;
	td->base2   = (base >> 24) & 0xFF;
	td->base3   = (base >> 32);
	td->reserved = 0;

	__asm__ volatile ("ltr %0" : : "r"((u16)__TSS) : "memory");
}
