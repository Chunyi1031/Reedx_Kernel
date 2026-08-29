#ifndef _FUTEX_H_
#define _FUTEX_H_

#include <types.h>

//定时器中断调用: 唤醒所有超时的futex等待者
void futex_timeout_check(void);

//系统调用: int futex(int *uaddr, int futex_op, int val,
//                    const struct timespec *timeout, int *uaddr2, int val3)
long sys_futex(long uaddr, long futex_op, long val, long timeout, long uaddr2, long val3);

#endif
