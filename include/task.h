#ifndef _TASK_H_
#define _TASK_H_

#include <klib.h>

#define TASK_READY      0
#define TASK_RUNNING    1
#define TASK_BLOCKED    2
#define TASK_TERMINATED 3

//线程上下文
typedef struct task_context {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) task_context_t;

struct list_node {
    struct list_node *prev;//指向前一个节点
    struct list_node *next;//指向下一个节点
};

typedef struct spinlock {
    volatile int locked;//0:未锁定,1:已锁定
} spinlock_t;

typedef struct list_node wait_queue_head_t;//等待队列头

typedef struct semaphore {
	int count;//信号量计数值(>0有资源,<=0无资源且|count|=等待者数)
	wait_queue_head_t wq;//等待队列
	spinlock_t lock;//保护信号量的自旋锁
} semaphore_t;

typedef struct task_struct {
    task_context_t      context;    //线程上下文
    pid_t               pid;        //进程ID
    pid_t               tgid;       //线程组ID
    int                 state;      //进程状态
    char                name[16];   //进程名
    void*               kernel_stack;//内核栈指针
    uint64_t            stack_size; //内核栈大小
    struct list_node    list;       //就绪链表节点
    struct list_node    wait_node;  //等待队列节点
    uint64_t            wake_up_ticks;//msleep超时tick
} task_struct;

typedef struct mutex {
    int locked;//0=未锁定,1=已锁定
    task_struct *owner;//持有者(NULL=无持有者,只有持有者能解锁)
    wait_queue_head_t wq;//等待队列
    spinlock_t lock;//保护内部状态
} mutex_t;

typedef struct msg_queue {
	void **buf;//消息指针循环缓冲区
	int capacity;//缓冲区容量
	int head;//读位置
	int tail;//写位置
	semaphore_t slots;//空闲槽位信号量(初始=capacity)
	semaphore_t items;//已填充消息信号量(初始=0)
	mutex_t lock;
} msg_queue_t;

typedef struct condition {
	wait_queue_head_t wq;//等待队列
	spinlock_t lock;
} condition_t;

extern task_struct* current_task;//当前运行的任务
extern task_struct* kernel_task;//内核任务
extern task_struct* idle_task;//空闲任务

void timeout_wake_check(void);//检查并唤醒超时任务
#define DEFINE_WAIT_QUEUE(name)  wait_queue_head_t name = {&(name), &(name)}

void TaskInit();//初始化多任务环境
void TaskListAdd(task_struct* t);//将任务加入任务链表
void TaskListRemove(task_struct* t);//将任务从任务链表中移除
task_struct* TaskFind(pid_t pid);//根据PID查找任务
task_struct* TaskPickNext(void);//从任务链表中取出下一个就绪任务
task_struct* TaskPeekNext(void);//查看下一个就绪任务
/**
 * @brief 获取所有存在的任务结构体指针
 * @param buf  输出缓冲区，存放 task_struct* 数组
 * @param max  缓冲区最大容量（指针个数）
 * @return 实际收集到的任务数量
 */
int TaskGetAll(task_struct **buf, int max);
/**
 * @brief 睡眠指定毫秒
 * @param ms 睡眠时间（毫秒）
 */
void msleep(uint64_t ms);
void sleep_on(wait_queue_head_t *wq);//将当前任务挂到等待队列并切走
void wake_up(wait_queue_head_t *wq);//唤醒等待队列上的一个任务
void wake_up_all(wait_queue_head_t *wq);//唤醒等待队列上的全部任务
/**
 * @brief 初始化信号量
 * @param sem 信号量结构体指针
 * @param initial_count 初始计数值
 */
void sem_init(semaphore_t *sem, int initial_count);
/**
 * @brief P操作:获取信号量,count<=0时阻塞
 * @param sem 信号量结构体指针
 */
void sem_down(semaphore_t *sem);
/**
 * @brief V操作:释放信号量,唤醒一个等待者
 * @param sem 信号量结构体指针
 */
void sem_up(semaphore_t *sem);
/**
 * @brief 非阻塞尝试获取信号量
 * @param sem 信号量结构体指针
 * @return 成功返回1,失败返回0
 */
_Bool sem_trydown(semaphore_t *sem);
/**
 * @brief 初始化互斥锁
 * @param m 互斥锁结构体指针
 */
void mutex_init(mutex_t *m);
/**
 * @brief 加锁,已被持有时阻塞等待
 * @param m 互斥锁结构体指针
 */
void mutex_lock(mutex_t *m);
/**
 * @brief 解锁(仅持有者可调用),唤醒等待者
 * @param m 互斥锁结构体指针
 */
void mutex_unlock(mutex_t *m);
/**
 * @brief 非阻塞尝试加锁:成功返1,失败返0
 * @param m 互斥锁结构体指针
 * @return 成功返回true,失败返回false
 */
_Bool mutex_trylock(mutex_t *m);
/**
 * @brief 初始化消息队列
 * @param q 消息队列结构体指针
 * @param buf 消息缓冲区指针数组（调用者分配）
 * @param capacity 缓冲区容量
 */
void msgq_init(msg_queue_t *q, void **buf, int capacity);
/**
 * @brief 发送消息(队列满时阻塞)
 * @param q 消息队列结构体指针
 * @param msg 消息指针
 */
void msgq_send(msg_queue_t *q, void *msg);
void *msgq_recv(msg_queue_t *q);//接收消息(队列空时阻塞)
/**
 * @brief 非阻塞发送消息
 * @param q 消息队列结构体指针
 * @param msg 消息指针
 * @return 成功返回true,队列满返回false
 */
_Bool msgq_trysend(msg_queue_t *q, void *msg);
void *msgq_tryrecv(msg_queue_t *q);//非阻塞接收:成功返指针,空返NULL
void cond_init(condition_t *cv);//初始化条件变量
void cond_wait(condition_t *cv, mutex_t *m);//释放m并睡眠,被唤醒后重新获取m
void cond_signal(condition_t *cv);//唤醒一个等待者
void cond_broadcast(condition_t *cv);//唤醒所有等待者

void TaskExit();
void schedule();//调度函数

/**
 * @brief 杀死指定任务
 * @param t 任务结构体指针
 * @author Liu Chunyi
 */
void TaskKill(task_struct* t);
/**
 * @brief 创建内核线程
 * @param entry 线程入口函数
 * @param stack_size 栈大小
 * @param name 线程名称
 * @return 成功返回线程结构体指针,失败返回NULL
 * @author Liu Chunyi
 */
task_struct* CreateKernelThread(void (*entry)(void),uint64_t stack_size,const char* name);//创建内核线程

#endif