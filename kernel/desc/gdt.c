/*
 * kernel/desc/gdt.c — GDT 初始化与加载
 *
 * 原理：
 *   1. 定义静态 GDT 表（16 项，参考 Linux x86_64 布局）
 *   2. 使用 GDT_ENTRY_INIT 宏逐项填充描述符
 *   3. 构造 10 字节 GDTR 指针，执行 lgdt 加载
 *   4. 通过远返回（lretq）刷新 CS 进入新的 64 位代码段
 *   5. 重载 DS/ES/FS/GS/SS 全部数据段寄存器
 *
 *   注意：在 64 位长模式下 CS/DS/ES/SS 的基址和限长被硬件忽略，
 *   但 FS/GS 的基址仍然有效（通过 MSR 或描述符），这里全部设为平坦模式。
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/cpu/common.c 中的 GDT 初始化
 */
#include <desc.h>

/*
 * GDT 表 — 必须在全局数据段中定义，确保 lgdt 时地址有效
 *
 * 布局（与 Linux x86_64 一致）：
 *   [0]  NULL         — 硬件要求第一项必须为零
 *   [1]  KERNEL32_CS  — 内核 32 位代码段（兼容模式切换到 32 位）
 *   [2]  KERNEL_CS    — 内核 64 位代码段（当前运行环境）
 *   [3]  KERNEL_DS    — 内核数据段
 *   [4]  USER32_CS    — 用户 32 位代码段（用户态兼容模式）
 *   [5]  USER_DS      — 用户数据段
 *   [6]  USER_CS      — 用户 64 位代码段
 *   [7-15]            — 保留（TSS/LDT/percpu 等后续扩展）
 *
 * base=0, limit=0xFFFFF, G=1 → 覆盖 4GB，64 位模式足够
 */
static struct desc_struct gdt_table[GDT_ENTRIES] __attribute__((aligned(16))) = {
	[GDT_ENTRY_NULL]         = GDT_ENTRY_INIT(0, 0, 0),
	[GDT_ENTRY_KERNEL32_CS]  = GDT_ENTRY_INIT(DESC_KERNEL_CODE32, 0, 0xFFFFF),
	[GDT_ENTRY_KERNEL_CS]    = GDT_ENTRY_INIT(DESC_CODE64,        0, 0xFFFFF),
	[GDT_ENTRY_KERNEL_DS]    = GDT_ENTRY_INIT(DESC_DATA,          0, 0xFFFFF),
	[GDT_ENTRY_USER32_CS]    = GDT_ENTRY_INIT(DESC_USER_CODE32,   0, 0xFFFFF),
	[GDT_ENTRY_USER_DS]      = GDT_ENTRY_INIT(DESC_USER_DATA,     0, 0xFFFFF),
	[GDT_ENTRY_USER_CS]      = GDT_ENTRY_INIT(DESC_USER_CODE64,   0, 0xFFFFF),
};

/*
 * setup_gdt — 加载 GDT 并刷新全部段寄存器
 *
 * 调用时机：内核启动早期，在进入 C 代码后、使用任何段相关功能前。
 *
 * 步骤：
 *   1. 构造 GDTR 指针（表大小 = GDT_ENTRIES * 8 - 1）
 *   2. 执行 lgdt 指令加载新 GDT 到 GDTR
 *   3. lretq 远返回到 1f 标签，同时将 CS 设为 __KERNEL_CS
 *   4. 依次重载 DS/ES/FS/GS/SS 为 __KERNEL_DS（平坦数据段）
 */
__attribute__((optimize("-O0")))
void setup_gdt(void)
{
	struct desc_ptr gdtr;
	/* GDTR.size = 表字节数 - 1 */
	gdtr.size = (u16)(sizeof(gdt_table) - 1);
	gdtr.address = (u64)&gdt_table;
	/* lgdt 加载新 GDT 基址 */
	__asm__ volatile (
		"lgdt %0\n\t"
		:
		: "m" (gdtr)
		: "memory"
	);
	/*
	 * 远返回刷新 CS:
	 *   push 新 CS 选择子 → push 返回地址(标签 1) → lretq 弹出 RIP 和 CS
	 * 在 64 位模式下必须使用 lretq（REX.W 前缀的 far ret）
	 */
	__asm__ volatile (
		"pushq %0\n\t"
		"pushq $1f\n\t"
		"lretq\n"
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
}
