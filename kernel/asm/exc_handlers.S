/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux7.1.3生成
 *
 * kernel/asm/exc_handlers.s-异常处理程序入口（宏批量生成）
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/head_64.S (early_idt_handler_array)
 */

#define NUM_EXCEPTION_VECTORS   32
/*
 * 错误码位掩码：位 i 为 1 表示第 i 号异常 CPU 自动压入错误码。
 * 含错误码的向量：8(#DF) 10(#TS) 11(#NP) 12(#SS) 13(#GP)
 *                  14(#PF) 17(#AC) 30(#SX)
 */
#define EXCEPTION_ERRCODE_MASK  0x40027D00

.section .text, "ax"

.globl exc_entries_start
.globl int_14_handler
.globl int_13_handler
.type int_14_handler, @function
.type int_13_handler, @function
.align 16
exc_entries_start:
	.set i, 0
	.rept NUM_EXCEPTION_VECTORS
	.align 16
	.if (EXCEPTION_ERRCODE_MASK >> i) & 1
		/* 有错误码：CPU 已压入，只需压向量号 */
		pushq $i
	.else
		/* 无错误码：压入 dummy 0 统一栈帧布局 */
		pushq $0
		pushq $i
	.endif
	jmp exc_common_handler
	.set i, i + 1
	.endr

/*
 * exc_common_handler — 异常公共入口
 * 后续 push 15 个寄存器 + sub $8 对齐后，vec_num 位于 RSP+128。
 */
exc_common_handler:
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
	movq 144(%rsp), %rsi
	call exc_dispatch
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

int_13_handler:
	cli
	pushq %rax
	pushq %rcx
	pushq %rdx
	pushq %rbx
	pushq %rbp
	pushq %rsi
	pushq %rdi
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	sub $8, %rsp
	movq 128(%rsp), %rdi
	movq 136(%rsp), %rsi
	call interrupt_GP
	movq gp_recover_ip(%rip), %rax
	testq %rax, %rax
	jz 1f
	movq %rax, 136(%rsp)
	movq $0, gp_recover_ip(%rip)
	addq $8, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rdi
	popq %rsi
	popq %rbp
	popq %rbx
	popq %rdx
	popq %rcx
	popq %rax
	addq $8, %rsp
	iretq
1:
	ud2

int_14_handler:
	cli
	pushq %rax
	pushq %rcx
	pushq %rdx
	pushq %rbx
	pushq %rbp
	pushq %rsi
	pushq %rdi
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	sub $8, %rsp 
	mov %cr2, %rdi
	movq 128(%rsp), %rsi
	movq 136(%rsp), %rdx
	call interrupt_PF
	addq $8, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rdi
	popq %rsi
	popq %rbp
	popq %rbx
	popq %rdx
	popq %rcx
	popq %rax
	addq $8, %rsp
	iretq
	sti
