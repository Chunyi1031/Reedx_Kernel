/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * kernel/asm/exc_handlers.S
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 */
/*
 * 由Hermes Agent + DeepSeek-V4-Pro生成
 *
 * kernel/asm/exc_handlers.s-异常处理程序入口（宏批量生成）
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

/*
 * panic_enter — 软件 panic 的汇编入口
 * 调用时 %rdi = 消息指针, %rsi = panic 的调用点地址。这里按 exc_frame_t 的
 * 布局摆好一帧再交给 C, 让 panic 打出来的现场和硬件异常是同一套格式。
 */
.globl panic_enter
.type panic_enter, @function
panic_enter:
	subq $192, %rsp			/* 192 是 exc_frame_t(184) 向上取整, 留对齐余量 */
	movq %r15, 8(%rsp)
	movq %r14, 16(%rsp)
	movq %r13, 24(%rsp)
	movq %r12, 32(%rsp)
	movq %r11, 40(%rsp)
	movq %r10, 48(%rsp)
	movq %r9,  56(%rsp)
	movq %r8,  64(%rsp)
	movq %rbp, 72(%rsp)
	movq %rdi, 80(%rsp)		/* 消息指针稍后还要用, 先存现场 */
	movq %rsi, 88(%rsp)		/* panic 的调用点地址 */
	movq %rdx, 96(%rsp)
	movq %rcx, 104(%rsp)
	movq %rbx, 112(%rsp)
	movq %rax, 120(%rsp)
	movq $0xFF, 128(%rsp)		/* vector: 标记这是软件 panic, 不是硬件异常 */
	movq $0, 136(%rsp)		/* errcode */
	movq 88(%rsp), %rax
	movq %rax, 144(%rsp)		/* rip = panic 的调用点 */
	xorq %rax, %rax
	movw %cs, %ax
	movq %rax, 152(%rsp)		/* cs */
	pushfq
	popq %rax
	movq %rax, 160(%rsp)		/* rflags */
	leaq 200(%rsp), %rax
	movq %rax, 168(%rsp)		/* rsp = 调用 panic 时的栈顶 */
	xorq %rax, %rax
	movw %ss, %ax
	movq %rax, 176(%rsp)		/* ss */
	movq 80(%rsp), %rsi		/* 第二个参数: 消息指针 */
	movq %rsp, %rdi			/* 第一个参数: 帧指针 */
	andq $-16, %rsp			/* 调用前把栈对齐 */
	call panic_report
	hlt				/* panic_report 不返回, 这里只是保险 */

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
 * 后续 push 15 个寄存器 + sub $8 对齐后，栈帧就与 exc_frame_t 一一对应，
 * 直接把 %rsp 当参数交给 exc_dispatch。
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
	movq %rsp, %rdi
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
	pushq $13				/* 向量号, 让栈帧与公共入口一致 */
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
	movq %rsp, %rdi
	call interrupt_GP
	movq gp_recover_ip(%rip), %rax
	testq %rax, %rax
	jz 1f
	movq %rax, 144(%rsp)
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
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax
	addq $16, %rsp
	iretq
1:
	ud2

int_14_handler:
	cli
	pushq $14
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
	movq %rsp, %rdi
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
