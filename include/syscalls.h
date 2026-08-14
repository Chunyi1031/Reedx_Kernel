#ifndef _SYSCALLS_H_
#define _SYSCALLS_H_

#include <types.h>

//POSIX/x86_64系统调用号
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_NANOSLEEP       35
#define SYS_GETPID          39
#define SYS_EXIT            60
#define SYS_EXIT_GROUP      231
#define SYSCALL_TABLE_SIZE  256

//errno值（POSIX）
#define EPERM               1
#define ENOENT              2
#define EINTR               4
#define EIO                 5
#define EBADF               9
#define EAGAIN              11
#define ENOMEM              12
#define EACCES              13
#define EFAULT              14
#define EBUSY               16
#define EEXIST              17
#define EINVAL              22
#define ENOSYS              38

extern void syscall_entry(void);//汇编入口（kernel/asm/syscall.S）

void InitSyscall(void);//初始化SYSCALL机制

/**
 * @brief 系统调用分发器（由汇编入口调用）
 * @param num 调用号
 * @param a1/a2/a3 参数 1~3
 * @return 系统调用返回值（负数 = -errno）
 */
long syscall_dispatch(long num, long a1, long a2, long a3);

#endif
