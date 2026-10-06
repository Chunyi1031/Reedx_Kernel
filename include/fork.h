#ifndef _FORK_H_
#define _FORK_H_

#include <syscalls.h>

/**
 * @brief 复制当前进程
 * @return 成功返回子进程 PID，失败返回 -1
 */
pid_t do_fork(void);

/**
 * @brief 复制当前进程
 * @param child_stack 子进程用户栈顶
 * @param tls 子进程FS段基址
 * @return 成功返回子进程PID，失败返回-1
 */
pid_t do_fork_ex(uintptr_t child_stack, uint64_t tls);

long sys_clone(long flags, long child_stack, long ptid, long ctid, long tls, long a6);

/**
 * @brief 系统调用fork处理函数
 * @return 父进程返回子进程PID，子进程返回 0
 */
long sys_fork(long a, long b, long c, long a4, long a5, long a6);

/**
 * @brief 系统调用vfork处理函数
 * @return 父进程返回子进程PID，子进程返回 0
 */
long sys_vfork(long a, long b, long c, long a4, long a5, long a6);

#endif
