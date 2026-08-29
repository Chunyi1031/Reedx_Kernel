#ifndef _FORK_H_
#define _FORK_H_

#include <syscalls.h>

/**
 * @brief 复制当前进程
 * @return 成功返回子进程 PID，失败返回 -1
 */
pid_t do_fork(void);

/**
 * @brief 系统调用fork处理函数
 * @return 父进程返回子进程PID，子进程返回 0
 */
long sys_fork(long a, long b, long c, long a4, long a5, long a6);

#endif
