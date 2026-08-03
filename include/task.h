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
    uint64_t            wake_up_ticks;//msleep超时tick(0=无超时)
} task_struct;

extern task_struct* current_task;//当前运行的任务
extern task_struct* kernel_task;//内核任务
extern task_struct* idle_task;//空闲任务

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
void msleep(uint64_t ms);//睡眠指定毫秒(阻塞,由timer IRQ唤醒)
void sleep_on(wait_queue_head_t *wq);//将当前任务挂到等待队列并切走
void wake_up(wait_queue_head_t *wq);//唤醒等待队列上的一个任务
void wake_up_all(wait_queue_head_t *wq);//唤醒等待队列上的全部任务
void sem_init(semaphore_t *sem, int initial_count);//初始化信号量
void sem_down(semaphore_t *sem);//P操作:获取信号量,count<=0时阻塞
void sem_up(semaphore_t *sem);//V操作:释放信号量,唤醒一个等待者
_Bool sem_trydown(semaphore_t *sem);//非阻塞尝试获取

void TaskExit();//任务退出
void schedule();//调度函数
void TaskKill(task_struct* t);//结束任务

task_struct* CreateKernelThread(void (*entry)(void),uint64_t stack_size,const char* name);//创建内核线程
void timeout_wake_check(void);//检查并唤醒超时任务(由timer IRQ调用)

#define DEFINE_WAIT_QUEUE(name)  wait_queue_head_t name = {&(name), &(name)}

#endif