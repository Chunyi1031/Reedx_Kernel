#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

#include <types.h>

typedef struct spinlock {
    volatile int locked;//0:未锁定,1:已锁定
} spinlock_t;

#define spin_lock_init(lock) ((lock)->locked = 0)	//初始化自旋锁

//获取自旋锁（阻塞）
#define spin_lock(lock)                                                      \
	do {                                                                     \
		while (__sync_lock_test_and_set(&(lock)->locked, 1))                \
			__asm__ volatile("pause");                                     \
	} while (0)

//释放自旋锁
#define spin_unlock(lock) __sync_lock_release(&(lock)->locked)

//尝试获取自旋锁（非阻塞）
#define spin_trylock(lock) (!__sync_lock_test_and_set(&(lock)->locked, 1))

//查询锁状态（非原子读，仅调试用途）
#define spin_is_locked(lock) ((lock)->locked)

//关中断并获取自旋锁
#define spin_lock_irqsave(lock, flags)				\
	do {								\
		__asm__ volatile("pushfq\n popq %0"			\
				 : "=g"(flags)				\
				 :					\
				 : "memory");				\
		__asm__ volatile("cli" ::: "memory");			\
		spin_lock(lock);					\
	} while (0)

//释放自旋锁并恢复中断状态
#define spin_unlock_irqrestore(lock, flags)			\
	do {								\
		spin_unlock(lock);					\
		__asm__ volatile("pushq %0\n popfq"			\
				 :					\
				 : "g"(flags)				\
				 : "memory", "cc");			\
	} while (0)

#endif
