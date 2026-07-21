/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * kernel/asm/irq_handlers.S — PIC 硬件中断处理程序入口（宏批量生成）
 *
 *
 * 参考：Linux 7.1.3 arch/x86/include/asm/irq_vectors.h (IRQ 向量布局)
 *              Linux 7.1.3 早期中断入口模式（与 exc_handlers.S 一致）
 */

#define NUM_IRQ_VECTORS 16

.section .text, "ax"

.globl irq_entries_start
.type irq_entries_start, @object
.align 16
irq_entries_start:
	.set i, 0
	.rept NUM_IRQ_VECTORS
	.align 16
	pushq $0
	pushq $(0x20 + i)
	jmp irq_common_handler
	.set i, i + 1
	.endr
.align 16

irq_common_handler:
	cli
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	subq $8, %rsp
	movq 128(%rsp), %rdi
	call irq_dispatch
	addq $8, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax
	addq $16, %rsp
	iretq
	sti