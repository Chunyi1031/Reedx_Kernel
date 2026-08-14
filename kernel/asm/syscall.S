/*
 * kernel/asm/syscall.S — syscall 指令入口（ring3 → ring0）
 *
 * 由DeepSeek V4 Pro生成
 */

.section .bss
.align 4096
/* 单一用户任务时期的全局内核栈（后续集成调度器后改为 per-task） */
syscall_kernel_stack:
    .skip 4096
syscall_kernel_stack_top:
/* 保存用户 RSP（进入内核栈前） */
user_rsp_save:
    .skip 8

.section .text, "ax"
.globl syscall_entry
.type syscall_entry, @function
syscall_entry:
    /* 保存用户 RSP，切换到内核栈 */
    movq %rsp, user_rsp_save(%rip)
    leaq syscall_kernel_stack_top(%rip), %rsp

    /* 压入全部通用寄存器（含 syscall 硬件修改过的 rcx/r11） */
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

    /* 栈布局（rsp 指向 r15）：
     *   +0   r15 ... +32  r11(用户RFLAGS) +40  r10
     *   +72  rdi(参数1) +80 rsi(参数2) +88 rdx(参数3)
     *   +96  rcx(用户返回RIP) +104 rbx +112 rax(调用号)
     */

    /* 按 SysV ABI 传参给 C 分发器 */
    movq 112(%rsp), %rdi    /* rax = 调用号 */
    movq 72(%rsp), %rsi     /* rdi = 参数1 */
    movq 80(%rsp), %rdx     /* rsi = 参数2 */
    movq 88(%rsp), %rcx     /* rdx = 参数3 */
    subq $8, %rsp           /* 保持 16 字节栈对齐（call 前） */
    call syscall_dispatch
    addq $8, %rsp

    /* 返回值写回 rax 槽位 */
    movq %rax, 112(%rsp)

    /* 恢复寄存器（逆序弹栈） */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11               /* 恢复用户 RFLAGS（sysretq 需要） */
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx               /* 恢复用户返回 RIP（sysretq 需要） */
    popq %rbx
    popq %rax               /* 返回值 */

    /* 恢复用户 RSP，返回 ring3 */
    movq user_rsp_save(%rip), %rsp
    sysretq
