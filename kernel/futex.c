/*
 由DeepSeek V4 Flash生成
*/

#include <task.h>
#include <syscalls.h>
#include <irq.h>
#include <klib.h>
#include <mm/pmm.h>
#include <spinlock.h>
#include <futex.h>

/*
 * 极简futex实现(单核协作式调度)
 * 以用户虚拟地址为key的哈希等待队列, 支持:
 *   FUTEX_WAIT / FUTEX_WAKE / FUTEX_WAIT_BITSET / FUTEX_WAKE_BITSET
 *   FUTEX_REQUEUE / FUTEX_CMP_REQUEUE(简化为仅唤醒)
 *   FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME 标志剥除
 * 超时通过 SYSTEM_TimerTicks 截止tick + futex_timeout_check()(定时器中断)实现
 */

#define FUTEX_HASH_BITS   8
#define FUTEX_HASH_SIZE   (1 << FUTEX_HASH_BITS)

typedef struct futex_waitq {
    uintptr_t           key;   //用户futex地址
    struct list_node    wq;    //等待队列(wait_node挂载点)
    spinlock_t          lock;
    struct futex_waitq  *next; //哈希桶链表
} futex_waitq_t;

static futex_waitq_t *futex_hash[FUTEX_HASH_SIZE];

//本地链表辅助
static inline int futex_list_empty(struct list_node *h){ return h->next == h; }
static inline void futex_list_del(struct list_node *n){
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = NULL;
    n->next = NULL;
}
static inline void futex_list_add_tail(struct list_node *n, struct list_node *h){
    n->prev = h->prev;
    n->next = h;
    h->prev->next = n;
    h->prev = n;
}
#define futex_offsetof(t, m) ((size_t)&(((t*)0)->m))
#define futex_task_of(n)     ((task_struct*)((char*)(n) - futex_offsetof(task_struct, wait_node)))
#define futex_for_each_safe(pos, tmp, h) \
    for(pos = (h)->next, tmp = pos->next; pos != (h); pos = tmp, tmp = pos->next)

static uint32_t futex_hashfn(uintptr_t key){
    return (uint32_t)((key >> 4) ^ (key >> 16)) & (FUTEX_HASH_SIZE - 1);
}

//按key查找等待队列; create=1时不存在则新建(不释放, 生命周期同内核)
static futex_waitq_t *futex_get_waitq(uintptr_t key, int create){
    uint32_t h = futex_hashfn(key);
    futex_waitq_t *q;
    for(q = futex_hash[h]; q; q = q->next)
        if(q->key == key) return q;
    if(!create) return NULL;
    q = (futex_waitq_t*)PHYS_TO_VIRT(Pmm_Malloc(1));
    if(!q) return NULL;
    memset(q, 0, 4096);
    q->key = key;
    q->wq.prev = &q->wq;
    q->wq.next = &q->wq;
    spin_lock_init(&q->lock);
    q->next = futex_hash[h];
    futex_hash[h] = q;
    return q;
}

//FUTEX_WAIT: 若*uaddr==val则阻塞, 否则返回-EAGAIN; 被唤醒返回0, 超时返回-ETIMEDOUT
static long futex_wait(uintptr_t key, uint32_t *uaddr, long val, const timespec_t *timeout){
    uint32_t uval;
    //单检: 值已不匹配直接返回
    if(copy_from_user(&uval, uaddr, sizeof(uval))) return -EFAULT;
    if(uval != (uint32_t)val) return -EAGAIN;
    futex_waitq_t *q = futex_get_waitq(key, 1);
    if(!q) return -ENOMEM;
    unsigned long flags;
    spin_lock_irqsave(&q->lock, flags);
    //双检: 挂队列前值可能已变
    if(copy_from_user(&uval, uaddr, sizeof(uval)) == 0 && uval != (uint32_t)val){
        spin_unlock_irqrestore(&q->lock, flags);
        return -EAGAIN;
    }
    //解析超时(绝对截止tick)
    uint64_t deadline = 0;
    if(timeout){
        timespec_t ts;
        if(copy_from_user(&ts, timeout, sizeof(ts))){
            spin_unlock_irqrestore(&q->lock, flags);
            return -EFAULT;
        }
        if(ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L){
            spin_unlock_irqrestore(&q->lock, flags);
            return -EINVAL;
        }
        uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        deadline = SYSTEM_TimerTicks + (ms + 9) / 10 + 1;
    }
    //挂队列并阻塞
    current_task->futex_timedout = 0;
    current_task->wake_up_ticks = deadline;
    futex_list_add_tail(&current_task->wait_node, &q->wq);
    current_task->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&q->lock, flags);
    schedule();
    return current_task->futex_timedout ? -ETIMEDOUT : 0;
}

//FUTEX_WAKE: 唤醒至多nr_wake个等待者, 返回实际唤醒数
static long futex_wake(uintptr_t key, long nr_wake){
    if(nr_wake <= 0) return 0;
    futex_waitq_t *q = futex_get_waitq(key, 0);
    if(!q) return 0;
    unsigned long flags;
    spin_lock_irqsave(&q->lock, flags);
    int woken = 0;
    while(!futex_list_empty(&q->wq) && woken < nr_wake){
        task_struct *task = futex_task_of(q->wq.next);
        futex_list_del(&task->wait_node);
        task->futex_timedout = 0;
        task->wake_up_ticks = 0;
        if(task->state == TASK_BLOCKED) TaskListAdd(task);
        woken++;
    }
    spin_unlock_irqrestore(&q->lock, flags);
    return woken;
}

//定时器中断调用: 唤醒所有超时的futex等待者(置futex_timedout=1)
void futex_timeout_check(void){
    for(int h = 0; h < FUTEX_HASH_SIZE; h++){
        futex_waitq_t *q = futex_hash[h];
        if(!q) continue;
        unsigned long flags;
        spin_lock_irqsave(&q->lock, flags);
        struct list_node *pos, *tmp;
        futex_for_each_safe(pos, tmp, &q->wq){
            task_struct *task = futex_task_of(pos);
            if(task->wake_up_ticks > 0 && task->wake_up_ticks <= SYSTEM_TimerTicks){
                futex_list_del(&task->wait_node);
                task->futex_timedout = 1;
                task->wake_up_ticks = 0;
                if(task->state == TASK_BLOCKED) TaskListAdd(task);
            }
        }
        spin_unlock_irqrestore(&q->lock, flags);
    }
}

/*
 * int futex(int *uaddr, int futex_op, int val,
 *           const struct timespec *timeout, int *uaddr2, int val3)
 */
long sys_futex(long uaddr, long futex_op, long val, long timeout, long uaddr2, long val3){
    (void)uaddr2; (void)val3;
    if(!current_task || !current_task->mm) return -ENOSYS;
    if(!uaddr) return -EFAULT;
    int op = (int)futex_op;
    //剥除标志位
    if(op & FUTEX_PRIVATE_FLAG) op &= ~FUTEX_PRIVATE_FLAG;
    if(op & FUTEX_CLOCK_REALTIME) op &= ~FUTEX_CLOCK_REALTIME;
    uintptr_t key = (uintptr_t)uaddr;
    switch(op){
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET:
        return futex_wait(key, (uint32_t*)uaddr, val, (const timespec_t*)timeout);
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return futex_wake(key, val);
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        //简化: 只唤醒源队列, 不支持把等待者搬到uaddr2
        return futex_wake(key, val);
    default:
        return -ENOSYS;
    }
}
