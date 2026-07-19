/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * kernel/asm/idt_handler.s — 默认中断/异常处理程序汇编入口
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/head_64.S (early_idt_handler_common)
 *              arch/x86/entry/entry_64.S (中断入口保存/恢复寄存器模式)
 */

.section .text, "ax"
.globl default_int_handler_entry
.type default_int_handler_entry, @function

default_int_handler_entry:
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

	subq $8, %rsp/*栈对齐调整*/
	call default_int_handler/*调用中断处理程序*/
	addq $8, %rsp/*恢复栈对齐*/

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

	iretq
	sti
