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
#define SYS_FORK            57
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

//POSIX timespec
typedef struct timespec {
    long tv_sec;//秒
    long tv_nsec;//纳秒（0 ~ 999999999）
} timespec_t;

extern void syscall_entry(void);//汇编入口（kernel/asm/syscall.S）

void InitSyscall(void);//初始化SYSCALL机制

/**
 * @brief 内核从用户空间拷贝（返回未拷贝字节数，0 = 成功）
 * @param to   用户目标地址
 * @param from 内核源地址
 * @param n    字节数
 */
uint64_t copy_from_user(void *to, const void *from, uint64_t n);
/**
 * @brief 用户从内核拷贝（返回未拷贝字节数，0 = 成功）
 * @param to   内核目标地址
 * @param from 用户源地址
 * @param n    字节数
 */
uint64_t copy_to_user(void *to, const void *from, uint64_t n);

/**
 * @brief 系统调用分发器（由汇编入口调用）
 * @param num 调用号
 * @param a1/a2/a3 参数 1~3
 * @return 系统调用返回值（负数 = -errno）
 */
long syscall_dispatch(long num, long a1, long a2, long a3);

/**
 * @brief 系统调用
 * @param num 调用号
 * @param arg1/arg2/arg3 参数 1~3
 * @return 系统调用返回值
 */
#define syscall(num, arg1, arg2, arg3) ({ \
    long __ret; \
    __asm__ volatile( \
        "syscall" \
        : "=a" (__ret) \
        : "a" (num), \
          "D" (arg1), \
          "S" (arg2), \
          "d" (arg3) \
        : "rcx", "r11", "memory" \
    ); \
    __ret; \
})


#endif
