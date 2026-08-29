/*
 * kernel/asm/syscall.S — syscall 指令入口（ring3 → ring0）
 *
 * 由DeepSeek V4 Pro生成
 *
 * 栈布局（进入内核栈后，从栈顶向下 push 16 个槽位）：
 *   +0   r15 ... +56  r8
 *   +64  rbp  +72  rdi(参数1)  +80  rsi(参数2)  +88  rdx(参数3)
 *   +96  rcx(用户返回RIP)  +104 rbx  +112 rax(调用号/返回值)
 *   +120 用户RSP（切栈前保存，恢复时 pop %rsp 还原）
 * 16 push = 128 字节，call 前 rsp 保持 16 字节对齐。
 */

.section .text, "ax"
.globl syscall_entry
.type syscall_entry, @function
syscall_entry:
    /* 保存参数4(r10)到全局变量,再用 r10 中转用户 RSP
       (r10 是 caller-saved,mmap 等 6 参数系统调用会用到它) */
    movq %r10, syscall_arg6(%rip)
    movq %rsp, %r10
    movq user_kernel_stack_top(%rip), %rsp
    pushq %r10               /* +120 用户RSP */
    pushq %rax               /* +112 调用号 */
    pushq %rbx               /* +104 */
    pushq %rcx               /* +96  用户返回RIP */
    pushq %rdx               /* +88  参数3 */
    pushq %rsi               /* +80  参数2 */
    pushq %rdi               /* +72  参数1 */
    pushq %rbp               /* +64 */
    pushq %r8                /* +56 */
    pushq %r9                /* +48 */
    pushq %r10               /* +40  r10 已被中转占用，槽位无意义（r10 为 caller-saved） */
    pushq %r11               /* +32  用户RFLAGS */
    pushq %r12               /* +24 */
    pushq %r13               /* +16 */
    pushq %r14               /* +8 */
    pushq %r15               /* +0 */

    /* 按 SysV ABI 传参给 C 分发器 */
    movq 112(%rsp), %rdi    /* rax = 调用号 */
    movq 72(%rsp), %rsi     /* rdi = 参数1 */
    movq 80(%rsp), %rdx     /* rsi = 参数2 */
    movq 88(%rsp), %rcx     /* rdx = 参数3 */
    movq 56(%rsp), %r8      /* r8 = 参数5 */
    movq 48(%rsp), %r9      /* r9 = 参数6 */
    call syscall_dispatch

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

    /* rsp 现在指向用户 RSP 槽位，直接恢复并返回 ring3 */
    popq %rsp
    sysretq

/* 参数4(r10)保存区:6 参数系统调用(mmap等)的第4个参数 */
.section .data
.globl syscall_arg6
syscall_arg6:
    .quad 0
